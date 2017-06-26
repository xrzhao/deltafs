/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#pragma once

#include "deltafs_plfsio.h"
#include "deltafs_plfsio_format.h"
#include "deltafs_plfsio_log.h"

#include "pdlfs-common/env_files.h"
#include "pdlfs-common/port.h"

#include <string>
#include <vector>

namespace pdlfs {
namespace plfsio {

// Non-thread-safe append-only in-memory table.
class WriteBuffer {
 public:
  explicit WriteBuffer() : num_entries_(0), finished_(false) {}
  ~WriteBuffer() {}

  size_t memory_usage() const;  // Report real memory usage

  void Reserve(uint32_t num_entries, size_t buffer_size);
  size_t CurrentBufferSize() const { return buffer_.size(); }
  uint32_t NumEntries() const { return num_entries_; }
  void Add(const Slice& key, const Slice& value);
  Iterator* NewIterator() const;
  void FinishAndSort();
  void Reset();

 private:
  struct STLLessThan;
  // Starting offsets of inserted entries
  std::vector<uint32_t> offsets_;
  std::string buffer_;
  uint32_t num_entries_;
  bool finished_;

  // No copying allowed
  void operator=(const WriteBuffer&);
  WriteBuffer(const WriteBuffer&);

  class Iter;
};

struct OutputStats {  // All final sizes include padding and block trailers
  OutputStats();

  // Total size of data blocks
  size_t final_data_size;
  size_t data_size;

  // Total size of meta index blocks and the root meta index block
  size_t final_meta_index_size;
  size_t meta_index_size;

  // Total size of index blocks
  size_t final_index_size;
  size_t index_size;

  // Total size of filter blocks
  size_t final_filter_size;
  size_t filter_size;

  // Total size of user data compacted
  size_t value_size;
  size_t key_size;
};

// Write sorted table contents into a pair of log files.
class TableLogger {
 public:
  TableLogger(const DirOptions& options, LogSink* data, LogSink* indx);
  ~TableLogger();

  bool ok() const { return status_.ok(); }
  Status status() const { return status_; }

  void Add(const Slice& key, const Slice& value);
  Status Finish();

  // End the current block and force the start of a new data block.
  // REQUIRES: Finish() has not been called.
  void EndBlock();

  // Flush buffered data blocks and finalize their indexes.
  // REQUIRES: Finish() has not been called.
  void Commit();

  // Force the start of a new table.
  // Caller may optionally specify a corresponding filter block.
  // REQUIRES: Finish() has not been called.
  template <typename T>
  void EndTable(T* filter, ChunkType filter_type);

  // Force the start of a new epoch.
  // REQUIRES: Finish() has not been called.
  void MakeEpoch();

 private:
  const DirOptions& options_;
  OutputStats output_stats_;
  friend class DirLogger;

  // No copying allowed
  void operator=(const TableLogger&);
  TableLogger(const TableLogger&);

  Status status_;
  Footer footer_;  // Footer template
  std::string smallest_key_;
  std::string largest_key_;
  std::string last_key_;
  uint32_t num_uncommitted_indx_;  // Number of uncommitted index entries
  uint32_t num_uncommitted_data_;  // Number of uncommitted data blocks
  bool pending_restart_;  // Restart a new data block using an exiting buffer
  BlockBuilder data_block_;
  BlockBuilder indx_block_;  // Locate the data blocks within a table
  BlockBuilder meta_block_;  // Locate the tables within an epoch
  BlockBuilder root_block_;  // Locate each epoch
  bool pending_indx_entry_;
  BlockHandle pending_indx_handle_;
  bool pending_meta_entry_;
  TableHandle pending_meta_handle_;
  bool pending_root_entry_;
  BlockHandle pending_root_handle_;
  uint32_t num_tables_;  // Number of tables generated within the current epoch
  uint32_t num_epochs_;  // Number of epochs generated
  std::string uncommitted_indexes_;
  LogSink* data_sink_;
  LogWriter indx_logger_;
  LogSink* indx_sink_;
  bool finished_;
};

// Sequentially format and write data as multiple sorted runs
// of indexed tables. Implementation is thread-safe and
// uses background threads.
class DirLogger {
 public:
  DirLogger(const DirOptions& options, port::Mutex* mu, port::CondVar* cv);

  Status Open(LogSink* data, LogSink* indx);

  // Report compaction stats
  const OutputStats* output_stats() const { return &tb_->output_stats_; }
  size_t memory_usage() const;  // Report real memory usage

  // REQUIRES: mutex_ has been locked
  Status Wait();  // Wait for all on-going compactions to finish
  // May trigger a new compaction
  Status Add(const Slice& key, const Slice& value);

  // Force a compaction and maybe wait for it
  struct FlushOptions {
    explicit FlushOptions(bool ef = false, bool fi = false)
        : no_wait(true), dry_run(false), epoch_flush(ef), finalize(fi) {}

