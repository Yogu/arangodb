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
////////////////////////////////////////////////////////////////////////////////
#pragma once
#include <vector>
#include <span>
#include <cstddef>
#include <iosfwd>

namespace arangodb {
namespace velocypack {
class Slice;
}
class LocalDocumentId;

struct IndexStreamOptions {
  std::vector<std::size_t> usedKeyFields;
  std::vector<std::size_t> projectedFields;
};

std::ostream& operator<<(std::ostream&, IndexStreamOptions const&);

template<typename SliceType, typename DocIdType>
struct IndexStreamIterator {
  virtual ~IndexStreamIterator() = default;
  // load the current position into the span.
  // returns false if the end of the index range was reached.
  virtual bool position(std::span<SliceType>) const = 0;
  // seek to the given position. The span is updated with the actual new
  // position. returns false if the end of the index range was reached.
  virtual bool seek(std::span<SliceType>) = 0;

  // loads the document id and fills the projections (if any)
  virtual DocIdType load(std::span<SliceType> projections) const = 0;

  // load the current position into the span.
  // returns false if either the index is exhausted or the next element has a
  // different key set. In that case `key` is updated with the found key.
  // returns true if an entry with key was found. LocalDocumentId and
  // projections are loaded.
  virtual bool next(std::span<SliceType> key, DocIdType&,
                    std::span<SliceType> projections) = 0;

  // cache the current key. The span has to stay valid until this function
  // is called again.
  virtual void cacheCurrentKey(std::span<SliceType>) = 0;

  // called to reset the iterator to the initial position and loads that
  // positions keys into span. returns false if the iterator is exhausted.
  virtual bool reset(std::span<SliceType> span) = 0;
};

struct AqlIndexStreamIterator
    : IndexStreamIterator<velocypack::Slice, LocalDocumentId> {};

}  // namespace arangodb
