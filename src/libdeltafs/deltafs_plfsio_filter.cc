/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_filter.h"
#include "deltafs_plfsio_format.h"

#include "deltafs_plfsio.h"

#include <assert.h>

namespace pdlfs {
namespace plfsio {

BloomBlock::BloomBlock(const DirOptions& options, size_t bytes_to_reserve)
    : bits_per_key_(options.bf_bits_per_key) {
  // Round down to reduce probing cost a little bit
  k_ = static_cast<uint32_t>(bits_per_key_ * 0.69);  // 0.69 =~ ln(2)
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
  // Reserve an extra byte for storing the k
  if (bytes_to_reserve != 0) {
    space_.reserve(bytes_to_reserve + 1);
  }
  finished_ = true;  // Pending further initialization
  bits_ = 0;
}

BloomBlock::~BloomBlock() {}

int BloomBlock::chunk_type() {
  return static_cast<int>(ChunkType::kSbfChunk);  // Standard bloom filter
}

void BloomBlock::Reset(uint32_t num_keys) {
  bits_ = static_cast<uint32_t>(num_keys * bits_per_key_);
  // For small n, we can see a very high false positive rate.
  // Fix it by enforcing a minimum bloom filter length.
  if (bits_ < 64) {
    bits_ = 64;
  }
  uint32_t bytes = (bits_ + 7) / 8;
  finished_ = false;
  space_.clear();
  space_.resize(bytes, 0);
  // Remember # of probes in filter
  space_.push_back(static_cast<char>(k_));
  // Finalize # bits
  bits_ = bytes * 8;
}

void BloomBlock::AddKey(const Slice& key) {
  assert(!finished_);  // Finish() has not been called
  // Use double-hashing to generate a sequence of hash values.
  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  for (size_t j = 0; j < k_; j++) {
    const uint32_t b = h % bits_;
    space_[b / 8] |= (1 << (b % 8));
    h += delta;
  }
}

Slice BloomBlock::Finish() {
  assert(!finished_);
  finished_ = true;
  return space_;
}

bool BloomKeyMayMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 2) {
    return true;  // Consider it a match
  }
  const uint32_t bits = static_cast<uint32_t>((len - 1) * 8);

  const char* array = input.data();
  // Use the encoded k so that we can read filters generated by
  // bloom filters created using different parameters.
  const uint32_t k = static_cast<unsigned char>(array[len - 1]);
  if (k > 30) {
    // Reserved for potentially new encodings for short bloom filters.
    // Consider it a match.
    return true;
  }

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  for (size_t j = 0; j < k; j++) {
    const uint32_t b = h % bits;
    if ((array[b / 8] & (1 << (b % 8))) == 0) {
      return false;
    }
    h += delta;
  }

  return true;
}

// Encoding a bitmap as-is, uncompressed. Used for debugging only.
// Not intended for production.
// The first byte is used to store the key size in # bits.
class UncompressedFormat {
 public:
  UncompressedFormat(const DirOptions& options, std::string* space)
      : key_bits_(options.bm_key_bits), space_(space) {
    bits_ = 1u << key_bits_;  // Logic domain space (total # unique keys)
  }

  void Reset(uint32_t num_keys) {
    space_->clear();
    // Remember the key size in # bits
    space_->push_back(static_cast<char>(key_bits_));
    const size_t bytes = (bits_ + 7) / 8;  // Bitmap size (uncompressed)
    space_->resize(1 + bytes, 0);
  }

  // Set the i-th bit to "1". If the i-th bit is already set,
  // no action needs to be taken.
  void Set(uint32_t i) {
    assert(i < bits_);  // Must not flow out of the key space
    (*space_)[1 + (i / 8)] |= 1 << (i % 8);
  }

  // Finalize the buffer and return the final representation.
  Slice Finish() { return *space_; }

