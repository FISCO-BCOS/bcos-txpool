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
 * @brief an implementation of using memory to store transactions
 * @file MemoryStorage.cpp
 * @author: yujiechen
 * @date 2021-05-07
 */
#include "MemoryStorage.h"
#include <bcos-framework/interfaces/protocol/CommonError.h>
#include <tbb/parallel_invoke.h>

using namespace bcos;
using namespace bcos::txpool;
using namespace bcos::crypto;
using namespace bcos::protocol;

MemoryStorage::MemoryStorage(TxPoolConfig::Ptr _config) : m_config(_config)
{
    m_notifier = std::make_shared<ThreadPool>("txNotifier", m_config->notifierWorkerNum());
    m_worker = std::make_shared<ThreadPool>("txpoolWorker", 1);
}

TransactionStatus MemoryStorage::submitTransaction(
    bytesPointer _txData, TxSubmitCallback _txSubmitCallback)
{
    try
    {
        auto tx = m_config->txFactory()->createTransaction(ref(*_txData), false);
        return submitTransaction(tx, _txSubmitCallback);
    }
    catch (std::exception const& e)
    {
        TXPOOL_LOG(WARNING) << LOG_DESC("Invalid transaction for decode exception")
                            << LOG_KV("error", boost::diagnostic_information(e));
        notifyInvalidReceipt(HashType(), TransactionStatus::Malform, _txSubmitCallback);
        return TransactionStatus::Malform;
    }
}

TransactionStatus MemoryStorage::submitTransaction(
    Transaction::Ptr _tx, TxSubmitCallback _txSubmitCallback)
{
    if (_txSubmitCallback)
    {
        _tx->setSubmitCallback(_txSubmitCallback);
    }
    // verify the transaction
    auto result = m_config->txValidator()->verify(_tx);
    _tx->setImportTime(utcTime());
    if (result == TransactionStatus::None)
    {
        result = insert(_tx);
        {
            WriteGuard l(x_missedTxs);
            m_missedTxs.unsafe_erase(_tx->hash());
        }
    }
    auto txSubmitCallback = _tx->submitCallback();
    if (result != TransactionStatus::None && txSubmitCallback)
    {
        notifyInvalidReceipt(_tx->hash(), result, txSubmitCallback);
    }
    return result;
}

void MemoryStorage::notifyInvalidReceipt(
    HashType const& _txHash, TransactionStatus _status, TxSubmitCallback _txSubmitCallback)
{
    if (!_txSubmitCallback)
    {
        return;
    }
    // notify txResult
    auto txResult = m_config->txResultFactory()->createTxSubmitResult(_txHash, (int32_t)_status);
    auto error = std::make_shared<Error>(CommonError::SUCCESS, "success");
    _txSubmitCallback(error, txResult);
    TXPOOL_LOG(WARNING) << LOG_DESC("notifyReceipt: reject invalid tx")
                        << LOG_KV("tx", _txHash.abridged()) << LOG_KV("exception", _status);
}

TransactionStatus MemoryStorage::insert(Transaction::ConstPtr _tx)
{
    ReadGuard l(x_txpoolMutex);
    if (size() >= m_config->poolLimit())
    {
        return TransactionStatus::TxPoolIsFull;
    }
    auto txHash = _tx->hash();
    if (m_txsTable.count(txHash))
    {
        return TransactionStatus::AlreadyInTxPool;
    }
    auto txIterator = m_txsQueue.emplace(_tx).first;
    m_txsTable[txHash] = txIterator;
    m_onReady();
    preCommitTransaction(_tx);
    return TransactionStatus::None;
}

