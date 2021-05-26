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
 * @brief fake ledger
 * @file FakeLedger.h
 * @author: yujiechen
 * @date 2021-05-25
 */
#pragma once
#include <bcos-framework/interfaces/ledger/LedgerConfig.h>
#include <bcos-framework/interfaces/ledger/LedgerInterface.h>
#include <bcos-framework/interfaces/protocol/Block.h>
#include <bcos-framework/testutils/protocol/FakeBlock.h>
#include <bcos-framework/testutils/protocol/FakeBlockHeader.h>

using namespace bcos;
using namespace bcos::ledger;
using namespace bcos::protocol;
using namespace bcos::crypto;
using namespace bcos::consensus;
namespace bcos
{
namespace test
{
class FakeLedger : public LedgerInterface
{
public:
    using Ptr = std::shared_ptr<FakeLedger>;
    FakeLedger(
        BlockFactory::Ptr _blockFactory, size_t _blockNumber, size_t _txsSize, size_t _receiptsSize)
      : m_blockFactory(_blockFactory), m_ledgerConfig(std::make_shared<LedgerConfig>())
    {
        auto genesisBlock = init(nullptr, true, 0, 0, 0);
        m_ledger.push_back(genesisBlock);
        m_hash2Block[genesisBlock->blockHeader()->hash()] = 0;

        auto parentHeader = genesisBlock->blockHeader();
        for (size_t i = 1; i < _blockNumber; i++)
        {
            auto block = init(parentHeader, true, i, _txsSize, _receiptsSize);
            parentHeader = block->blockHeader();
            m_ledger.push_back(block);
            m_hash2Block[block->blockHeader()->hash()] = i;
        }
        auto latestBlock = m_ledger[m_ledger.size() - 1];
        updateLedgerConfig(latestBlock->blockHeader());
    }

    Block::Ptr init(BlockHeader::Ptr _parentBlockHeader, bool _withHeader, BlockNumber _blockNumber,
        size_t _txsSize, size_t _receiptSize)
    {
        auto block = fakeAndCheckBlock(
            m_blockFactory->cryptoSuite(), m_blockFactory, false, _txsSize, _receiptSize, 0, 0);
        if (!_withHeader)
        {
            return block;
        }
        ParentInfoList parentInfo;
        if (_parentBlockHeader != nullptr)
        {
            parentInfo.push_back(
                ParentInfo{_parentBlockHeader->number(), _parentBlockHeader->hash()});
        }
        auto rootHash =
            m_blockFactory->cryptoSuite()->hashImpl()->hash(std::to_string(_blockNumber));
        u256 gasUsed = 1232342523;

        auto sigImpl = m_blockFactory->cryptoSuite()->signatureImpl();
        auto sealerList = fakeSealerList(m_keyPairVec, sigImpl, 4);
        SignatureList signatureList;
        // fake blockHeader
        auto blockHeader =
            fakeAndTestBlockHeader(m_blockFactory->cryptoSuite(), 0, parentInfo, rootHash, rootHash,
                rootHash, _blockNumber, gasUsed, utcTime(), 0, sealerList, bytes(), signatureList);
        signatureList = fakeSignatureList(sigImpl, m_keyPairVec, blockHeader->hash());
        blockHeader->setSignatureList(signatureList);
        block->setBlockHeader(blockHeader);
        return block;
    }

    Block::Ptr populateFromHeader(BlockHeader::Ptr _blockHeader)
    {
        auto block = init(nullptr, false, _blockHeader->number(), 5, 5);
        block->setBlockHeader(_blockHeader);
        return block;
    }

    void updateLedgerConfig(BlockHeader::Ptr _blockHeader)
    {
        m_ledgerConfig->setBlockNumber(_blockHeader->number());
        m_ledgerConfig->setHash(_blockHeader->hash());
    }

    // Consensus and block-sync module use this interface to commit block
    void asyncCommitBlock(BlockHeader::Ptr _blockHeader,
        std::function<void(Error::Ptr, LedgerConfig::Ptr)> _onCommitBlock) override
    {
        if (_blockHeader->number() != m_ledgerConfig->blockNumber() + 1)
        {
            _onCommitBlock(std::make_shared<Error>(-1, "invalid block"), nullptr);
            return;
        }
        auto block = populateFromHeader(_blockHeader);
        m_ledger.push_back(block);
        updateLedgerConfig(_blockHeader);
        _onCommitBlock(nullptr, m_ledgerConfig);
    }

    // the txpool module use this interface to store txs
    void asyncStoreTransactions(std::shared_ptr<std::vector<bytesPointer>> _txToStore,
        HashListPtr _txHashList, std::function<void(Error::Ptr)> _onTxStored) override
    {
        size_t i = 0;
        for (auto const& hash : *_txHashList)
        {
            auto txData = (*_txToStore)[i];
            m_txsHashToData[hash] = txData;
        }
        _onTxStored(nullptr);
    }

    // the dispatcher use this interface to store receipts
    void asyncStoreReceipts(storage::TableFactoryInterface::Ptr, Block::Ptr,
        std::function<void(Error::Ptr)> _onReceiptStored) override
    {
        _onReceiptStored(nullptr);
    }

    // maybe sync module or rpc module need this interface to return header/txs/receipts
    void asyncGetBlockDataByNumber(
        BlockNumber, int32_t, std::function<void(Error::Ptr, Block::Ptr)>) override
    {}

    // TODO: maybe only RPC module need this interface
    void asyncGetBlockNumberByHash(
        crypto::HashType const&, std::function<void(Error::Ptr, BlockNumber)>) override
    {}