 private:
  // Key size in bits
  const size_t key_bits_;
  // Underlying space for the bitmap
  std::string* const space_;
  // Total bits in the bitmap
  size_t bits_;
};

// Encoding a bitmap using varint.
// The first byte is used to store the key size in # bits.
class VarintFormat {
 public:
  VarintFormat(const DirOptions& options, std::string* space)
      : key_bits_(options.bm_key_bits), space_(space) {
    bits_ = 1u << key_bits_;  // Logic domain space (total # unique keys)
  }

  // Reset filter state and resize buffer space.
  // Use num_keys to estimate bitmap density.
  void Reset(uint32_t num_keys) {
    // TODO
  }

  // Set the i-th bit to "1". If the i-th bit is already set,
  // no action needs to be taken.
  void Set(uint32_t i) {
    // TODO
  }

  // Finalize the filter.
  // Return the final representation.
  Slice Finish() {
    // TODO
    return *space_;
  }

 private:
  // Key size in bits
  const size_t key_bits_;  // Domain space
  // Underlying space for the bitmap
  std::string* const space_;
  // Logic bits in the bitmap.
  // The actual memory used may differ due to compression.
  size_t bits_;
};

template <typename T>
int BitmapBlock<T>::chunk_type() {
  return static_cast<int>(ChunkType::kBmpChunk);
}

template <typename T>
BitmapBlock<T>::BitmapBlock(const DirOptions& options, size_t bytes_to_reserve)
    : key_bits_(options.bm_key_bits) {
  // Reserve an extra byte for storing the key size in bits
  if (bytes_to_reserve != 0) {
    space_.reserve(bytes_to_reserve + 1);
  }
  fmt_ = new T(options, &space_);
  finished_ = true;  // Pending further initialization
  mask_ = ~static_cast<uint32_t>(0) << key_bits_;
  mask_ = ~mask_;
}

template <typename T>
void BitmapBlock<T>::Reset(uint32_t num_keys) {
  assert(fmt_ != NULL);
  fmt_->Reset(num_keys);
  finished_ = false;
}

// Insert a key (1-4 bytes) into the bitmap filter. If the key has more than 4
// bytes, the rest bytes are ignored. If a key has less than 4 bytes, it will be
// zero-padded to 4 bytes.
// Inserting a key is achieved by first converting the key into an int, i,
// and then setting the i-th bit of the bitmap to "1".
// To convert a key into an int, the first 4 bytes of the key is interpreted as
// the little-endian representation of a 32-bit int. As illustrated below,
// the conversion may be seen as using the "first" 32 bits of the byte array to
// construct an int.
//
// [07.06.05.04.03.02.01.00]  [15.14.13.12.11.10.09.08] [...] [...]
//  <------------ byte 0 ->    <------------ byte 1 ->
//
template <typename T>
void BitmapBlock<T>::AddKey(const Slice& key) {
  assert(!finished_);  // Finish() has not been called
  char tmp[4] = {0};
  memcpy(tmp, key.data(), std::min(key.size(), sizeof(tmp)));
  uint32_t i = DecodeFixed32(tmp);
  i &= mask_;
  assert(fmt_ != NULL);
  fmt_->Set(i);
}

template <typename T>
Slice BitmapBlock<T>::Finish() {
  assert(fmt_ != NULL);
  finished_ = true;
  return fmt_->Finish();
}

template <typename T>
BitmapBlock<T>::~BitmapBlock() {
  delete fmt_;
}

template class BitmapBlock<UncompressedFormat>;

template class BitmapBlock<VarintFormat>;

int EmptyFilterBlock::chunk_type() {
  return static_cast<int>(ChunkType::kUnknown);
}

EmptyFilterBlock::EmptyFilterBlock(const DirOptions&, size_t) {
  space_.resize(0);
#if __cplusplus >= 201103L
  space_.shrink_to_fit();  // not available before c++11
#endif
}

}  // namespace plfsio
}  // namespace pdlfs