void MemoryStorage::preCommitTransaction(bcos::protocol::Transaction::ConstPtr _tx)
{
    auto self = std::weak_ptr<MemoryStorage>(shared_from_this());
    m_worker->enqueue([self, _tx]() {
        try
        {
            auto txpoolStorage = self.lock();
            if (!txpoolStorage)
            {
                return;
            }
            auto encodedData = _tx->encode(false);
            auto txsToStore = std::make_shared<std::vector<bytesPointer>>();
            txsToStore->emplace_back(
                std::make_shared<bytes>(encodedData.begin(), encodedData.end()));
            auto txsHash = std::make_shared<HashList>();
            txsHash->emplace_back(_tx->hash());
            txpoolStorage->m_config->ledger()->asyncStoreTransactions(
                txsToStore, txsHash, [txpoolStorage, _tx](Error::Ptr _error) {
                    if (_error->errorCode() == CommonError::SUCCESS)
                    {
                        return;
                    }
                    // Note: Will it cause an avalanche here?
                    txpoolStorage->preCommitTransaction(_tx);
                    TXPOOL_LOG(WARNING) << LOG_DESC("asyncPreStoreTransaction failed")
                                        << LOG_KV("errorCode", _error->errorCode())
                                        << LOG_KV("errorMsg", _error->errorMessage())
                                        << LOG_KV("tx", _tx->hash().abridged());
                });
        }
        catch (std::exception const& e)
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("preCommitTransaction exception")
                                << LOG_KV("error", boost::diagnostic_information(e))
                                << LOG_KV("tx", _tx->hash().abridged());
        }
    });
}

void MemoryStorage::batchInsert(Transactions const& _txs)
{
    for (auto tx : _txs)
    {
        insert(tx);
    }
    WriteGuard l(x_missedTxs);
    for (auto tx : _txs)
    {
        m_missedTxs.unsafe_erase(tx->hash());
    }
}

Transaction::ConstPtr MemoryStorage::removeWithoutLock(bcos::crypto::HashType const& _txHash)
{
    if (!m_txsTable.count(_txHash))
    {
        return nullptr;
    }
    auto txIterator = m_txsTable[_txHash];
    auto tx = *(txIterator);
    m_txsTable.unsafe_erase(_txHash);
    m_txsQueue.unsafe_erase(txIterator);
    return tx;
}

Transaction::ConstPtr MemoryStorage::remove(bcos::crypto::HashType const& _txHash)
{
    WriteGuard l(x_txpoolMutex);
    return removeWithoutLock(_txHash);
}

Transaction::ConstPtr MemoryStorage::removeSubmittedTxWithoutLock(
    TransactionSubmitResult::Ptr _txSubmitResult)
{
    auto tx = removeWithoutLock(_txSubmitResult->txHash());
    if (!tx)
    {
        return nullptr;
    }
    notifyTxResult(tx, _txSubmitResult);
    return tx;
}

Transaction::ConstPtr MemoryStorage::removeSubmittedTx(TransactionSubmitResult::Ptr _txSubmitResult)
{
    auto tx = remove(_txSubmitResult->txHash());
    if (!tx)
    {
        return nullptr;
    }
    notifyTxResult(tx, _txSubmitResult);
    return tx;
}
void MemoryStorage::notifyTxResult(
    Transaction::ConstPtr _tx, TransactionSubmitResult::Ptr _txSubmitResult)
{
    auto txSubmitCallback = _tx->submitCallback();
    if (!txSubmitCallback)
    {
        return;
    }
    // notify the transaction result to RPC
    auto self = std::weak_ptr<MemoryStorage>(shared_from_this());
    m_notifier->enqueue([self, _tx, _txSubmitResult, txSubmitCallback]() {
        try
        {
            auto memoryStorage = self.lock();
            if (!memoryStorage)
            {
                return;
            }
            auto error = std::make_shared<Error>(CommonError::SUCCESS, "success");
            txSubmitCallback(error, _txSubmitResult);
            // TODO: remove this log
            TXPOOL_LOG(TRACE) << LOG_DESC("notify submit result")
                              << LOG_KV("tx", _tx->hash().abridged());
        }
        catch (std::exception const& e)
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("notifyTxResult failed")
                                << LOG_KV("tx", _tx->hash().abridged())
                                << LOG_KV("errorInfo", boost::diagnostic_information(e));
        }
    });
}

