// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#define __STDC_LIMIT_MACROS

#include "pebblesdb/db.h"
#include "pebblesdb/filter_policy.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "pebblesdb/cache.h"
#include "pebblesdb/env.h"
#include "pebblesdb/table.h"
#include "util/hash.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "db/murmurhash3.h"

namespace leveldb {

static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}

namespace {
class AtomicCounter {
 private:
  port::Mutex mu_;
  int count_;
 public:
  AtomicCounter() : count_(0) { }
  void Increment() {
    IncrementBy(1);
  }
  void IncrementBy(int count) {
    MutexLock l(&mu_);
    count_ += count;
  }
  int Read() {
    MutexLock l(&mu_);
    return count_;
  }
  void Reset() {
    MutexLock l(&mu_);
    count_ = 0;
  }
};

void DelayMilliseconds(int millis) {
  Env::Default()->SleepForMicroseconds(millis * 1000);
}
}

// Special Env used to delay background operations
class SpecialEnv : public EnvWrapper {
 public:

  pthread_t StartThreadAndReturnThreadId(void (*f)(void*), void* a) {
	  return 0;
  }

  void WaitForThread(unsigned long int th, void** return_status) {
  }

  pthread_t GetThreadId() {
	  return 0;
  }

  // sstable/log Sync() calls are blocked while this pointer is non-NULL.
  port::AtomicPointer delay_data_sync_;

  // sstable/log Sync() calls return an error.
  port::AtomicPointer data_sync_error_;

  // Simulate no-space errors while this pointer is non-NULL.
  port::AtomicPointer no_space_;

  // Simulate non-writable file system while this pointer is non-NULL
  port::AtomicPointer non_writable_;

  // Force sync of manifest files to fail while this pointer is non-NULL
  port::AtomicPointer manifest_sync_error_;

  // Force write to manifest files to fail while this pointer is non-NULL
  port::AtomicPointer manifest_write_error_;

  bool count_random_reads_;
  AtomicCounter random_read_counter_;

  explicit SpecialEnv(Env* base) : EnvWrapper(base) {
    delay_data_sync_.Release_Store(NULL);
    data_sync_error_.Release_Store(NULL);
    no_space_.Release_Store(NULL);
    non_writable_.Release_Store(NULL);
    count_random_reads_ = false;
    manifest_sync_error_.Release_Store(NULL);
    manifest_write_error_.Release_Store(NULL);
  }

  Status NewWritableFile(const std::string& f, WritableFile** r) {
    ConcurrentWritableFile* _r;
    Status s = this->NewConcurrentWritableFile(f, &_r);
    *r = _r;
    return s;
  }

  Status NewConcurrentWritableFile(const std::string& f, ConcurrentWritableFile** r) {
    class DataFile : public ConcurrentWritableFile {
     private:
      SpecialEnv* env_;
      ConcurrentWritableFile* base_;

     public:
      DataFile(SpecialEnv* env, ConcurrentWritableFile* base)
          : env_(env),
            base_(base) {
      }
      ~DataFile() { delete base_; }
      Status WriteAt(uint64_t offset, const Slice& data) {
        if (env_->no_space_.Acquire_Load() != NULL) {
          // Drop writes on the floor
          return Status::OK();
        } else {
          return base_->WriteAt(offset, data);
        }
      }
      Status Append(const Slice& data) {
        if (env_->no_space_.Acquire_Load() != NULL) {
          // Drop writes on the floor
          return Status::OK();
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() {
        if (env_->data_sync_error_.Acquire_Load() != NULL) {
          return Status::IOError("simulated data sync error");
        }
        while (env_->delay_data_sync_.Acquire_Load() != NULL) {
          DelayMilliseconds(100);
        }
        return base_->Sync();
      }
    };
    class ManifestFile : public ConcurrentWritableFile {
     private:
      SpecialEnv* env_;
      ConcurrentWritableFile* base_;
     public:
      ManifestFile(SpecialEnv* env, ConcurrentWritableFile* b) : env_(env), base_(b) { }
      ~ManifestFile() { delete base_; }
      Status WriteAt(uint64_t offset, const Slice& data) {
        if (env_->manifest_write_error_.Acquire_Load() != NULL) {
          return Status::IOError("simulated writer error");
        } else {
          return base_->WriteAt(offset, data);
        }
      }
      Status Append(const Slice& data) {
        if (env_->manifest_write_error_.Acquire_Load() != NULL) {
          return Status::IOError("simulated writer error");
        } else {
          return base_->Append(data);
        }
      }
      Status Close() { return base_->Close(); }
      Status Flush() { return base_->Flush(); }
      Status Sync() {
        if (env_->manifest_sync_error_.Acquire_Load() != NULL) {
          return Status::IOError("simulated sync error");
        } else {
          return base_->Sync();
        }
      }
    };

    if (non_writable_.Acquire_Load() != NULL) {
      return Status::IOError("simulated write error");
    }

    Status s = target()->NewConcurrentWritableFile(f, r);
    if (s.ok()) {
      if (strstr(f.c_str(), ".ldb") != NULL ||
          strstr(f.c_str(), ".log") != NULL) {
        *r = new DataFile(this, *r);
      } else if (strstr(f.c_str(), "MANIFEST") != NULL) {
        *r = new ManifestFile(this, *r);
      }
    }
    return s;
  }

  Status NewRandomAccessFile(const std::string& f, const FileOptions& o, RandomAccessFile** r) {
    class CountingFile : public RandomAccessFile {
     private:
      RandomAccessFile* target_;
      AtomicCounter* counter_;
     public:
      CountingFile(RandomAccessFile* target, AtomicCounter* counter)
          : target_(target), counter_(counter) {
      }
      virtual ~CountingFile() { delete target_; }
      virtual Status Read(uint64_t offset, size_t n, Slice* result,
                          char* scratch) const {
        counter_->Increment();
        return target_->Read(offset, n, result, scratch);
      }
    };

    Status s = target()->NewRandomAccessFile(f, o, r);
    if (s.ok() && count_random_reads_) {
      *r = new CountingFile(*r, &random_read_counter_);
    }
    return s;
  }
};

class DBTest {
 private:
  const FilterPolicy* filter_policy_;

  // Sequence of option configurations to try
  enum OptionConfig {
    kDefault,
    kFilter,
    kUncompressed,
    kEnd
  };
  int option_config_;

 public:
  std::string dbname_;
  SpecialEnv* env_;
  DB* db_;

  Options last_options_;

  DBTest() : option_config_(kDefault),
             env_(new SpecialEnv(Env::Default())) {
    filter_policy_ = NewBloomFilterPolicy(10);
    dbname_ = test::TmpDir() + "/db_test";
    DestroyDB(dbname_, Options());
    db_ = NULL;
    Reopen();
  }

  ~DBTest() {
    delete db_;
    DestroyDB(dbname_, Options());
    delete env_;
    delete filter_policy_;
  }

  // Switch to a fresh database with the next option configuration to
  // test.  Return false if there are no more configurations to test.
  bool ChangeOptions() {
    option_config_++;
    if (option_config_ >= kEnd) {
      return false;
    } else {
      DestroyAndReopen();
      return true;
    }
  }

  // Return the current option configuration.
  Options CurrentOptions() {
    Options options;
    switch (option_config_) {
      case kFilter:
        options.filter_policy = filter_policy_;
        break;
      case kUncompressed:
        options.compression = kNoCompression;
        break;
      default:
        break;
    }
    return options;
  }

  DBImpl* dbfull() {
    return reinterpret_cast<DBImpl*>(db_);
  }

  void Reopen(Options* options = NULL) {
    ASSERT_OK(TryReopen(options));
  }

  void Close() {
    delete db_;
    db_ = NULL;
  }

  void DestroyAndReopen(Options* options = NULL) {
    delete db_;
    db_ = NULL;
    DestroyDB(dbname_, Options());
    ASSERT_OK(TryReopen(options));
  }

  Status TryReopen(Options* options) {
	if (db_ != NULL) {
		delete db_;
	}
    db_ = NULL;
    Options opts;
    if (options != NULL) {
      opts = *options;
    } else {
      opts = CurrentOptions();
      opts.create_if_missing = true;
    }
    last_options_ = opts;

    return DB::Open(opts, dbname_, &db_);
  }

  Status Put(const std::string& k, const std::string& v) {
    return db_->Put(WriteOptions(), k, v);
  }

  Status Delete(const std::string& k) {
    return db_->Delete(WriteOptions(), k);
  }

  std::string Get(const std::string& k, const Snapshot* snapshot = NULL) {
    ReadOptions options;
    options.snapshot = snapshot;
    std::string result;
    Status s = db_->Get(options, k, &result);
    if (s.IsNotFound()) {
      result = "NOT_FOUND";
    } else if (!s.ok()) {
      result = s.ToString();
    }
    return result;
  }

  // Return a string that contains all key,value pairs in order,
  // formatted like "(k1->v1)(k2->v2)".
  std::string Contents() {
    std::vector<std::string> forward;
    std::string result;
    Iterator* iter = db_->NewIterator(ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      std::string s = IterStatus(iter);
      result.push_back('(');
      result.append(s);
      result.push_back(')');
      forward.push_back(s);
    }

    // Check reverse iteration results are the reverse of forward results
    size_t matched = 0;
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      ASSERT_LT(matched, forward.size());
      ASSERT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
      matched++;
    }
    ASSERT_EQ(matched, forward.size());

    delete iter;
    return result;
  }

  // Iterate through the DB in both forward and backward direction and verify their consistency
  // Returns the number of entries in the DB.
  int VerifyIteration(int print_every = 100000000) {
	int count = 0;
    std::vector<std::string> forward;
    std::string result;
    Iterator* iter = db_->NewIterator(ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      std::string s = IterStatus(iter);
      result.push_back('(');
      result.append(s);
      result.push_back(')');
      forward.push_back(s);
      count++;
    }

    // Check reverse iteration results are the reverse of forward results
    size_t matched = 0;
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
      ASSERT_LT(matched, forward.size());
      ASSERT_EQ(IterStatus(iter), forward[forward.size() - matched - 1]);
      matched++;
    }
    ASSERT_EQ(matched, forward.size());

    delete iter;
    return forward.size();
  }

  std::string AllEntriesFor(const Slice& user_key) {
    Iterator* iter = dbfull()->TEST_NewInternalIterator();
    InternalKey target(user_key, kMaxSequenceNumber, kTypeValue);
    iter->Seek(target.Encode());
    std::string result;
    if (!iter->status().ok()) {
      result = iter->status().ToString();
    } else {
      result = "[ ";
      bool first = true;
      while (iter->Valid()) {
        ParsedInternalKey ikey;
        if (!ParseInternalKey(iter->key(), &ikey)) {
          result += "CORRUPTED";
        } else {
          if (last_options_.comparator->Compare(ikey.user_key, user_key) != 0) {
            break;
          }
          if (!first) {
            result += ", ";
          }
          first = false;
          switch (ikey.type) {
            case kTypeValue:
              result += iter->value().ToString();
              break;
            case kTypeDeletion:
              result += "DEL";
              break;
          }
        }
        iter->Next();
      }
      if (!first) {
        result += " ";
      }
      result += "]";
    }
    delete iter;
    return result;
  }

  int NumTableFilesAtLevel(int level) {
    std::string property;
    ASSERT_TRUE(
        db_->GetProperty("leveldb.num-files-at-level" + NumberToString(level),
                         &property));
    return atoi(property.c_str());
  }