    // Do not wait for compaction to finish
    // Default: true
    bool no_wait;
    // Status checks only
    // Default: false
    bool dry_run;
    // Force a new epoch
    // Default: false
    bool epoch_flush;
    // Finalize the directory
    // Default: false
    bool finalize;
  };
  Status Flush(const FlushOptions& options);

  // Sync and pre-close log files before de-referencing them
  Status SyncAndClose();

  void Ref() { refs_++; }

  void Unref() {
    assert(refs_ > 0);
    refs_--;
    if (refs_ == 0) {
      delete this;
    }
  }

 private:
  WritableFileStats io_stats_;
  friend class DirWriterImpl;
  friend class DirWriter;
  ~DirLogger();

  Status Prepare(bool force = false, bool epoch_flush = false,
                 bool finalize = false);

  // No copying allowed
  void operator=(const DirLogger&);
  DirLogger(const DirLogger&);

  static void BGWork(void*);
  void MaybeScheduleCompaction();
  void CompactMemtable();
  void DoCompaction();

  // Constant after construction
  const DirOptions& options_;
  port::CondVar* const bg_cv_;
  port::Mutex* const mu_;
  size_t bf_bits_;
  size_t bf_bytes_;          // Target bloom filter size
  uint32_t entries_per_tb_;  // Number of entries packed per table
  size_t tb_bytes_;          // Target table size

  // State below is protected by mutex_
  uint32_t num_flush_requested_;
  uint32_t num_flush_completed_;
  bool has_bg_compaction_;
  void* filter_;  // void* since different types of filter might be used
  WriteBuffer* mem_buf_;
  WriteBuffer* imm_buf_;
  bool imm_buf_is_epoch_flush_;
  bool imm_buf_is_final_;
  WriteBuffer buf0_;
  WriteBuffer buf1_;
  TableLogger* tb_;
  LogSink* data_;
  LogSink* indx_;
  bool opened_;
  int refs_;
};

// Retrieve directory contents from a pair of indexed log files.
class Dir {
 public:
  Dir(const DirOptions& options, port::Mutex*, port::CondVar*);

  // Open a directory reader on top of a given directory index partition.
  // Return OK on success, or a non-OK status on errors.
  Status Open(LogSource* indx);

  // Obtain the value to a key from all epochs.
  // All value found will be appended to "dst"
  // Return OK on success, or a non-OK status on errors.
  Status Read(const Slice& key, std::string* dst);

  void RebindDataSource(LogSource* data);

  void Ref() { refs_++; }

  void Unref() {
    assert(refs_ > 0);
    refs_--;
    if (refs_ == 0) {
      delete this;
    }
  }

 private:
  SequentialFileStats io_stats_;
  friend class DirReaderImpl;
  friend class DirReader;
  ~Dir();

  typedef void (*Saver)(void* arg, const Slice& key, const Slice& value);

  // Obtain the value to a specific key from a given data block.
  // If key is found, "saver" will be called. Set *exhausted to true if keys
  // larger than the given one have been seen.
  // NOTE: "saver" may be called multiple times.
  // Return OK on success, or a non-OK status on errors.
  Status Fetch(const Slice& key, const BlockHandle& handle, Saver saver, void*,
               bool* exhausted);

  // Return true if the given key matches a specific filter block.
  bool KeyMayMatch(const Slice& key, const BlockHandle& handle);

  // Obtain the value to a specific key from a given table.
  // If key is found, "saver" will be called.
  // NOTE: "saver" may be called multiple times.
  // Return OK on success, or a non-OK status on errors.
  Status Fetch(const Slice& key, const TableHandle& handle, Saver saver, void*);

  // Obtain the value to a specific key within a given epoch.
  // If key is found, value is appended to *ctx->dst.
  // NOTE: a key may appear multiple times within a single epoch.
  // Store an OK status in *ctx->status on success, or a non-OK status on
  // errors.
  struct GetContext {
    Iterator* rt_iter;
    std::string* dst;
    int num_open_reads;
    std::vector<uint32_t>* offsets;  // Only used during parallel reads
    std::string* buffer;
    Status* status;
  };
  void Get(const Slice& key, uint32_t epoch, GetContext* ctx);

  Status TryGet(const Slice& key, const BlockHandle& handle, uint32_t epoch,
                GetContext* ctx);

  static void Merge(GetContext* ctx);

  struct BGItem {
    GetContext* ctx;
    uint32_t epoch;
    Slice key;
    Dir* dir;
  };
  static void BGWork(void*);

  // No copying allowed
  void operator=(const Dir&);
  Dir(const Dir&);

  struct STLLessThan;
  // Constant after construction
  const DirOptions& options_;
  uint32_t num_epoches_;
  LogSource* data_;
  LogSource* indx_;

  port::Mutex* mu_;
  port::CondVar* bg_cv_;
  Block* rt_;
  int refs_;
};

}  // namespace plfsio
}  // namespace pdlfs
