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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Job.h"
#include "Supervision.h"

namespace arangodb {
namespace consensus {

struct CleanOutServer : public Job {
  CleanOutServer(Node const& snapshot, AgentInterface* agent,
                 std::string const& jobId,
                 std::string const& creator = std::string(),
                 std::string const& server = std::string());

  CleanOutServer(Node const& snapshot, AgentInterface* agent, JOB_STATUS status,
                 std::string const& jobId);

  virtual ~CleanOutServer();

  virtual JOB_STATUS status() override final;
  virtual bool create(
      std::shared_ptr<VPackBuilder> envelope = nullptr) override final;
  virtual void run(bool&) override final;
  virtual bool start(bool&) override final;
  virtual Result abort(std::string const& reason) override final;

  // Check if all shards' replication factors can be satisfied after clean out.
  bool checkFeasibility();
  bool scheduleMoveShards(std::shared_ptr<velocypack::Builder>& trx);
  void scheduleJobsR2(std::shared_ptr<velocypack::Builder>& trx,
                      DatabaseID const& database, size_t&);

  std::string _server;
};
}  // namespace consensus
}  // namespace arangodb