  int TotalTableFiles() {
    int result = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      result += NumTableFilesAtLevel(level);
    }
    return result;
  }

  /* return number of guard files. */
  int NumGuardsAtLevel(int level) {
    std::string property;
    ASSERT_TRUE(
        db_->GetProperty("leveldb.num-guards-at-level" + NumberToString(level),
                         &property));
    return atoi(property.c_str());
  }

  // Return the number of files belonging to any guard for a given level
  int NumGuardFilesAtLevel(int level) {
	  std::string property;
	  ASSERT_TRUE(db_->GetProperty("leveldb.num-guard-files-at-level" + NumberToString(level),
			  &property));
	  return atoi(property.c_str());
  }

  // Return the number of sentinel files in a given level
  int NumSentinelFilesAtLevel(int level) {
	  std::string property;
	  ASSERT_TRUE(db_->GetProperty("leveldb.num-sentinel-files-at-level" + NumberToString(level),
			  &property));
	  return atoi(property.c_str());
  }

  std::string GuardDetailsAtLevel(int level) {
	  std::string property;
	  db_->GetProperty("leveldb.guard-details-at-level" + NumberToString(level), &property);
	  return property;
  }

  std::string SentinelDetailsAtLevel(int level) {
	  std::string property;
	  db_->GetProperty("leveldb.sentinel-details-at-level" + NumberToString(level), &property);
	  return property;
  }

  int TotalGuards() {
    int result = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      result += NumGuardsAtLevel(level);
    }
    return result;
  }
  
  std::string GuardDetails() {
	  std::string guard_details;
	  guard_details.clear();
	  for (unsigned level = 0; level < config::kNumLevels; level++) {
		  guard_details.append(GuardDetailsAtLevel(level));
	  }
	  return guard_details;
  }

  int NumGuardFiles() {
	  int num_guard_files = 0;
	  for (unsigned level = 0; level < config::kNumLevels; level++) {
		  num_guard_files += NumGuardFilesAtLevel(level);
	  }
	  return num_guard_files;
  }

  int NumSentinelFiles() {
	  int num_sentinel_files = 0;
	  for (unsigned level = 0; level < config::kNumLevels; level++) {
		  num_sentinel_files += NumSentinelFilesAtLevel(level);
	  }
	  return num_sentinel_files;
  }

  void print_file_counts() {
  	  printf("------------File counts--------------\n");
  	  printf("Total Table files     : %d\n", TotalTableFiles());
  	  printf("Count Files           : %d\n", CountFiles());
  	  printf("Sentinel files        : %d\n", NumSentinelFiles());
  	  printf("Guard files           : %d\n", NumGuardFiles());
  	  printf("Sentinel + guard files: %d\n", NumSentinelFiles() + NumGuardFiles());
  	  printf("--------------------------------------\n");
  }

  void print_current_db_contents() {
	  std::string current_db_state;
	  printf("----------------------Current DB state-----------------------\n");
	  db_->GetCurrentVersionState(&current_db_state);
	  printf("%s\n", current_db_state.c_str());
	  printf("-------------------------------------------------------------\n");
  }

  std::string SentinelDetails() {
	  std::string sentinel_details;
	  sentinel_details.clear();
	  for (unsigned level = 0; level < config::kNumLevels; level++) {
		  sentinel_details.append(SentinelDetailsAtLevel(level));
	  }
	  return sentinel_details;
  }

  // Return spread of files per level
  std::string FilesPerLevel() {
    std::string result;
    int last_non_zero_offset = 0;
    for (int level = 0; level < config::kNumLevels; level++) {
      int f = NumTableFilesAtLevel(level);
      char buf[100];
      snprintf(buf, sizeof(buf), "%s%d", (level ? "," : ""), f);
      result += buf;
      if (f > 0) {
        last_non_zero_offset = result.size();
      }
    }
    result.resize(last_non_zero_offset);
    return result;
  }

  int CountFiles() {
    std::vector<std::string> files;
    env_->GetChildren(dbname_, &files);
    return static_cast<int>(files.size());
  }

  uint64_t Size(const Slice& start, const Slice& limit) {
    Range r(start, limit);
    uint64_t size;
    db_->GetApproximateSizes(&r, 1, &size);
    return size;
  }

  void Compact(const Slice& start, const Slice& limit) {
    db_->CompactRange(&start, &limit);
  }

  // Do n memtable compactions, each of which produces an sstable
  // covering the range [small,large].
  void MakeTables(int n, const std::string& small, const std::string& large) {
    for (int i = 0; i < n; i++) {
      Put(small, "begin");
      Put(large, "end");
      dbfull()->TEST_CompactMemTable();
    }
  }

  // Prevent pushing of new sstables into deeper levels by adding
  // tables that cover a specified range to all levels.
  void FillLevels(const std::string& smallest, const std::string& largest) {
    MakeTables(config::kNumLevels, smallest, largest);
  }

  void DumpFileCounts(const char* label) {
    fprintf(stderr, "---\n%s:\n", label);
    fprintf(stderr, "maxoverlap: %lld\n",
            static_cast<long long>(
                dbfull()->TEST_MaxNextLevelOverlappingBytes()));
    for (int level = 0; level < config::kNumLevels; level++) {
      int num = NumTableFilesAtLevel(level);
      if (num > 0) {
        fprintf(stderr, "  level %3d : %d files\n", level, num);
      }
    }
  }

  std::string DumpSSTableList() {
    std::string property;
    db_->GetProperty("leveldb.sstables", &property);
    return property;
  }

  std::string IterStatus(Iterator* iter) {
    std::string result;
    if (iter->Valid()) {
      result = iter->key().ToString() + "->" + iter->value().ToString();
    } else {
      result = "(invalid)";
    }
    return result;
  }

  bool DeleteAnSSTFile() {
    std::vector<std::string> filenames;
    ASSERT_OK(env_->GetChildren(dbname_, &filenames));
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) && type == kTableFile) {
        ASSERT_OK(env_->DeleteFile(TableFileName(dbname_, number)));
        return true;
      }
    }
    return false;
  }

  // Returns number of files renamed.
  int RenameSSTToLDB() {
    std::vector<std::string> filenames;
    ASSERT_OK(env_->GetChildren(dbname_, &filenames));
    uint64_t number;
    FileType type;
    int files_renamed = 0;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) && type == kTableFile) {
        const std::string from = TableFileName(dbname_, number);
        const std::string to = LDBTableFileName(dbname_, number);
        ASSERT_OK(env_->RenameFile(from, to));
        files_renamed++;
      }
    }
    return files_renamed;
  }
};

TEST(DBTest, Empty) {
  do {
    ASSERT_TRUE(db_ != NULL);
    ASSERT_EQ("NOT_FOUND", Get("foo"));
  } while (ChangeOptions());
}

TEST(DBTest, ReadWrite) {
  do {
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_OK(Put("bar", "v2"));
    ASSERT_OK(Put("foo", "v3"));
    ASSERT_EQ("v3", Get("foo"));
    ASSERT_EQ("v2", Get("bar"));
  } while (ChangeOptions());
}

TEST(DBTest, PutDeleteGet) {
  do {
    ASSERT_OK(db_->Put(WriteOptions(), "foo", "v1"));
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_OK(db_->Put(WriteOptions(), "foo", "v2"));
    ASSERT_EQ("v2", Get("foo"));
    ASSERT_OK(db_->Delete(WriteOptions(), "foo"));
    ASSERT_EQ("NOT_FOUND", Get("foo"));
  } while (ChangeOptions());
}

TEST(DBTest, GetFromImmutableLayer) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    options.write_buffer_size = 100000;  // Small write buffer
    Reopen(&options);

    ASSERT_OK(Put("foo", "v1"));
    ASSERT_EQ("v1", Get("foo"));

    env_->delay_data_sync_.Release_Store(env_);      // Block sync calls
    Put("k1", std::string(100000, 'x'));             // Fill memtable
    Put("k2", std::string(100000, 'y'));             // Trigger compaction
    ASSERT_EQ("v1", Get("foo"));
    env_->delay_data_sync_.Release_Store(NULL);      // Release sync calls
  } while (ChangeOptions());
}

TEST(DBTest, GetFromVersions) {
  do {
    ASSERT_OK(Put("foo", "v1"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("v1", Get("foo"));
  } while (ChangeOptions());
}

TEST(DBTest, GetSnapshot) {
  do {
    // Try with both a short key and a long key
    for (int i = 0; i < 2; i++) {
      std::string key = (i == 0) ? std::string("foo") : std::string(200, 'x');
      ASSERT_OK(Put(key, "v1"));
      const Snapshot* s1 = db_->GetSnapshot();
      ASSERT_OK(Put(key, "v2"));
      ASSERT_EQ("v2", Get(key));
      ASSERT_EQ("v1", Get(key, s1));
      dbfull()->TEST_CompactMemTable();
      ASSERT_EQ("v2", Get(key));
      ASSERT_EQ("v1", Get(key, s1));
      db_->ReleaseSnapshot(s1);
    }
  } while (ChangeOptions());
}

TEST(DBTest, GetLevel0Ordering) {
  do {
    // Check that we process level-0 files in correct order.  The code
    // below generates two level-0 files where the earlier one comes
    // before the later one in the level-0 file list since the earlier
    // one has a smaller "smallest" key.
    ASSERT_OK(Put("bar", "b"));
    ASSERT_OK(Put("foo", "v1"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_OK(Put("foo", "v2"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("v2", Get("foo"));
  } while (ChangeOptions());
}

TEST(DBTest, GetOrderedByLevels) {
  do {
    ASSERT_OK(Put("foo", "v1"));
    Compact("a", "z");
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_OK(Put("foo", "v2"));
    ASSERT_EQ("v2", Get("foo"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("v2", Get("foo"));
  } while (ChangeOptions());
}

TEST(DBTest, GetPicksCorrectFile) {
  do {
    // Arrange to have multiple files in a non-level-0 level.
    ASSERT_OK(Put("a", "va"));
    Compact("a", "b");
    ASSERT_OK(Put("x", "vx"));
    Compact("x", "y");
    ASSERT_OK(Put("f", "vf"));
    Compact("f", "g");
    ASSERT_EQ("va", Get("a"));
    ASSERT_EQ("vf", Get("f"));
    ASSERT_EQ("vx", Get("x"));
  } while (ChangeOptions());
}

TEST(DBTest, IterEmpty) {
  Iterator* iter = db_->NewIterator(ReadOptions());

  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->Seek("foo");
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  delete iter;
}

TEST(DBTest, IterSingle) {
  ASSERT_OK(Put("a", "va"));
  Iterator* iter = db_->NewIterator(ReadOptions());

  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");
  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");
  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->Seek("");
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->Seek("a");
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->Seek("b");
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  delete iter;
}

TEST(DBTest, IterMulti) {
  ASSERT_OK(Put("a", "va"));
  ASSERT_OK(Put("b", "vb"));
  ASSERT_OK(Put("c", "vc"));
  Iterator* iter = db_->NewIterator(ReadOptions());

  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "b->vb");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");
  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "b->vb");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "(invalid)");
  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->Seek("");
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Seek("a");
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Seek("ax");
  ASSERT_EQ(IterStatus(iter), "b->vb");
  iter->Seek("b");
  ASSERT_EQ(IterStatus(iter), "b->vb");
  iter->Seek("z");
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  // Switch from reverse to forward
  iter->SeekToLast();
  iter->Prev();
  iter->Prev();
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "b->vb");

  // Switch from forward to reverse
  iter->SeekToFirst();
  iter->Next();
  iter->Next();
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "b->vb");

  // Make sure iter stays at snapshot
  ASSERT_OK(Put("a",  "va2"));
  ASSERT_OK(Put("a2", "va3"));
  ASSERT_OK(Put("b",  "vb2"));
  ASSERT_OK(Put("c",  "vc2"));
  ASSERT_OK(Delete("b"));
  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "b->vb");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");
  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "b->vb");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  delete iter;
}

TEST(DBTest, IterSmallAndLargeMix) {
  ASSERT_OK(Put("a", "va"));
  ASSERT_OK(Put("b", std::string(100000, 'b')));
  ASSERT_OK(Put("c", "vc"));
  ASSERT_OK(Put("d", std::string(100000, 'd')));
  ASSERT_OK(Put("e", std::string(100000, 'e')));

  Iterator* iter = db_->NewIterator(ReadOptions());

  iter->SeekToFirst();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "b->" + std::string(100000, 'b'));
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "d->" + std::string(100000, 'd'));
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "e->" + std::string(100000, 'e'));
  iter->Next();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  iter->SeekToLast();
  ASSERT_EQ(IterStatus(iter), "e->" + std::string(100000, 'e'));
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "d->" + std::string(100000, 'd'));
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "c->vc");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "b->" + std::string(100000, 'b'));
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "a->va");
  iter->Prev();
  ASSERT_EQ(IterStatus(iter), "(invalid)");

  delete iter;
}

