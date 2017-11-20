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
#include <algorithm>
#include <typeinfo>  // operator typeid

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
class UncompressedFormat {
 public:
  UncompressedFormat(const DirOptions& options, std::string* space)
      : key_bits_(options.bm_key_bits), space_(space) {
    bits_ = 1u << key_bits_;  // Logic domain space (total # unique keys)
  }

  void Reset(uint32_t num_keys) {
    space_->clear();
    const size_t bytes = (bits_ + 7) / 8;  // Bitmap size (uncompressed)
    space_->resize(bytes, 0);
  }

  // Set the i-th bit to "1". If the i-th bit is already set,
  // no action needs to be taken.
  void Set(uint32_t i) {
    assert(i < bits_);  // Must not flow out of the key space
    (*space_)[i / 8] |= 1 << (i % 8);
  }

  // Finalize the bitmap representation.
  // Return the final buffer size.
  size_t Finish() { return space_->size(); }

  // Return true iff the i-th bit is set in the given bitmap.
  static bool Test(uint32_t i, size_t key_bits, const Slice& input) {
    const size_t bits = input.size() * 8;
    if (i < bits) {
      return 0 != (input[i / 8] & (1 << (i % 8)));
    } else {
      return false;
    }
  }

 private:
  // Key size in bits
  const size_t key_bits_;
  // Underlying space for the bitmap
  std::string* const space_;
  // Total bits in the bitmap
  size_t bits_;
};

// Encoding a bitmap using a modified varint scheme.
class CompressedFormat {
 public:
  CompressedFormat(const DirOptions& options, std::string* space)
      : key_bits_(options.bm_key_bits), space_(space) {
    bits_ = 1u << key_bits_;  // Logic domain space (total # unique keys)
    bucket_num_ = 1u << (key_bits_ - 8);
  }

  // Reset filter state and resize buffer space.
  // Use num_keys to estimate bitmap density.
  void Reset(uint32_t num_keys) {
    space_->clear();
    working_space_.clear();
    overflowed.clear();
    // Calculate bucket size in probability
    // Extra byte to store the number of key in the bucket
    bucket_size_ = (num_keys + bucket_num_ - 1) / bucket_num_ + 1;
    working_space_.resize(bucket_size_ * bucket_num_, 0);
    // Calculate the approximate final result size
    size_t approx_size = (num_keys * 10 + 7) / 8;  // Assume 10 bits/key
    space_->reserve(approx_size);
  }

  // Set the i-th bit to "1". If the i-th bit is already set,
  // no action needs to be taken.
  void Set(uint32_t i) {
    int bucket_index = i >> 8;
    // Read bucket key number
    unsigned char key_index = working_space_[bucket_index * bucket_size_];
    if (key_index < bucket_size_ - 1) {
      // Append to the bucket
      working_space_[bucket_index * bucket_size_ + key_index + 1] =
          i & ((1 << 8) - 1);
    } else {
      // Append to overflow vector
      overflowed.push_back(i);
    }
    // Update the bucket key number
    working_space_[bucket_index * bucket_size_] = key_index + 1;
  }

  size_t memory_usage() const {
    return space_->capacity() + working_space_.capacity() +
           overflowed.size() * sizeof(size_t);
  }

  // Finalize the bitmap representation.
  // Return the final buffer size.
  size_t Finish();

  // Return true iff the i-th bit is set in the given bitmap.
  static bool Test(uint32_t bit, size_t key_bits, const Slice& input);

 protected:
  // Key size in bits
  const size_t key_bits_;  // Domain space
  // Underlying space for the bitmap
  std::string* const space_;
  // Logic bits in the bitmap.
  // The actual memory used may differ due to compression.
  size_t bits_;

  // Space for working space.
  std::string working_space_;

  // Variable specified for construction.
  size_t bucket_num_;
  size_t bucket_size_;
  std::vector<size_t> overflowed;
};