void MemoryStorage::batchRemove(BlockNumber _batchId, TransactionSubmitResults const& _txsResult)
{
    NonceListPtr nonceList = std::make_shared<NonceList>();
    {
        // batch remove
        WriteGuard l(x_txpoolMutex);
        for (auto txResult : _txsResult)
        {
            auto tx = removeSubmittedTxWithoutLock(txResult);
            nonceList->emplace_back(tx->nonce());
        }
    }
    // update the ledger nonce
    m_config->txPoolNonceChecker()->batchInsert(_batchId, nonceList);
    // update the txpool nonce
    m_config->txPoolNonceChecker()->batchRemove(*nonceList);
}

TransactionsPtr MemoryStorage::fetchTxs(HashList& _missedTxs, HashList const& _txs)
{
    ReadGuard l(x_txpoolMutex);
    auto fetchedTxs = std::make_shared<Transactions>();
    _missedTxs.clear();
    for (auto const& hash : _txs)
    {
        if (!m_txsTable.count(hash))
        {
            _missedTxs.emplace_back(hash);
            continue;
        }
        auto tx = *(m_txsTable[hash]);
        fetchedTxs->emplace_back(std::const_pointer_cast<Transaction>(tx));
    }
    return fetchedTxs;
}

ConstTransactionsPtr MemoryStorage::fetchNewTxs(size_t _txsLimit)
{
    ReadGuard l(x_txpoolMutex);
    auto fetchedTxs = std::make_shared<ConstTransactions>();
    for (auto tx : m_txsQueue)
    {
        if (tx->synced())
        {
            continue;
        }
        tx->setSynced(true);
        fetchedTxs->emplace_back(tx);
        if (fetchedTxs->size() >= _txsLimit)
        {
            break;
        }
    }
    return fetchedTxs;
}

HashListPtr MemoryStorage::batchFetchTxs(
    size_t _txsLimit, TxsHashSetPtr _avoidTxs, bool _avoidDuplicate)
{
    UpgradableGuard l(x_txpoolMutex);
    auto fetchedTxs = std::make_shared<HashList>();
    for (auto tx : m_txsQueue)
    {
        if (_avoidDuplicate && tx->sealed())
        {
            continue;
        }
        auto txHash = tx->hash();
        if (m_invalidTxs.count(txHash))
        {
            continue;
        }
        auto result = m_config->txValidator()->duplicateTx(tx);
        if (result == TransactionStatus::NonceCheckFail)
        {
            continue;
        }
        if (result == TransactionStatus::BlockLimitCheckFail)
        {
            m_invalidTxs.insert(txHash);
            m_invalidNonces.insert(tx->nonce());
            continue;
        }
        if (_avoidTxs && _avoidTxs->count(txHash))
        {
            continue;
        }
        fetchedTxs->emplace_back(tx->hash());
        tx->setSealed(true);
        m_sealedTxsSize++;

        if (fetchedTxs->size() >= _txsLimit)
        {
            break;
        }
    }
    notifyUnsealedTxsSize();
    UpgradeGuard ul(l);
    removeInvalidTxs();
    return fetchedTxs;
}