TEST(DBTest, IterMultiWithDelete) {
  do {
    ASSERT_OK(Put("a", "va"));
    ASSERT_OK(Put("b", "vb"));
    ASSERT_OK(Put("c", "vc"));
    ASSERT_OK(Delete("b"));
    ASSERT_EQ("NOT_FOUND", Get("b"));

    Iterator* iter = db_->NewIterator(ReadOptions());
    iter->Seek("c");
    ASSERT_EQ(IterStatus(iter), "c->vc");
    iter->Prev();
    ASSERT_EQ(IterStatus(iter), "a->va");
    delete iter;
  } while (ChangeOptions());
}

TEST(DBTest, Recover) {
  do {
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_OK(Put("baz", "v5"));

    Reopen();
    ASSERT_EQ("v1", Get("foo"));

    ASSERT_EQ("v1", Get("foo"));
    ASSERT_EQ("v5", Get("baz"));
    ASSERT_OK(Put("bar", "v2"));
    ASSERT_OK(Put("foo", "v3"));

    Reopen();
    ASSERT_EQ("v3", Get("foo"));
    ASSERT_OK(Put("foo", "v4"));
    ASSERT_EQ("v4", Get("foo"));
    ASSERT_EQ("v2", Get("bar"));
    ASSERT_EQ("v5", Get("baz"));
  } while (ChangeOptions());
}

TEST(DBTest, RecoveryWithEmptyLog) {
  do {
    ASSERT_OK(Put("foo", "v1"));
    ASSERT_OK(Put("foo", "v2"));
    Reopen();
    Reopen();
    ASSERT_OK(Put("foo", "v3"));
    Reopen();
    ASSERT_EQ("v3", Get("foo"));
  } while (ChangeOptions());
}

// Check that writes done during a memtable compaction are recovered
// if the database is shutdown during the memtable compaction.
TEST(DBTest, RecoverDuringMemtableCompaction) {
  do {
    Options options = CurrentOptions();
    options.env = env_;
    options.write_buffer_size = 1000000;
    Reopen(&options);

    // Trigger a long memtable compaction and reopen the database during it
    ASSERT_OK(Put("foo", "v1"));                         // Goes to 1st log file
    ASSERT_OK(Put("big1", std::string(10000000, 'x')));  // Fills memtable
    ASSERT_OK(Put("big2", std::string(1000, 'y')));      // Triggers compaction
    ASSERT_OK(Put("bar", "v2"));                         // Goes to new log file

    Reopen(&options);
    ASSERT_EQ("v1", Get("foo"));
    ASSERT_EQ("v2", Get("bar"));
    ASSERT_EQ(std::string(10000000, 'x'), Get("big1"));
    ASSERT_EQ(std::string(1000, 'y'), Get("big2"));
  } while (ChangeOptions());
}

static std::string Key(int i) {
  char buf[100];
  snprintf(buf, sizeof(buf), "key%06d", i);
  return std::string(buf);
}

TEST(DBTest, MinorCompactionsHappen) {
  Options options = CurrentOptions();
  options.write_buffer_size = 10000;
  Reopen(&options);

  const int N = 500;

  int starting_num_tables = TotalTableFiles();
  for (int i = 0; i < N; i++) {
    ASSERT_OK(Put(Key(i), Key(i) + std::string(1000, 'v')));
  }
  int ending_num_tables = TotalTableFiles();
  ASSERT_GT(ending_num_tables, starting_num_tables);

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(Key(i) + std::string(1000, 'v'), Get(Key(i)));
  }

  Reopen();

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(Key(i) + std::string(1000, 'v'), Get(Key(i)));
  }
}

TEST(DBTest, RecoverWithLargeLog) {
  {
    Options options = CurrentOptions();
    Reopen(&options);
    ASSERT_OK(Put("big1", std::string(200000, '1')));
    ASSERT_OK(Put("big2", std::string(200000, '2')));
    ASSERT_OK(Put("small3", std::string(10, '3')));
    ASSERT_OK(Put("small4", std::string(10, '4')));
    ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  }

  // Make sure that if we re-open with a small write buffer size that
  // we flush table files in the middle of a large log file.
  Options options = CurrentOptions();
  options.write_buffer_size = 100000;
  Reopen(&options);
  ASSERT_EQ(NumTableFilesAtLevel(0), 3);
  ASSERT_EQ(std::string(200000, '1'), Get("big1"));
  ASSERT_EQ(std::string(200000, '2'), Get("big2"));
  ASSERT_EQ(std::string(10, '3'), Get("small3"));
  ASSERT_EQ(std::string(10, '4'), Get("small4"));
  ASSERT_GT(NumTableFilesAtLevel(0), 1);
}

TEST(DBTest, CompactionsGenerateMultipleFiles) {
  Options options = CurrentOptions();
  options.write_buffer_size = 100000000;        // Large write buffer
  Reopen(&options);

  Random rnd(301);

  // Write 96MB (960 values, each 100K)
  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  std::vector<std::string> values;
  for (int i = 0; i < 960; i++) {
    values.push_back(RandomString(&rnd, 100000));
    ASSERT_OK(Put(Key(i), values[i]));
  }

  // Reopening moves updates to level-0
  Reopen(&options);
  dbfull()->TEST_CompactRange(0, NULL, NULL);

  ASSERT_EQ(NumTableFilesAtLevel(0), 0);
  ASSERT_GT(NumTableFilesAtLevel(1), 1);
  for (int i = 0; i < 960; i++) {
    ASSERT_EQ(Get(Key(i)), values[i]);
  }
}

// In HyperLevelDB, this test is useless because we have no "max files" cap.
#if 0
TEST(DBTest, RepeatedWritesToSameKey) {
  Options options = CurrentOptions();
  options.env = env_;
  options.write_buffer_size = 100000;  // Small write buffer
  Reopen(&options);

  // We must have at most one file per level except for level-0,
  // which may have up to kL0_StopWritesTrigger files.
  const int kMaxFiles = config::kNumLevels + config::kL0_StopWritesTrigger;

  Random rnd(301);
  std::string value = RandomString(&rnd, 2 * options.write_buffer_size);
  for (int i = 0; i < 5 * kMaxFiles; i++) {
    Put("key", value);
    ASSERT_LE(TotalTableFiles(), kMaxFiles);
    fprintf(stderr, "after %d: %d files\n", int(i+1), TotalTableFiles());
  }
}
#endif

TEST(DBTest, SparseMerge) {
  Options options = CurrentOptions();
  options.compression = kNoCompression;
  Reopen(&options);

  FillLevels("A", "Z");

  // Suppose there is:
  //    small amount of data with prefix A
  //    large amount of data with prefix B
  //    small amount of data with prefix C
  // and that recent updates have made small changes to all three prefixes.
  // Check that we do not do a compaction that merges all of B in one shot.
  const std::string value(1000, 'x');
  Put("A", "va");
  // Write approximately 100MB of "B" values
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    Put(key, value);
  }
  Put("C", "vc");
  dbfull()->TEST_CompactMemTable();
  dbfull()->TEST_CompactRange(0, NULL, NULL);

  // Make sparse update
  Put("A",    "va2");
  Put("B100", "bvalue2");
  Put("C",    "vc2");
  dbfull()->TEST_CompactMemTable();

  // this test used to test whether or not compactions would push as high as
  // possible.
  // Hint: we don't do that anymore.
}

static bool Between(uint64_t val, uint64_t low, uint64_t high) {
  bool result = (val >= low) && (val <= high);
  if (!result) {
    fprintf(stderr, "Value %llu is not in range [%llu, %llu]\n",
            (unsigned long long)(val),
            (unsigned long long)(low),
            (unsigned long long)(high));
  }
  return result;
}

TEST(DBTest, ApproximateSizes) {
  do {
    Options options = CurrentOptions();
    options.write_buffer_size = 100000000;        // Large write buffer
    options.compression = kNoCompression;
    DestroyAndReopen();

    ASSERT_TRUE(Between(Size("", "xyz"), 0, 0));
    Reopen(&options);
    ASSERT_TRUE(Between(Size("", "xyz"), 0, 0));

    // Write 8MB (80 values, each 100K)
    ASSERT_EQ(NumTableFilesAtLevel(0), 0);
    const int N = 80;
    static const int S1 = 100000;
    static const int S2 = 105000;  // Allow some expansion from metadata
    Random rnd(301);
    for (int i = 0; i < N; i++) {
      ASSERT_OK(Put(Key(i), RandomString(&rnd, S1)));
    }

    // 0 because GetApproximateSizes() does not account for memtable space
    ASSERT_TRUE(Between(Size("", Key(50)), 0, 0));

    // Check sizes across recovery by reopening a few times
    for (int run = 0; run < 3; run++) {
      Reopen(&options);

      for (int compact_start = 0; compact_start < N; compact_start += 10) {
        for (int i = 0; i < N; i += 10) {
          ASSERT_TRUE(Between(Size("", Key(i)), S1*i, S2*i));
          ASSERT_TRUE(Between(Size("", Key(i)+".suffix"), S1*(i+1), S2*(i+1)));
          ASSERT_TRUE(Between(Size(Key(i), Key(i+10)), S1*10, S2*10));
        }
        ASSERT_TRUE(Between(Size("", Key(50)), S1*50, S2*50));
        ASSERT_TRUE(Between(Size("", Key(50)+".suffix"), S1*50, S2*50));

        std::string cstart_str = Key(compact_start);
        std::string cend_str = Key(compact_start + 9);
        Slice cstart = cstart_str;
        Slice cend = cend_str;
        dbfull()->TEST_CompactRange(0, &cstart, &cend);
      }

      ASSERT_EQ(NumTableFilesAtLevel(0), 0);
      ASSERT_GT(NumTableFilesAtLevel(1), 0);
    }
  } while (ChangeOptions());
}

