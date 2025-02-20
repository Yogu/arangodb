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
/// @author Daniel Larkin
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "RocksDBEngine/RocksDBCuckooIndexEstimator.h"

#include "Basics/Exceptions.h"
#include "Basics/ReadLocker.h"
#include "Basics/ScopeGuard.h"
#include "Basics/WriteLocker.h"
#include "Metrics/Gauge.h"
#include "RocksDBEngine/RocksDBFormat.h"

#include <snappy.h>

#undef ARANGODB_DEBUG_MEMORY_USAGE

namespace arangodb {

template<class Key>
RocksDBCuckooIndexEstimator<Key>::RocksDBCuckooIndexEstimator(
    metrics::Gauge<uint64_t>* memoryUsageMetric)
    : _memoryUsageMetric(memoryUsageMetric),
      _randState(0x2636283625154737ULL),
      _logSize(0),
      _size(0),
      _niceSize(0),
      _sizeMask(0),
      _sizeShift(0),
      _allocSize(0),
      _base(nullptr),
      _counters(nullptr),
      _nrUsed(0),
      _nrCuckood(0),
      _nrTotal(0),
      _appliedSeq(0),
      _needToPersist(false),
      _memoryUsage(0) {}

template<class Key>
RocksDBCuckooIndexEstimator<Key>::RocksDBCuckooIndexEstimator(
    metrics::Gauge<uint64_t>* memoryUsageMetric, uint64_t size)
    : RocksDBCuckooIndexEstimator<Key>(memoryUsageMetric) {
  // Inflate size so that we have some padding to avoid failure
  size *= 2;
  size = (size >= 1024) ? size : 1024;  // want 256 buckets minimum

  // First find the smallest power of two that is not smaller than size:
  size /= kSlotsPerBucket;
  _size = size;
  initializeDefault();
}

template<class Key>
RocksDBCuckooIndexEstimator<Key>::RocksDBCuckooIndexEstimator(
    metrics::Gauge<uint64_t>* memoryUsageMetric, std::string_view serialized)
    : RocksDBCuckooIndexEstimator<Key>(memoryUsageMetric) {
  // note: this may throw!
  deserialize(serialized);
}

template<class Key>
RocksDBCuckooIndexEstimator<Key>::~RocksDBCuckooIndexEstimator() {
  freeMemory();
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::drain() {
  WRITE_LOCKER(locker, _lock);
  drainNoLock();
}

template<class Key>
/*static*/ bool RocksDBCuckooIndexEstimator<Key>::isFormatSupported(
    std::string_view serialized) {
  TRI_ASSERT(serialized.size() > sizeof(_appliedSeq) + sizeof(char));
  switch (serialized[sizeof(_appliedSeq)]) {
    case SerializeFormat::UNCOMPRESSED:
    case SerializeFormat::COMPRESSED:
      return true;
  }
  return false;
}

/**
 * @brief Serialize estimator for persistence, applying any buffered updates
 *
 * Format is hard-coded, and must support older formats for backwards
 * compatibility. The first 8 bytes consist of a sequence number S. All
 * updates prior to and including S are reflected in the serialization. Any
 * updates after S must be kept in the WALs and "replayed" during recovery.
 *
 * Applies any buffered updates and updates the "committed" seq/tick state.
 *
 * @param  serialized String for output
 * @param  commitSeq  Above that are still uncommited operations
 */
template<class Key>
void RocksDBCuckooIndexEstimator<Key>::serialize(
    std::string& serialized, rocksdb::SequenceNumber maxCommitSeq,
    SerializeFormat format) {
  // We always have to start with the commit seq, type and then the length

  // commit seq, above that is an uncommited operations
  //    rocksdb::SequenceNumber commitSeq = committableSeq();
  // must apply updates first to be valid, WAL needs to preserve
  rocksdb::SequenceNumber appliedSeq = applyUpdates(maxCommitSeq);
  TRI_ASSERT(appliedSeq <= maxCommitSeq);

  {
    // Sorry we need a consistent state, so we have to read-lock
    READ_LOCKER(locker, _lock);

    appliedSeq =
        std::max(appliedSeq, _appliedSeq.load(std::memory_order_acquire));
    TRI_ASSERT(appliedSeq !=
               std::numeric_limits<rocksdb::SequenceNumber>::max());
    rocksutils::uint64ToPersistent(serialized, appliedSeq);

    // type byte
    serialized += format;

    // note where we left off. we need this for the compressed format later
    size_t leftOff = serialized.size();

    // length
    uint64_t serialLength =
        (sizeof(SerializeFormat) + sizeof(uint64_t) + sizeof(_size) +
         sizeof(_nrUsed) + sizeof(_nrCuckood) + sizeof(_nrTotal) +
         sizeof(_niceSize) + sizeof(_logSize) +
         (_size * kSlotSize * kSlotsPerBucket)) +
        (_size * kCounterSize * kSlotsPerBucket);

    serialized.reserve(sizeof(uint64_t) + serialLength);
    // We always prepend the length, so parsing is easier
    rocksutils::uint64ToPersistent(serialized, serialLength);

    // Add all member variables
    appendHeader(serialized);
    // Add the data blob
    appendDataBlob(serialized);

    // compression is always on top of the UNCOMPRESSED format, so we run the
    // compression only after we have written out the full UNCOMPRESSED data
    if (format == SerializeFormat::COMPRESSED) {
      // compression starts at the point where we left off, i.e. at the byte
      // following the format byte.
      // we compress data in a scratch buffer, because compression input and
      // output must not overlap.
      std::string scratch;
      snappy::Compress(serialized.data() + leftOff, serialized.size() - leftOff,
                       &scratch);

      // scratch now contains the compressed value of UNCOMPRESSED

      TRI_ASSERT(serialized.size() > leftOff);
      // serialized still contains the UNCOMPRESSED data. rewind it to the
      // byte following the format byte, so we can now append the compressed
      // data instead.
      serialized.resize(leftOff);

      // append compressed size
      rocksutils::uint64ToPersistent(serialized, scratch.size());
      // append compressed blob
      serialized.append(scratch);
    }

    bool havePendingUpdates = !_insertBuffers.empty() ||
                              !_removalBuffers.empty() ||
                              !_truncateBuffer.empty();
    _needToPersist.store(havePendingUpdates, std::memory_order_release);
  }

  _appliedSeq.store(appliedSeq, std::memory_order_release);
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::appendHeader(std::string& result) const {
  rocksutils::uint64ToPersistent(result, _size);
  rocksutils::uint64ToPersistent(result, _nrUsed);
  rocksutils::uint64ToPersistent(result, _nrCuckood);
  rocksutils::uint64ToPersistent(result, _nrTotal);
  rocksutils::uint64ToPersistent(result, _niceSize);
  rocksutils::uint64ToPersistent(result, _logSize);
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::appendDataBlob(
    std::string& result) const {
  // Size is as follows: nrOfBuckets * kSlotsPerBucket * SlotSize
  TRI_ASSERT((_size * kSlotSize * kSlotsPerBucket) <=
             _size * kSlotSize * kSlotsPerBucket);
  for (uint64_t i = 0; i < (_size * kSlotSize * kSlotsPerBucket);
       i += kSlotSize) {
    rocksutils::uint16ToPersistent(result,
                                   *(reinterpret_cast<uint16_t*>(_base + i)));
  }

  TRI_ASSERT((_size * kCounterSize * kSlotsPerBucket) <=
             _size * kCounterSize * kSlotsPerBucket);
  for (uint64_t i = 0; i < (_size * kCounterSize * kSlotsPerBucket);
       i += kCounterSize) {
    rocksutils::uint32ToPersistent(
        result, *(reinterpret_cast<uint32_t*>(_counters + i)));
  }
}

/// @brief only call directly during startup/recovery; otherwise buffer
template<class Key>
void RocksDBCuckooIndexEstimator<Key>::clear() {
  WRITE_LOCKER(locker, _lock);
  // Reset Stats
  _nrTotal = 0;
  _nrCuckood = 0;
  _nrUsed = 0;

  // Reset filter content
  // Now initialize all slots in all buckets with zero data:
  for (uint32_t b = 0; b < _size; ++b) {
    for (size_t i = 0; i < kSlotsPerBucket; ++i) {
      Slot f = findSlot(b, i);
      f.injectCounter(findCounter(b, i));
      f.reset();
    }
  }

  _needToPersist.store(true, std::memory_order_release);
}

template<class Key>
Result RocksDBCuckooIndexEstimator<Key>::bufferTruncate(
    rocksdb::SequenceNumber seq) {
  Result res = basics::catchVoidToResult([&]() -> void {
    WRITE_LOCKER(locker, _lock);
    _truncateBuffer.emplace(seq);
    _needToPersist.store(true, std::memory_order_release);
    increaseMemoryUsage(bufferedEntrySize());
    checkInvariants();
  });
  return res;
}

template<class Key>
double RocksDBCuckooIndexEstimator<Key>::computeEstimate() {
  READ_LOCKER(locker, _lock);
  if (0 == _nrTotal) {
    TRI_ASSERT(0 == _nrUsed);
    // If we do not have any documents, we have a rather constant estimate.
    return 1.0;
  }
  TRI_ASSERT(_nrUsed <= _nrTotal);
  if (_nrUsed > _nrTotal) {
    _nrTotal = _nrUsed;  // should never happen, but will keep estimates valid
                         // for production where the above assert is disabled
  }

  return (static_cast<double>(_nrUsed) / static_cast<double>(_nrTotal));
}

template<class Key>
bool RocksDBCuckooIndexEstimator<Key>::lookup(Key const& k) const {
  // look up a key, return either false if no pair with key k is
  // found or true.
  uint64_t hash1 = _hasherKey(k);
  uint64_t pos1 = hashToPos(hash1);
  uint16_t fingerprint = keyToFingerprint(k);
  // We compute the second hash already here to allow the result to
  // survive a mispredicted branch in the first loop. Is this sensible?
  uint64_t hash2 = _hasherPosFingerprint(pos1, fingerprint);
  uint64_t pos2 = hashToPos(hash2);
  bool found = false;
  {
    READ_LOCKER(guard, _lock);
    findSlotNoCuckoo(pos1, pos2, fingerprint, found);
  }
  return found;
}

/// @brief only call directly during startup/recovery; otherwise buffer
template<class Key>
void RocksDBCuckooIndexEstimator<Key>::insert(Key const& k) {
  // insert the key k
  //
  // The inserted key will have its fingerprint input entered in the table. If
  // there is a collision and a fingerprint needs to be cuckooed, a certain
  // number of attempts will be made. After that, a given fingerprint may
  // simply be expunged. If something is expunged, the function will return
  // false, otherwise true.

  uint64_t hash1 = _hasherKey(k);
  uint64_t pos1 = hashToPos(hash1);
  uint16_t fingerprint = keyToFingerprint(k);
  // We compute the second hash already here to let it survive a
  // mispredicted
  // branch in the first loop:
  uint64_t hash2 = _hasherPosFingerprint(pos1, fingerprint);
  uint64_t pos2 = hashToPos(hash2);

  {
    WRITE_LOCKER(guard, _lock);
    Slot slot = findSlotCuckoo(pos1, pos2, fingerprint);
    if (slot.isEmpty()) {
      // Free slot. insert ourself.
      slot.init(fingerprint);
      ++_nrUsed;
      TRI_ASSERT(_nrUsed > 0);
    } else {
      TRI_ASSERT(slot.isEqual(fingerprint));
      slot.increase();
    }
    ++_nrTotal;
    _needToPersist.store(true, std::memory_order_release);
  }
}

/// @brief vectorized version of insert, for multiple keys at once
template<class Key>
void RocksDBCuckooIndexEstimator<Key>::insert(std::vector<Key> const& keys) {
  if (!keys.empty()) {
    WRITE_LOCKER(guard, _lock);

    for (auto const& k : keys) {
      uint64_t hash1 = _hasherKey(k);
      uint64_t pos1 = hashToPos(hash1);
      uint16_t fingerprint = keyToFingerprint(k);
      // We compute the second hash already here to let it survive a
      // mispredicted
      // branch in the first loop:
      uint64_t hash2 = _hasherPosFingerprint(pos1, fingerprint);
      uint64_t pos2 = hashToPos(hash2);

      Slot slot = findSlotCuckoo(pos1, pos2, fingerprint);
      if (slot.isEmpty()) {
        // Free slot. insert ourself.
        slot.init(fingerprint);
        ++_nrUsed;
        TRI_ASSERT(_nrUsed > 0);
      } else {
        TRI_ASSERT(slot.isEqual(fingerprint));
        slot.increase();
      }
      ++_nrTotal;
    }

    _needToPersist.store(true, std::memory_order_release);
  }
}

/// @brief only call directly during startup/recovery; otherwise buffer
template<class Key>
bool RocksDBCuckooIndexEstimator<Key>::remove(Key const& k) {
  // remove one element with key k, if one is in the table. Return true if
  // a key was removed and false otherwise.
  // look up a key, return either false if no pair with key k is
  // found or true.
  uint64_t hash1 = _hasherKey(k);
  uint64_t pos1 = hashToPos(hash1);
  uint16_t fingerprint = keyToFingerprint(k);
  // We compute the second hash already here to allow the result to
  // survive a mispredicted branch in the first loop. Is this sensible?
  uint64_t hash2 = _hasherPosFingerprint(pos1, fingerprint);
  uint64_t pos2 = hashToPos(hash2);

  bool found = false;
  {
    WRITE_LOCKER(guard, _lock);

    Slot slot = findSlotNoCuckoo(pos1, pos2, fingerprint, found);
    if (found) {
      // only decrease the total if we actually found it
      --_nrTotal;
      if (!slot.decrease()) {
        // Removed last element. Have to remove
        slot.reset();
        --_nrUsed;
      }
    } else if (_nrCuckood > 0) {
      // If we get here we assume that the element was once inserted, but
      // removed by cuckoo
      // Reduce nrCuckood;
      // not included in _nrTotal, just decrease here
      --_nrCuckood;
    }
    _needToPersist.store(true, std::memory_order_release);
  }

  return found;
}

/// @brief only call directly during startup/recovery; otherwise buffer
template<class Key>
void RocksDBCuckooIndexEstimator<Key>::remove(std::vector<Key> const& keys) {
  if (!keys.empty()) {
    WRITE_LOCKER(guard, _lock);

    for (auto const& k : keys) {
      // remove one element with key k, if one is in the table. Return true if
      // a key was removed and false otherwise.
      // look up a key, return either false if no pair with key k is
      // found or true.
      uint64_t hash1 = _hasherKey(k);
      uint64_t pos1 = hashToPos(hash1);
      uint16_t fingerprint = keyToFingerprint(k);
      // We compute the second hash already here to allow the result to
      // survive a mispredicted branch in the first loop. Is this sensible?
      uint64_t hash2 = _hasherPosFingerprint(pos1, fingerprint);
      uint64_t pos2 = hashToPos(hash2);

      bool found = false;
      Slot slot = findSlotNoCuckoo(pos1, pos2, fingerprint, found);
      if (found) {
        // only decrease the total if we actually found it
        --_nrTotal;
        if (!slot.decrease()) {
          // Removed last element. Have to remove
          slot.reset();
          --_nrUsed;
        }
      } else if (_nrCuckood > 0) {
        // If we get here we assume that the element was once inserted, but
        // removed by cuckoo
        // Reduce nrCuckood;
        // not included in _nrTotal, just decrease here
        --_nrCuckood;
      }
    }
    _needToPersist.store(true, std::memory_order_release);
  }
}

/**
 * @brief Buffer updates to this estimator to be applied when appropriate
 *
 * Buffers updates associated with a given commit seq/tick. Will hold updates
 * until all previous blockers have been removed to ensure a consistent state
 * for sync/recovery and avoid any missed updates.
 *
 * @param  seq      The seq/tick post-commit, prior to call
 * @param  inserts  Vector of hashes to insert
 * @param  removals Vector of hashes to remove
 * @return          May return error if any functions throw (e.g. alloc)
 */
template<class Key>
Result RocksDBCuckooIndexEstimator<Key>::bufferUpdates(
    rocksdb::SequenceNumber seq, std::vector<Key>&& inserts,
    std::vector<Key>&& removals) {
  TRI_ASSERT(!inserts.empty() || !removals.empty());
  Result res = basics::catchVoidToResult([&]() -> void {
    WRITE_LOCKER(locker, _lock);

    if (!inserts.empty()) {
      uint64_t memoryUsage =
          bufferedEntrySize() + bufferedEntryItemSize() * inserts.size();
      _insertBuffers.emplace(seq, std::move(inserts));
      increaseMemoryUsage(memoryUsage);
    }
    if (!removals.empty()) {
      uint64_t memoryUsage =
          bufferedEntrySize() + bufferedEntryItemSize() * removals.size();
      _removalBuffers.emplace(seq, std::move(removals));
      increaseMemoryUsage(memoryUsage);
    }

    _needToPersist.store(true, std::memory_order_release);
    checkInvariants();
  });
  return res;
}

/// @brief call with output from committableSeq(current), and before serialize
template<class Key>
rocksdb::SequenceNumber RocksDBCuckooIndexEstimator<Key>::applyUpdates(
    rocksdb::SequenceNumber commitSeq) {
  rocksdb::SequenceNumber appliedSeq = 0;
  Result res = basics::catchVoidToResult([&]() -> void {
    std::vector<Key> inserts;
    std::vector<Key> removals;

    // truncate will increase this sequence
    rocksdb::SequenceNumber ignoreSeq = 0;
    while (true) {
      bool foundTruncate = false;
      // find out if we have buffers to apply
      {
        WRITE_LOCKER(locker, _lock);

        uint64_t memoryUsage = 0;
        {
          // check for a truncate marker
          auto it = _truncateBuffer.begin();  // sorted ASC
          while (it != _truncateBuffer.end() && *it <= commitSeq) {
            ignoreSeq = *it;
            TRI_ASSERT(ignoreSeq != 0);
            foundTruncate = true;
            appliedSeq = std::max(appliedSeq, ignoreSeq);
            memoryUsage += bufferedEntrySize();
            it = _truncateBuffer.erase(it);
          }
        }
        TRI_ASSERT(ignoreSeq <= commitSeq);

        // check for inserts
        auto it = _insertBuffers.begin();  // sorted ASC
        while (it != _insertBuffers.end() && it->first <= commitSeq) {
          if (it->first <= ignoreSeq) {
            TRI_ASSERT(it->first <= appliedSeq);
            memoryUsage += bufferedEntrySize() +
                           bufferedEntryItemSize() * it->second.size();
            it = _insertBuffers.erase(it);
            continue;
          }
          inserts = std::move(it->second);
          TRI_ASSERT(!inserts.empty());
          appliedSeq = std::max(appliedSeq, it->first);
          memoryUsage +=
              bufferedEntrySize() + bufferedEntryItemSize() * inserts.size();
          _insertBuffers.erase(it);
          break;
        }

        // check for removals
        it = _removalBuffers.begin();  // sorted ASC
        while (it != _removalBuffers.end() && it->first <= commitSeq) {
          if (it->first <= ignoreSeq) {
            TRI_ASSERT(it->first <= appliedSeq);
            memoryUsage += bufferedEntrySize() +
                           bufferedEntryItemSize() * it->second.size();
            it = _removalBuffers.erase(it);
            continue;
          }
          removals = std::move(it->second);
          TRI_ASSERT(!removals.empty());
          appliedSeq = std::max(appliedSeq, it->first);
          memoryUsage +=
              bufferedEntrySize() + bufferedEntryItemSize() * removals.size();
          _removalBuffers.erase(it);
          break;
        }

        decreaseMemoryUsage(memoryUsage);
        checkInvariants();
      }

      if (foundTruncate) {
        clear();  // clear estimates
      }

      // no inserts or removals left to apply, drop out of loop
      if (inserts.empty() && removals.empty()) {
        break;
      }

      // apply inserts
      insert(inserts);
      inserts.clear();

      // apply removals
      remove(removals);
      removals.clear();
    }  // </while(true)>
  });
  return appliedSeq;
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::deserialize(
    std::string_view serialized) {
  // minimum size
  TRI_ASSERT(serialized.size() > sizeof(_appliedSeq) + 1);

  char const* current = serialized.data();

  _appliedSeq = rocksutils::uint64FromPersistent(current);
  current += sizeof(_appliedSeq);

  SerializeFormat format = static_cast<SerializeFormat>(*current);
  // Skip format char
  ++current;

  if (format == SerializeFormat::UNCOMPRESSED) {
    // UNCOMPRESSED format.
    // we have a subroutine which we can invoke on it.
    return deserializeUncompressedBody(
        serialized.substr(current - serialized.data()));
  }

  if (format == SerializeFormat::COMPRESSED) {
    // COMPRESSED format.
    // in order to handle this, we first need to uncompress the data.
    // the uncompressed data then is the data in UNCOMPRESSED format.

    // read compressed length
    uint64_t compressedLength = rocksutils::uint64FromPersistent(current);
    TRI_ASSERT(compressedLength == serialized.size() - sizeof(uint64_t) -
                                       sizeof(SerializeFormat) -
                                       sizeof(uint64_t));

    current += sizeof(uint64_t);

    // uncompress data in scratch buffer
    std::string scratch;
    if (!snappy::Uncompress(current, compressedLength, &scratch)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "unable to uncompress data in compressed "
                                     "index selectivity estimates");
    }
    // now scratch contains the UNCOMPRESSED data

    // from now on, we have an UNCOMPRESSED value, and can pretend
    // it was always like this
    return deserializeUncompressedBody(std::string_view(scratch));
  }

  THROW_ARANGO_EXCEPTION_MESSAGE(
      TRI_ERROR_INTERNAL,
      "unable to restore index estimates: invalid format found");
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::deserializeUncompressedBody(
    std::string_view serialized) {
  // Assert that we have at least the member variables
  constexpr size_t minRequiredSize =
      sizeof(uint64_t) + sizeof(_size) + sizeof(_nrUsed) + sizeof(_nrCuckood) +
      sizeof(_nrTotal) + sizeof(_niceSize) + sizeof(_logSize);
  TRI_ASSERT(serialized.size() > minRequiredSize);
  if (serialized.size() <= minRequiredSize) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "unable to restore index estimates: invalid format found");
  }

  char const* current = serialized.data();
  uint64_t length = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);
  // Validate that the serialized format is exactly as long as
  // we expect it to be
  TRI_ASSERT(serialized.size() + sizeof(SerializeFormat) == length);

  _size = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);

  if (_size <= 256) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "unable to unserialize index estimates");
  }

  _nrUsed = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);

  _nrCuckood = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);

  _nrTotal = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);

  _niceSize = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);

  _logSize = rocksutils::uint64FromPersistent(current);
  current += sizeof(uint64_t);

  deriveSizesAndAlloc();

  // Validate that we have enough data in the serialized format.
  TRI_ASSERT(serialized.size() ==
             sizeof(uint64_t) + sizeof(_size) + sizeof(_nrUsed) +
                 sizeof(_nrCuckood) + sizeof(_nrTotal) + sizeof(_niceSize) +
                 sizeof(_logSize) + (_size * kSlotSize * kSlotsPerBucket) +
                 (_size * kCounterSize * kSlotsPerBucket));

  // Insert the raw data
  // Size is as follows: nrOfBuckets * kSlotsPerBucket * SlotSize
  TRI_ASSERT((_size * kSlotSize * kSlotsPerBucket) <=
             _size * kSlotSize * kSlotsPerBucket);

  for (uint64_t i = 0; i < (_size * kSlotSize * kSlotsPerBucket);
       i += kSlotSize) {
    *(reinterpret_cast<uint16_t*>(_base + i)) =
        rocksutils::uint16FromPersistent(current);
    current += kSlotSize;
  }

  TRI_ASSERT((_size * kCounterSize * kSlotsPerBucket) <=
             _size * kCounterSize * kSlotsPerBucket);

  for (uint64_t i = 0; i < (_size * kCounterSize * kSlotsPerBucket);
       i += kCounterSize) {
    *(reinterpret_cast<uint32_t*>(_counters + i)) =
        rocksutils::uint32FromPersistent(current);
    current += kCounterSize;
  }
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::initializeDefault() {
  _niceSize = 256;
  _logSize = 8;
  while (_niceSize < _size) {
    _niceSize <<= 1;
    _logSize += 1;
  }

  deriveSizesAndAlloc();

  // Now initialize all slots in all buckets with zero data:
  for (uint32_t b = 0; b < _size; ++b) {
    for (size_t i = 0; i < kSlotsPerBucket; ++i) {
      Slot f = findSlot(b, i);
      f.injectCounter(findCounter(b, i));
      f.reset();
    }
  }
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::increaseMemoryUsage(
    uint64_t value) noexcept {
  _memoryUsage += value;
  if (ADB_LIKELY(_memoryUsageMetric != nullptr)) {
    _memoryUsageMetric->fetch_add(value);
  }
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::decreaseMemoryUsage(
    uint64_t value) noexcept {
  TRI_ASSERT(_memoryUsage >= value);
  _memoryUsage -= value;
  if (ADB_LIKELY(_memoryUsageMetric != nullptr)) {
    _memoryUsageMetric->fetch_sub(value);
  }
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::deriveSizesAndAlloc() {
  _sizeMask = _niceSize - 1;
  _sizeShift = static_cast<uint32_t>((64 - _logSize) / 2);

  // bytes for slots
  std::size_t bytesForSlots = _size * kSlotSize * kSlotsPerBucket;
  // we assume it to be a multiple of 64, cache-line aligned
  TRI_ASSERT(bytesForSlots % 64 == 0);

  std::size_t bytesForCounters = _size * kCounterSize * kSlotsPerBucket;

  // give 64 bytes padding to enable 64-byte alignment
  _allocSize = bytesForSlots + bytesForCounters + 64;

  _allocBase = std::make_unique<char[]>(_allocSize);

  _base = reinterpret_cast<char*>(
      (reinterpret_cast<uintptr_t>(_allocBase.get()) + 63) &
      ~((uintptr_t)0x3fu));  // to actually implement the 64-byte alignment,
  // shift base pointer within allocated space to
  // 64-byte boundary
  TRI_ASSERT(reinterpret_cast<uintptr_t>(_base) % 64 == 0);

  _counters = _base + bytesForSlots;
  TRI_ASSERT(reinterpret_cast<uintptr_t>(_counters) % 64 == 0);

  increaseMemoryUsage(_allocSize);
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::drainNoLock() {
  uint64_t memoryUsage = 0;
  for (auto const& it : _insertBuffers) {
    memoryUsage +=
        bufferedEntrySize() + bufferedEntryItemSize() * it.second.size();
  }

  for (auto const& it : _removalBuffers) {
    memoryUsage +=
        bufferedEntrySize() + bufferedEntryItemSize() * it.second.size();
  }

  memoryUsage += bufferedEntrySize() * _truncateBuffer.size();

  _insertBuffers.clear();
  _removalBuffers.clear();
  _truncateBuffer.clear();

  decreaseMemoryUsage(memoryUsage);
  checkInvariants();
}

template<class Key>
void RocksDBCuckooIndexEstimator<Key>::freeMemory() {
  WRITE_LOCKER(locker, _lock);

  drainNoLock();

  // only to validate that our math is correct and we are not missing
  // anything.
  TRI_ASSERT(_allocSize == _memoryUsage);
  decreaseMemoryUsage(_allocSize);
  TRI_ASSERT(_memoryUsage == 0);

  _nrTotal = 0;
  _nrCuckood = 0;
  _nrUsed = 0;

  _allocBase.reset();
  _base = nullptr;
  _counters = nullptr;
  _allocSize = 0;

  checkInvariants();
}

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
template<class Key>
void RocksDBCuckooIndexEstimator<Key>::checkInvariants() const {
  // invariants check is disabled because it slows down
  // everything considerably. can be turned back on for
  // debugging.
#if ARANGODB_DEBUG_MEMORY_USAGE
  uint64_t memoryUsage = 0;
  for (auto const& it : _insertBuffers) {
    memoryUsage +=
        bufferedEntrySize() + bufferedEntryItemSize() * it.second.size();
  }

  for (auto const& it : _removalBuffers) {
    memoryUsage +=
        bufferedEntrySize() + bufferedEntryItemSize() * it.second.size();
  }

  memoryUsage += bufferedEntrySize() * _truncateBuffer.size();
  TRI_ASSERT(_memoryUsage == memoryUsage + _allocSize);
#endif
}
#endif

template class arangodb::RocksDBCuckooIndexEstimator<uint64_t>;

}  // namespace arangodb
