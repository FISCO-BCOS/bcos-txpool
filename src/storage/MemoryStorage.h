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
 * @file MemoryStorage.h
 * @author: yujiechen
 * @date 2021-05-07
 */
#pragma once
#include "TxPoolConfig.h"
#define TBB_PREVIEW_CONCURRENT_ORDERED_CONTAINERS 1
#include <bcos-framework/libutilities/ThreadPool.h>
#include <tbb/concurrent_set.h>
#include <tbb/concurrent_unordered_map.h>

namespace bcos
{
namespace txpool
{
struct TransactionCompare
{
    bool operator()(
        bcos::protocol::Transaction::Ptr _first, bcos::protocol::Transaction::Ptr _second) const
    {
        return _first->importTime() <= _second->importTime();
    }
};
class MemoryStorage : public TxPoolStorageInterface,
                      public std::enable_shared_from_this<MemoryStorage>
{
public:
    explicit MemoryStorage(TxPoolConfig::Ptr _config);
    ~MemoryStorage() override {}

    bool insert(bcos::protocol::Transaction::Ptr _tx) override;
    void batchInsert(bcos::protocol::Transactions const& _txs) override;

    bcos::protocol::Transaction::Ptr remove(bcos::crypto::HashType const& _txHash) override;
    void batchRemove(bcos::protocol::BlockNumber _batchId,
        bcos::protocol::TransactionSubmitResults const& _txsResult) override;
    bcos::protocol::Transaction::Ptr removeSubmittedTx(
        bcos::protocol::TransactionSubmitResult::Ptr _txSubmitResult) override;

    bcos::protocol::TransactionsPtr fetchTxs(
        TxsHashSetPtr _missedTxs, TxsHashSetPtr _txsList) override;
    bcos::protocol::TransactionsPtr fetchNewTxs(size_t _txsLimit) override;
    bcos::protocol::TransactionsPtr batchFetchTxs(
        size_t _txsLimit, TxsHashSetPtr _avoidTxs, bool _avoidDuplicate = true) override;

    size_t size() const override;
    void clear() override;

protected:
    virtual void notifyTxResult(bcos::protocol::Transaction::Ptr _tx,
        bcos::protocol::TransactionSubmitResult::Ptr _txSubmitResult);

    virtual void removeInvalidTxs();

private:
    TxPoolConfig::Ptr m_config;
    ThreadPool::Ptr m_notifier;

    using TransactionQueue =
        tbb::concurrent_set<bcos::protocol::Transaction::Ptr, TransactionCompare>;
    TransactionQueue m_txsQueue;
    // to accelerate txs query for unordered for unorder performance is higher than non-unorder in
    // big data scenarios
    tbb::concurrent_unordered_map<bcos::crypto::HashType, TransactionQueue::iterator,
        std::hash<bcos::crypto::HashType>>
        m_txsTable;

    tbb::concurrent_set<bcos::crypto::HashType> m_invalidTxs;
    tbb::concurrent_set<bcos::protocol::NonceType> m_invalidNonces;
};
}  // namespace txpool
}  // namespace bcos