TEST(DBTest, ApproximateSizes_MixOfSmallAndLarge) {
  do {
    Options options = CurrentOptions();
    options.compression = kNoCompression;
    Reopen();

    Random rnd(301);
    std::string big1 = RandomString(&rnd, 100000);
    ASSERT_OK(Put(Key(0), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(Key(1), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(Key(2), big1));
    ASSERT_OK(Put(Key(3), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(Key(4), big1));
    ASSERT_OK(Put(Key(5), RandomString(&rnd, 10000)));
    ASSERT_OK(Put(Key(6), RandomString(&rnd, 300000)));
    ASSERT_OK(Put(Key(7), RandomString(&rnd, 10000)));

    // Check sizes across recovery by reopening a few times
    for (int run = 0; run < 3; run++) {
      Reopen(&options);

      ASSERT_TRUE(Between(Size("", Key(0)), 0, 0));
      ASSERT_TRUE(Between(Size("", Key(1)), 10000, 11000));
      ASSERT_TRUE(Between(Size("", Key(2)), 20000, 21000));
      ASSERT_TRUE(Between(Size("", Key(3)), 120000, 121000));
      ASSERT_TRUE(Between(Size("", Key(4)), 130000, 131000));
      ASSERT_TRUE(Between(Size("", Key(5)), 230000, 231000));
      ASSERT_TRUE(Between(Size("", Key(6)), 240000, 241000));
      ASSERT_TRUE(Between(Size("", Key(7)), 540000, 541000));
      ASSERT_TRUE(Between(Size("", Key(8)), 550000, 560000));

      ASSERT_TRUE(Between(Size(Key(3), Key(5)), 110000, 111000));

      dbfull()->TEST_CompactRange(0, NULL, NULL);
    }
  } while (ChangeOptions());
}

TEST(DBTest, IteratorPinsRef) {
  Put("foo", "hello");

  // Get iterator that will yield the current contents of the DB.
  Iterator* iter = db_->NewIterator(ReadOptions());

  // Write to force compactions
  Put("foo", "newvalue1");
  for (int i = 0; i < 100; i++) {
    ASSERT_OK(Put(Key(i), Key(i) + std::string(100000, 'v'))); // 100K values
  }
  Put("foo", "newvalue2");

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ("foo", iter->key().ToString());
  ASSERT_EQ("hello", iter->value().ToString());
  iter->Next();
  ASSERT_TRUE(!iter->Valid());
  delete iter;
}

TEST(DBTest, Snapshot) {
  do {
    Put("foo", "v1");
    const Snapshot* s1 = db_->GetSnapshot();
    Put("foo", "v2");
    const Snapshot* s2 = db_->GetSnapshot();
    Put("foo", "v3");
    const Snapshot* s3 = db_->GetSnapshot();

    Put("foo", "v4");
    ASSERT_EQ("v1", Get("foo", s1));
    ASSERT_EQ("v2", Get("foo", s2));
    ASSERT_EQ("v3", Get("foo", s3));
    ASSERT_EQ("v4", Get("foo"));

    db_->ReleaseSnapshot(s3);
    ASSERT_EQ("v1", Get("foo", s1));
    ASSERT_EQ("v2", Get("foo", s2));
    ASSERT_EQ("v4", Get("foo"));

    db_->ReleaseSnapshot(s1);
    ASSERT_EQ("v2", Get("foo", s2));
    ASSERT_EQ("v4", Get("foo"));

    db_->ReleaseSnapshot(s2);
    ASSERT_EQ("v4", Get("foo"));
  } while (ChangeOptions());
}

TEST(DBTest, HiddenValuesAreRemoved) {
  do {
    Random rnd(301);
    FillLevels("a", "z");

    std::string big = RandomString(&rnd, 50000);
    Put("foo", big);
    Put("pastfoo", "v");
    const Snapshot* snapshot = db_->GetSnapshot();
    Put("foo", "tiny");
    Put("pastfoo2", "v2");        // Advance sequence number one more

    ASSERT_OK(dbfull()->TEST_CompactMemTable());
    ASSERT_GT(NumTableFilesAtLevel(0), 0);

    ASSERT_EQ(big, Get("foo", snapshot));
    ASSERT_TRUE(Between(Size("", "pastfoo"), 50000, 60000));
    db_->ReleaseSnapshot(snapshot);
    ASSERT_EQ(AllEntriesFor("foo"), "[ tiny, " + big + " ]");
    Slice x("x");

    dbfull()->TEST_CompactRange(0, NULL, &x);
    ASSERT_EQ(NumTableFilesAtLevel(0), 0);
    ASSERT_GE(NumTableFilesAtLevel(1), 1);
    dbfull()->TEST_CompactRange(1, NULL, &x);
    ASSERT_EQ(AllEntriesFor("foo"), "[ tiny ]");

    ASSERT_TRUE(Between(Size("", "pastfoo"), 0, 1000));
  } while (ChangeOptions());
}

TEST(DBTest, DeletionMarkers1) {
  Put("foo", "v1");
  ASSERT_OK(dbfull()->TEST_CompactMemTable());
  const int last = config::kMaxMemCompactLevel;

  // Place a table at level last-1 to prevent merging with preceding mutation
  Put("a", "begin");
  Put("z", "end");
  dbfull()->TEST_CompactMemTable();

  Delete("foo");
  Put("foo", "v2");
  ASSERT_EQ(AllEntriesFor("foo"), "[ v2, DEL, v1 ]");
  ASSERT_OK(dbfull()->TEST_CompactMemTable());  // Moves to level last-2
  ASSERT_EQ(AllEntriesFor("foo"), "[ v2, DEL, v1 ]");
  Slice z("z");
  dbfull()->TEST_CompactRange(4, NULL, &z);
  // DEL and v1 remain because we aren't compacting that level (0)
  // (DEL can be eliminated because v2 hides v1).
  ASSERT_EQ(AllEntriesFor("foo"), "[ v2, DEL, v1 ]");
  dbfull()->TEST_CompactRange(0, NULL, NULL);
  // Merging last-1 w/ last, so we are the base level for "foo", so
  // DEL is removed.  (as is v1).
  ASSERT_EQ(AllEntriesFor("foo"), "[ v2 ]");
}

TEST(DBTest, DeletionMarkers2) {
  Put("foo", "v1");
  ASSERT_OK(dbfull()->TEST_CompactMemTable());
  const int last = config::kMaxMemCompactLevel;

  // Place a table at level last-1 to prevent merging with preceding mutation
  Put("a", "begin");
  Put("z", "end");
  dbfull()->TEST_CompactMemTable();

  Delete("foo");
  ASSERT_EQ(AllEntriesFor("foo"), "[ DEL, v1 ]");
  ASSERT_OK(dbfull()->TEST_CompactMemTable());  // Moves to level last-2
  ASSERT_EQ(AllEntriesFor("foo"), "[ DEL, v1 ]");
  dbfull()->TEST_CompactRange(5, NULL, NULL);
  // DEL kept: "last" file overlaps
  ASSERT_EQ(AllEntriesFor("foo"), "[ DEL, v1 ]");
  dbfull()->TEST_CompactRange(0, NULL, NULL);
  // Merging 1 level with 0, so we are the base level for "foo", so
  // DEL is removed.  (as is v1).
  ASSERT_EQ(AllEntriesFor("foo"), "[ ]");
}

TEST(DBTest, OverlapInLevel0) {
  do {
    ASSERT_EQ(config::kMaxMemCompactLevel, 2) << "Fix test to match config";

    // Fill levels 1 and 2 to disable the pushing of new memtables to levels > 0.
    ASSERT_OK(Put("100", "v100"));
    ASSERT_OK(Put("999", "v999"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_OK(Delete("100"));
    ASSERT_OK(Delete("999"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("2", FilesPerLevel());

    // Make files spanning the following ranges in level-0:
    //  files[0]  200 .. 900
    //  files[1]  300 .. 500
    // Note that files are sorted by smallest key.
    ASSERT_OK(Put("300", "v300"));
    ASSERT_OK(Put("500", "v500"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_OK(Put("200", "v200"));
    ASSERT_OK(Put("600", "v600"));
    ASSERT_OK(Put("900", "v900"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("2", FilesPerLevel()); // since number of files per sentinel is set to 2

    // Compact away the placeholder files we created initially
    dbfull()->TEST_CompactRange(0, NULL, NULL);
    ASSERT_EQ("0,1", FilesPerLevel());

    // Do a memtable compaction.  Before bug-fix, the compaction would
    // not detect the overlap with level-0 files and would incorrectly place
    // the deletion in a deeper level.
    ASSERT_OK(Delete("600"));
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("1,1", FilesPerLevel());
    ASSERT_EQ("NOT_FOUND", Get("600"));
  } while (ChangeOptions());
}

TEST(DBTest, ComparatorCheck) {
  class NewComparator : public Comparator {
   public:
    virtual const char* Name() const { return "leveldb.NewComparator"; }
    virtual int Compare(const Slice& a, const Slice& b) const {
      return BytewiseComparator()->Compare(a, b);
    }
    virtual void FindShortestSeparator(std::string* s, const Slice& l) const {
      BytewiseComparator()->FindShortestSeparator(s, l);
    }
    virtual void FindShortSuccessor(std::string* key) const {
      BytewiseComparator()->FindShortSuccessor(key);
    }
  };
  NewComparator cmp;
  Options new_options = CurrentOptions();
  new_options.comparator = &cmp;
  Status s = TryReopen(&new_options);
  ASSERT_TRUE(!s.ok());
  ASSERT_TRUE(s.ToString().find("comparator") != std::string::npos)
      << s.ToString();
}

TEST(DBTest, CustomComparator) {
  class NumberComparator : public Comparator {
   public:
    virtual const char* Name() const { return "test.NumberComparator"; }
    virtual int Compare(const Slice& a, const Slice& b) const {
      return ToNumber(a) - ToNumber(b);
    }
    virtual void FindShortestSeparator(std::string* s, const Slice& l) const {
      ToNumber(*s);     // Check format
      ToNumber(l);      // Check format
    }
    virtual void FindShortSuccessor(std::string* key) const {
      ToNumber(*key);   // Check format
    }
   private:
    static int ToNumber(const Slice& x) {
      // Check that there are no extra characters.
      ASSERT_TRUE(x.size() >= 2 && x[0] == '[' && x[x.size()-1] == ']')
          << EscapeString(x);
      int val;
      char ignored;
      ASSERT_TRUE(sscanf(x.ToString().c_str(), "[%i]%c", &val, &ignored) == 1)
          << EscapeString(x);
      return val;
    }
  };
  NumberComparator cmp;
  Options new_options = CurrentOptions();
  new_options.create_if_missing = true;
  new_options.comparator = &cmp;
  new_options.filter_policy = NULL;     // Cannot use bloom filters
  new_options.write_buffer_size = 1000;  // Compact more often
  DestroyAndReopen(&new_options);
  ASSERT_OK(Put("[10]", "ten"));
  ASSERT_OK(Put("[0x14]", "twenty"));
  for (int i = 0; i < 2; i++) {
    ASSERT_EQ("ten", Get("[10]"));
    ASSERT_EQ("ten", Get("[0xa]"));
    ASSERT_EQ("twenty", Get("[20]"));
    ASSERT_EQ("twenty", Get("[0x14]"));
    ASSERT_EQ("NOT_FOUND", Get("[15]"));
    ASSERT_EQ("NOT_FOUND", Get("[0xf]"));
    Compact("[0]", "[9999]");
  }

  for (int run = 0; run < 2; run++) {
    for (int i = 0; i < 1000; i++) {
      char buf[100];
      snprintf(buf, sizeof(buf), "[%d]", i*10);
      ASSERT_OK(Put(buf, buf));
    }
    Compact("[0]", "[1000000]");
  }
}

/*TEST(DBTest, ManualCompaction) {
  ASSERT_EQ(config::kMaxMemCompactLevel, 2)
      << "Need to update this test to match kMaxMemCompactLevel";

  MakeTables(3, "p", "q");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // Compaction range falls before files
  Compact("", "c");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // Compaction range falls after files
  Compact("r", "z");
  ASSERT_EQ("1,1,1", FilesPerLevel());

  // Compaction range overlaps files
  Compact("p1", "p9");
  ASSERT_EQ("0,0,1", FilesPerLevel());

  // Populate a different range
  MakeTables(3, "c", "e");
  ASSERT_EQ("1,1,2", FilesPerLevel());

  // Compact just the new range
  Compact("b", "f");
  ASSERT_EQ("0,0,2", FilesPerLevel());

  // Compact all
  MakeTables(1, "a", "z");
  ASSERT_EQ("0,1,2", FilesPerLevel());
  db_->CompactRange(NULL, NULL);
  ASSERT_EQ("0,0,1", FilesPerLevel());
}*/

TEST(DBTest, DBOpen_Options) {
  std::string dbname = test::TmpDir() + "/db_options_test";
  DestroyDB(dbname, Options());

  // Does not exist, and create_if_missing == false: error
  DB* db = NULL;
  Options opts;
  opts.create_if_missing = false;
  Status s = DB::Open(opts, dbname, &db);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "does not exist") != NULL);
  ASSERT_TRUE(db == NULL);

  // Does not exist, and create_if_missing == true: OK
  opts.create_if_missing = true;
  s = DB::Open(opts, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != NULL);

  delete db;
  db = NULL;

  // Does exist, and error_if_exists == true: error
  opts.create_if_missing = false;
  opts.error_if_exists = true;
  s = DB::Open(opts, dbname, &db);
  ASSERT_TRUE(strstr(s.ToString().c_str(), "exists") != NULL);
  ASSERT_TRUE(db == NULL);

  // Does exist, and error_if_exists == false: OK
  opts.create_if_missing = true;
  opts.error_if_exists = false;
  s = DB::Open(opts, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != NULL);

  delete db;
  db = NULL;
}

TEST(DBTest, Locking) {
  DB* db2 = NULL;
  Status s = DB::Open(CurrentOptions(), dbname_, &db2);
  ASSERT_TRUE(!s.ok()) << "Locking did not prevent re-opening db";
}

// Check that number of files does not grow when we are out of space
TEST(DBTest, NoSpace) {
  Options options = CurrentOptions();
  options.env = env_;
  Reopen(&options);

  ASSERT_OK(Put("foo", "v1"));
  ASSERT_EQ("v1", Get("foo"));
  Compact("a", "z");
  const int num_files = CountFiles();
  env_->no_space_.Release_Store(env_);   // Force out-of-space errors
  for (int i = 0; i < 10; i++) {
    for (int level = 0; level < config::kNumLevels-1; level++) {
      dbfull()->TEST_CompactRange(level, NULL, NULL);
    }
  }
  env_->no_space_.Release_Store(NULL);
  ASSERT_LT(CountFiles(), num_files + 3);
}

TEST(DBTest, NonWritableFileSystem) {
  Options options = CurrentOptions();
  options.write_buffer_size = 1000;
  options.env = env_;
  Reopen(&options);
  ASSERT_OK(Put("foo", "v1"));
  env_->non_writable_.Release_Store(env_);  // Force errors for new files
  std::string big(100000, 'x');
  int errors = 0;
  for (int i = 0; i < 20; i++) {
    fprintf(stderr, "iter %d; errors %d\n", i, errors);
    if (!Put("foo", big).ok()) {
      errors++;
      DelayMilliseconds(100);
    }
  }
  ASSERT_GT(errors, 0);
  env_->non_writable_.Release_Store(NULL);
}

TEST(DBTest, WriteSyncError) {
  // Check that log sync errors cause the DB to disallow future writes.

  // (a) Cause log sync calls to fail
  Options options = CurrentOptions();
  options.env = env_;
  Reopen(&options);
  env_->data_sync_error_.Release_Store(env_);

  // (b) Normal write should succeed
  WriteOptions w;
  ASSERT_OK(db_->Put(w, "k1", "v1"));
  ASSERT_EQ("v1", Get("k1"));

  // (c) Do a sync write; should fail
  w.sync = true;
  ASSERT_TRUE(!db_->Put(w, "k2", "v2").ok());
  ASSERT_EQ("v1", Get("k1"));
  ASSERT_EQ("NOT_FOUND", Get("k2"));

  // (d) make sync behave normally
  env_->data_sync_error_.Release_Store(NULL);

  // (e) Do a non-sync write; should fail
  w.sync = false;
  ASSERT_TRUE(!db_->Put(w, "k3", "v3").ok());
  ASSERT_EQ("v1", Get("k1"));
  ASSERT_EQ("NOT_FOUND", Get("k2"));
  ASSERT_EQ("NOT_FOUND", Get("k3"));
}

TEST(DBTest, ManifestWriteError) {
  // Test for the following problem:
  // (a) Compaction produces file F
  // (b) Log record containing F is written to MANIFEST file, but Sync() fails
  // (c) GC deletes F
  // (d) After reopening DB, reads fail since deleted F is named in log record

  // We iterate twice.  In the second iteration, everything is the
  // same except the log record never makes it to the MANIFEST file.
  for (int iter = 0; iter < 2; iter++) {
    port::AtomicPointer* error_type = (iter == 0)
        ? &env_->manifest_sync_error_
        : &env_->manifest_write_error_;

    // Insert foo=>bar mapping
    Options options = CurrentOptions();
    options.env = env_;
    options.create_if_missing = true;
    options.error_if_exists = false;
    DestroyAndReopen(&options);
    ASSERT_OK(Put("foo", "bar"));
    ASSERT_EQ("bar", Get("foo"));

    // Memtable compaction (will succeed)
    dbfull()->TEST_CompactMemTable();
    ASSERT_EQ("bar", Get("foo"));
    const int last = 0;
    ASSERT_EQ(NumTableFilesAtLevel(last), 1);   // foo=>bar is now in last level

    // Merging compaction (will fail)
    error_type->Release_Store(env_);
    dbfull()->TEST_CompactRange(last, NULL, NULL);  // Should fail
    ASSERT_EQ("bar", Get("foo"));

    // Recovery: should not lose data
    error_type->Release_Store(NULL);
    Reopen(&options);
    ASSERT_EQ("bar", Get("foo"));
  }
}

TEST(DBTest, MissingSSTFile) {
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_EQ("bar", Get("foo"));

  // Dump the memtable to disk.
  dbfull()->TEST_CompactMemTable();
  ASSERT_EQ("bar", Get("foo"));

  Close();
  ASSERT_TRUE(DeleteAnSSTFile());
  Options options = CurrentOptions();
  options.paranoid_checks = true;
  Status s = TryReopen(&options);
  ASSERT_TRUE(!s.ok());
  ASSERT_TRUE(s.ToString().find("issing") != std::string::npos)
      << s.ToString();
}

TEST(DBTest, StillReadSST) {
  ASSERT_OK(Put("foo", "bar"));
  ASSERT_EQ("bar", Get("foo"));

  // Dump the memtable to disk.
  dbfull()->TEST_CompactMemTable();
  ASSERT_EQ("bar", Get("foo"));
  Close();
  ASSERT_GT(RenameSSTToLDB(), 0);
  Options options = CurrentOptions();
  options.paranoid_checks = true;
  Status s = TryReopen(&options);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ("bar", Get("foo"));
}

TEST(DBTest, FilesDeletedAfterCompaction) {
  ASSERT_OK(Put("foo", "v2"));
  Compact("a", "z");
  const int num_files = CountFiles();
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(Put("foo", "v2"));
    Compact("a", "z");
  }
  ASSERT_EQ(CountFiles(), num_files);
}

TEST(DBTest, BloomFilter) {
  env_->count_random_reads_ = true;
  Options options = CurrentOptions();
  options.env = env_;
  options.block_cache = NewLRUCache(0);  // Prevent cache hits
  options.filter_policy = NewBloomFilterPolicy(10);
  Reopen(&options);

  // Populate multiple layers
  const int N = 10000;
  for (int i = 0; i < N; i++) {
    ASSERT_OK(Put(Key(i), Key(i)));
  }
  Compact("a", "z");
  for (int i = 0; i < N; i += 100) {
    ASSERT_OK(Put(Key(i), Key(i)));
  }
  dbfull()->TEST_CompactMemTable();

  // Prevent auto compactions triggered by seeks
  env_->delay_data_sync_.Release_Store(env_);

  // Lookup present keys.  Should rarely read from small sstable.
  for (int i = 0; i < N; i++) {
    ASSERT_EQ(Key(i), Get(Key(i)));
  }
 
  //Count number of files read to Get() N existing values in the db
  //Extra reads should be less than 3%
  int reads_1 = db_->total_files_read;
  fprintf(stderr, "%d present => %d reads\n", N, reads_1);
  ASSERT_GE(reads_1, N);
  ASSERT_LE(reads_1, N + 3*N/100);

  // Lookup present keys.  Should rarely read from either sstable.
  for (int i = 0; i < N; i++) {
    ASSERT_EQ("NOT_FOUND", Get(Key(i) + ".missing"));
  }
  
  //Files read to serve Get() of N non existant keys.
  //Should not exceed 3%
  int reads_2 = db_->total_files_read - reads_1;
  fprintf(stderr, "%d missing => %d reads\n", N, reads_2);
  ASSERT_LE(reads_2, 3*N/100);

  env_->delay_data_sync_.Release_Store(NULL);
  Close();
  delete options.block_cache;
  delete options.filter_policy;
}

// Multi-threaded test:
namespace {

static const int kNumThreads = 4;
static const int kTestSeconds = 10;
static const int kNumKeys = 1000;

struct MTState {
  DBTest* test;
  port::AtomicPointer stop;
  port::AtomicPointer counter[kNumThreads];
  port::AtomicPointer thread_done[kNumThreads];
};

struct MTThread {
  MTState* state;
  int id;
};

static void MTThreadBody(void* arg) {
  MTThread* t = reinterpret_cast<MTThread*>(arg);
  int id = t->id;
  DB* db = t->state->test->db_;
  uintptr_t counter = 0;
  fprintf(stderr, "... starting thread %d\n", id);
  Random rnd(1000 + id);
  std::string value;
  char valbuf[1500];
  while (t->state->stop.Acquire_Load() == NULL) {
    t->state->counter[id].Release_Store(reinterpret_cast<void*>(counter));

    int key = rnd.Uniform(kNumKeys);
    char keybuf[20];
    snprintf(keybuf, sizeof(keybuf), "%016d", key);

    if (rnd.OneIn(2)) {
      // Write values of the form <key, my id, counter>.
      // We add some padding for force compactions.
      snprintf(valbuf, sizeof(valbuf), "%d.%d.%-1000d",
               key, id, static_cast<int>(counter));
      ASSERT_OK(db->Put(WriteOptions(), Slice(keybuf), Slice(valbuf)));
    } else {
      // Read a value and verify that it matches the pattern written above.
      Status s = db->Get(ReadOptions(), Slice(keybuf), &value);
      if (s.IsNotFound()) {
        // Key has not yet been written
      } else {
        // Check that the writer thread counter is >= the counter in the value
        ASSERT_OK(s);
        int k, w, c;
        ASSERT_EQ(3, sscanf(value.c_str(), "%d.%d.%d", &k, &w, &c)) << value;
        ASSERT_EQ(k, key);
        ASSERT_GE(w, 0);
        ASSERT_LT(w, kNumThreads);
        ASSERT_LE(static_cast<uintptr_t>(c), reinterpret_cast<uintptr_t>(
            t->state->counter[w].Acquire_Load()));
      }
    }
    counter++;
  }
  t->state->thread_done[id].Release_Store(t);
  fprintf(stderr, "... stopping thread %d after %d ops\n", id, int(counter));
}

}  // namespace

TEST(DBTest, MultiThreaded) {
  do {
    // Initialize state
    MTState mt;
    mt.test = this;
    mt.stop.Release_Store(0);
    for (int id = 0; id < kNumThreads; id++) {
      mt.counter[id].Release_Store(0);
      mt.thread_done[id].Release_Store(0);
    }

    // Start threads
    MTThread thread[kNumThreads];
    for (int id = 0; id < kNumThreads; id++) {
      thread[id].state = &mt;
      thread[id].id = id;
      env_->StartThread(MTThreadBody, &thread[id]);
    }

    // Let them run for a while
    DelayMilliseconds(kTestSeconds * 1000);

    // Stop the threads and wait for them to finish
    mt.stop.Release_Store(&mt);
    for (int id = 0; id < kNumThreads; id++) {
      while (mt.thread_done[id].Acquire_Load() == NULL) {
        DelayMilliseconds(100);
      }
    }
  } while (ChangeOptions());
}

namespace {
typedef std::map<std::string, std::string> KVMap;
}

class ModelDB: public DB {
 public:
  class ModelSnapshot : public Snapshot {
   public:
    KVMap map_;
  };

  explicit ModelDB(const Options& options): options_(options) { }
  ~ModelDB() { }
  virtual Status Put(const WriteOptions& o, const Slice& k, const Slice& v) {
    return DB::Put(o, k, v);
  }
  virtual Status Delete(const WriteOptions& o, const Slice& key) {
    return DB::Delete(o, key);
  }
  virtual Status Get(const ReadOptions& options,
                     const Slice& key, std::string* value) {
    assert(false);      // Not implemented
    return Status::NotFound(key);
  }
  virtual Status GetCurrentVersionState(std::string* value) {
	  assert(false);
	  return Status::NotSupported("not_supported");
  }
  virtual void PrintTimerAudit() {}

  virtual Iterator* NewIterator(const ReadOptions& options) {
    if (options.snapshot == NULL) {
      KVMap* saved = new KVMap;
      *saved = map_;
      return new ModelIter(saved, true);
    } else {
      const KVMap* snapshot_state =
          &(reinterpret_cast<const ModelSnapshot*>(options.snapshot)->map_);
      return new ModelIter(snapshot_state, false);
    }
  }
  virtual void GetReplayTimestamp(std::string* timestamp) {
  }
  virtual void AllowGarbageCollectBeforeTimestamp(const std::string& timestamp) {
  }
  virtual bool ValidateTimestamp(const std::string&) {
    return false;
  }
  virtual int CompareTimestamps(const std::string&, const std::string&) {
    return 0;
  }
  virtual Status GetReplayIterator(const std::string& timestamp,
                                   ReplayIterator** iter) {
    *iter = NULL;
    return Status::OK();
  }
  virtual void ReleaseReplayIterator(ReplayIterator* iter) {
  }
  virtual const Snapshot* GetSnapshot() {
    ModelSnapshot* snapshot = new ModelSnapshot;
    snapshot->map_ = map_;
    return snapshot;
  }

  virtual void ReleaseSnapshot(const Snapshot* snapshot) {
    delete reinterpret_cast<const ModelSnapshot*>(snapshot);
  }
  virtual Status Write(const WriteOptions& options, WriteBatch* batch) {
    class Handler : public WriteBatch::Handler {
     public:
      KVMap* map_;
      virtual void Put(const Slice& key, const Slice& value) {
        (*map_)[key.ToString()] = value.ToString();
      }
      virtual void Delete(const Slice& key) {
        map_->erase(key.ToString());
      }
      virtual void HandleGuard(const Slice& key, unsigned level) {
	assert(0);
      }
    };
    Handler handler;
    handler.map_ = &map_;
    return batch->Iterate(&handler);
  }

  virtual bool GetProperty(const Slice& property, std::string* value) {
    return false;
  }
  virtual void GetApproximateSizes(const Range* r, int n, uint64_t* sizes) {
    for (int i = 0; i < n; i++) {
      sizes[i] = 0;
    }
  }
  virtual void CompactRange(const Slice* start, const Slice* end) {
  }
  virtual Status LiveBackup(const Slice& name) {
  }

 private:
  class ModelIter: public Iterator {
   public:
    ModelIter(const KVMap* map, bool owned)
        : map_(map), owned_(owned), iter_(map_->end()) {
    }
    ~ModelIter() {
      if (owned_) delete map_;
    }
    virtual bool Valid() const { return iter_ != map_->end(); }
    virtual void SeekToFirst() { iter_ = map_->begin(); }
    virtual void SeekToLast() {
      if (map_->empty()) {
        iter_ = map_->end();
      } else {
        iter_ = map_->find(map_->rbegin()->first);
      }
    }
    virtual void Seek(const Slice& k) {
      iter_ = map_->lower_bound(k.ToString());
    }
    virtual void Next() { ++iter_; }
    virtual void Prev() { --iter_; }
    virtual Slice key() const { return iter_->first; }
    virtual Slice value() const { return iter_->second; }
    virtual const Status& status() const { return status_; }
   private:
    const KVMap* const map_;
    const bool owned_;  // Do we own map_
    KVMap::const_iterator iter_;
    Status status_;
  };
  const Options options_;
  KVMap map_;
};

static std::string RandomKey(Random* rnd) {
  int len = (rnd->OneIn(3)
             ? 1                // Short sometimes to encourage collisions
             : (rnd->OneIn(100) ? rnd->Skewed(10) : rnd->Uniform(10)));
  return test::RandomKey(rnd, len);
}

static bool CompareIterators(int step,
                             DB* model,
                             DB* db,
                             const Snapshot* model_snap,
                             const Snapshot* db_snap) {
  ReadOptions options;
  options.snapshot = model_snap;
  Iterator* miter = model->NewIterator(options);
  options.snapshot = db_snap;
  Iterator* dbiter = db->NewIterator(options);
  bool ok = true;
  int count = 0;
  for (miter->SeekToFirst(), dbiter->SeekToFirst();
       ok && miter->Valid() && dbiter->Valid();
       miter->Next(), dbiter->Next()) {
    count++;
    if (miter->key().compare(dbiter->key()) != 0) {
      fprintf(stderr, "step %d: Key mismatch: '%s' vs. '%s'\n",
              step,
              EscapeString(miter->key()).c_str(),
              EscapeString(dbiter->key()).c_str());
      ok = false;
      break;
    }

    if (miter->value().compare(dbiter->value()) != 0) {
      fprintf(stderr, "step %d: Value mismatch for key '%s': '%s' vs. '%s'\n",
              step,
              EscapeString(miter->key()).c_str(),
              EscapeString(miter->value()).c_str(),
              EscapeString(miter->value()).c_str());
      ok = false;
    }
  }

  if (ok) {
    if (miter->Valid() != dbiter->Valid()) {
      fprintf(stderr, "step %d: Mismatch at end of iterators: %d vs. %d\n",
              step, miter->Valid(), dbiter->Valid());
      ok = false;
    }
  }
  fprintf(stderr, "%d entries compared: ok=%d\n", count, ok);
  delete miter;
  delete dbiter;
  return ok;
}

TEST(DBTest, Randomized) {
/*  Random rnd(test::RandomSeed());
  do {
    ModelDB model(CurrentOptions());
    const int N = 10000;
    const Snapshot* model_snap = NULL;
    const Snapshot* db_snap = NULL;
    std::string k, v;
    for (int step = 0; step < N; step++) {
      if (step % 100 == 0) {
        fprintf(stderr, "Step %d of %d\n", step, N);
      }
      // TODO(sanjay): Test Get() works
      int p = rnd.Uniform(100);
      if (p < 45) {                               // Put
        k = RandomKey(&rnd);
        v = RandomString(&rnd,
                         rnd.OneIn(20)
                         ? 100 + rnd.Uniform(100)
                         : rnd.Uniform(8));
        ASSERT_OK(model.Put(WriteOptions(), k, v));
        ASSERT_OK(db_->Put(WriteOptions(), k, v));

      } else if (p < 90) {                        // Delete
        k = RandomKey(&rnd);
        ASSERT_OK(model.Delete(WriteOptions(), k));
        ASSERT_OK(db_->Delete(WriteOptions(), k));


      } else {                                    // Multi-element batch
        WriteBatch b;
        const int num = rnd.Uniform(8);
        for (int i = 0; i < num; i++) {
          if (i == 0 || !rnd.OneIn(10)) {
            k = RandomKey(&rnd);
          } else {
            // Periodically re-use the same key from the previous iter, so
            // we have multiple entries in the write batch for the same key
          }
          if (rnd.OneIn(2)) {
            v = RandomString(&rnd, rnd.Uniform(10));
            b.Put(k, v);
          } else {
            b.Delete(k);
          }
        }
        ASSERT_OK(model.Write(WriteOptions(), &b));
        ASSERT_OK(db_->Write(WriteOptions(), &b));
      }

      if ((step % 100) == 0) {
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));
        ASSERT_TRUE(CompareIterators(step, &model, db_, model_snap, db_snap));
        // Save a snapshot from each DB this time that we'll use next
        // time we compare things, to make sure the current state is
        // preserved with the snapshot
        if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
        if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);

        Reopen();
        ASSERT_TRUE(CompareIterators(step, &model, db_, NULL, NULL));

        model_snap = model.GetSnapshot();
        db_snap = db_->GetSnapshot();
      }
    }
    if (model_snap != NULL) model.ReleaseSnapshot(model_snap);
    if (db_snap != NULL) db_->ReleaseSnapshot(db_snap);
  } while (ChangeOptions());*/
}

TEST(DBTest, Replay) {
  std::string ts;
  db_->GetReplayTimestamp(&ts);
  ASSERT_OK(Put("key", "v0"));
  ASSERT_OK(Put("key", "v1"));
  ASSERT_OK(Put("key", "v2"));
  ASSERT_OK(Put("key", "v3"));
  ASSERT_OK(Put("key", "v4"));
  ASSERT_OK(Put("key", "v5"));
  ASSERT_OK(Put("key", "v6"));
  ASSERT_OK(Put("key", "v7"));
  ASSERT_OK(Put("key", "v8"));
  ASSERT_OK(Put("key", "v9"));

  // get the iterator
  ReplayIterator* iter = NULL;
  ASSERT_OK(db_->GetReplayIterator(ts, &iter));

  // Iterate over what was there to start with
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(iter->HasValue());
  ASSERT_EQ("key", iter->key().ToString());
  ASSERT_EQ("v9", iter->value().ToString());
  iter->Next();
  // The implementation is allowed to return things twice.
  // This is a case where it will.
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(iter->HasValue());
  ASSERT_EQ("key", iter->key().ToString());
  ASSERT_EQ("v9", iter->value().ToString());
  iter->Next();
  // Now it's no longer valid.
  ASSERT_TRUE(!iter->Valid());
  ASSERT_TRUE(!iter->Valid());

  // Add another and iterate some more
  ASSERT_OK(Put("key", "v10"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(iter->HasValue());
  ASSERT_EQ("key", iter->key().ToString());
  ASSERT_EQ("v10", iter->value().ToString());
  iter->Next();
  ASSERT_TRUE(!iter->Valid());

  // Dump the memtable
  dbfull()->TEST_CompactMemTable();

  // Write into the new MemTable and iterate some more
  ASSERT_OK(Put("key", "v11"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(iter->HasValue());
  ASSERT_EQ("key", iter->key().ToString());
  ASSERT_EQ("v11", iter->value().ToString());
  iter->Next();
  ASSERT_TRUE(!iter->Valid());

  // What does it do on delete?
  ASSERT_OK(Delete("key"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_TRUE(!iter->HasValue());
  ASSERT_EQ("key", iter->key().ToString());
  iter->Next();
  ASSERT_TRUE(!iter->Valid());
}

uint64_t micros() {
	return Env::Default()->NowMicros();
}

void print_timer_info(std::string msg, uint64_t a, uint64_t b) {
	uint64_t diff = llabs(a-b);
	printf("%s: %lu micros (%f ms)\n", msg.c_str(), diff, diff/1000.0);
}

void print_divider() {
//	printf("==============================================================================\n");
}

TEST(DBTest, FLSMInsert) {
  Options options = CurrentOptions();
  options.compression = kNoCompression;
  Reopen(&options);

  const std::string value(1000, 'x');
  Put("A", "va");
  // Write approximately 100MB of "B" values
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    Put(key, value);
  }
  Put("C", "vc");
  ASSERT_TRUE(true);
}

TEST(DBTest, FLSMSentinelInsertReopen)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write 10 keys of "B" values
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");

	  unsigned total_table_files, sentinel_files, guard_files;

	  Reopen();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  ASSERT_EQ(guard_files, 0);
	  ASSERT_EQ(total_table_files, sentinel_files);

}

TEST(DBTest, FLSMSentinelInsertCompactionReopen)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write 10 keys of "B" values
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");

	  unsigned total_table_files, sentinel_files, guard_files;

//	  print_file_counts();

//	  printf("\n\nTriggering a memtable compaction . . .\n");
	  dbfull()->TEST_CompactMemTable();
//	  printf("Memtable compaction done !\n\n");

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

//	  print_file_counts();
	  ASSERT_EQ(guard_files, 0);
	  ASSERT_EQ(total_table_files, sentinel_files);

//	  printf("\n\nReopening database . . .\n");
	  Reopen();
//	  printf("Database reopened !\n");

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

//	  print_file_counts();
	  ASSERT_EQ(guard_files, 0);
	  ASSERT_EQ(total_table_files, sentinel_files);
}

// Verify if lookup works correctly from sentinel files after reopen
TEST(DBTest, FLSMSentinelReadReopen)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write approximately 10MB of "B" values
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");

	  // Verify read from memtable
//	  printf("---------------Verifying read from memtable--------------\n");
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    ASSERT_EQ(value, Get(key));
	  }
	  ASSERT_EQ("vc", Get("C"));
//	  printf("Verified !\n");

//	  printf("\nReopening database . . .\n");
	  Reopen();
//	  printf("Database reopened.\n");

	  // Verify read after reopen
//	  printf("\n----------Verifying read after reopening database--------\n");
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    ASSERT_EQ(value, Get(key));
	  }
	  ASSERT_EQ("vc", Get("C"));
//	  printf("Verified !\n\n");
}

// Verify if lookup works correctly from sentinel files after compaction followed by reopen
TEST(DBTest, FLSMSentinelReadCompactionReopen)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write approximately 10MB of "B" values
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");

	  // Verify read from memtable
//	  printf("---------------Verifying read from memtable--------------\n");
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    ASSERT_EQ(value, Get(key));
	  }
	  ASSERT_EQ("vc", Get("C"));
//	  printf("Verified !\n");

//	  printf("\nTriggering a memtable compaction . . .\n");
	  dbfull()->TEST_CompactMemTable();
//	  printf("Memtable compaction done.\n");

	  // Verify read after compaction
//	  printf("-------------Verifying read after compaction-------------\n");
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    ASSERT_EQ(value, Get(key));
	  }
	  ASSERT_EQ("vc", Get("C"));
//	  printf("Verified !\n");

//	  printf("\nReopening database . . .\n");
	  Reopen();
//	  printf("Database reopened.\n");

	  // Verify read after reopen
//	  printf("\n----------Verifying read after reopening database--------\n");
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    ASSERT_EQ(value, Get(key));
	  }
	  ASSERT_EQ("vc", Get("C"));
//	  printf("Verified !\n\n");
}

/*void CheckIfGuardKey(Slice key, int num_bits_, int bit_decrement_) {
	// Need to hash and check the last few bits.
	void* input = (void*) key.data();
	size_t size = key.size();
	const unsigned int murmur_seed = 42;
	unsigned int hash_result;
	MurmurHash3_x86_32(input, size, murmur_seed, &hash_result);

	// Go through each level, starting from the top and checking if it
	// is a guard on that level.

	unsigned num_bits = num_bits_;
	const static int bit_decrement = bit_decrement_;
	unsigned bit_mask;

	for (unsigned i = 0; i < config::kNumLevels; i++) {

	  assert(num_bits > 0 && num_bits < 32);
	  bit_mask = (1 << num_bits) - 1;

	  if ((hash_result & bit_mask) == bit_mask) {
		// found a guard
		// Insert the guard to this level and all the lower levels
		for (unsigned j = i; j < config::kNumLevels; j++) {
//			printf("CheckIfGuardKey() :: Level %d Guard key: %s with num_bits: %d and bit_decrement: %d\n", j, key.data(), num_bits_, bit_decrement);
		}
		break;
	  }
	  // Check next level
	  num_bits -= bit_decrement;
	}
}*/

// Verify if guard scheme is working properly end to end just to visualize the way files are compacted to further levelss
TEST(DBTest, FLSMGuardsE2ETest)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 0; i < 10; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");
	  for (int i = 3700; i < 3800; i++) {
		    char key[100];
		    snprintf(key, sizeof(key), "B%010d", i);
		    Put(key, value);
	  }
	  for (int i = 37000; i < 37100; i++) {
		    char key[100];
		    snprintf(key, sizeof(key), "B%010d", i);
		    Put(key, value);
	  }
	  for (int i = 400; i < 500; i++) {
		    char key[100];
		    snprintf(key, sizeof(key), "B%010d", i);
		    Put(key, value);
	  }
	  unsigned total_table_files, sentinel_files, guard_files;

	  Reopen();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);

	  // Write around 100 MB of "B" values
	  for (int i = 55800; i < 55850; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  // Write around 100 MB of "B" values
	  for (int i = 100; i < 200; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  // Write around 100 MB of "B" values
	  for (int i = 200; i < 300; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);

	  Env::Default()->SleepForMicroseconds(2000000);
	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);

	  // Write around 100 MB of "B" values
	  for (int i = 78500; i < 78600; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  for (int i = 78600; i < 78610; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  for (int i = 78610; i < 78620; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  for (int i = 78620; i < 78630; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }

	  Reopen();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  Env::Default()->SleepForMicroseconds(2000000);
	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);

	  Reopen();
}

// Verify if updating a key is working fine
TEST(DBTest, FLSMGuardsE2EUpdate)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  // Updating value for key B repeatedly
	  Put("A", "va");
	  for (int i = 0; i < 100000; i++) {
		  char val[100];
		  snprintf(val, sizeof(val), "B%010d", i);
		  Put("B", val);
	  }

	  Reopen();

	  ASSERT_EQ(Get("B"), "B0000099999");
}

// Verify if reads from guard files are working fine after inserting large number of keys in increasing order
TEST(DBTest, FLSMGuardsE2EInsertIncRead)  {
	  uint64_t a, b;
	  int num_values = 100000, print_every = 100000, count = 0, value_size = 100;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  a = micros();
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 0; i < num_values; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
		count++;
	  }
	  Put("C", "vc");
	  b = micros();

	  Reopen();

	  a = micros();
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < num_values; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    std::string return_val = Get(key);
	    if (return_val.compare(value) != 0) {
	    	printf("FLSMGuardsE2EInsertIncRead :: Lookup failed for key %s !!\n", key);
	    }
	    ASSERT_EQ(value, return_val);
	    count++;
	  }
	  ASSERT_EQ("vc", Get("C"));
	  b = micros();
}

unsigned int RangeDifference(Slice a, Slice b) {
    void* input_a = (void*) a.data();
    size_t size_a = a.size();
    void* input_b = (void*) b.data();
    size_t size_b = b.size();
    const unsigned int murmur_seed = 42;
    unsigned int hash_a;
    MurmurHash3_x86_32(input_a, size_a, murmur_seed, &hash_a);
    unsigned int hash_b;
    MurmurHash3_x86_32(input_b, size_b, murmur_seed, &hash_b);
    printf("hash_a: %lu hash_b: %lu\n", hash_a, hash_b);
    if (hash_a > hash_b) {
    	return hash_a - hash_b;
    }
    return hash_b - hash_a;
}

// Verify if reads from guard files are working fine after inserting large number of keys in increasing order
TEST(DBTest, FLSMGuardsE2EInsertIncReadRandom)  {
	  uint64_t a, b;
	  char* test_name = "FLSMGuardsE2EInsertIncReadRandom";
	  int num_values = 100000, print_every = 10000, count = 0, value_size = 1000;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  a = micros();
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 0; i < num_values; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
		count++;
	  }
	  Put("C", "vc");
	  b = micros();

	  Reopen();

	  a = micros();
	  ASSERT_EQ("va", Get("A"));
	  for (int j = 0; j < 17; j++) {
		  for (int i = j; i < num_values; i+=17) {
			    char key[100];
			    snprintf(key, sizeof(key), "B%010d", i);
			    std::string return_val = Get(key);
			    if (return_val.compare(value) != 0) {
			    	printf("%s :: Lookup failed for key %s !!\n", test_name, key);
			    }
			    ASSERT_EQ(value, return_val);
			    count++;
		  }
	  }
	  ASSERT_EQ("vc", Get("C"));
	  b = micros();
}

// Verify if reads from guard files are working fine after inserting large number of keys in decreasing order
TEST(DBTest, FLSMGuardsE2EInsertDescRead)  {
	  uint64_t a, b;
	  int num_values = 1000000, print_every = 100000, count = 0, value_size = 100;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  a = micros();
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 99999; i >= 0; i--) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
		count++;
	  }
	  Put("C", "vc");
	  b = micros();

	  count = 0;
	  a = micros();
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < 100000; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    std::string return_val = Get(key);
	    if (return_val.compare(value) != 0) {
	    	printf("FLSMGuardsE2EInsertDescRead :: Lookup failed for key %s\n", key);
	    }
	    ASSERT_EQ(value, return_val);
	    count++;
	  }
	  ASSERT_EQ("vc", Get("C"));
	  b = micros();
}

