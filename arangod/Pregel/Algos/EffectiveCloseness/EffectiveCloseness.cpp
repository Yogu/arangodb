////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "EffectiveCloseness.h"
#include <memory>
#include "Pregel/Aggregator.h"
#include "Pregel/Algorithm.h"
#include "Pregel/Algos/EffectiveCloseness/HLLCounterFormat.h"
#include "Pregel/IncomingCache.h"
#include "Pregel/MasterContext.h"
#include "Pregel/VertexComputation.h"

using namespace arangodb;
using namespace arangodb::pregel;
using namespace arangodb::pregel::algos;

MessageFormat<HLLCounter>* EffectiveCloseness::messageFormat() const {
  return new HLLCounterFormat();
}
[[nodiscard]] auto EffectiveCloseness::messageFormatUnique() const
    -> std::unique_ptr<message_format> {
  return std::make_unique<HLLCounterFormat>();
}

MessageCombiner<HLLCounter>* EffectiveCloseness::messageCombiner() const {
  return new HLLCounterCombiner();
}
[[nodiscard]] auto EffectiveCloseness::messageCombinerUnique() const
    -> std::unique_ptr<message_combiner> {
  return std::make_unique<HLLCounterCombiner>();
}

struct ECComputation : public VertexComputation<ECValue, int8_t, HLLCounter> {
  ECComputation() {}

  void compute(MessageIterator<HLLCounter> const& messages) override {
    ECValue* value = mutableVertexData();

    if (globalSuperstep() == 0) {
      value->counter.addNode(pregelId());
    }

    uint32_t seenCountBefore = value->counter.getCount();
    for (HLLCounter const* inCounter : messages) {
      value->counter.merge(*inCounter);
    }

    uint32_t seenCountAfter = value->counter.getCount();
    if ((seenCountBefore != seenCountAfter) || (globalSuperstep() == 0)) {
      sendMessageToAllNeighbours(value->counter);
    }

    // determine last iteration for which we set a value,
    // we need to copy this to all iterations up to this one
    // because the number of reachable vertices stays the same
    // when the compute method is not invoked
    if (value->shortestPaths.size() < globalSuperstep()) {
      size_t i = value->shortestPaths.size();
      uint32_t numReachable = value->shortestPaths.back();
      for (; i < globalSuperstep(); i++) {
        value->shortestPaths.push_back(numReachable);
      }
    }
    // subtract 1 because our own bit is counted as well
    if (value->shortestPaths.size() > globalSuperstep()) {
      value->shortestPaths[globalSuperstep()] = seenCountAfter - 1;
    } else {
      value->shortestPaths.push_back(seenCountAfter - 1);
    }
    voteHalt();
  }
};

VertexComputation<ECValue, int8_t, HLLCounter>*
EffectiveCloseness::createComputation(
    std::shared_ptr<WorkerConfig const>) const {
  return new ECComputation();
}

struct ECGraphFormat : public GraphFormat<ECValue, int8_t> {
  const std::string _resultField;

  explicit ECGraphFormat(std::string const& result)
      : GraphFormat<ECValue, int8_t>(), _resultField(result) {}

  size_t estimatedEdgeSize() const override { return 0; }

  void copyVertexData(arangodb::velocypack::Options const&,
                      std::string const& documentId,
                      arangodb::velocypack::Slice document, ECValue& targetPtr,
                      uint64_t vertexId) const override {}

  bool buildVertexDocument(arangodb::velocypack::Builder& b,
                           ECValue const* ptr) const override {
    size_t numVerticesReachable = 0;
    size_t sumLengths = 0;
    for (size_t i = 1; i < ptr->shortestPaths.size(); i++) {
      uint32_t newlyReachable =
          ptr->shortestPaths[i] - ptr->shortestPaths[i - 1];
      sumLengths += i * newlyReachable;
      if (ptr->shortestPaths[i] > numVerticesReachable) {
        numVerticesReachable = ptr->shortestPaths[i];
      }
    }
    double closeness = 0.0;
    if (numVerticesReachable > 0) {
      closeness = (double)sumLengths / (double)numVerticesReachable;
    }
    b.add(_resultField, VPackValue(closeness));
    return true;
  }
};

std::shared_ptr<GraphFormat<ECValue, int8_t> const>
EffectiveCloseness::inputFormat() const {
  return std::make_shared<ECGraphFormat>(_resultField);
}

struct EffectiveClosenessWorkerContext : public WorkerContext {
  EffectiveClosenessWorkerContext(
      std::unique_ptr<AggregatorHandler> readAggregators,
      std::unique_ptr<AggregatorHandler> writeAggregators)
      : WorkerContext(std::move(readAggregators),
                      std::move(writeAggregators)){};
};
[[nodiscard]] auto EffectiveCloseness::workerContext(
    std::unique_ptr<AggregatorHandler> readAggregators,
    std::unique_ptr<AggregatorHandler> writeAggregators,
    velocypack::Slice userParams) const -> WorkerContext* {
  return new EffectiveClosenessWorkerContext(std::move(readAggregators),
                                             std::move(writeAggregators));
}
[[nodiscard]] auto EffectiveCloseness::workerContextUnique(
    std::unique_ptr<AggregatorHandler> readAggregators,
    std::unique_ptr<AggregatorHandler> writeAggregators,
    velocypack::Slice userParams) const -> std::unique_ptr<WorkerContext> {
  return std::make_unique<EffectiveClosenessWorkerContext>(
      std::move(readAggregators), std::move(writeAggregators));
}

struct EffectiveClosenessMasterContext : public MasterContext {
  EffectiveClosenessMasterContext(
      uint64_t vertexCount, uint64_t edgeCount,
      std::unique_ptr<AggregatorHandler> aggregators)
      : MasterContext(vertexCount, edgeCount, std::move(aggregators)){};
};
[[nodiscard]] auto EffectiveCloseness::masterContext(
    std::unique_ptr<AggregatorHandler> aggregators,
    arangodb::velocypack::Slice userParams) const -> MasterContext* {
  return new EffectiveClosenessMasterContext(0, 0, std::move(aggregators));
}
[[nodiscard]] auto EffectiveCloseness::masterContextUnique(
    uint64_t vertexCount, uint64_t edgeCount,
    std::unique_ptr<AggregatorHandler> aggregators,
    arangodb::velocypack::Slice userParams) const
    -> std::unique_ptr<MasterContext> {
  return std::make_unique<EffectiveClosenessMasterContext>(
      vertexCount, edgeCount, std::move(aggregators));
}
