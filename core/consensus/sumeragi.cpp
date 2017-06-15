/*
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *          http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <crypto/hash.hpp>
#include <crypto/signature.hpp>
#include <validation/stateful/validator.hpp>
#include <connection/api/command_service.hpp>
#include <connection/consensus/service.hpp>
#include <connection/consensus/client.hpp>
#include <logger/logger.hpp>
#include <common/timer.hpp>
#include <common/datetime.hpp>
#include <thread_pool.hpp>
#include <vector>
#include <set>

#include "sumeragi.hpp"

/**
 * |ーーー|　|ーーー|　|ーーー|　|ーーー|
 * |　ス　|ー|　メ　|ー|　ラ　|ー|　ギ　|
 * |ーーー|　|ーーー|　|ーーー|　|ーーー|
 *
 * A chain-based byzantine fault tolerant consensus algorithm, based in large
 * part on BChain:
 *
 * Duan, S., Meling, H., Peisert, S., & Zhang, H. (2014). Bchain: Byzantine
 * replication with high throughput and embedded reconfiguration. In
 * International Conference on Principles of Distributed Systems (pp. 91-106).
 * Springer.
 */

namespace consensus {
    namespace sumeragi {

        using iroha::protocol::Block;
        using iroha::protocol::Signature;

        connection::consensus::SumeragiClient sender;
        logger::Logger log("sumeragi");

        static ThreadPool pool(ThreadPoolOptions{
            .threads_count = 0,
            //config::IrohaConfigManager::getInstance().getConcurrency(0),
            .worker_queue_size = 1024
            //config::IrohaConfigManager::getInstance().getPoolWorkerQueueSize(1024),
        });

        void initialize() {

            connection::consensus::receive(
                [](const Block &block) {
                    // TODO: Judge committed
                    if ( /*check is_committed*/ false) {

                    } else {
                        // send processTransaction(event) as a task to processing pool
                        // this returns std::future<void> object
                        // (std::future).get() method locks processing until result of
                        // processTransaction will be available but processTransaction returns
                        // void, so we don't have to call it and wait
                        // std::function<void()>&& task =
                        //    std::bind(processTransaction, std::move(event));
                        // pool.process(std::move(task));

                        std::function<void()> &&task = std::bind(processBlock, block);
                        pool.process(std::move(task));
                    }
                });
        }

        // TODO: Append block to db and calc merkle root.
        std::string appendBlock(const Block &block) {
            return std::string();
        }

        Block createSignedBlock(const Block &block, const std::string &merkleRoot) {
            auto pk = "pk"; // TODO: peer service
            auto sk = "sk";

            auto sigblob = crypto::signature::sign(merkleRoot, pk, sk);
            std::string str_sigblob;
            for (auto e: sigblob) str_sigblob.push_back(e);

            Signature newSignature;
            *newSignature.mutable_pubkey() = pk;
            *newSignature.mutable_signature() = str_sigblob;

            Block ret;
            ret.CopyFrom(block);
            ret.mutable_header()->set_created_time(common::datetime::unixtime());
            *ret.mutable_header()->mutable_peer_signature()->Add() = newSignature;

            return ret;
        }

        bool isLeader(const Block &block) {
            //auto validLeader = true; // TODO: Use peer service
            auto validNumOfSignatures = block.header().peer_signature().size() == 1;
            if (validNumOfSignatures) return true;
            return false;
        }

        size_t getMaxFaulty() {
            return 3;/*peer::service::getActivePeerList().size() / 3; */ // TODO: Peer service
        }

        size_t getNumValidatingPeers() {
            return getMaxFaulty() * 2 + 1;
        }

        size_t getNumAllPeers() {
            return 4; // TODO: peer service
        }

        void setTimeOutCommit(const Block &block) {
            timer::setAwkTimerForCurrentThread(3000, [block] {
                panic(block);
            });
        }

        /**
         * returns expected tail to send committed block.
         * if returned value = -1, all peers has been used.
         */

        int getNextOrder() {
            static int currentProxyTail = static_cast<int>(getNumValidatingPeers()) - 1;
            if (currentProxyTail >= getNumAllPeers()) {
                return -1;
            }
            return currentProxyTail++;
        }

        size_t countValidSignatures(const Block &block) {
            size_t numValidSignatures = 0;
            std::set<std::string> usedPubkeys;

            auto peerSigs = block.header().peer_signature();
            for (auto const &sig: peerSigs) {
                // FIXME: bytes in proto -> std::string in C++ (null value problem)
                if (usedPubkeys.count(sig.pubkey())) continue;
                const auto bodyMessage = block.body().SerializeAsString();
                const auto hash = crypto::hash::sha3_256_hex(bodyMessage);
                if (crypto::signature::verify(sig.signature(), hash, sig.pubkey())) {
                    numValidSignatures++;
                    usedPubkeys.insert(sig.pubkey());
                }
            }

            return numValidSignatures;
        }

        void processBlock(const Block &block) {

            // Stateful Validation
            auto valid = validator::stateful::validate(block);
            if (!valid) {
                log.info("Stateful validation failed.");
                return;
            }

            // Add Signature
            auto merkleRoot = appendBlock(block);
            auto newBlock = createSignedBlock(block, merkleRoot);

            if (isLeader(newBlock)) {
                sender.broadCast(newBlock);
                setTimeOutCommit(newBlock);
                return;
            }

            auto numValidSignatures = countValidSignatures(newBlock);

            if (numValidSignatures < getNumValidatingPeers()) {
                auto next = getNextOrder();
                if (next < 0) {
                    log.error("getNextOrder() < 0 in processBlock");
                    return;
                }
                sender.unicast(newBlock, static_cast<size_t>(next));
                setTimeOutCommit(newBlock);
            } else {
                if (numValidSignatures == getNumValidatingPeers()) {
                    sender.commit(newBlock);
                    setTimeOutCommit(newBlock);
                }
            }
        }


/**
 *
 * For example, given:
 * if f := 1, then
 *  _________________    _________________
 * /        A        \  /        B        \
 * |---|  |---|  |---|  |---|  |---|  |---|
 * | 0 |--| 1 |--| 2 |--| 3 |--| 4 |--| 5 |
 * |---|  |---|  |---|  |---|  |---|  |---|,
 *
 * if 2f+1 signature are not received within the timer's limit, then
 * the set of considered validators, A, is expanded by 1.
 *  ________________________    __________
 * /           A            \  /    B     \
 * |---|  |---|  |---|  |---|  |---|  |---|
 * | 0 |--| 1 |--| 2 |--| 3 |--| 4 |--| 5 |
 * |---|  |---|  |---|  |---|  |---|  |---|.
 */

        void panic(const Block &block) {
            auto next = getNextOrder();
            if (next < 0) {
                log.info("否認");
                return;
            }
            sender.unicast(block, static_cast<size_t>(next));
            setTimeOutCommit(block);
        }

    }  // namespace sumeragi
}  // namespace consensus
