/**
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @brief implementation for txpool
 * @file TxPool.cpp
 * @author: yujiechen
 * @date 2021-05-10
 */
#include "TxPool.h"
#include "bcos-txpool/txpool/validator/TxValidator.h"
#include "validator/LedgerNonceChecker.h"
#include <bcos-framework/interfaces/protocol/CommonError.h>
#include <bcos-framework/libtool/LedgerConfigFetcher.h>
#include <tbb/parallel_for.h>
using namespace bcos;
using namespace bcos::txpool;
using namespace bcos::protocol;
using namespace bcos::crypto;
using namespace bcos::sync;
using namespace bcos::consensus;
using namespace bcos::tool;
void TxPool::start()
{
    if (m_running)
    {
        TXPOOL_LOG(WARNING) << LOG_DESC("The txpool has already been started!");
        return;
    }
    m_transactionSync->start();
    m_running = true;
    TXPOOL_LOG(INFO) << LOG_DESC("Start the txpool.");
}

void TxPool::stop()
{
    if (!m_running)
    {
        TXPOOL_LOG(WARNING) << LOG_DESC("The txpool has already been stopped!");
        return;
    }
    if (m_worker)
    {
        m_worker->stop();
    }
    if (m_txpoolStorage)
    {
        m_txpoolStorage->stop();
    }
    m_transactionSync->stop();
    m_running = false;
    TXPOOL_LOG(INFO) << LOG_DESC("Stop the txpool.");
}

void TxPool::asyncSubmit(bytesPointer _txData, TxSubmitCallback _txSubmitCallback)
{
    asyncSubmitTransaction(_txData, _txSubmitCallback);
}

bool TxPool::checkExistsInGroup(TxSubmitCallback _txSubmitCallback)
{
    auto syncConfig = m_transactionSync->config();
    if (!_txSubmitCallback || syncConfig->existsInGroup())
    {
        return true;
    }
    // notify txResult
    auto txResult = m_config->txResultFactory()->createTxSubmitResult(
        HashType(), (int32_t)TransactionStatus::RequestNotBelongToTheGroup);
    _txSubmitCallback(nullptr, txResult);
    TXPOOL_LOG(WARNING) << LOG_DESC("Do not send transactions to nodes that are not in the group");
    return false;
}

void TxPool::asyncSealTxs(size_t _txsLimit, TxsHashSetPtr _avoidTxs,
    std::function<void(Error::Ptr, HashListPtr, HashListPtr)> _sealCallback)
{
    auto fetchedTxs = std::make_shared<HashList>();
    auto sysTxs = std::make_shared<HashList>();
    m_txpoolStorage->batchFetchTxs(fetchedTxs, sysTxs, _txsLimit, _avoidTxs, true);
    _sealCallback(nullptr, fetchedTxs, sysTxs);
}

void TxPool::asyncNotifyBlockResult(BlockNumber _blockNumber,
    TransactionSubmitResultsPtr _txsResult, std::function<void(Error::Ptr)> _onNotifyFinished)
{
    m_txpoolStorage->batchRemove(_blockNumber, *_txsResult);
    if (!_onNotifyFinished)
    {
        return;
    }
    _onNotifyFinished(nullptr);
}