// Verify if reads from guard files are working fine after inserting large number of keys in random order
TEST(DBTest, FLSMGuardsE2EInsertRandomRead)  {
	  uint64_t a, b;
	  int num_values = 100000, print_every = 10000, count = 0, value_size = 1000;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  a = micros();
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int j = 0; j < 7; j++) {
		  for (int i = j; i < num_values; i+=7) {
			char key[100];
			snprintf(key, sizeof(key), "B%010d", i);
			Put(key, value);
			count++;
		  }
	  }
	  Put("C", "vc");
	  b = micros();

	  Reopen();

	  count = 0;
	  a = micros();
	  ASSERT_EQ("va", Get("A"));
	  for (int i = 0; i < num_values; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    std::string return_val = Get(key);
	    if (return_val.compare(value) != 0) {
	    	printf("FLSMGuardsE2EReadRandom :: Lookup failed for key %s\n", key);
	    }
	    ASSERT_EQ(value, return_val);
	    count++;
	  }
	  ASSERT_EQ("vc", Get("C"));
	  b = micros();
}

// Verify if reads from guard files are working fine after inserting large number of keys in increasing order
TEST(DBTest, FLSMGuardsE2EInsertRandomReadRandom)  {
	  uint64_t a, b;
	  char* test_name = "FLSMGuardsE2EInsertRandomReadRandom";
	  int num_values = 1000000, print_every = 100000, count = 0, value_size = 100;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  a = micros();
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int j = 0; j < 29; j++) {
		  for (int i = j; i < num_values; i+=29) {
			char key[100];
			snprintf(key, sizeof(key), "B%010d", i);
			Put(key, value);
			count++;
		  }
	  }
	  Put("C", "vc");
	  b = micros();

	  dbfull()->TEST_CompactMemTable();

	  Reopen();

	  a = micros();
	  ASSERT_EQ("va", Get("A"));
	  count = 0;
	  for (int j = 0; j < 61; j++) {
		  for (int i = j; i < num_values; i+=61) {
			    char key[100];
			    snprintf(key, sizeof(key), "B%010d", i);
			    std::string return_val = Get(key);
			    if (return_val.compare(value) != 0) {
			    	printf("%s :: Lookup failed for key %s !!\n", test_name, key);
			    }
			    ASSERT_EQ(value, return_val);
			    count++;
		  }
	  }
	  ASSERT_EQ("vc", Get("C"));
	  b = micros();
}


