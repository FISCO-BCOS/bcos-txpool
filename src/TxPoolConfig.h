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
 * @brief Transaction pool configuration module,
 *        including transaction pool dependent modules and related configuration information
 * @file TxPoolConfig.h
 * @author: yujiechen
 * @date 2021-05-08
 */
#pragma once
#include "txpool/interfaces/NonceCheckerInterface.h"
#include "txpool/interfaces/TxPoolStorageInterface.h"
#include "txpool/interfaces/TxValidatorInterface.h"
#include <bcos-framework/interfaces/protocol/BlockFactory.h>
#include <bcos-framework/interfaces/protocol/TransactionFactory.h>
#include <bcos-framework/interfaces/protocol/TransactionSubmitResultFactory.h>
namespace bcos
{
namespace txpool
{
class TxPoolConfig
{
public:
    using Ptr = std::shared_ptr<TxPoolConfig>;
    TxPoolConfig(TxValidatorInterface::Ptr _txValidator,
        bcos::protocol::TransactionSubmitResultFactory::Ptr _txResultFactory,
        bcos::protocol::TransactionFactory::Ptr _txFactory,
        bcos::protocol::BlockFactory::Ptr _blockFactory)
      : m_txValidator(_txValidator),
        m_txResultFactory(_txResultFactory),
        m_txFactory(_txFactory),
        m_blockFactory(_blockFactory)
    {}

    virtual ~TxPoolConfig() {}

    virtual void setNotifierWorkerNum(size_t _notifierWorkerNum)
    {
        m_notifierWorkerNum = _notifierWorkerNum;
    }
    virtual size_t notifierWorkerNum() const { return m_notifierWorkerNum; }

    virtual void setVerifyWorkerNum(size_t _verifyWorkerNum)
    {
        m_verifyWorkerNum = _verifyWorkerNum;
    }
    virtual size_t verifyWorkerNum() const { return m_verifyWorkerNum; }

    virtual void setPoolLimit(size_t _poolLimit) { m_poolLimit = _poolLimit; }
    virtual size_t poolLimit() const { return m_poolLimit; }

    NonceCheckerInterface::Ptr txPoolNonceChecker() { return m_txPoolNonceChecker; }
    NonceCheckerInterface::Ptr ledgerNonceChecker() { return m_ledgerNonceChecker; }

    TxValidatorInterface::Ptr txValidator() { return m_txValidator; }
    bcos::protocol::TransactionSubmitResultFactory::Ptr txResultFactory()
    {
        return m_txResultFactory;
    }

    bcos::protocol::BlockFactory::Ptr blockFactory() { return m_blockFactory; }
    void setBlockFactory(bcos::protocol::BlockFactory::Ptr _blockFactory)
    {
        m_blockFactory = _blockFactory;
    }

    bcos::protocol::TransactionFactory::Ptr txFactory() { return m_txFactory; }
    void setTxFactory(bcos::protocol::TransactionFactory::Ptr _txFactory)
    {
        m_txFactory = _txFactory;
    }

private:
    TxValidatorInterface::Ptr m_txValidator;
    bcos::protocol::TransactionSubmitResultFactory::Ptr m_txResultFactory;
    bcos::protocol::TransactionFactory::Ptr m_txFactory;
    bcos::protocol::BlockFactory::Ptr m_blockFactory;
    // TODO: create the nonceChecker
    NonceCheckerInterface::Ptr m_txPoolNonceChecker;
    NonceCheckerInterface::Ptr m_ledgerNonceChecker;
    size_t m_poolLimit = 15000;
    size_t m_notifierWorkerNum = 1;
    size_t m_verifyWorkerNum = 1;
};
}  // namespace txpool
}  // namespace bcos