void TxPool::asyncVerifyBlock(PublicPtr _generatedNodeID, bytesConstRef const& _block,
    std::function<void(Error::Ptr, bool)> _onVerifyFinished)
{
    auto block = m_config->blockFactory()->createBlock(_block);
    auto onVerifyFinishedWrapper = [_onVerifyFinished, block](Error::Ptr _error, bool _ret) {
        TXPOOL_LOG(INFO) << LOG_DESC("asyncVerifyBlock")
                         << LOG_KV("consNum",
                                block->blockHeader() ? block->blockHeader()->number() : -1)
                         << LOG_KV("code", _error ? _error->errorCode() : 0)
                         << LOG_KV("msg", _error ? _error->errorMessage() : "success")
                         << LOG_KV("result", _ret);
        if (!_onVerifyFinished)
        {
            return;
        }
        _onVerifyFinished(_error, _ret);
    };
    // Note: here must has thread pool for lock in the callback
    // use single thread here to decrease thread competition
    auto self = std::weak_ptr<TxPool>(shared_from_this());
    m_verifier->enqueue([self, _generatedNodeID, block, onVerifyFinishedWrapper]() {
        try
        {
            auto txpool = self.lock();
            if (!txpool)
            {
                onVerifyFinishedWrapper(
                    std::make_shared<Error>(-1, "asyncVerifyBlock failed for lock txpool failed"),
                    false);
                return;
            }
            size_t txsSize = block->transactionsHashSize();
            if (txsSize == 0)
            {
                onVerifyFinishedWrapper(nullptr, true);
                return;
            }
            auto missedTxs = std::make_shared<HashList>();
            auto txpoolStorage = txpool->m_txpoolStorage;
            for (size_t i = 0; i < txsSize; i++)
            {
                auto txHash = block->transactionHash(i);
                if (!(txpoolStorage->exist(txHash)))
                {
                    missedTxs->emplace_back(txHash);
                }
            }
            if (missedTxs->size() == 0)
            {
                TXPOOL_LOG(DEBUG) << LOG_DESC("asyncVerifyBlock: hit all transactions in txpool")
                                  << LOG_KV("consNum",
                                         block->blockHeader() ? block->blockHeader()->number() : -1)
                                  << LOG_KV("nodeId",
                                         txpool->m_transactionSync->config()->nodeID()->shortHex());
                onVerifyFinishedWrapper(nullptr, true);
                return;
            }
            TXPOOL_LOG(DEBUG) << LOG_DESC("asyncVerifyBlock")
                              << LOG_KV("consNum",
                                     block->blockHeader() ? block->blockHeader()->number() : -1)
                              << LOG_KV("totalTxs", txsSize)
                              << LOG_KV("missedTxs", missedTxs->size());
            txpool->m_transactionSync->requestMissedTxs(
                _generatedNodeID, missedTxs, block, onVerifyFinishedWrapper);
        }
        catch (std::exception const& e)
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("asyncVerifyBlock exception")
                                << LOG_KV("error", boost::diagnostic_information(e));
        }
    });
}

void TxPool::asyncNotifyTxsSyncMessage(Error::Ptr _error, std::string const& _uuid,
    NodeIDPtr _nodeID, bytesConstRef _data, std::function<void(Error::Ptr _error)> _onRecv)
{
    auto self = std::weak_ptr<TxPool>(shared_from_this());
    m_transactionSync->onRecvSyncMessage(
        _error, _nodeID, _data, [self, _uuid, _nodeID](bytesConstRef _respData) {
            try
            {
                auto txpool = self.lock();
                if (!txpool)
                {
                    return;
                }
                txpool->m_sendResponseHandler(
                    _uuid, bcos::protocol::ModuleID::TxsSync, _nodeID, _respData);
            }
            catch (std::exception const& e)
            {
                TXPOOL_LOG(WARNING) << LOG_DESC("asyncNotifyTxsSyncMessage: sendResponse failed")
                                    << LOG_KV("error", boost::diagnostic_information(e))
                                    << LOG_KV("uuid", _uuid) << LOG_KV("dst", _nodeID->shortHex());
            }
        });
    if (!_onRecv)
    {
        return;
    }
    _onRecv(nullptr);
}

void TxPool::notifyConsensusNodeList(
    ConsensusNodeList const& _consensusNodeList, std::function<void(Error::Ptr)> _onRecvResponse)
{
    m_transactionSync->config()->setConsensusNodeList(_consensusNodeList);
    if (!_onRecvResponse)
    {
        return;
    }
    _onRecvResponse(nullptr);
}

void TxPool::notifyObserverNodeList(
    ConsensusNodeList const& _observerNodeList, std::function<void(Error::Ptr)> _onRecvResponse)
{
    m_transactionSync->config()->setObserverList(_observerNodeList);
    if (!_onRecvResponse)
    {
        return;
    }
    _onRecvResponse(nullptr);
}

