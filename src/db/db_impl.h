// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DB_IMPL_H_
#define STORAGE_LEVELDB_DB_DB_IMPL_H_

#define __STDC_LIMIT_MACROS

#include <deque>
#include <list>
#include <set>
#ifdef _LIBCPP_VERSION
#include <memory>
#else
#include <tr1/memory>
#endif
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/replay_iterator.h"
#include "db/snapshot.h"
#include "pebblesdb/db.h"
#include "pebblesdb/env.h"
#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/timer.h"
#include "db/version_set.h"

namespace leveldb {
#ifdef _LIBCPP_VERSION
#define SHARED_PTR std::shared_ptr
#else
#define SHARED_PTR std::tr1::shared_ptr
#endif

class MemTable;
class TableCache;
class Version;
class VersionEdit;
class VersionSet;

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  virtual ~DBImpl();

  // Implementations of the DB interface
  virtual Status Put(const WriteOptions&, const Slice& key, const Slice& value);
  virtual Status Delete(const WriteOptions&, const Slice& key);
  virtual Status Write(const WriteOptions& options, WriteBatch* updates);
  virtual Status Get(const ReadOptions& options,
                     const Slice& key,
                     std::string* value);
  virtual Status GetCurrentVersionState(std::string* value);
  virtual Iterator* NewIterator(const ReadOptions&);
  virtual void GetReplayTimestamp(std::string* timestamp);
  virtual void AllowGarbageCollectBeforeTimestamp(const std::string& timestamp);
  virtual bool ValidateTimestamp(const std::string& timestamp);
  virtual int CompareTimestamps(const std::string& lhs, const std::string& rhs);
  virtual Status GetReplayIterator(const std::string& timestamp,
                                   ReplayIterator** iter);
  virtual void ReleaseReplayIterator(ReplayIterator* iter);
  virtual const Snapshot* GetSnapshot();
  virtual void ReleaseSnapshot(const Snapshot* snapshot);
  virtual bool GetProperty(const Slice& property, std::string* value);
  virtual void GetApproximateSizes(const Range* range, int n, uint64_t* sizes);
  virtual void CompactRange(const Slice* begin, const Slice* end);
  virtual Status LiveBackup(const Slice& name);
  virtual void PrintTimerAudit();
  virtual void ClearTimer();

  void TEST_CompactAllLevels();

  void TEST_CompactOnce();
  // Extra methods (for testing) that are not in the public DB interface

  // Compact any files in the named level that overlap [*begin,*end]
  void TEST_CompactRange(unsigned level, const Slice* begin, const Slice* end);

  // Force current memtable contents to be compacted.
  Status TEST_CompactMemTable();

  // Return an internal iterator over the current state of the database.
  // The keys of this iterator are internal keys (see format.h).
  // The returned iterator should be deleted when no longer needed.
  Iterator* TEST_NewInternalIterator();

  // Return the maximum overlapping data (in bytes) at next level for any
  // file at a level >= 1.
  int64_t TEST_MaxNextLevelOverlappingBytes();

  // Hacky function to forcefully fit all files to a single level inorder to do a comparison for seek benchmark
  void TEST_ComapactFilesToSingleLevel();

  // Hacky function to forcefully fit all files to a single level inorder to do a comparison for seek benchmark
  void TEST_ReduceNumActiveLevelsByOne();

  // Record a sample of bytes read at the specified internal key.
  // Samples are taken approximately once every config::kReadBytesPeriod
  // bytes.
  void RecordReadSample(Slice key);

  // Peek at the last sequence;
  // REQURES: mutex_ not held
  SequenceNumber LastSequence();

 private:
  friend class DB;
  struct CompactionState;
  struct Writer;

  Iterator* NewInternalIterator(const ReadOptions&, uint64_t number,
                                SequenceNumber* latest_snapshot,
                                uint32_t* seed, bool external_sync);

  Status NewDB();

  // Recover the descriptor from persistent storage.  May do a significant
  // amount of work to recover recently logged updates.  Any changes to
  // be made to the descriptor are added to *edit.
  Status Recover(VersionEdit* edit) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  void MaybeIgnoreError(Status* s) const;

  // Delete any unneeded files and stale in-memory entries.
  void DeleteObsoleteFiles();
  
  // A background thread to compact the in-memory write buffer to disk.
  // Switches to a new log-file/memtable and writes a new descriptor iff
  // successful.
  static void CompactMemTableWrapper(void* db)
  { reinterpret_cast<DBImpl*>(db)->CompactMemTableThread(); }
  void CompactMemTableThread();

