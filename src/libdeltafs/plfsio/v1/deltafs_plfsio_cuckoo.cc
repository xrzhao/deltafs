/*
 * Copyright (c) 2015-2018 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_cuckoo.h"
#include "deltafs_plfsio_types.h"

#include <math.h>

namespace pdlfs {
namespace plfsio {

template <size_t b = 16>
struct CuckooBucket {  // Fixed 4 items per bucket
  unsigned x0 : b;
  unsigned x1 : b;
  unsigned x2 : b;
  unsigned x3 : b;
};

template <size_t bits_per_item = 16>
class CuckooReader {
 public:
  explicit CuckooReader(const Slice& input) : input_(input) {}
  ~CuckooReader() {}

  uint32_t Read(size_t i, size_t j) const {
    if (i >= NumBuckets()) return 0;
    const char* c = input_.data();
    const BucketType* const b = reinterpret_cast<const BucketType*>(c);
    j = j % kItemsPerBucket;
    if (j == 0) return b[i].x0;
    if (j == 1) return b[i].x1;
    if (j == 2) return b[i].x2;
    if (j == 3) return b[i].x3;
    return 0;
  }

  size_t NumBuckets() const {  // Return the effective number of buckets
    return input_.size() / kBytesPerBucket;
  }

 private:
  template <size_t>
  friend class CuckooKeyTester;
  typedef CuckooBucket<bits_per_item> BucketType;
  static const int kBytesPerBucket = sizeof(BucketType);
  static const int kItemsPerBucket = 4;
  Slice input_;
};

template <size_t bits_per_item = 16>
class CuckooTable {
 public:
  CuckooTable(const DirOptions& options)
      : num_buckets_(0), frac_(options.cuckoo_frac) {}

  static uint64_t UpperPower2(uint64_t x) {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    x++;
    return x;
  }

  void Reset(uint32_t num_keys) {
    space_.resize(0);
    num_buckets_ = static_cast<size_t>(
        ceil(1.0 / frac_ * (num_keys + kItemsPerBucket - 1) / kItemsPerBucket));
    num_buckets_ = UpperPower2(num_buckets_);
    space_.resize(num_buckets_ * kBytesPerBucket, 0);
  }

  void Write(size_t i, size_t j, uint32_t x) {
    assert(i < num_buckets_ && j < kItemsPerBucket);
    char* c = const_cast<char*>(space_.data());
    BucketType* const b = reinterpret_cast<BucketType*>(c);
    assert(x != 0);
    if (j == 0) b[i].x0 = x;
    if (j == 1) b[i].x1 = x;
    if (j == 2) b[i].x2 = x;
    if (j == 3) b[i].x3 = x;
  }

  uint32_t Read(size_t i, size_t j) const {
    assert(i < num_buckets_ && j < kItemsPerBucket);
    const char* c = space_.data();
    const BucketType* const b = reinterpret_cast<const BucketType*>(c);
    if (j == 0) return b[i].x0;
    if (j == 1) return b[i].x1;
    if (j == 2) return b[i].x2;
    if (j == 3) return b[i].x3;
    return 0;
  }

 private:
  template <size_t>
  friend class CuckooBlock;
  typedef CuckooBucket<bits_per_item> BucketType;
  static const int kBytesPerBucket = sizeof(BucketType);
  static const int kItemsPerBucket = 4;
  size_t num_buckets_;  // Total number of hash buckets, over-allocated by frac_
  std::string space_;
  double frac_;  // Target table occupation rate

  // No copying allowed
  void operator=(const CuckooTable&);
  CuckooTable(const CuckooTable&);
};

template <size_t bits_per_key>
CuckooBlock<bits_per_key>::CuckooBlock(const DirOptions& options,
                                       size_t bytes_to_reserve)
    : max_cuckoo_moves_(options.cuckoo_max_moves),
      finished_(true),  // Reset(num_keys) must be called before inserts
      rnd_(options.cuckoo_seed) {
  rep_ = new Rep(options);
  if (bytes_to_reserve != 0) {
    rep_->space_.reserve(bytes_to_reserve + 8);
  }
}

template <size_t bits_per_key>
CuckooBlock<bits_per_key>::~CuckooBlock() {
  delete rep_;
}

template <size_t bits_per_key>
void CuckooBlock<bits_per_key>::Reset(uint32_t num_keys) {
  rep_->Reset(num_keys);
  finished_ = false;
}

template <size_t bits_per_key>
Slice CuckooBlock<bits_per_key>::Finish() {
  assert(!finished_);
  finished_ = true;
  Rep* const r = rep_;
  PutFixed32(&r->space_, r->num_buckets_);
  PutFixed32(&r->space_, bits_per_key);
  return r->space_;
}

template <size_t bits_per_key>
void CuckooBlock<bits_per_key>::AddKey(const Slice& key) {
  Rep* const r = rep_;
  uint32_t fp = CuckooFingerprint(key, bits_per_key);
  uint32_t hash = CuckooHash(key);
  size_t i = hash % r->num_buckets_;
  // Our goal is to put fp into bucket i
  for (int count = 0; count < max_cuckoo_moves_; count++) {
    if (bits_per_key < 32) assert((fp & (~((1u << bits_per_key) - 1))) == 0);
    for (size_t j = 0; j < r->kItemsPerBucket; j++) {
      uint32_t cur = r->Read(i, j);
      if (cur == fp) return;  // Done
      if (cur == 0) {
        r->Write(i, j, fp);
        return;
      }
    }
    if (count != 0) {  // Kick out a victim so we can put fp in
      size_t v = rnd_.Next() % r->kItemsPerBucket;
      uint32_t old = r->Read(i, v);
      assert(old != 0 && old != fp);
      r->Write(i, v, fp);
      fp = old;
    }

    i = CuckooAlt(i, fp) % r->num_buckets_;
  }

  victims_.insert(fp);
}

template <size_t bits_per_key>
size_t CuckooBlock<bits_per_key>::bytes_per_bucket() const {
  return static_cast<size_t>(rep_->kBytesPerBucket);
}

template <size_t bits_per_key>
size_t CuckooBlock<bits_per_key>::num_buckets() const {
  return rep_->num_buckets_;
}

template <size_t bits_per_key>
class CuckooKeyTester {
 public:
  bool operator()(const Slice& key, const Slice& input) {
    const char* const tail = input.data() + input.size();
#ifndef NDEBUG
    assert(input.size() >= 8);
    size_t bits = DecodeFixed32(tail - 4);
    assert(bits == bits_per_key);
#endif
    size_t num_bucket = DecodeFixed32(tail - 8);
    uint32_t fp = CuckooFingerprint(key, bits_per_key);
    uint32_t hash = CuckooHash(key);

    CuckooReader<bits_per_key> reader(Slice(input.data(), input.size() - 8));
    size_t i1 = hash % num_bucket;
    size_t i2 = CuckooAlt(i1, fp) % num_bucket;

    for (size_t j = 0; j < reader.kItemsPerBucket; j++) {
      if (reader.Read(i1, j) == fp) {
        return true;
      } else if (reader.Read(i2, j) == fp) {
        return true;
      }
    }

    return false;
  }
};

template class CuckooBlock<32>;
template class CuckooBlock<24>;
template class CuckooBlock<20>;
template class CuckooBlock<16>;
template class CuckooBlock<10>;

bool CuckooKeyMayMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 8) {
    return true;
  }

  const char* tail = input.data() + input.size();
  size_t bits = DecodeFixed32(tail - 4);
  switch (int(bits)) {
#define CASE(n) \
  case n:       \
    return CuckooKeyTester<n>()(key, input)
    CASE(32);
    CASE(24);
    CASE(20);
    CASE(16);
    CASE(10);
#undef CASE
    default:
      return true;
  }
}

}  // namespace plfsio
}  // namespace pdlfs
