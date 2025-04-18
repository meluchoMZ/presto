/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <folly/Synchronized.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <memory>
#include <unordered_map>

#include "presto_cpp/main/SessionProperties.h"
#include "presto_cpp/presto_protocol/core/presto_protocol_core.h"
#include "velox/core/QueryCtx.h"

namespace facebook::presto {
class QueryContextCache {
 public:
  using QueryCtxWeakPtr = std::weak_ptr<velox::core::QueryCtx>;
  using QueryIdList = std::list<protocol::QueryId>;
  using QueryCtxCacheValue = std::pair<QueryCtxWeakPtr, QueryIdList::iterator>;
  using QueryCtxMap = std::unordered_map<protocol::QueryId, QueryCtxCacheValue>;

  QueryContextCache(size_t initial_capacity = kInitialCapacity)
      : capacity_(initial_capacity) {}

  size_t capacity() const {
    return capacity_;
  }
  size_t size() const {
    return queryCtxs_.size();
  }

  std::shared_ptr<velox::core::QueryCtx> get(protocol::QueryId queryId) {
    auto iter = queryCtxs_.find(queryId);
    if (iter != queryCtxs_.end()) {
      queryIds_.erase(iter->second.second);

      if (auto queryCtx = iter->second.first.lock()) {
        // Move the queryId to front, if queryCtx is still alive.
        queryIds_.push_front(queryId);
        iter->second.second = queryIds_.begin();
        return queryCtx;
      } else {
        queryCtxs_.erase(iter);
      }
    }
    return nullptr;
  }

  std::shared_ptr<velox::core::QueryCtx> insert(
      protocol::QueryId queryId,
      std::shared_ptr<velox::core::QueryCtx> queryCtx) {
    if (queryCtxs_.size() >= capacity_) {
      evict();
    }
    queryIds_.push_front(queryId);
    queryCtxs_[queryId] =
        std::make_pair(folly::to_weak_ptr(queryCtx), queryIds_.begin());
    return queryCtx;
  }

  void evict() {
    // Evict least recently used queryCtx if it is not referenced elsewhere.
    for (auto victim = queryIds_.end(); victim != queryIds_.begin();) {
      --victim;
      if (!queryCtxs_[*victim].first.lock()) {
        queryCtxs_.erase(*victim);
        queryIds_.erase(victim);
        return;
      }
    }

    // All queries are still inflight. Increase capacity.
    capacity_ = std::max(kInitialCapacity, capacity_ * 2);
  }
  const QueryCtxMap& ctxs() const {
    return queryCtxs_;
  }

  void testingClear();

 private:
  size_t capacity_;

  QueryCtxMap queryCtxs_;
  QueryIdList queryIds_;

  static constexpr size_t kInitialCapacity = 256UL;
};

class QueryContextManager {
 public:
  QueryContextManager(
      folly::Executor* driverExecutor,
      folly::Executor* spillerExecutor);

  std::shared_ptr<velox::core::QueryCtx> findOrCreateQueryCtx(
      const protocol::TaskId& taskId,
      const protocol::TaskUpdateRequest& taskUpdateRequest);

  /// Calls the given functor for every present query context.
  void visitAllContexts(std::function<void(
                            const protocol::QueryId&,
                            const velox::core::QueryCtx*)> visitor) const;

  /// Test method to clear the query context cache.
  void testingClearCache();

  const SessionProperties& getSessionProperties() const {
    return sessionProperties_;
  }

 private:
  std::shared_ptr<velox::core::QueryCtx> findOrCreateQueryCtx(
      const protocol::TaskId& taskId,
      std::unordered_map<std::string, std::string>&& configStrings,
      std::unordered_map<
          std::string,
          std::unordered_map<std::string, std::string>>&&
          connectorConfigStrings);

  std::unordered_map<std::string, std::string> toVeloxConfigs(
      const protocol::SessionRepresentation& session);

  folly::Executor* const driverExecutor_{nullptr};
  folly::Executor* const spillerExecutor_{nullptr};

  folly::Synchronized<QueryContextCache> queryContextCache_;
  SessionProperties sessionProperties_;
};

} // namespace facebook::presto