    void asyncGetBlockHashByNumber(BlockNumber _blockNumber,
        std::function<void(Error::Ptr, crypto::HashType const&)> _onGetBlock) override
    {
        auto const& hash = m_ledger[_blockNumber]->blockHeader()->hash();
        _onGetBlock(nullptr, hash);
    }

    void asyncGetBlockNumber(std::function<void(Error::Ptr, BlockNumber)> _onGetBlock) override
    {
        _onGetBlock(nullptr, m_ledgerConfig->blockNumber());
    }

    void asyncGetBatchTxsByHashList(crypto::HashListPtr _txHashList, bool,
        std::function<void(
            Error::Ptr, TransactionsPtr, std::shared_ptr<std::map<std::string, MerkleProofPtr>>)>
            _onGetTx) override
    {
        auto txs = std::make_shared<Transactions>();
        for (auto const& hash : *_txHashList)
        {
            if (m_txsHashToData.count(hash))
            {
                auto tx = m_blockFactory->transactionFactory()->createTransaction(
                    ref(*(m_txsHashToData[hash])), false);
                txs->emplace_back(tx);
            }
        }
        _onGetTx(nullptr, txs, nullptr);
    }

    // TODO: maybe for RPC
    void asyncGetTransactionReceiptByHash(crypto::HashType const&, bool,
        std::function<void(Error::Ptr, TransactionReceipt::ConstPtr, MerkleProofPtr)> _onGetTx)
        override
    {
        _onGetTx(nullptr, nullptr, nullptr);
    }

    void asyncGetTotalTransactionCount(std::function<void(Error::Ptr, int64_t _totalTxCount,
            int64_t _failedTxCount, BlockNumber _latestBlockNumber)>
            _callback) override
    {
        _callback(nullptr, m_totalTxCount, 0, m_ledgerConfig->blockNumber());
    }

    void asyncGetSystemConfigByKey(std::string const& _key,
        std::function<void(Error::Ptr, std::string, BlockNumber)> _onGetConfig) override
    {
        std::string value = "";
        if (m_systemConfig.count(_key))
        {
            value = m_systemConfig[_key];
        }
        _onGetConfig(nullptr, value, m_ledgerConfig->blockNumber());
    }

    void asyncGetNodeListByType(std::string const& _type,
        std::function<void(Error::Ptr, ConsensusNodeListPtr)> _onGetNodeList) override
    {
        if (_type == CONSENSUS_SEALER)
        {
            auto consensusNodes = std::make_shared<ConsensusNodeList>();
            *consensusNodes = m_ledgerConfig->consensusNodeList();
            _onGetNodeList(nullptr, consensusNodes);
            return;
        }
        if (_type == CONSENSUS_OBSERVER)
        {
            auto observerNodes = std::make_shared<ConsensusNodeList>();
            *observerNodes = m_ledgerConfig->observerNodeList();
            _onGetNodeList(nullptr, observerNodes);
            return;
        }
        _onGetNodeList(std::make_shared<Error>(-1, "invalid Type"), nullptr);
    }

    void asyncGetNonceList(BlockNumber _startNumber, int64_t _offset,
        std::function<void(Error::Ptr, std::shared_ptr<std::map<BlockNumber, NonceListPtr>>)>
            _onGetList) override
    {
        if (_startNumber > m_ledgerConfig->blockNumber())
        {
            _onGetList(nullptr, nullptr);
        }
        auto endNumber = std::min(_startNumber + _offset - 1, m_ledgerConfig->blockNumber());
        auto nonceList = std::make_shared<std::map<BlockNumber, NonceListPtr>>();
        for (auto index = _startNumber; index <= endNumber; index++)
        {
            auto nonces = m_ledger[index]->nonces();
            nonceList->insert(std::make_pair(index, nonces));
        }
        _onGetList(nullptr, nonceList);
    }

    void setStatus(bool _normal) { m_statusNormal = _normal; }
    void setTotalTxCount(size_t _totalTxCount) { m_totalTxCount = _totalTxCount; }
    void setSystemConfig(std::string const& _key, std::string const& _value)
    {
        m_systemConfig[_key] = _value;
    }

    void setConsensusNodeList(ConsensusNodeListPtr _consensusNodes)
    {
        m_ledgerConfig->setConsensusNodeList(*_consensusNodes);
    }
    void setObserverNodeList(ConsensusNodeListPtr _observerNodes)
    {
        m_ledgerConfig->setObserverNodeList(*_observerNodes);
    }

    LedgerConfig::Ptr ledgerConfig() { return m_ledgerConfig; }
    BlockNumber blockNumber() { return m_ledgerConfig->blockNumber(); }
    std::vector<Block::Ptr> const& ledgerData() { return m_ledger; }

    size_t storedTxsSize() { return m_txsHashToData.size(); }
    std::map<HashType, bytesPointer> const& txsHashToData() { return m_txsHashToData; }

private:
    BlockFactory::Ptr m_blockFactory;
    std::vector<KeyPairInterface::Ptr> m_keyPairVec;
    LedgerConfig::Ptr m_ledgerConfig;

    size_t m_totalTxCount;
    bool m_statusNormal = true;

    std::vector<Block::Ptr> m_ledger;
    std::map<HashType, BlockNumber> m_hash2Block;

    std::map<HashType, bytesPointer> m_txsHashToData;
    std::map<std::string, std::string> m_systemConfig;
};
}  // namespace test
}  // namespace bcos