// Verify if reads from guard files are working fine after inserting large number of keys in random order
TEST(DBTest, FLSMGuardsE2EScanComplete)  {
	  int num_values = 1000000, print_every = 100000, count = 0, value_size = 100;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int j = 0; j < 1117; j++) {
		  for (int i = j; i < num_values; i+=1117) {
			char key[100];
			snprintf(key, sizeof(key), "B%010d", i);
			Put(key, value);
			count++;
		  }
	  }
	  Put("C", "vc");

	  Reopen();

	  int num_entries = VerifyIteration(print_every);
	  ASSERT_EQ(num_entries, num_values+2);
}

// Verify if scan is working fine when there are files in only one level
TEST(DBTest, FLSMGuardsE2EScanOneLevel)  {
	  int num_values = 1000000, print_every = 100000, count = 0, value_size = 1000;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 101702; i < 101802; i++) {
		char key[100];
		snprintf(key, sizeof(key), "B%010d", i);
		Put(key, value);
		count++;
	  }
	  Put("C", "vc");

	  Reopen();

	  int num_entries = VerifyIteration(print_every);
	  ASSERT_EQ(num_entries, 102);
}

bool is_valid_key_for_random_seek(int n) {
	if (n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0 || n % 57 == 0) {
		return true;
	}
	return false;
}

