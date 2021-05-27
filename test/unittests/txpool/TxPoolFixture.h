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
 * @brief fixture for the txpool
 * @file TxPoolFixture.h
 * @author: yujiechen
 * @date 2021-05-25
 */
#pragma once
#include "TxPool.h"
#include "TxPoolConfig.h"
#include "TxPoolFactory.h"
#include "libprotocol/TransactionSubmitResultFactoryImpl.h"
#include "libprotocol/protobuf/PBBlockFactory.h"
#include "libprotocol/protobuf/PBBlockHeaderFactory.h"
#include "libprotocol/protobuf/PBTransactionFactory.h"
#include "libprotocol/protobuf/PBTransactionReceiptFactory.h"
#include "sync/TransactionSync.h"
#include "test/unittests/common/FakeFrontService.h"
#include "test/unittests/common/FakeLedger.h"
#include "test/unittests/common/FakeSealer.h"
#include "txpool/validator/TxValidator.h"
#include <bcos-framework/interfaces/consensus/ConsensusNode.h>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <thread>

using namespace bcos;
using namespace bcos::protocol;
using namespace bcos::crypto;
using namespace bcos::ledger;
using namespace bcos::consensus;
using namespace bcos::front;
using namespace bcos::sync;

namespace bcos
{
namespace test
{
class TxPoolFixture
{
public:
    using Ptr = std::shared_ptr<TxPoolFixture>;
    TxPoolFixture(NodeIDPtr _nodeId, CryptoSuite::Ptr _cryptoSuite, std::string const& _groupId,
        std::string const& _chainId, int64_t _blockLimit, FakeFrontService::Ptr _frontService)
      : m_nodeId(_nodeId),
        m_cryptoSuite(_cryptoSuite),
        m_groupId(_groupId),
        m_chainId(_chainId),
        m_blockLimit(_blockLimit),
        m_frontService(_frontService)
    {
        auto blockHeaderFactory = std::make_shared<PBBlockHeaderFactory>(_cryptoSuite);
        auto txFactory = std::make_shared<PBTransactionFactory>(_cryptoSuite);
        auto receiptFactory = std::make_shared<PBTransactionReceiptFactory>(_cryptoSuite);
        m_blockFactory =
            std::make_shared<PBBlockFactory>(blockHeaderFactory, txFactory, receiptFactory);
        m_txResultFactory = std::make_shared<TransactionSubmitResultFactoryImpl>();
        m_ledger = std::make_shared<FakeLedger>(m_blockFactory, 20, 10, 10);

        m_txPoolFactory = std::make_shared<TxPoolFactory>(_nodeId, _cryptoSuite, m_txResultFactory,
            m_blockFactory, m_frontService, m_ledger, m_groupId, m_chainId, m_blockLimit);
        m_sealer = std::make_shared<FakeSealer>();
        m_txpool = std::dynamic_pointer_cast<TxPool>(m_txPoolFactory->txpool());
        m_sync = std::dynamic_pointer_cast<TransactionSync>(m_txpool->transactionSync());

        m_frontService->addTxPool(_nodeId, m_txpool);
    }
    virtual ~TxPoolFixture() {}

    TxPoolFactory::Ptr txPoolFactory() { return m_txPoolFactory; }
    TxPool::Ptr txpool() { return m_txpool; }
    FakeLedger::Ptr ledger() { return m_ledger; }
    NodeIDPtr nodeID() { return m_nodeId; }
    std::string const& chainId() { return m_chainId; }
    std::string const& groupId() { return m_groupId; }
    FakeFrontService::Ptr frontService() { return m_frontService; }
    TransactionSync::Ptr sync() { return m_sync; }
    void appendSealer(NodeIDPtr _nodeId)
    {
        auto consensusNode = std::make_shared<ConsensusNode>(_nodeId);
        m_ledger->ledgerConfig()->mutableConsensusNodeList().emplace_back(consensusNode);
        m_txpool->transactionSync()->config()->setConsensusNodeList(
            m_ledger->ledgerConfig()->consensusNodeList());
        updateConnectedNodeList();
    }
    void init()
    {
        // init the txpool
        m_txPoolFactory->init(m_sealer);
    }

private:
    void updateConnectedNodeList()
    {
        NodeIDSet nodeIdSet;
        for (auto node : m_ledger->ledgerConfig()->consensusNodeList())
        {
            nodeIdSet.insert(node->nodeID());
        }
        m_txpool->transactionSync()->config()->setConnectedNodeList(nodeIdSet);
    }

private:
    NodeIDPtr m_nodeId;
    CryptoSuite::Ptr m_cryptoSuite;
    BlockFactory::Ptr m_blockFactory;
    TransactionSubmitResultFactory::Ptr m_txResultFactory;
    std::string m_groupId;
    std::string m_chainId;
    int64_t m_blockLimit;

    FakeLedger::Ptr m_ledger;
    FakeFrontService::Ptr m_frontService;
    TxPoolFactory::Ptr m_txPoolFactory;
    FakeSealer::Ptr m_sealer;
    TxPool::Ptr m_txpool;
    TransactionSync::Ptr m_sync;
};

inline void checkTxSubmit(TxPoolInterface::Ptr _txpool, TxPoolStorageInterface::Ptr _storage,
    Transaction::Ptr _tx, HashType const& _expectedTxHash, uint32_t _expectedStatus,
    size_t expectedTxSize, bool _needWaitResult = true, bool _waitNothing = false)
{
    bool verifyFinish = false;
    auto encodedData = _tx->encode();
    auto txData = std::make_shared<bytes>(encodedData.begin(), encodedData.end());
    _txpool->asyncSubmit(
        txData,
        [&](Error::Ptr _error, TransactionSubmitResult::Ptr _result) {
            BOOST_CHECK(_error == nullptr);
            BOOST_CHECK(_result->txHash() == _expectedTxHash);
            BOOST_CHECK(_result->status() == _expectedStatus);
            verifyFinish = true;
        },
        [&](Error::Ptr _error) { BOOST_CHECK(_error == nullptr); });
    if (_waitNothing)
    {
        return;
    }
    while (!verifyFinish && _needWaitResult)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!_needWaitResult)
    {
        while (_storage->size() != expectedTxSize)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    BOOST_CHECK(_storage->size() == expectedTxSize);
}

}  // namespace test
}  // namespace bcos