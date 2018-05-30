/**
 * Copyright Soramitsu Co., Ltd. 2017 All Rights Reserved.
 * http://soramitsu.co.jp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "synchronizer/impl/synchronizer_impl.hpp"

#include <utility>
#include "ametsuchi/mutable_storage.hpp"
#include "backend/protobuf/block.hpp"
#include "backend/protobuf/empty_block.hpp"

namespace iroha {
  namespace synchronizer {

    SynchronizerImpl::SynchronizerImpl(
        std::shared_ptr<network::ConsensusGate> consensus_gate,
        std::shared_ptr<validation::ChainValidator> validator,
        std::shared_ptr<ametsuchi::MutableFactory> mutableFactory,
        std::shared_ptr<network::BlockLoader> blockLoader)
        : validator_(std::move(validator)),
          mutableFactory_(std::move(mutableFactory)),
          blockLoader_(std::move(blockLoader)) {
      log_ = logger::log("synchronizer");
      consensus_gate->on_commit().subscribe(
          subscription_, [&](shared_model::interface::BlockVariantType block) {
            this->process_commit(block);
          });
    }

    SynchronizerImpl::~SynchronizerImpl() {
      subscription_.unsubscribe();
    }

    void SynchronizerImpl::process_commit(
        shared_model::interface::BlockVariantType &commit_message_variant) {
      log_->info("processing commit");
      auto storageResult = mutableFactory_->createMutableStorage();
      std::unique_ptr<ametsuchi::MutableStorage> storage;
      storageResult.match(
          [&](expected::Value<std::unique_ptr<ametsuchi::MutableStorage>>
                  &_storage) { storage = std::move(_storage.value); },
          [&](expected::Error<std::string> &error) {
            storage = nullptr;
            log_->error(error.error);
          });
      if (not storage) {
        return;
      }

      auto commit_message = iroha::visit_in_place(
          commit_message_variant,
          [](const std::shared_ptr<shared_model::interface::Block> block) {
            return block;
          },
          [](const std::shared_ptr<shared_model::interface::EmptyBlock>
                 empty_block)
              -> std::shared_ptr<shared_model::interface::Block> {
            auto proto_empty_block =
                std::static_pointer_cast<shared_model::proto::EmptyBlock>(
                    empty_block);
            return std::make_shared<shared_model::proto::Block>(
                proto_empty_block->getTransport());
          });

      if (validator_->validateBlock(*commit_message, *storage)) {
        // Block can be applied to current storage
        // Commit to main Ametsuchi
        mutableFactory_->commit(std::move(storage));

        auto single_commit = rxcpp::observable<>::just(commit_message);

        notifier_.get_subscriber().on_next(single_commit);
      } else {
        // Block can't be applied to current storage
        // Download all missing blocks
        for (const auto &signature : commit_message->signatures()) {
          auto storageResult = mutableFactory_->createMutableStorage();
          std::unique_ptr<ametsuchi::MutableStorage> storage;
          storageResult.match(
              [&](expected::Value<std::unique_ptr<ametsuchi::MutableStorage>>
                      &_storage) { storage = std::move(_storage.value); },
              [&](expected::Error<std::string> &error) {
                storage = nullptr;
                log_->error(error.error);
              });
          if (not storage) {
            return;
          }
          auto chain = blockLoader_->retrieveBlocks(
              shared_model::crypto::PublicKey(signature.publicKey()));
          // Check chain last commit
          auto is_chain_end_expected =
              chain.as_blocking().last()->hash() == commit_message->hash();

          if (validator_->validateChain(chain, *storage)
              and is_chain_end_expected) {
            // Peer send valid chain
            mutableFactory_->commit(std::move(storage));
            notifier_.get_subscriber().on_next(chain);
            // You are synchronized
            return;
          }
        }
      }
    }

    rxcpp::observable<Commit> SynchronizerImpl::on_commit_chain() {
      return notifier_.get_observable();
    }
  }  // namespace synchronizer
}  // namespace iroha