class VarintFormat : public CompressedFormat {
 public:
  VarintFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  size_t Finish() {
    std::sort(overflowed.begin(), overflowed.end());
    size_t overflowed_idx = 0;
    size_t last_one = 0;
    std::vector<size_t> bucket_keys;
    // For every bucket
    for (size_t i = 0; i < bucket_num_; i++) {
      // Bucket key size
      unsigned char key_num =
          static_cast<unsigned char>(working_space_[bucket_size_ * i]);
      size_t offset = bucket_size_ * i + 1;
      // Clear vector for repeated use.
      bucket_keys.clear();
      for (int j = 0; j < key_num; j++) {
        if (j < bucket_size_ - 1)
          bucket_keys.push_back(
              static_cast<unsigned char>(working_space_[offset + j]) +
              (i << 8));
        else
          bucket_keys.push_back(overflowed[overflowed_idx++]);
      }
      std::sort(bucket_keys.begin(), bucket_keys.end());
      for (std::vector<size_t>::iterator it = bucket_keys.begin();
           it != bucket_keys.end(); ++it) {
        size_t distance = *it - last_one;
        last_one = *it;
        // Encoding the distance to variable length encode.
        unsigned char b = static_cast<unsigned char>(distance % 128);
        while (distance / 128 > 0) {
          // Set continue bit
          b |= 1 << 7;
          space_->push_back(b);
          distance /= 128;
          b = static_cast<unsigned char>(distance % 128);
        }
        space_->push_back(b);
      }
    }
    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& input) {
    size_t index = 0;
    const unsigned char signal_mask = 1 << 7;
    const unsigned char bit_mask = signal_mask - 1;
    for (size_t i = 0; i < input.size(); i++) {
      size_t runLen = 0;
      size_t bytes = 0;
      while ((input[i] & signal_mask) != 0) {
        runLen += static_cast<size_t>(input[i] & bit_mask) << bytes;
        i++;
        bytes += 7;
      }
      runLen += static_cast<size_t>(input[i]) << bytes;
      if (index + runLen == bit)
        return true;
      else if (index + runLen > bit)
        return false;
      else
        index += runLen;
    }
    return false;
  }
};

class VarintPlusFormat : public CompressedFormat {
 public:
  VarintPlusFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  size_t Finish() {
    std::sort(overflowed.begin(), overflowed.end());
    size_t overflowed_idx = 0;
    size_t last_one = 0;
    std::vector<size_t> bucket_keys;
    // For every bucket
    for (size_t i = 0; i < bucket_num_; i++) {
      // Bucket key size
      unsigned char key_num =
          static_cast<unsigned char>(working_space_[bucket_size_ * i]);
      size_t offset = bucket_size_ * i + 1;
      // Clear vector for repeated use.
      bucket_keys.clear();
      for (int j = 0; j < key_num; j++) {
        if (j < bucket_size_ - 1)
          bucket_keys.push_back(
              static_cast<unsigned char>(working_space_[offset + j]) +
              (i << 8));
        else
          bucket_keys.push_back(overflowed[overflowed_idx++]);
      }
      std::sort(bucket_keys.begin(), bucket_keys.end());
      for (std::vector<size_t>::iterator it = bucket_keys.begin();
           it != bucket_keys.end(); ++it) {
        size_t distance = *it - last_one;
        last_one = *it;
        // Encoding the distance to variable length plus encode.
        if (distance <= 254) {
          space_->push_back(static_cast<unsigned char>(distance));
        } else {
          space_->push_back(static_cast<unsigned char>(255));
          distance -= 254;
          unsigned char b = static_cast<unsigned char>(distance % 128);
          while (distance / 128 > 0) {
            // Set continue bit
            b |= 1 << 7;
            space_->push_back(b);
            distance /= 128;
            b = static_cast<unsigned char>(distance % 128);
          }
          space_->push_back(b);
        }
      }
    }
    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& input) {
    const unsigned char signal_mask = 1 << 7;
    const unsigned char bit_mask = signal_mask - 1;
    size_t index = 0;
    for (size_t i = 0; i < input.size(); i++) {
      size_t runLen = 0;
      size_t bytes = 0;
      if (static_cast<unsigned char>(input[i]) != 255) {
        runLen += static_cast<unsigned char>(input[i]);
      } else {
        runLen += 254;
        i++;
        while ((input[i] & signal_mask) != 0) {
          runLen += static_cast<size_t>(input[i] & bit_mask) << bytes;
          i++;
          bytes += 7;
        }
        runLen += static_cast<size_t>(input[i]) << bytes;
      }
      if (index + runLen == bit)
        return true;
      else if (index + runLen > bit)
        return false;
      else
        index += runLen;
    }
    return false;
  }
};