void MemoryStorage::removeInvalidTxs()
{
    auto self = std::weak_ptr<MemoryStorage>(shared_from_this());
    m_notifier->enqueue([self]() {
        try
        {
            auto memoryStorage = self.lock();
            if (!memoryStorage)
            {
                return;
            }
            if (memoryStorage->m_invalidTxs.size() == 0)
            {
                return;
            }
            tbb::parallel_invoke(
                [memoryStorage]() {
                    // remove invalid txs
                    for (auto const& txHash : memoryStorage->m_invalidTxs)
                    {
                        auto txResult =
                            memoryStorage->m_config->txResultFactory()->createTxSubmitResult(
                                txHash, (int32_t)TransactionStatus::BlockLimitCheckFail);
                        memoryStorage->removeSubmittedTxWithoutLock(txResult);
                    }
                },
                [memoryStorage]() {
                    // remove invalid nonce
                    for (auto const& nonce : memoryStorage->m_invalidNonces)
                    {
                        memoryStorage->m_config->txPoolNonceChecker()->remove(nonce);
                    }
                });
            memoryStorage->m_invalidTxs.clear();
            memoryStorage->m_invalidNonces.clear();
        }
        catch (std::exception const& e)
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("removeInvalidTxs exception")
                                << LOG_KV("errorInfo", boost::diagnostic_information(e));
        }
    });
}

void MemoryStorage::clear()
{
    WriteGuard l(x_txpoolMutex);
    // Note: must clear m_txsTable firstly
    m_txsTable.clear();
    m_txsQueue.clear();
}

HashListPtr MemoryStorage::filterUnknownTxs(HashList const& _txsHashList, NodeIDPtr _peer)
{
    ReadGuard l(x_txpoolMutex);
    for (auto txHash : _txsHashList)
    {
        if (!m_txsTable.count(txHash))
        {
            continue;
        }
        auto tx = *(m_txsTable[txHash]);
        tx->appendKnownNode(_peer);
    }
    auto unknownTxsList = std::make_shared<HashList>();
    UpgradableGuard missedTxsLock(x_missedTxs);
    for (auto const& txHash : _txsHashList)
    {
        if (m_txsTable.count(txHash))
        {
            continue;
        }
        if (m_missedTxs.count(txHash))
        {
            continue;
        }
        unknownTxsList->push_back(txHash);
        m_missedTxs.insert(txHash);
    }
    if (m_missedTxs.size() >= m_config->poolLimit())
    {
        UpgradeGuard ul(missedTxsLock);
        m_missedTxs.clear();
    }
    return unknownTxsList;
}

void MemoryStorage::batchMarkTxs(bcos::crypto::HashList const& _txsHashList, bool _sealFlag)
{
    ReadGuard l(x_txpoolMutex);
    for (auto txHash : _txsHashList)
    {
        if (!m_txsTable.count(txHash))
        {
            TXPOOL_LOG(WARNING) << LOG_DESC("batchMarkTxs: missing transaction")
                                << LOG_KV("tx", txHash.abridged()) << LOG_KV("sealFlag", _sealFlag);
            continue;
        }
        auto tx = *(m_txsTable[txHash]);
        tx->setSealed(_sealFlag);
        if (_sealFlag && !tx->sealed())
        {
            m_sealedTxsSize++;
        }
        if (!_sealFlag && tx->sealed())
        {
            m_sealedTxsSize--;
        }
    }
    notifyUnsealedTxsSize();
}

size_t MemoryStorage::size() const
{
    ReadGuard l(x_txpoolMutex);
    return m_txsTable.size();
}

size_t MemoryStorage::unSealedTxsSize()
{
    ReadGuard l(x_txpoolMutex);
    if (m_txsTable.size() < m_sealedTxsSize)
    {
        m_sealedTxsSize = m_txsTable.size();
        return 0;
    }
    return (m_txsTable.size() - m_sealedTxsSize);
}

void MemoryStorage::notifyUnsealedTxsSize()
{
    auto unsealedTxsSize = unSealedTxsSize();
    m_config->sealer()->asyncNoteUnSealedTxsSize(unsealedTxsSize, [this](Error::Ptr _error) {
        if (_error->errorCode() == CommonError::SUCCESS)
        {
            return;
        }
        TXPOOL_LOG(WARNING) << LOG_DESC("notifyUnsealedTxsSize failed, retry again")
                            << LOG_KV("errorCode", _error->errorCode())
                            << LOG_KV("errorMsg", _error->errorMessage());
        this->notifyUnsealedTxsSize();
    });
}