  Status RecoverLogFile(uint64_t log_number,
                        VersionEdit* edit,
                        SequenceNumber* max_sequence)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base, uint64_t* number)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  Status WriteLevel0TableGuards(MemTable* mem, VersionEdit* edit, Version* base,
		  std::vector<uint64_t> &numbers, FileLevelFilterBuilder* file_level_filter_builder,
		  std::vector<std::string*>* file_level_filters)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  Status SequenceWriteBegin(Writer* w, WriteBatch* updates)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void SequenceWriteEnd(Writer* w, WriteBatch* updates_with_guards, Status status)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  // REQUIRES: writers_mutex_ not held
  void WaitOutWriters();

  static void CompactLevelWrapper(void* db)
  { reinterpret_cast<DBImpl*>(db)->CompactLevelThread(); }
  void CompactLevelThread();
  Status BackgroundCompaction() EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  Status BackgroundCompactionGuards(FileLevelFilterBuilder* file_level_filter_builder) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  void RecordBackgroundError(const Status& s);

  void CleanupCompaction(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status DoCompactionWork(CompactionState* compact)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status DoCompactionWorkForGuardsInALevel(CompactionState* compact)
  	  EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status DoCompactionWorkGuards(CompactionState* compact,
		  std::vector<GuardMetaData*> complete_guards_used_in_bg_compaction,
		  FileLevelFilterBuilder* file_level_filter_builder)
  	  EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input, FileLevelFilterBuilder* file_level_filter_builder,
		  std::vector<uint64_t>* file_numbers, std::vector<std::string*>* file_level_filters);
  Status InstallCompactionResults(CompactionState* compact, const int level_to_add_new_files, std::vector<uint64_t> file_numbers, std::vector<std::string*> file_level_filters)
      EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Constant after construction
  Env* const env_;
  const InternalKeyComparator internal_comparator_;
  const InternalFilterPolicy internal_filter_policy_;
  const Options options_;  // options_.comparator == &internal_comparator_
  const FileOptions file_options_;
  const FilterPolicy* filter_policy_;
  bool owns_info_log_;
  bool owns_cache_;
  const std::string dbname_;

  // table_cache_ provides its own synchronization
  TableCache* table_cache_;

  // Lock over the persistent DB state.  Non-NULL iff successfully acquired.
  FileLock* db_lock_;

  // State below is protected by mutex_
  port::Mutex mutex_;
  port::AtomicPointer shutting_down_;
  MemTable* mem_;
  MemTable* imm_;                // Memtable being compacted
  port::AtomicPointer has_imm_;  // So bg thread can detect non-NULL imm_
  SHARED_PTR<WritableFile> logfile_;
  uint64_t logfile_number_;
  SHARED_PTR<log::Writer> log_;
  uint32_t seed_;                // For sampling.

  // Synchronize writers
  port::Mutex writers_mutex_;
  uint64_t writers_upper_;
  Writer* writers_tail_;

  SnapshotList snapshots_;

  // Set of table files to protect from deletion because they are
  // part of ongoing compactions.
  std::set<uint64_t> pending_outputs_;

  bool allow_background_activity_;
  bool levels_locked_[leveldb::config::kNumLevels];
  int num_bg_threads_;
  // Tell the foreground that background has done something of note
  port::CondVar bg_fg_cv_;
  // Communicate with compaction background thread
  port::CondVar bg_compaction_cv_;
  // Communicate with memtable->L0 background thread
  port::CondVar bg_memtable_cv_;
  // Mutual exlusion protecting the LogAndApply func
  port::CondVar bg_log_cv_;
  bool bg_log_occupied_;

  // Information for a manual compaction
  struct ManualCompaction {
    ManualCompaction()
      : level(),
        done(),
        begin(),
        end(),
        tmp_storage() {
    }
    unsigned level;
    bool done;
    const InternalKey* begin;   // NULL means beginning of key range
    const InternalKey* end;     // NULL means end of key range
    InternalKey tmp_storage;    // Used to keep track of compaction progress
   private:
    ManualCompaction(const ManualCompaction&);
    ManualCompaction& operator = (const ManualCompaction&);
  };
  ManualCompaction* manual_compaction_;

  SequenceNumber manual_garbage_cutoff_;

  // replay iterators
  std::list<ReplayIteratorImpl*> replay_iters_;

  // how many reads have we done in a row, uninterrupted by writes
  uint64_t straight_reads_;

  uint64_t guard_array_[config::kNumLevels];
  
  VersionSet* versions_;

  Timer* timer;

  int num_bg_compaction_threads_;

  // Information for ongoing backup processes
  port::CondVar backup_cv_;
  port::AtomicPointer backup_in_progress_; // non-NULL in progress
  uint64_t backup_waiters_; // how many threads waiting to backup
  bool backup_waiter_has_it_;
  bool backup_deferred_delete_; // DeleteObsoleteFiles delayed by backup; protect with mutex_

  // Have we encountered a background error in paranoid mode?
  Status bg_error_;

  // Per level compaction stats.  stats_[level] stores the stats for
  // compactions that produced data for the specified "level".
  struct CompactionStats {
    int64_t micros;
    int64_t bytes_read;
    int64_t bytes_written;

    CompactionStats() : micros(0), bytes_read(0), bytes_written(0) { }

    void Add(const CompactionStats& c) {
      this->micros += c.micros;
      this->bytes_read += c.bytes_read;
      this->bytes_written += c.bytes_written;
    }
  };
  CompactionStats stats_[config::kNumLevels];

  // No copying allowed
  DBImpl(const DBImpl&);
  void operator=(const DBImpl&);

  const Comparator* user_comparator() const {
    return internal_comparator_.user_comparator();
  }
};

// Sanitize db options.  The caller should delete result.info_log if
// it is not equal to src.info_log.
extern Options SanitizeOptions(const std::string& db,
                               const InternalKeyComparator* icmp,
                               const InternalFilterPolicy* ipolicy,
                               const Options& src);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DB_IMPL_H_