class PForDeltaFormat : public CompressedFormat {
 public:
  PForDeltaFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  size_t Finish() {
    std::sort(overflowed.begin(), overflowed.end());
    size_t overflowed_idx = 0;
    size_t last_one = 0;
    std::vector<size_t> bucket_keys;

    std::vector<size_t> cohort;
    cohort.reserve(cohort_size_);
    size_t cohort_or = 0;
    // For every bucket
    for (size_t i = 0; i < bucket_num_; i++) {
      // Bucket key size
      unsigned char key_num =
          static_cast<unsigned char>(working_space_[bucket_size_ * i]);
      size_t offset = bucket_size_ * i + 1;
      // Clear vector for repeated use.
      bucket_keys.clear();
      for (int j = 0; j < key_num; j++) {
        if (j < bucket_size_ - 1)
          bucket_keys.push_back(
              static_cast<unsigned char>(working_space_[offset + j]) +
              (i << 8));
        else
          bucket_keys.push_back(overflowed[overflowed_idx++]);
      }
      std::sort(bucket_keys.begin(), bucket_keys.end());
      for (std::vector<size_t>::iterator it = bucket_keys.begin();
           it != bucket_keys.end(); ++it) {
        size_t distance = *it - last_one;
        last_one = *it;
        // Encoding the distance using pForDelta.
        cohort.push_back(distance);
        cohort_or |= distance;
        // If full
        if (cohort.size() == cohort_size_) {
          EncodingCohort(cohort, cohort_or);
          cohort_or = 0;
          cohort.clear();
        }
      }
    }
    if (cohort.size() > 0) {
      EncodingCohort(cohort, cohort_or);
    }
    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& input) {
    size_t index = 0;
    size_t cohort_num = 0;
    unsigned char bit_num;
    unsigned char b = 0;
    int byte_index = -1;
    for (size_t i = 0; i < input.size();) {
      assert(byte_index == -1);

      bit_num = static_cast<unsigned char>(input[i++]);
      cohort_num = cohort_size_;
      if ((input.size() - i) * 8 / bit_num < cohort_num) {
        cohort_num = (input.size() - i) * 8 / bit_num;
      }
      while (cohort_num > 0) {
        size_t runLen = 0;
        int runLen_index = bit_num - 1;
        while (runLen_index >= 0) {
          if (byte_index < 0) {
            if (i < input.size()) {
              b = static_cast<unsigned char>(input[i]);
              byte_index = 7;
              i++;
            } else {
              return false;
            }
          }
          runLen |= (b & (1 << byte_index--)) > 0 ? (1 << runLen_index) : 0;
          runLen_index -= 1;
        }
        cohort_num -= 1;
        if (index + runLen == bit)
          return true;
        else if (index + runLen > bit)
          return false;
        else
          index += runLen;
      }
    }
    return false;
  }

 private:
  void EncodingCohort(std::vector<size_t>& cohort, size_t cohort_or) {
    unsigned char bit_num = LeftMostOneBit(cohort_or);
    space_->push_back(bit_num);

    unsigned char b = 0;  // tmp byte to fill bit by bit
    int byte_index = 7;   // Start fill from the most significant bit.
    int dis_index = bit_num - 1;
    // Encoding cohort
    for (std::vector<size_t>::iterator it = cohort.begin(); it != cohort.end();
         ++it) {
      dis_index = bit_num - 1;
      while (dis_index >= 0) {
        b |= (*it & (1 << dis_index--)) >= 1 ? (1 << byte_index) : 0;
        if (byte_index-- == 0) {
          space_->push_back(b);
          b = 0;
          byte_index = 7;
        }
      }
    }
    if (byte_index != 7) {
      space_->push_back(b);
    }
  }

  // We assume that cohort size is multiple of 8.
  const static size_t cohort_size_ = 128;
};

// Roaring bitmap format bucket size 2^8
class RoaringFormat : public CompressedFormat {
 public:
  RoaringFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  void Reset(uint32_t num_keys) {
    CompressedFormat::Reset(num_keys);
    bucket_size_max_bit_ = 0;
  }

