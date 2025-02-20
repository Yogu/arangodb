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

#pragma once

#include "Basics/Common.h"
#include "Cluster/ClusterTypes.h"
#include "Futures/Future.h"
#include "Transaction/MethodsApi.h"
#include "VocBase/voc-types.h"

#include <velocypack/Slice.h>
#include <set>

namespace arangodb {

struct OperationOptions;
class Result;
class TransactionState;

namespace transaction {
class Methods;
}
namespace ClusterTrxMethods {
using arangodb::futures::Future;

struct IsServerIdLessThan {
  bool operator()(ServerID const& lhs, ServerID const& rhs) const noexcept;
};

using SortedServersSet = std::set<ServerID, IsServerIdLessThan>;

/// @brief begin a transaction on all followers
Future<Result> beginTransactionOnLeaders(
    TransactionState&, ClusterTrxMethods::SortedServersSet const& leaders,
    transaction::MethodsApi api);

/// @brief commit a transaction on a subordinate
Future<arangodb::Result> commitTransaction(transaction::Methods& trx,
                                           transaction::MethodsApi api);

/// @brief commit a transaction on a subordinate
Future<arangodb::Result> abortTransaction(transaction::Methods& trx,
                                          transaction::MethodsApi api);

/// @brief add the transaction ID header for servers
template<typename MapT>
void addTransactionHeader(transaction::Methods const& trx,
                          ServerID const& server, MapT& headers);

/// @brief add transaction ID header for setting up AQL snippets
template<typename MapT>
void addAQLTransactionHeader(transaction::Methods const& trx,
                             ServerID const& server, MapT& headers);

/// @brief check whether this is a kind el cheapo transaction
bool isElCheapo(transaction::Methods const& trx);
bool isElCheapo(TransactionState const& state);
}  // namespace ClusterTrxMethods
}  // namespace arangodb
