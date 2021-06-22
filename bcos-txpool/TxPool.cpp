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
#include <bcos-framework/interfaces/protocol/CommonError.h>
#include <tbb/parallel_for.h>
using namespace bcos;
using namespace bcos::txpool;
using namespace bcos::protocol;
using namespace bcos::crypto;
using namespace bcos::sync;
using namespace bcos::consensus;

void TxPool::start()
{
    m_transactionSync->start();
}

void TxPool::stop()
{
    m_transactionSync->stop();
}

void TxPool::asyncSubmit(bytesPointer _txData, TxSubmitCallback _txSubmitCallback,
    std::function<void(Error::Ptr)> _onRecv)
{
    asyncSubmitTransaction(_txData, _txSubmitCallback);
    if (!_onRecv)
    {
        return;
    }
    _onRecv(nullptr);
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
    auto onVerifyFinishedWrapper = [_onVerifyFinished](Error::Ptr _error, bool _ret) {
        if (!_onVerifyFinished)
        {
            return;
        }
        _onVerifyFinished(_error, _ret);
    };

    auto block = m_config->blockFactory()->createBlock(_block);
    auto self = std::weak_ptr<TxPool>(shared_from_this());
    m_worker->enqueue([self, _generatedNodeID, block, onVerifyFinishedWrapper]() {
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
                                  << LOG_KV("nodeId",
                                         txpool->m_transactionSync->config()->nodeID()->shortHex());
                onVerifyFinishedWrapper(nullptr, true);
                return;
            }
            TXPOOL_LOG(DEBUG) << LOG_DESC("asyncVerifyBlock") << LOG_KV("totalTxs", txsSize)
                              << LOG_KV("missedTxs", missedTxs->size());
            txpool->m_transactionSync->requestMissedTxs(
                _generatedNodeID, missedTxs, onVerifyFinishedWrapper);
        }
        catch (std::exception const& e)
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("asyncVerifyBlock exception")
                                << LOG_KV("error", boost::diagnostic_information(e));
        }
    });
}

void TxPool::asyncNotifyTxsSyncMessage(Error::Ptr _error, NodeIDPtr _nodeID, bytesConstRef _data,
    std::function<void(bytesConstRef _respData)> _sendResponse,
    std::function<void(Error::Ptr _error)> _onRecv)
{
    m_transactionSync->onRecvSyncMessage(_error, _nodeID, _data, _sendResponse);
    if (!_onRecv)
    {
        return;
    }
    _onRecv(nullptr);
}

void TxPool::notifyConnectedNodes(
    NodeIDSet const& _connectedNodes, std::function<void(Error::Ptr)> _onRecvResponse)
{
    m_transactionSync->config()->setConnectedNodeList(_connectedNodes);
    if (!_onRecvResponse)
    {
        return;
    }
    _onRecvResponse(nullptr);
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

// Note: the transaction must be all hit in local txpool
void TxPool::asyncFillBlock(HashListPtr _txsHash,
    std::function<void(Error::Ptr, bcos::protocol::TransactionsPtr)> _onBlockFilled)
{
    HashListPtr missedTxs = std::make_shared<HashList>();
    auto txs = m_txpoolStorage->fetchTxs(*missedTxs, *_txsHash);
    if (missedTxs->size() > 0)
    {
        TXPOOL_LOG(WARNING) << LOG_DESC("asyncFillBlock failed for missing some transactions")
                            << LOG_KV("missedTxsSize", missedTxs->size());
        _onBlockFilled(
            std::make_shared<Error>(CommonError::TransactionsMissing, "TransactionsMissing"),
            nullptr);
        return;
    }
    TXPOOL_LOG(DEBUG) << LOG_DESC("asyncFillBlock: hit all transactions")
                      << LOG_KV("size", txs->size());
    _onBlockFilled(nullptr, txs);
}


void TxPool::asyncMarkTxs(
    HashListPtr _txsHash, bool _sealedFlag, std::function<void(Error::Ptr)> _onRecvResponse)
{
    m_txpoolStorage->batchMarkTxs(*_txsHash, _sealedFlag);
    if (!_onRecvResponse)
    {
        return;
    }
    _onRecvResponse(nullptr);
}