  void Set(uint32_t i) {
    // Copy from parent
    int bucket_index = i >> 8;
    // Read bucket key number
    unsigned char key_index = working_space_[bucket_index * bucket_size_];
    if (key_index < bucket_size_ - 1) {
      // Append to the bucket
      working_space_[bucket_index * bucket_size_ + key_index + 1] =
          i & ((1 << 8) - 1);
    } else {
      // Append to overflow vector
      overflowed.push_back(i);
    }
    // Update the bucket key number
    working_space_[bucket_index * bucket_size_] = key_index + 1;

    // Update max bit
    bucket_size_max_bit_ |= static_cast<size_t>(key_index + 1);
  }

  size_t Finish() {
    std::sort(overflowed.begin(), overflowed.end());
    size_t overflowed_idx = 0;

    unsigned char bits_per_len = LeftMostOneBit(bucket_size_max_bit_);
    space_->resize(1 + (bits_per_len * bucket_num_ + 7) / 8, 0);
    (*space_)[0] = bits_per_len;

    // Leave the space at the head to store the size for every buckets
    // The index of the byte at space_ to encode bucket size.
    size_t bucket_len_byte_idx = 1;

    unsigned char bucket_len_byte = 0;
    int bucket_len_bit_idx = 7;  // The index of bit in the constructing byte.

    std::vector<unsigned char> bucket_keys;
    // For every bucket
    for (size_t i = 0; i < bucket_num_; i++) {
      // Bucket key size
      unsigned char key_num =
          static_cast<unsigned char>(working_space_[bucket_size_ * i]);
      size_t offset = bucket_size_ * i + 1;
      // Clear vector for repeated use.
      bucket_keys.clear();
      for (int j = 0; j < key_num; j++) {
        if (j < bucket_size_ - 1)
          bucket_keys.push_back(
              static_cast<unsigned char>(working_space_[offset + j]));
        else
          bucket_keys.push_back(
              static_cast<unsigned char>(overflowed[overflowed_idx++] & 255));
      }
      std::sort(bucket_keys.begin(), bucket_keys.end());
      // Encoding bucket size
      int len_index = bits_per_len - 1;
      while (len_index >= 0) {
        bucket_len_byte |=
            (key_num & (1 << len_index--)) >= 1 ? (1 << bucket_len_bit_idx) : 0;
        if (bucket_len_bit_idx-- == 0) {
          (*space_)[bucket_len_byte_idx++] = bucket_len_byte;
          bucket_len_byte = 0;
          bucket_len_bit_idx = 7;
        }
      }
      // Fill the offsets of the bucket
      for (std::vector<unsigned char>::iterator it = bucket_keys.begin();
           it != bucket_keys.end(); ++it) {
        // Encoding the distance to roaring bitmap encode.
        space_->push_back(*it);
      }
    }

    if (bucket_len_bit_idx != 7) {
      (*space_)[bucket_len_byte_idx] = bucket_len_byte;
      ;
    }

    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& input) {
    uint32_t bucket_idx = bit >> 8;
    unsigned char bit_per_len = input[0];
    int len_byte_idx = 1;
    size_t offset = 0;
    size_t len;

    // Get the offset of the bucket
    unsigned char b = 0;
    int b_idx = -1;
    while (bucket_idx-- > 0) {
      len = 0;
      int len_index = bit_per_len - 1;
      while (len_index >= 0) {
        if (b_idx < 0) {
          b = static_cast<unsigned char>(input[len_byte_idx++]);
          b_idx = 7;
        }
        len |= (b & (1 << b_idx--)) > 0 ? (1 << len_index) : 0;
        len_index -= 1;
      }
      offset += len;
    }

    // Get the size of the bucket
    size_t bucket_size = 0;
    int bucket_size_index = bit_per_len - 1;
    while (bucket_size_index >= 0) {
      if (b_idx < 0) {
        b = static_cast<unsigned char>(input[len_byte_idx++]);
        b_idx = 7;
      }
      bucket_size |= (b & (1 << b_idx--)) > 0 ? (1 << bucket_size_index) : 0;
      bucket_size_index -= 1;
    }

    size_t start_idx =
        1 + ((1 << (key_bits - 8)) /*bucket number*/ * bit_per_len + 7) / 8 +
        offset;
    unsigned char target_offset = static_cast<unsigned char>(bit & 255);
    unsigned char key_offset;
    for (size_t i = start_idx; i < start_idx + bucket_size; i++) {
      key_offset = static_cast<unsigned char>(input[i]);
      if (key_offset == target_offset)
        return true;
      else if (key_offset > target_offset)
        return false;
    }
    return false;
  }