void TxPool::getTxsFromLocalLedger(HashListPtr _txsHash, HashListPtr _missedTxs,
    std::function<void(Error::Ptr, TransactionsPtr)> _onBlockFilled)
{
    // fetch from the local ledger
    auto self = std::weak_ptr<TxPool>(shared_from_this());
    m_worker->enqueue([self, _txsHash, _missedTxs, _onBlockFilled]() {
        auto txpool = self.lock();
        if (!txpool)
        {
            _onBlockFilled(
                std::make_shared<Error>(CommonError::TransactionsMissing, "TransactionsMissing"),
                nullptr);
            return;
        }
        auto sync = txpool->m_transactionSync;
        sync->requestMissedTxs(nullptr, _missedTxs, nullptr,
            [txpool, _txsHash, _onBlockFilled](Error::Ptr _error, bool _verifyResult) {
                if (_error || !_verifyResult)
                {
                    TXPOOL_LOG(WARNING)
                        << LOG_DESC("getTxsFromLocalLedger failed")
                        << LOG_KV("code", _error ? _error->errorCode() : 0)
                        << LOG_KV("msg", _error ? _error->errorMessage() : "fetchSucc")
                        << LOG_KV("verifyResult", _verifyResult);
                    _onBlockFilled(std::make_shared<Error>(
                                       CommonError::TransactionsMissing, "TransactionsMissing"),
                        nullptr);
                    return;
                }
                TXPOOL_LOG(INFO) << LOG_DESC(
                    "asyncFillBlock miss and try to get the transaction from the ledger success");
                txpool->fillBlock(_txsHash, _onBlockFilled, false);
            });
    });
}

// Note: the transaction must be all hit in local txpool
void TxPool::asyncFillBlock(
    HashListPtr _txsHash, std::function<void(Error::Ptr, TransactionsPtr)> _onBlockFilled)
{
    fillBlock(_txsHash, _onBlockFilled, true);
}

void TxPool::fillBlock(HashListPtr _txsHash,
    std::function<void(Error::Ptr, TransactionsPtr)> _onBlockFilled, bool _fetchFromLedger)
{
    HashListPtr missedTxs = std::make_shared<HashList>();
    auto txs = m_txpoolStorage->fetchTxs(*missedTxs, *_txsHash);
    if (missedTxs->size() > 0)
    {
        TXPOOL_LOG(WARNING) << LOG_DESC("asyncFillBlock failed for missing some transactions")
                            << LOG_KV("missedTxsSize", missedTxs->size());
        if (_fetchFromLedger)
        {
            TXPOOL_LOG(INFO) << LOG_DESC("getTxsFromLocalLedger")
                             << LOG_KV("txsSize", _txsHash->size())
                             << LOG_KV("missedSize", missedTxs->size());
            getTxsFromLocalLedger(_txsHash, missedTxs, _onBlockFilled);
        }
        else
        {
            _onBlockFilled(
                std::make_shared<Error>(CommonError::TransactionsMissing, "TransactionsMissing"),
                nullptr);
        }
        return;
    }
    TXPOOL_LOG(DEBUG) << LOG_DESC("asyncFillBlock: hit all transactions")
                      << LOG_KV("size", txs->size());
    _onBlockFilled(nullptr, txs);
}


void TxPool::asyncMarkTxs(HashListPtr _txsHash, bool _sealedFlag,
    bcos::protocol::BlockNumber _batchId, bcos::crypto::HashType const& _batchHash,
    std::function<void(Error::Ptr)> _onRecvResponse)
{
    m_txpoolStorage->batchMarkTxs(*_txsHash, _batchId, _batchHash, _sealedFlag);
    if (!_onRecvResponse)
    {
        return;
    }
    _onRecvResponse(nullptr);
}

void TxPool::asyncResetTxPool(std::function<void(Error::Ptr)> _onRecvResponse)
{
    // mark all the transactions as unsealed
    m_txpoolStorage->batchMarkAllTxs(false);
    if (!_onRecvResponse)
    {
        return;
    }
    TXPOOL_LOG(INFO) << LOG_DESC("asyncResetTxPool") << LOG_KV("txsSize", m_txpoolStorage->size());
    _onRecvResponse(nullptr);
}