// Verify if seek of random keys is working fine
TEST(DBTest, FLSMGuardsE2ESeekRandom)  {
	  uint64_t a, b;
	  Random rand(1354);
	  int num_values = 1000000, print_every = 100000, count = 0, value_size = 1000;
	  int num_seeks = 10000;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  int inserted = 0;
	  for (int j = 0; j < 1237; j++) {
		  for (int i = j; i < num_values; i+=1237) {
			count++;
			if (!is_valid_key_for_random_seek(i)) {
				// Only the numbers which are multiples of at least one of 5, 7, 11 are inserted.
				continue;
			}
			inserted++;
			char key[100];
			snprintf(key, sizeof(key), "B%010d", i);
			Put(key, value);
		  }
	  }
	  Put("C", "vc");

	  Reopen();

	  int num_entries = VerifyIteration(print_every);
	  ASSERT_EQ(num_entries, inserted + 2);

	  for (int i = 0; i < num_seeks; i++) {
		  int r = rand.Next() % num_values;
		  char key[100];
		  snprintf(key, sizeof(key), "B%010d", r);
		  Iterator* it = db_->NewIterator(ReadOptions());
		  it->Seek(key);
		  int j;
		  for (j = r; j < num_values; j++) {
			  if (is_valid_key_for_random_seek(j)) {
				  break;
			  }
		  }

		  if (j == num_values) {
			ASSERT_EQ(it->key().ToString(), "C");
			ASSERT_EQ(it->value().ToString(), "vc");
		  } else {
			  char key2[100];
			  snprintf(key2, sizeof(key2), "B%010d", j);
			  ASSERT_EQ(strcmp(it->key().ToString().c_str(), key2), 0);
			  ASSERT_EQ(value, it->value().ToString());
		  }
		  delete it;
	  }

	  Iterator *it = db_->NewIterator(ReadOptions());
	  it->Seek("c");
	  ASSERT_TRUE(!it->Valid());
	  delete it;
}