 private:
  size_t bucket_size_max_bit_ = 0;
};

// Partitioned Roaring bitmap format bucket size 2^8
class PRoaringFormat : public CompressedFormat {
 public:
  PRoaringFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space), partition_num_(bucket_num_ >> 8) {}

  void Reset(uint32_t num_keys) {
    CompressedFormat::Reset(num_keys);

    bucket_size_max_bit_ = 0;
    partition_sum_.resize(partition_num_, 0);
  }

  void Set(uint32_t i) {
    // Copy from parent
    int bucket_index = i >> 8;
    // Read bucket key number
    unsigned char key_index = working_space_[bucket_index * bucket_size_];
    if (key_index < bucket_size_ - 1) {
      // Append to the bucket
      working_space_[bucket_index * bucket_size_ + key_index + 1] = i & 0xff;
    } else {
      // Append to overflow vector
      overflowed.push_back(i);
    }
    // Update the bucket key number
    working_space_[bucket_index * bucket_size_] = key_index + 1;

    int partition_index = bucket_index >> 8;
    // Update max bit
    bucket_size_max_bit_ |= static_cast<size_t>(key_index + 1);
    partition_sum_[partition_index] += 1;
  }

  size_t Finish() {
    std::sort(overflowed.begin(), overflowed.end());
    size_t overflowed_idx = 0;

    // Reserve space for header (partition sum & bits)
    space_->resize(partition_num_ * sizeof(uint16_t) + 1, 0);

    for (size_t i = 0; i < partition_num_; i++) {
      (*space_)[2 * i] = static_cast<char>(partition_sum_[i] & 0xff);
      (*space_)[2 * i + 1] = static_cast<char>((partition_sum_[i] >> 8) & 0xff);
    }

    unsigned char bits_per_len = LeftMostOneBit(bucket_size_max_bit_);
    (*space_)[partition_num_ * sizeof(uint16_t)] = bits_per_len;

    // Leave the space at the head to store the size for every buckets
    // The index of the byte at space_ to encode bucket size.
    size_t bucket_len_byte_idx = space_->size();

    space_->resize(space_->size() + (bits_per_len * bucket_num_ + 7) / 8, 0);

    unsigned char bucket_len_byte = 0;
    int bucket_len_bit_idx = 7;  // The index of bit in the constructing byte.

    std::vector<unsigned char> bucket_keys;
    // For every bucket
    for (size_t i = 0; i < bucket_num_; i++) {
      // Bucket key size
      unsigned char key_num =
          static_cast<unsigned char>(working_space_[bucket_size_ * i]);
      size_t offset = bucket_size_ * i + 1;
      // Clear vector for repeated use.
      bucket_keys.clear();
      for (int j = 0; j < key_num; j++) {
        if (j < bucket_size_ - 1)
          bucket_keys.push_back(
              static_cast<unsigned char>(working_space_[offset + j]));
        else
          bucket_keys.push_back(
              static_cast<unsigned char>(overflowed[overflowed_idx++] & 255));
      }
      std::sort(bucket_keys.begin(), bucket_keys.end());
      // Encoding bucket size
      int len_index = bits_per_len - 1;
      while (len_index >= 0) {
        bucket_len_byte |=
            (key_num & (1 << len_index--)) >= 1 ? (1 << bucket_len_bit_idx) : 0;
        if (bucket_len_bit_idx-- == 0) {
          (*space_)[bucket_len_byte_idx++] = bucket_len_byte;
          bucket_len_byte = 0;
          bucket_len_bit_idx = 7;
        }
      }
      // Fill the offsets of the bucket
      for (std::vector<unsigned char>::iterator it = bucket_keys.begin();
           it != bucket_keys.end(); ++it) {
        // Encoding the distance to roaring bitmap encode.
        space_->push_back(*it);
      }
    }

    if (bucket_len_bit_idx != 7) {
      (*space_)[bucket_len_byte_idx] = bucket_len_byte;
      ;
    }

    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& input) {
    size_t partition_idx = bit >> 16;
    size_t bucket_idx = (bit & 0xff00) >> 8;
    size_t bucket_number = 1 << (key_bits - 8);
    size_t partition_num = bucket_number >> 8;

    size_t offset = 0;
    // Traverse partition lookup table
    for (int i = 0; i < partition_idx; i++) {
      offset += (input[2 * i] + (input[2 * i + 1] << 8));
    }

    unsigned char bit_per_len = input[2 * partition_num];
    int len_byte_idx =
        2 * partition_num + 1 +
        (bit_per_len * partition_idx << (8 - 3));  // *256 / 8(bits/byte)
    size_t len;

    // Get the offset of the bucket
    unsigned char b = 0;
    int b_idx = -1;
    while (bucket_idx-- > 0) {
      len = 0;
      int len_index = bit_per_len - 1;
      while (len_index >= 0) {
        if (b_idx < 0) {
          b = static_cast<unsigned char>(input[len_byte_idx++]);
          b_idx = 7;
        }
        len |= (b & (1 << b_idx--)) > 0 ? (1 << len_index) : 0;
        len_index -= 1;
      }
      offset += len;
    }

    // Get the size of the bucket
    size_t bucket_size = 0;
    int bucket_size_index = bit_per_len - 1;
    while (bucket_size_index >= 0) {
      if (b_idx < 0) {
        b = static_cast<unsigned char>(input[len_byte_idx++]);
        b_idx = 7;
      }
      bucket_size |= (b & (1 << b_idx--)) > 0 ? (1 << bucket_size_index) : 0;
      bucket_size_index -= 1;
    }

    size_t start_idx =
        2 * partition_num + 1 + (bucket_number * bit_per_len + 7) / 8 + offset;
    unsigned char target_offset = static_cast<unsigned char>(bit & 0xff);
    unsigned char key_offset;
    for (size_t i = start_idx; i < start_idx + bucket_size; i++) {
      key_offset = static_cast<unsigned char>(input[i]);
      if (key_offset == target_offset)
        return true;
      else if (key_offset > target_offset)
        return false;
    }
    return false;
  }

 private:
  size_t partition_num_;
  size_t bucket_size_max_bit_ = 0;
  std::vector<uint16_t> partition_sum_;
};