void TxPool::init()
{
    initSendResponseHandler();
    auto ledgerConfigFetcher = std::make_shared<LedgerConfigFetcher>(m_config->ledger());
    TXPOOL_LOG(INFO) << LOG_DESC("fetch LedgerConfig information");
    ledgerConfigFetcher->fetchBlockNumberAndHash();
    ledgerConfigFetcher->fetchConsensusNodeList();
    ledgerConfigFetcher->fetchObserverNodeList();
    ledgerConfigFetcher->waitFetchFinished();
    TXPOOL_LOG(INFO) << LOG_DESC("fetch LedgerConfig success");

    auto blockLimit = m_config->blockLimit();
    auto ledgerConfig = ledgerConfigFetcher->ledgerConfig();
    auto startNumber =
        (ledgerConfig->blockNumber() > blockLimit ? (ledgerConfig->blockNumber() - blockLimit + 1) :
                                                    0);
    if (startNumber > 0)
    {
        auto toNumber = ledgerConfig->blockNumber();
        auto fetchedSize = std::min(blockLimit, (toNumber - startNumber + 1));
        TXPOOL_LOG(INFO) << LOG_DESC("fetch history nonces information")
                         << LOG_KV("startNumber", startNumber)
                         << LOG_KV("fetchedSize", fetchedSize);
        ledgerConfigFetcher->fetchNonceList(startNumber, fetchedSize);
    }
    ledgerConfigFetcher->waitFetchFinished();
    TXPOOL_LOG(INFO) << LOG_DESC("fetch history nonces success");

    // create LedgerNonceChecker and set it into the validator
    TXPOOL_LOG(INFO) << LOG_DESC("init txs validator");
    auto ledgerNonceChecker = std::make_shared<LedgerNonceChecker>(
        ledgerConfigFetcher->nonceList(), ledgerConfig->blockNumber(), blockLimit);

    auto validator = std::dynamic_pointer_cast<TxValidator>(m_config->txValidator());
    validator->setLedgerNonceChecker(ledgerNonceChecker);
    TXPOOL_LOG(INFO) << LOG_DESC("init txs validator success");

    // init syncConfig
    TXPOOL_LOG(INFO) << LOG_DESC("init sync config");
    auto txsSyncConfig = m_transactionSync->config();
    txsSyncConfig->setConsensusNodeList(ledgerConfig->consensusNodeList());
    txsSyncConfig->setObserverList(ledgerConfig->observerNodeList());
    TXPOOL_LOG(INFO) << LOG_DESC("init sync config success");

    auto self = std::weak_ptr<TxPool>(shared_from_this());
    txsSyncConfig->frontService()->asyncGetNodeIDs(
        [self](Error::Ptr _error, std::shared_ptr<const crypto::NodeIDs> _nodeIDs) {
            if (_error != nullptr)
            {
                TXPOOL_LOG(WARNING)
                    << LOG_DESC("asyncGetNodeIDs failed") << LOG_KV("code", _error->errorCode())
                    << LOG_KV("msg", _error->errorMessage());
                return;
            }
            try
            {
                if (!_nodeIDs || _nodeIDs->size() == 0)
                {
                    return;
                }
                auto txpool = self.lock();
                if (!txpool)
                {
                    return;
                }
                NodeIDSet nodeIdSet(_nodeIDs->begin(), _nodeIDs->end());
                txpool->m_transactionSync->config()->setConnectedNodeList(std::move(nodeIdSet));
                TXPOOL_LOG(INFO) << LOG_DESC("asyncGetNodeIDs")
                                 << LOG_KV("connectedSize", _nodeIDs->size());
            }
            catch (std::exception const& e)
            {
                TXPOOL_LOG(WARNING) << LOG_DESC("asyncGetNodeIDs exception")
                                    << LOG_KV("error", boost::diagnostic_information(e));
            }
        });
}

void TxPool::initSendResponseHandler()
{
    // set the sendResponse callback
    std::weak_ptr<bcos::front::FrontServiceInterface> weakFrontService =
        m_transactionSync->config()->frontService();
    m_sendResponseHandler = [weakFrontService](std::string const& _id, int _moduleID,
                                NodeIDPtr _dstNode, bytesConstRef _data) {
        try
        {
            auto frontService = weakFrontService.lock();
            if (!frontService)
            {
                return;
            }
            frontService->asyncSendResponse(
                _id, _moduleID, _dstNode, _data, [_id, _moduleID, _dstNode](Error::Ptr _error) {
                    if (_error)
                    {
                        TXPOOL_LOG(WARNING) << LOG_DESC("sendResonse failed") << LOG_KV("uuid", _id)
                                            << LOG_KV("module", std::to_string(_moduleID))
                                            << LOG_KV("dst", _dstNode->shortHex())
                                            << LOG_KV("code", _error->errorCode())
                                            << LOG_KV("msg", _error->errorMessage());
                    }
                });
        }
        catch (std::exception const& e)
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("sendResonse exception")
                                << LOG_KV("error", boost::diagnostic_information(e))
                                << LOG_KV("uuid", _id) << LOG_KV("moduleID", _moduleID)
                                << LOG_KV("peer", _dstNode->shortHex());
        }
    };
}