// Verify if seek is working fine when there are files in only one level
TEST(DBTest, FLSMGuardsE2ESeekOneLevel)  {
	  int print_every = 100000, count = 0, value_size = 1000;
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(value_size, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 101702; i < 101802; i++) {
		char key[100];
		snprintf(key, sizeof(key), "B%010d", i);
		Put(key, value);
		count++;
	  }
	  Put("C", "vc");

	  Reopen();

	  Iterator* it = db_->NewIterator(ReadOptions());
	  int k = 500;
      char key[100];
	  snprintf(key, sizeof(key), "B%010d", k);
	  it->Seek(key);
	  ASSERT_EQ(it->key().ToString(), "B0000101702");

	  k = 101705;
	  snprintf(key, sizeof(key), "B%010d", k);
	  it->Seek(key);
	  ASSERT_EQ(it->key().ToString(), "B0000101705");

	  k = 101803;
	  snprintf(key, sizeof(key), "B%010d", k);
	  it->Seek(key);
	  ASSERT_EQ(it->key().ToString(), "C");

	  delete it;
}

// Verify if files are getting inserted into guards after reopen
TEST(DBTest, FLSMGuardsInsertReopen)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 0; i < 100000; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");

	  unsigned total_table_files, sentinel_files, guard_files;

	  Reopen();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);
}

// Verify if files are getting inserted into guards after reopen
TEST(DBTest, FLSMGuardsInsertCompactionReopen)  {
	  Options options = CurrentOptions();
	  options.compression = kNoCompression;
	  Reopen(&options);

	  const std::string value(1000, 'x');
	  Put("A", "va");
	  // Write around 100 MB of "B" values
	  for (int i = 0; i < 100000; i++) {
	    char key[100];
	    snprintf(key, sizeof(key), "B%010d", i);
	    Put(key, value);
	  }
	  Put("C", "vc");

	  unsigned total_table_files, sentinel_files, guard_files;

	  dbfull()->TEST_CompactMemTable();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);

	  Reopen();

	  total_table_files = TotalTableFiles();
	  guard_files = NumGuardFiles();
	  sentinel_files = NumSentinelFiles();

	  ASSERT_EQ(total_table_files, guard_files + sentinel_files);
}

/* Check if guards are recovered after a crash. */
TEST(DBTest, FLSMRecover) {
  Options options = CurrentOptions();
  options.compression = kNoCompression;
  Reopen(&options);
  
  const std::string value(1000, 'x');
  for (int i = 0; i < 100000; i++) {
    char key[100];
    snprintf(key, sizeof(key), "B%010d", i);
    Put(key, value);
  }
  uint64_t before = TotalGuards();
  Reopen();
  uint64_t after = TotalGuards();
  ASSERT_TRUE(before == after);
}
  
std::string MakeKey(unsigned int num) {
  char buf[30];
  snprintf(buf, sizeof(buf), "%016u", num);
  return std::string(buf);
}

void BM_LogAndApply(int iters, int num_base_files) {
  std::string dbname = test::TmpDir() + "/leveldb_test_benchmark";
  DestroyDB(dbname, Options());

  DB* db = NULL;
  Options opts;
  opts.create_if_missing = true;
  Status s = DB::Open(opts, dbname, &db);
  ASSERT_OK(s);
  ASSERT_TRUE(db != NULL);

  delete db;
  db = NULL;

  Env* env = Env::Default();

  port::Mutex mu;
  port::CondVar cv(&mu);
  bool wt;
  MutexLock l(&mu);

  // These need to be passed to LogAndApply
  std::vector<uint64_t> file_numbers;
  std::vector<std::string*> file_level_filters;

  InternalKeyComparator cmp(BytewiseComparator());
  Options options;
  FileOptions file_options(options);
  VersionSet vset(dbname, &options, &file_options, NULL, &cmp, NULL);
  ASSERT_OK(vset.Recover());
  VersionEdit vbase;
  uint64_t fnum = 1;
  for (int i = 0; i < num_base_files; i++) {
    InternalKey start(MakeKey(2*fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2*fnum+1), 1, kTypeDeletion);
    vbase.AddFile(2, fnum++, 1 /* file size */, start, limit);
  }
  ASSERT_OK(vset.LogAndApply(&vbase, &mu, &cv, &wt, file_numbers, file_level_filters, 1));

  uint64_t start_micros = env->NowMicros();

  for (int i = 0; i < iters; i++) {
    VersionEdit vedit;
    vedit.DeleteFile(2, fnum);
    InternalKey start(MakeKey(2*fnum), 1, kTypeValue);
    InternalKey limit(MakeKey(2*fnum+1), 1, kTypeDeletion);
    vedit.AddFile(2, fnum++, 1 /* file size */, start, limit);
    vset.LogAndApply(&vedit, &mu, &cv, &wt, file_numbers, file_level_filters, 1);
  }
  uint64_t stop_micros = env->NowMicros();
  unsigned int us = stop_micros - start_micros;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", num_base_files);
  fprintf(stderr,
          "BM_LogAndApply/%-6s   %8d iters : %9u us (%7.0f us / iter)\n",
          buf, iters, us, ((float)us) / iters);
}

}  // namespace leveldb

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--benchmark") {
    leveldb::BM_LogAndApply(1000, 1);
    leveldb::BM_LogAndApply(1000, 100);
    leveldb::BM_LogAndApply(1000, 10000);
    leveldb::BM_LogAndApply(100, 100000);
    return 0;
  }

  return leveldb::test::RunAllTests();
}
