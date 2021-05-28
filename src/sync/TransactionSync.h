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
 * @brief implementation for transaction sync
 * @file TransactionSync.h
 * @author: yujiechen
 * @date 2021-05-10
 */
#pragma once

#include "TransactionSyncConfig.h"
#include "interfaces/TransactionSyncInterface.h"
#include <bcos-framework/interfaces/protocol/Protocol.h>
#include <bcos-framework/libutilities/ThreadPool.h>
#include <bcos-framework/libutilities/Worker.h>

namespace bcos
{
namespace sync
{
class TransactionSync : public TransactionSyncInterface,
                        public Worker,
                        public std::enable_shared_from_this<TransactionSync>
{
public:
    using Ptr = std::shared_ptr<TransactionSync>;
    explicit TransactionSync(TransactionSyncConfig::Ptr _config)
      : TransactionSyncInterface(_config),
        Worker("sync", 0),
        m_downloadTxsBuffer(std::make_shared<TxsSyncMsgList>()),
        m_worker(std::make_shared<ThreadPool>("sync", 1)),
        m_txsRequester(std::make_shared<ThreadPool>("txsRequester", 1))
    {
        m_config->txpoolStorage()->onReady([&]() { this->noteNewTransactions(); });
    }

    ~TransactionSync() {}

    void start() override;
    void stop() override;

    using SendResponseCallback = std::function<void(bytesConstRef _respData)>;
    void onRecvSyncMessage(bcos::Error::Ptr _error, bcos::crypto::NodeIDPtr _nodeID,
        bytesPointer _data, SendResponseCallback _sendResponse) override;

    using VerifyResponseCallback = std::function<void(Error::Ptr, bool)>;
    void requestMissedTxs(bcos::crypto::PublicPtr _generatedNodeID,
        bcos::crypto::HashListPtr _missedTxs, VerifyResponseCallback _onVerifyFinished) override;

    virtual void maintainTransactions();
    virtual void maintainDownloadingTransactions();

protected:
    void executeWorker() override;

    virtual void broadcastTxsFromRpc(bcos::protocol::ConstTransactionsPtr _txs);
    virtual void forwardTxsFromP2P(bcos::protocol::ConstTransactionsPtr _txs);
    virtual bcos::crypto::NodeIDListPtr selectPeers(bcos::protocol::Transaction::ConstPtr _tx,
        bcos::crypto::NodeIDSet const& _connectedPeers,
        bcos::consensus::ConsensusNodeList const& _consensusNodeList, size_t _expectedSize);
    virtual void onPeerTxsStatus(
        bcos::crypto::NodeIDPtr _fromNode, TxsSyncMsgInterface::Ptr _txsStatus);

    virtual void onReceiveTxsRequest(
        TxsSyncMsgInterface::Ptr _txsRequest, SendResponseCallback _sendResponse);

    // functions called by requestMissedTxs
    virtual void verifyFetchedTxs(Error::Ptr _error, bcos::crypto::NodeIDPtr _nodeID,
        bytesConstRef _data, bcos::crypto::HashListPtr _missedTxs,
        VerifyResponseCallback _onVerifyFinished);
    virtual void requestMissedTxsFromPeer(bcos::crypto::PublicPtr _generatedNodeID,
        bcos::crypto::HashListPtr _missedTxs, VerifyResponseCallback _onVerifyFinished);
    virtual size_t onGetMissedTxsFromLedger(std::set<bcos::crypto::HashType>& _missedTxs,
        Error::Ptr _error, bcos::protocol::TransactionsPtr _fetchedTxs,
        VerifyResponseCallback _onVerifyFinished);


    virtual bool downloadTxsBufferEmpty()
    {
        ReadGuard l(x_downloadTxsBuffer);
        return (m_downloadTxsBuffer->size() == 0);
    }

    virtual void appendDownloadTxsBuffer(TxsSyncMsgInterface::Ptr _txsBuffer)
    {
        WriteGuard l(x_downloadTxsBuffer);
        m_downloadTxsBuffer->emplace_back(_txsBuffer);
    }

    virtual TxsSyncMsgListPtr swapDownloadTxsBuffer()
    {
        UpgradableGuard l(x_downloadTxsBuffer);
        auto localBuffer = m_downloadTxsBuffer;
        UpgradeGuard ul(l);
        m_downloadTxsBuffer = std::make_shared<TxsSyncMsgList>();
        return localBuffer;
    }
    virtual bool importDownloadedTxs(
        bcos::crypto::NodeIDPtr _fromNode, bcos::protocol::Block::Ptr _txsBuffer);

    virtual bool importDownloadedTxs(
        bcos::crypto::NodeIDPtr _fromNode, bcos::protocol::TransactionsPtr _txs);

    void noteNewTransactions()
    {
        m_newTransactions = true;
        m_signalled.notify_all();
    }

private:
    TxsSyncMsgListPtr m_downloadTxsBuffer;
    SharedMutex x_downloadTxsBuffer;
    ThreadPool::Ptr m_worker;
    ThreadPool::Ptr m_txsRequester;

    std::atomic_bool m_running = {false};

    std::atomic_bool m_newTransactions = {false};

    // signal to notify all thread to work
    boost::condition_variable m_signalled;
    // mutex to access m_signalled
    boost::mutex x_signalled;
};
}  // namespace sync
}  // namespace bcos