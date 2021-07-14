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
 * @file TxPool.h
 * @author: yujiechen
 * @date 2021-05-10
 */
#pragma once
#include "bcos-txpool/TxPoolConfig.h"
#include "bcos-txpool/sync/interfaces/TransactionSyncInterface.h"
#include "bcos-txpool/txpool/interfaces/TxPoolStorageInterface.h"
#include <bcos-framework/interfaces/txpool/TxPoolInterface.h>
#include <bcos-framework/libutilities/ThreadPool.h>
namespace bcos
{
namespace txpool
{
class TxPool : public TxPoolInterface, public std::enable_shared_from_this<TxPool>
{
public:
    using Ptr = std::shared_ptr<TxPool>;
    TxPool(TxPoolConfig::Ptr _config, TxPoolStorageInterface::Ptr _txpoolStorage,
        bcos::sync::TransactionSyncInterface::Ptr _transactionSync)
      : m_config(_config), m_txpoolStorage(_txpoolStorage), m_transactionSync(_transactionSync)
    {
        m_worker = std::make_shared<ThreadPool>("submitter", _config->verifyWorkerNum());
    }

    ~TxPool() override { stop(); }

    void start() override;
    void stop() override;

    void asyncSubmit(
        bytesPointer _txData, bcos::protocol::TxSubmitCallback _txSubmitCallback) override;

    void asyncSealTxs(size_t _txsLimit, TxsHashSetPtr _avoidTxs,
        std::function<void(Error::Ptr, bcos::crypto::HashListPtr, bcos::crypto::HashListPtr)>
            _sealCallback) override;

    void asyncNotifyBlockResult(bcos::protocol::BlockNumber _blockNumber,
        bcos::protocol::TransactionSubmitResultsPtr _txsResult,
        std::function<void(Error::Ptr)> _onNotifyFinished) override;

    void asyncVerifyBlock(bcos::crypto::PublicPtr _generatedNodeID, bytesConstRef const& _block,
        std::function<void(Error::Ptr, bool)> _onVerifyFinished) override;

    void asyncNotifyTxsSyncMessage(bcos::Error::Ptr _error, std::string const& _uuid,
        bcos::crypto::NodeIDPtr _nodeID, bytesConstRef _data,
        std::function<void(Error::Ptr _error)> _onRecv) override;

    void notifyConnectedNodes(bcos::crypto::NodeIDSet const& _connectedNodes,
        std::function<void(Error::Ptr)> _onRecvResponse) override;

    void notifyConsensusNodeList(bcos::consensus::ConsensusNodeList const& _consensusNodeList,
        std::function<void(Error::Ptr)> _onRecvResponse) override;

    void asyncFillBlock(bcos::crypto::HashListPtr _txsHash,
        std::function<void(Error::Ptr, bcos::protocol::TransactionsPtr)> _onBlockFilled) override;

    void notifyObserverNodeList(bcos::consensus::ConsensusNodeList const& _observerNodeList,
        std::function<void(Error::Ptr)> _onRecvResponse) override;

    void asyncMarkTxs(bcos::crypto::HashListPtr _txsHash, bool _sealedFlag,
        std::function<void(Error::Ptr)> _onRecvResponse) override;

    void asyncResetTxPool(std::function<void(Error::Ptr)> _onRecvResponse) override;

    TxPoolConfig::Ptr txpoolConfig() { return m_config; }
    TxPoolStorageInterface::Ptr txpoolStorage() { return m_txpoolStorage; }

    bcos::sync::TransactionSyncInterface::Ptr transactionSync() { return m_transactionSync; }
    void setTransactionSync(bcos::sync::TransactionSyncInterface::Ptr _transactionSync)
    {
        m_transactionSync = _transactionSync;
    }

    virtual void init();
    virtual void registerUnsealedTxsNotifier(
        std::function<void(size_t, std::function<void(Error::Ptr)>)> _unsealedTxsNotifier)
    {
        m_txpoolStorage->registerUnsealedTxsNotifier(_unsealedTxsNotifier);
    }

    void asyncGetPendingTransactionSize(
        std::function<void(Error::Ptr, size_t)> _onGetTxsSize) override
    {
        if (!_onGetTxsSize)
        {
            return;
        }
        auto pendingTxsSize = m_txpoolStorage->size();
        _onGetTxsSize(nullptr, pendingTxsSize);
    }

protected:
    virtual bool checkExistsInGroup(bcos::protocol::TxSubmitCallback _txSubmitCallback);
    virtual void getTxsFromLocalLedger(bcos::crypto::HashListPtr _txsHash,
        bcos::crypto::HashListPtr _missedTxs,
        std::function<void(Error::Ptr, bcos::protocol::TransactionsPtr)> _onBlockFilled);

    virtual void fillBlock(bcos::crypto::HashListPtr _txsHash,
        std::function<void(Error::Ptr, bcos::protocol::TransactionsPtr)> _onBlockFilled,
        bool _fetchFromLedger = true);

    void initSendResponseHandler();

    template <typename T>
    void asyncSubmitTransaction(T _txData, bcos::protocol::TxSubmitCallback _txSubmitCallback)
    {
        // verify and try to submit the valid transaction
        auto self = std::weak_ptr<TxPool>(shared_from_this());
        m_worker->enqueue([self, _txData, _txSubmitCallback]() {
            try
            {
                auto txpool = self.lock();
                if (!txpool)
                {
                    return;
                }
                if (!txpool->checkExistsInGroup(_txSubmitCallback))
                {
                    return;
                }
                auto txpoolStorage = txpool->m_txpoolStorage;
                txpoolStorage->submitTransaction(_txData, _txSubmitCallback);
            }
            catch (std::exception const& e)
            {
                TXPOOL_LOG(WARNING) << LOG_DESC("asyncSubmit excepiton")
                                    << LOG_KV("errorInfo", boost::diagnostic_information(e));
            }
        });
    }

private:
    TxPoolConfig::Ptr m_config;
    TxPoolStorageInterface::Ptr m_txpoolStorage;
    bcos::sync::TransactionSyncInterface::Ptr m_transactionSync;

    std::function<void(std::string const& _id, int _moduleID, bcos::crypto::NodeIDPtr _dstNode,
        bytesConstRef _data)>
        m_sendResponseHandler;

    ThreadPool::Ptr m_worker;
    std::atomic_bool m_running = {false};
};
}  // namespace txpool
}  // namespace bcos