unsigned char LeftMostOneBit(uint32_t i) {
  if (i == 0) return 0;

  unsigned char bit_num;
#if defined(__GNUC__)
  bit_num = static_cast<unsigned char>(32 - __builtin_clz(i));
#else
  unsigned int n = 1;
  if (i >> 16 == 0) {
    n += 16;
    i <<= 16;
  }
  if (i >> 24 == 0) {
    n += 8;
    i <<= 8;
  }
  if (i >> 28 == 0) {
    n += 4;
    i <<= 4;
  }
  if (i >> 30 == 0) {
    n += 2;
    i <<= 2;
  }
  n -= i >> 31;
  bit_num = static_cast<unsigned char>(32 - n);
#endif
  return bit_num;
}

template <typename T>
int BitmapBlock<T>::chunk_type() {
  return static_cast<int>(ChunkType::kBmpChunk);
}

template <typename T>
BitmapBlock<T>::BitmapBlock(const DirOptions& options, size_t bytes_to_reserve)
    : key_bits_(options.bm_key_bits) {
  // Reserve extra 2 bytes for storing key_bits and the compression type
  if (bytes_to_reserve != 0) {
    space_.reserve(bytes_to_reserve + 2);
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

// To convert a key into an int, the first 4 bytes of the key is interpreted as
// the little-endian representation of a 32-bit int. As illustrated below,
// the conversion may be seen as using the "first" 32 bits of the byte array to
// construct an int.
//
// [07.06.05.04.03.02.01.00]  [15.14.13.12.11.10.09.08] [...] [...]
//  <------------ byte 0 ->    <------------ byte 1 ->
//
static uint32_t BitmapIndex(const Slice& key) {
  char tmp[4];
  memset(tmp, 0, sizeof(tmp));
  memcpy(tmp, key.data(), std::min(key.size(), sizeof(tmp)));
  return DecodeFixed32(tmp);
}

// Insert a key (1-4 bytes) into the bitmap filter. If the key has more than 4
// bytes, the rest bytes are ignored. If a key has less than 4 bytes, it will be
// zero-padded to 4 bytes.
// Inserting a key is achieved by first converting the key into an int, i,
// and then setting the i-th bit of the bitmap to "1".
template <typename T>
void BitmapBlock<T>::AddKey(const Slice& key) {
  assert(!finished_);  // Finish() has not been called
  uint32_t i = BitmapIndex(key);
  i &= mask_;
  assert(fmt_ != NULL);
  fmt_->Set(i);
}

template <typename T>
Slice BitmapBlock<T>::Finish() {
  assert(fmt_ != NULL);
  finished_ = true;
  size_t len = fmt_->Finish();
  space_.resize(len);
  // Remember the size of the domain space
  space_.push_back(static_cast<char>(key_bits_));
  // Remember the compression type
  if (typeid(T) == typeid(UncompressedFormat)) {
    space_.push_back(static_cast<char>(BitmapFormatType::kUncompressedBitmap));
  } else if (typeid(T) == typeid(VarintFormat)) {
    space_.push_back(static_cast<char>(BitmapFormatType::kVarintBitmap));
  } else if (typeid(T) == typeid(VarintPlusFormat)) {
    space_.push_back(static_cast<char>(BitmapFormatType::kVarintPlusBitmap));
  } else if (typeid(T) == typeid(PForDeltaFormat)) {
    space_.push_back(static_cast<char>(BitmapFormatType::kPForDeltaBitmap));
  } else if (typeid(T) == typeid(RoaringFormat)) {
    space_.push_back(static_cast<char>(BitmapFormatType::kRoaringBitmap));
  } else if (typeid(T) == typeid(PRoaringFormat)) {
    space_.push_back(static_cast<char>(BitmapFormatType::kPRoaringBitmap));
  }
  return space_;
}

template <typename T>
BitmapBlock<T>::~BitmapBlock() {
  delete fmt_;
}

template class BitmapBlock<UncompressedFormat>;

template class BitmapBlock<VarintFormat>;

template class BitmapBlock<VarintPlusFormat>;

template class BitmapBlock<PForDeltaFormat>;

template class BitmapBlock<RoaringFormat>;

template class BitmapBlock<PRoaringFormat>;

// Return true if the target key matches a given bitmap filter. Unlike bloom
// filters, bitmap filters are designed with no false positives.
bool BitmapKeyMustMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 2) {
    return false;  // Empty bitmap
  }

  Slice bitmap =
      input;  // Net bitmap representation (maybe in a compressed form)
  bitmap.remove_suffix(2);
  uint32_t i = BitmapIndex(key);

  // Recover the domain space
  const size_t key_bits = static_cast<unsigned char>(input[input.size() - 2]);

  size_t bits = 1u << key_bits;
  if (i >= bits) {
    return false;  // Out of bound
  }

  const int compression = input[input.size() - 1];
  if (compression == BitmapFormatType::kUncompressedBitmap) {
    return UncompressedFormat::Test(i, key_bits, bitmap);
  } else if (compression == BitmapFormatType::kVarintBitmap) {
    return VarintFormat::Test(i, key_bits, bitmap);
  } else if (compression == BitmapFormatType::kVarintPlusBitmap) {
    return VarintPlusFormat::Test(i, key_bits, bitmap);
  } else if (compression == BitmapFormatType::kPForDeltaBitmap) {
    return PForDeltaFormat::Test(i, key_bits, bitmap);
  } else if (compression == BitmapFormatType::kRoaringBitmap) {
    return RoaringFormat::Test(i, key_bits, bitmap);
  } else if (compression == BitmapFormatType::kPRoaringBitmap) {
    return PRoaringFormat::Test(i, key_bits, bitmap);
  } else {
    return true;
  }
}

int EmptyFilterBlock::chunk_type() {
  return static_cast<int>(ChunkType::kUnknown);
}

EmptyFilterBlock::EmptyFilterBlock(const DirOptions& o, size_t b) {
  space_.resize(0);
#if __cplusplus >= 201103L
  space_.shrink_to_fit();  // not available before c++11
#endif
}

}  // namespace plfsio
}  // namespace pdlfs
