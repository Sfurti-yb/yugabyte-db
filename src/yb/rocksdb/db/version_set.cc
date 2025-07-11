//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "yb/rocksdb/db/version_set.h"
#include <memory>

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <stdio.h>
#include <algorithm>
#include <map>
#include <set>
#include <climits>
#include <unordered_map>
#include <vector>
#include <string>

#include "yb/util/logging.h"
#include <boost/container/small_vector.hpp>

#include "yb/gutil/casts.h"

#include "yb/util/callsite_profiling.h"
#include "yb/util/format.h"
#include "yb/util/status_format.h"

#include "yb/rocksdb/db/filename.h"
#include "yb/rocksdb/db/file_numbers.h"
#include "yb/rocksdb/db/internal_stats.h"
#include "yb/rocksdb/db/log_reader.h"
#include "yb/rocksdb/db/log_writer.h"
#include "yb/rocksdb/db/memtable.h"
#include "yb/rocksdb/db/merge_context.h"
#include "yb/rocksdb/db/table_cache.h"
#include "yb/rocksdb/db/compaction.h"
#include "yb/rocksdb/db/version_builder.h"
#include "yb/rocksdb/db/writebuffer.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/merge_operator.h"
#include "yb/rocksdb/table/internal_iterator.h"
#include "yb/rocksdb/table/iterator_wrapper.h"
#include "yb/rocksdb/table/table_reader.h"
#include "yb/rocksdb/table/merger.h"
#include "yb/rocksdb/table/two_level_iterator.h"
#include "yb/rocksdb/table/format.h"
#include "yb/rocksdb/table/meta_blocks.h"
#include "yb/rocksdb/table/get_context.h"

#include "yb/rocksdb/util/coding.h"
#include "yb/rocksdb/util/file_reader_writer.h"
#include "yb/rocksdb/util/logging.h"
#include "yb/rocksdb/util/statistics.h"
#include "yb/rocksdb/util/stop_watch.h"

#include "yb/util/sync_point.h"
#include "yb/util/test_kill.h"
#include "yb/util/flags.h"

DEFINE_RUNTIME_bool(log_version_edits, false,
                    "Log RocksDB version edits as they are being written");

using std::unique_ptr;

namespace rocksdb {

namespace {

// Find File in LevelFilesBrief data structure
// Within an index range defined by left and right
int FindFileInRange(const InternalKeyComparator& icmp,
    const LevelFilesBrief& file_level,
    const Slice& key,
    uint32_t left,
    uint32_t right) {
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FdWithBoundaries& f = file_level.files[mid];
    if (icmp.InternalKeyComparator::Compare(f.largest.key, key) < 0) {
      // Key at "mid.largest" is < "target".  Therefore all
      // files at or before "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "mid.largest" is >= "target".  Therefore all files
      // after "mid" are uninteresting.
      right = mid;
    }
  }
  return right;
}

// Class to help choose the next file to search for the particular key.
// Searches and returns files level by level.
// We can search level-by-level since entries never hop across
// levels. Therefore we are guaranteed that if we find data
// in a smaller level, later levels are irrelevant (unless we
// are MergeInProgress).
class FilePicker {
 public:
  FilePicker(std::vector<FileMetaData*>* files, const Slice& user_key,
             const Slice& ikey, autovector<LevelFilesBrief>* file_levels,
             unsigned int num_levels, FileIndexer* file_indexer,
             const Comparator* user_comparator,
             const InternalKeyComparator* internal_comparator)
      : num_levels_(num_levels),
        curr_level_(-1),
        hit_file_level_(-1),
        search_left_bound_(0),
        search_right_bound_(FileIndexer::kLevelMaxIndex),
#ifndef NDEBUG
        files_(files),
#endif
        level_files_brief_(file_levels),
        is_hit_file_last_in_level_(false),
        user_key_(user_key),
        ikey_(ikey),
        file_indexer_(file_indexer),
        user_comparator_(user_comparator),
        internal_comparator_(internal_comparator) {
    // Setup member variables to search first level.
    search_ended_ = !PrepareNextLevel();
    if (!search_ended_) {
      // Prefetch Level 0 table data to avoid cache miss if possible.
      for (unsigned int i = 0; i < (*level_files_brief_)[0].num_files; ++i) {
        auto* r = (*level_files_brief_)[0].files[i].fd.table_reader;
        if (r) {
          r->Prepare(ikey);
        }
      }
    }
  }

  FdWithBoundaries* GetNextFile() {
    while (!search_ended_) {  // Loops over different levels.
      while (curr_index_in_curr_level_ < curr_file_level_->num_files) {
        // Loops over all files in current level.
        FdWithBoundaries* f = &curr_file_level_->files[curr_index_in_curr_level_];
        hit_file_level_ = curr_level_;
        is_hit_file_last_in_level_ =
            curr_index_in_curr_level_ == curr_file_level_->num_files - 1;
        int cmp_largest = -1;

        // Do key range filtering of files or/and fractional cascading if:
        // (1) not all the files are in level 0, or
        // (2) there are more than 3 Level 0 files
        // If there are only 3 or less level 0 files in the system, we skip
        // the key range filtering. In this case, more likely, the system is
        // highly tuned to minimize number of tables queried by each query,
        // so it is unlikely that key range filtering is more efficient than
        // querying the files.
        if (num_levels_ > 1 || curr_file_level_->num_files > 3) {
          // Check if key is within a file's range. If search left bound and
          // right bound point to the same find, we are sure key falls in
          // range.
          assert(
              curr_level_ == 0 ||
              curr_index_in_curr_level_ == start_index_in_curr_level_ ||
              user_comparator_->Compare(user_key_, f->smallest.user_key()) <= 0);

          int cmp_smallest = user_comparator_->Compare(user_key_, f->smallest.user_key());
          if (cmp_smallest >= 0) {
            cmp_largest = user_comparator_->Compare(user_key_, f->largest.user_key());
          }

          // Setup file search bound for the next level based on the
          // comparison results
          if (curr_level_ > 0) {
            file_indexer_->GetNextLevelIndex(curr_level_,
                                            curr_index_in_curr_level_,
                                            cmp_smallest, cmp_largest,
                                            &search_left_bound_,
                                            &search_right_bound_);
          }
          // Key falls out of current file's range
          if (cmp_smallest < 0 || cmp_largest > 0) {
            if (curr_level_ == 0) {
              ++curr_index_in_curr_level_;
              continue;
            } else {
              // Search next level.
              break;
            }
          }
        }
#ifndef NDEBUG
        // Sanity check to make sure that the files are correctly sorted
        if (prev_file_) {
          if (curr_level_ != 0) {
            int comp_sign = internal_comparator_->Compare(
                prev_file_->largest.key, f->smallest.key);
            assert(comp_sign < 0);
          } else {
            // level == 0, the current file cannot be newer than the previous
            // one. Use compressed data structure, has no attribute seqNo
            assert(curr_index_in_curr_level_ > 0);
            assert(!NewestFirstBySeqNo(files_[0][curr_index_in_curr_level_],
                  files_[0][curr_index_in_curr_level_-1]));
          }
        }
        prev_file_ = f;
#endif
        if (curr_level_ > 0 && cmp_largest < 0) {
          // No more files to search in this level.
          search_ended_ = !PrepareNextLevel();
        } else {
          ++curr_index_in_curr_level_;
        }
        return f;
      }
      // Start searching next level.
      search_ended_ = !PrepareNextLevel();
    }
    // Search ended.
    return nullptr;
  }

  // getter for current file level
  // for GET_HIT_L0, GET_HIT_L1 & GET_HIT_L2_AND_UP counts
  unsigned int GetHitFileLevel() { return hit_file_level_; }

  // Returns true if the most recent "hit file" (i.e., one returned by
  // GetNextFile()) is at the last index in its level.
  bool IsHitFileLastInLevel() { return is_hit_file_last_in_level_; }

 private:
  unsigned int num_levels_;
  unsigned int curr_level_;
  unsigned int hit_file_level_;
  int32_t search_left_bound_;
  int32_t search_right_bound_;
#ifndef NDEBUG
  std::vector<FileMetaData*>* files_;
#endif
  autovector<LevelFilesBrief>* level_files_brief_;
  bool search_ended_;
  bool is_hit_file_last_in_level_;
  LevelFilesBrief* curr_file_level_;
  unsigned int curr_index_in_curr_level_;
  unsigned int start_index_in_curr_level_;
  Slice user_key_;
  Slice ikey_;
  FileIndexer* file_indexer_;
  const Comparator* user_comparator_;
  const InternalKeyComparator* internal_comparator_;
#ifndef NDEBUG
  FdWithBoundaries* prev_file_;
#endif

  // Setup local variables to search next level.
  // Returns false if there are no more levels to search.
  bool PrepareNextLevel() {
    curr_level_++;
    while (curr_level_ < num_levels_) {
      curr_file_level_ = &(*level_files_brief_)[curr_level_];
      if (curr_file_level_->num_files == 0) {
        // When current level is empty, the search bound generated from upper
        // level must be [0, -1] or [0, FileIndexer::kLevelMaxIndex] if it is
        // also empty.
        assert(search_left_bound_ == 0);
        assert(search_right_bound_ == -1 ||
               search_right_bound_ == FileIndexer::kLevelMaxIndex);
        // Since current level is empty, it will need to search all files in
        // the next level
        search_left_bound_ = 0;
        search_right_bound_ = FileIndexer::kLevelMaxIndex;
        curr_level_++;
        continue;
      }

      // Some files may overlap each other. We find
      // all files that overlap user_key and process them in order from
      // newest to oldest. In the context of merge-operator, this can occur at
      // any level. Otherwise, it only occurs at Level-0 (since Put/Deletes
      // are always compacted into a single entry).
      int32_t start_index;
      if (curr_level_ == 0) {
        // On Level-0, we read through all files to check for overlap.
        start_index = 0;
      } else {
        // On Level-n (n>=1), files are sorted. Binary search to find the
        // earliest file whose largest key >= ikey. Search left bound and
        // right bound are used to narrow the range.
        if (search_left_bound_ == search_right_bound_) {
          start_index = search_left_bound_;
        } else if (search_left_bound_ < search_right_bound_) {
          if (search_right_bound_ == FileIndexer::kLevelMaxIndex) {
            search_right_bound_ =
                static_cast<int32_t>(curr_file_level_->num_files) - 1;
          }
          start_index =
              FindFileInRange(*internal_comparator_, *curr_file_level_, ikey_,
                              static_cast<uint32_t>(search_left_bound_),
                              static_cast<uint32_t>(search_right_bound_));
        } else {
          // search_left_bound > search_right_bound, key does not exist in
          // this level. Since no comparison is done in this level, it will
          // need to search all files in the next level.
          search_left_bound_ = 0;
          search_right_bound_ = FileIndexer::kLevelMaxIndex;
          curr_level_++;
          continue;
        }
      }
      start_index_in_curr_level_ = start_index;
      curr_index_in_curr_level_ = start_index;
#ifndef NDEBUG
      prev_file_ = nullptr;
#endif
      return true;
    }
    // curr_level_ = num_levels_. So, no more levels to search.
    return false;
  }
};

SstFileMetaData::BoundaryValues ConvertBoundaryValues(const FileMetaData::BoundaryValues& source) {
  SstFileMetaData::BoundaryValues result;
  result.key = source.key.user_key().ToBuffer();
  result.seqno = source.seqno;
  result.user_frontier = source.user_frontier;
  result.user_values = source.user_values;
  return result;
}

}  // anonymous namespace

VersionStorageInfo::~VersionStorageInfo() { delete[] files_; }

uint64_t VersionStorageInfo::NumFiles() const {
  uint64_t result = 0;
  for (int level = num_non_empty_levels_; level-- > 0;) {
    result += files_[level].size();
  }
  return result;
}

Version::~Version() {
  assert(refs_ == 0);

  // Remove from linked list
  prev_->next_ = next_;
  next_->prev_ = prev_;

  // Drop references to files
  for (int level = 0; level < storage_info_.num_levels_; level++) {
    for (size_t i = 0; i < storage_info_.files_[level].size(); i++) {
      FileMetaData* f = storage_info_.files_[level][i];
      assert(f->refs > 0);
      vset_->UnrefFile(cfd_, f);
    }
  }
}

int FindFile(const InternalKeyComparator& icmp,
             const LevelFilesBrief& file_level,
             const Slice& key) {
  return FindFileInRange(icmp, file_level, key, 0,
                         static_cast<uint32_t>(file_level.num_files));
}

void DoGenerateLevelFilesBrief(LevelFilesBrief* file_level,
        const std::vector<FileMetaData*>& files,
        Arena* arena) {
  assert(file_level);
  assert(arena);

  size_t num = files.size();
  file_level->num_files = num;
  char* mem = arena->AllocateAligned(num * sizeof(FdWithBoundaries));
  file_level->files = reinterpret_cast<FdWithBoundaries*>(mem);

  for (size_t i = 0; i < num; i++) {
    new (file_level->files + i) FdWithBoundaries(arena, *files[i]);
  }
}

static bool AfterFile(const Comparator* ucmp,
                      const Slice* user_key, const FdWithBoundaries* f) {
  // nullptr user_key occurs before all keys and is therefore never after *f
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

static bool BeforeFile(const Comparator* ucmp,
                       const Slice* user_key, const FdWithBoundaries* f) {
  // nullptr user_key occurs after all keys and is therefore never before *f
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

bool SomeFileOverlapsRange(
    const InternalKeyComparator& icmp,
    bool disjoint_sorted_files,
    const LevelFilesBrief& file_level,
    const Slice* smallest_user_key,
    const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    // Need to check against all files
    for (size_t i = 0; i < file_level.num_files; i++) {
      const FdWithBoundaries* f = file_level.files + i;
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return true;  // Overlap
      }
    }
    return false;
  }

  // Binary search over file list
  uint32_t index = 0;
  if (smallest_user_key != nullptr) {
    // Find the earliest possible internal key for smallest_user_key
    InternalKey small = InternalKey::MaxPossibleForUserKey(*smallest_user_key);
    index = FindFile(icmp, file_level, small.Encode());
  }

  if (index >= file_level.num_files) {
    // beginning of range is after all files, so no overlap.
    return false;
  }

  return !BeforeFile(ucmp, largest_user_key, &file_level.files[index]);
}

namespace {

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 16-byte value containing the file number and file size, both
// encoded using EncodeFixed64.
class LevelFileNumIterator final : public InternalIterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const LevelFilesBrief* flevel)
      : icmp_(icmp),
        flevel_(flevel),
        index_(static_cast<uint32_t>(flevel->num_files)),
        current_value_(0, 0, 0, 0) {  // Marks as invalid
  }

  const KeyValueEntry& Entry() const override {
    if (index_ >= flevel_->num_files) {
      return KeyValueEntry::Invalid();
    }

    auto file_meta = flevel_->files[index_];
    current_value_ = file_meta.fd;
    entry_ = KeyValueEntry {
      .key = file_meta.largest.key,
      .value = Slice(reinterpret_cast<const char*>(&current_value_), sizeof(FileDescriptor)),
    };
    return entry_;
  }

  const KeyValueEntry& Seek(Slice target) override {
    index_ = FindFile(icmp_, *flevel_, target);
    return Entry();
  }

  const KeyValueEntry& SeekToFirst() override {
    index_ = 0;
    return Entry();
  }

  const KeyValueEntry& SeekToLast() override {
    index_ = (flevel_->num_files == 0)
                 ? 0
                 : static_cast<uint32_t>(flevel_->num_files) - 1;
    return Entry();
  }

  const KeyValueEntry& Next() override {
    assert(Valid());
    index_++;
    return Entry();
  }

  const KeyValueEntry& Prev() override {
    assert(Valid());
    if (index_ == 0) {
      index_ = static_cast<uint32_t>(flevel_->num_files);  // Marks as invalid
    } else {
      index_--;
    }
    return Entry();
  }

  Status status() const override { return Status::OK(); }

 private:
  const InternalKeyComparator icmp_;
  const LevelFilesBrief* flevel_;
  uint32_t index_;
  mutable FileDescriptor current_value_;
  mutable KeyValueEntry entry_;
};

class LevelFileIteratorState : public TwoLevelIteratorState {
 public:
  // @param skip_filters Disables loading/accessing the filter block
  LevelFileIteratorState(TableCache* table_cache,
                         const ReadOptions& read_options,
                         const EnvOptions& env_options,
                         const InternalKeyComparatorPtr& icomparator,
                         HistogramImpl* file_read_hist, bool for_compaction,
                         bool prefix_enabled, bool skip_filters)
      : TwoLevelIteratorState(prefix_enabled),
        table_cache_(table_cache),
        read_options_(read_options),
        env_options_(env_options),
        icomparator_(icomparator),
        file_read_hist_(file_read_hist),
        for_compaction_(for_compaction),
        skip_filters_(skip_filters) {}

  InternalIterator* NewSecondaryIterator(const Slice& meta_handle) override {
    if (meta_handle.size() != sizeof(FileDescriptor)) {
      return NewErrorInternalIterator(
          STATUS(Corruption, "FileReader invoked with unexpected value"));
    } else {
      const FileDescriptor* fd =
          reinterpret_cast<const FileDescriptor*>(meta_handle.data());
      return table_cache_->NewIterator(
          read_options_, env_options_, icomparator_, *fd,
          Slice() /* filter */,
          nullptr /* don't need reference to table*/, file_read_hist_,
          for_compaction_, nullptr /* arena */, skip_filters_);
    }
  }

  bool PrefixMayMatch(const Slice& internal_key) override {
    return true;
  }

  bool MatchFilter(
      const IteratorFilter* filter, const QueryOptions& options, Slice user_key,
      FilterKeyCache* cache) override {
    return true;
  }

 private:
  TableCache* table_cache_;
  const ReadOptions read_options_;
  const EnvOptions& env_options_;
  InternalKeyComparatorPtr icomparator_;
  HistogramImpl* file_read_hist_;
  bool for_compaction_;
  bool skip_filters_;
};

// A wrapper of version builder which references the current version in
// constructor and unref it in the destructor.
// Both of the constructor and destructor need to be called inside DB Mutex.
class BaseReferencedVersionBuilder {
 public:
  explicit BaseReferencedVersionBuilder(ColumnFamilyData* cfd)
      : version_builder_(new VersionBuilder(
            cfd->current()->version_set()->env_options(), cfd->table_cache(),
            cfd->current()->storage_info(), cfd->ioptions()->info_log)),
        version_(cfd->current()) {
    version_->Ref();
  }
  ~BaseReferencedVersionBuilder() {
    delete version_builder_;
    version_->Unref();
  }
  VersionBuilder* version_builder() { return version_builder_; }

 private:
  VersionBuilder* version_builder_;
  Version* version_;
};
}  // anonymous namespace

Status Version::GetTableProperties(std::shared_ptr<const TableProperties>* tp,
                                   const FileMetaData* file_meta,
                                   const std::string* fname) const {
  auto table_cache = cfd_->table_cache();
  auto ioptions = cfd_->ioptions();
  Status s = table_cache->GetTableProperties(
      vset_->env_options_, cfd_->internal_comparator(), file_meta->fd,
      tp, true /* no io */);
  if (s.ok()) {
    return s;
  }

  // We only ignore error type `Incomplete` since it's by design that we
  // disallow table when it's not in table cache.
  if (!s.IsIncomplete()) {
    return s;
  }

  // 2. Table is not present in table cache, we'll read the table properties
  // directly from the properties block in the file.
  std::unique_ptr<RandomAccessFile> file;
  if (fname != nullptr) {
    s = ioptions->env->NewRandomAccessFile(
        *fname, &file, vset_->env_options_);
  } else {
    s = ioptions->env->NewRandomAccessFile(
        TableFileName(vset_->db_options_->db_paths, file_meta->fd.GetNumber(),
                      file_meta->fd.GetPathId()),
        &file, vset_->env_options_);
  }
  if (!s.ok()) {
    return s;
  }

  TableProperties* raw_table_properties;
  // By setting the magic number to kInvalidTableMagicNumber, we can by
  // pass the magic number check in the footer.
  std::unique_ptr<RandomAccessFileReader> file_reader(
      new RandomAccessFileReader(std::move(file)));
  s = ReadTableProperties(
      file_reader.get(), file_meta->fd.GetBaseFileSize(),
      Footer::kInvalidTableMagicNumber /* table's magic number */, vset_->env_,
      ioptions->info_log, &raw_table_properties);
  if (!s.ok()) {
    return s;
  }
  RecordTick(ioptions->statistics, NUMBER_DIRECT_LOAD_TABLE_PROPERTIES);

  *tp = std::shared_ptr<const TableProperties>(raw_table_properties);
  return s;
}

Status Version::GetPropertiesOfAllTables(TablePropertiesCollection* props) {
  Status s;
  for (int level = 0; level < storage_info_.num_levels_; level++) {
    s = GetPropertiesOfAllTables(props, level);
    if (!s.ok()) {
      return s;
    }
  }

  return Status::OK();
}

Status Version::GetPropertiesOfAllTables(TablePropertiesCollection* props,
                                         int level) {
  for (const auto& file_meta : storage_info_.files_[level]) {
    auto fname =
        TableFileName(vset_->db_options_->db_paths, file_meta->fd.GetNumber(),
                      file_meta->fd.GetPathId());
    // 1. If the table is already present in table cache, load table
    // properties from there.
    std::shared_ptr<const TableProperties> table_properties;
    Status s = GetTableProperties(&table_properties, file_meta, &fname);
    if (s.ok()) {
      props->insert({fname, table_properties});
    } else {
      return s;
    }
  }

  return Status::OK();
}

Status Version::GetPropertiesOfTablesInRange(
    const Range* range, std::size_t n, TablePropertiesCollection* props) const {
  for (int level = 0; level < storage_info_.num_non_empty_levels(); level++) {
    for (decltype(n) i = 0; i < n; i++) {
      // Convert user_key into a corresponding internal key.
      const InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
      const InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
      std::vector<FileMetaData*> files;
      storage_info_.GetOverlappingInputs(level, &k1, &k2, &files, -1, nullptr, false);
      for (const auto& file_meta : files) {
        auto fname =
            TableFileName(vset_->db_options_->db_paths,
                          file_meta->fd.GetNumber(), file_meta->fd.GetPathId());
        if (props->count(fname) == 0) {
          // 1. If the table is already present in table cache, load table
          // properties from there.
          std::shared_ptr<const TableProperties> table_properties;
          Status s = GetTableProperties(&table_properties, file_meta, &fname);
          if (s.ok()) {
            props->insert({fname, table_properties});
          } else {
            return s;
          }
        }
      }
    }
  }

  return Status::OK();
}

Status Version::GetAggregatedTableProperties(
    std::shared_ptr<const TableProperties>* tp, int level) {
  TablePropertiesCollection props;
  Status s;
  if (level < 0) {
    s = GetPropertiesOfAllTables(&props);
  } else {
    s = GetPropertiesOfAllTables(&props, level);
  }
  if (!s.ok()) {
    return s;
  }

  auto* new_tp = new TableProperties();
  for (const auto& item : props) {
    new_tp->Add(*item.second);
  }
  tp->reset(new_tp);
  return Status::OK();
}

size_t Version::GetMemoryUsageByTableReaders() {
  size_t total_usage = 0;
  for (auto& file_level : storage_info_.level_files_brief_) {
    for (size_t i = 0; i < file_level.num_files; i++) {
      total_usage += cfd_->table_cache()->GetMemoryUsageByTableReader(
          vset_->env_options_, cfd_->internal_comparator(),
          file_level.files[i].fd);
    }
  }
  return total_usage;
}

void Version::GetColumnFamilyMetaData(ColumnFamilyMetaData* cf_meta) {
  assert(cf_meta);
  assert(cfd_);

  cf_meta->name = cfd_->GetName();
  cf_meta->size = 0;
  cf_meta->file_count = 0;
  cf_meta->levels.clear();

  auto* ioptions = cfd_->ioptions();
  auto* vstorage = storage_info();

  for (int level = 0; level < cfd_->NumberLevels(); level++) {
    uint64_t level_size = 0;
    cf_meta->file_count += vstorage->LevelFiles(level).size();
    std::vector<SstFileMetaData> files;
    for (const auto& file : vstorage->LevelFiles(level)) {
      uint32_t path_id = file->fd.GetPathId();
      std::string file_path;
      if (path_id < ioptions->db_paths.size()) {
        file_path = ioptions->db_paths[path_id].path;
      } else {
        assert(!ioptions->db_paths.empty());
        file_path = ioptions->db_paths.back().path;
      }
      files.emplace_back(
          file->fd.GetNumber(),
          file_path,
          file->fd.GetTotalFileSize(),
          file->fd.GetBaseFileSize(),
          file->fd.GetBaseFileSize() +
              file->raw_key_size + file->raw_value_size,
          ConvertBoundaryValues(file->smallest),
          ConvertBoundaryValues(file->largest),
          file->imported,
          file->being_compacted);
      level_size += file->fd.GetTotalFileSize();
    }
    cf_meta->levels.emplace_back(
        level, level_size, std::move(files));
    cf_meta->size += level_size;
  }
}


uint64_t VersionStorageInfo::GetEstimatedActiveKeys() const {
  // Estimation will be inaccurate when:
  // (1) there exist merge keys
  // (2) keys are directly overwritten
  // (3) deletion on non-existing keys
  // (4) low number of samples
  if (current_num_samples_ == 0) {
    return 0;
  }

  if (current_num_non_deletions_ <= current_num_deletions_) {
    return 0;
  }

  uint64_t est = current_num_non_deletions_ - current_num_deletions_;

  uint64_t file_count = 0;
  for (int level = 0; level < num_levels_; ++level) {
    file_count += files_[level].size();
  }

  if (current_num_samples_ < file_count) {
    // casting to avoid overflowing
    return
      static_cast<uint64_t>(
        (est * static_cast<double>(file_count) / current_num_samples_)
      );
  } else {
    return est;
  }
}

template <class IteratorWrapperType, class... Args>
void SetupIteratorFilter(MergeIteratorInHeapBuilder<IteratorWrapperType>* builder, Args&&... args) {
  // Variable bloom filter is not supported by MergeIteratorInHeapBuilder.
  LOG_WITH_FUNC(DFATAL) << "Not supported";
}

template <class... Args>
void SetupIteratorFilter(MergeIteratorBuilder* builder, Args&&... args) {
  builder->SetupIteratorFilter(std::forward<Args>(args)...);
}

template <typename MergeIteratorBuilderType, typename CreateIteratorFunc>
void Version::AddLevel0Iterators(
    const ReadOptions& read_options,
    const EnvOptions& soptions,
    MergeIteratorBuilderType* merge_iter_builder,
    Arena* arena,
    const CreateIteratorFunc& create_iterator_func) {
  FilterKeyCache filter_key_cache(read_options.user_key_for_filter);
  auto filter_options = QueryOptions::FromReadOptions(read_options);
  bool added_iterator = false;
  for (size_t i = 0; i < storage_info_.LevelFilesBrief(0).num_files; i++) {
    const auto& file = storage_info_.LevelFilesBrief(0).files[i];
    if (!read_options.file_filter || read_options.file_filter->Filter(file)) {
      typename MergeIteratorBuilderType::IteratorType* file_iter;
      TableCache::TableReaderWithHandle trwh;
      Status s = cfd_->table_cache()->GetTableReaderForIterator(read_options, soptions,
          cfd_->internal_comparator(), file.fd, &trwh, cfd_->internal_stats()->GetFileReadHist(0),
          false);
      if (s.ok()) {
        if (!read_options.iterator_filter || read_options.defer_iterator_filter ||
            read_options.iterator_filter->Filter(
                filter_options, read_options.user_key_for_filter, &filter_key_cache,
                trwh.table_reader)) {
          file_iter = create_iterator_func(&trwh, i);
        } else {
          file_iter = nullptr;
        }
      } else {
        file_iter = NewErrorIterator<typename MergeIteratorBuilderType::IteratorType>(s, arena);
      }
      if (file_iter) {
        added_iterator = true;
        merge_iter_builder->AddIterator(file_iter);
      }
    }
  }

  if (added_iterator && read_options.defer_iterator_filter) {
    SetupIteratorFilter(merge_iter_builder, read_options.iterator_filter, filter_options);
  }
}

void Version::AddIterators(const ReadOptions& read_options,
                           const EnvOptions& soptions,
                           MergeIteratorBuilder* merge_iter_builder) {
  assert(storage_info_.finalized_);

  if (storage_info_.num_non_empty_levels() == 0) {
    // No file in the Version.
    return;
  }

  auto* arena = merge_iter_builder->GetArena();

  // Merge all level zero files together since they may overlap
  AddLevel0Iterators(
      read_options, soptions, merge_iter_builder, arena,
      [this, &read_options, arena](TableCache::TableReaderWithHandle* trwh, size_t i) {
        return cfd_->table_cache()->NewIterator(
            read_options, trwh, storage_info_.LevelFiles(0)[i]->UserFilter(), false, arena);
      });

  // For levels > 0, we can use a concatenating iterator that sequentially
  // walks through the non-overlapping files in the level, opening them
  // lazily.
  for (int level = 1; level < storage_info_.num_non_empty_levels(); level++) {
    if (storage_info_.LevelFilesBrief(level).num_files != 0) {
      auto* mem = arena->AllocateAligned(sizeof(LevelFileIteratorState));
      auto* state = new (mem)
          LevelFileIteratorState(cfd_->table_cache(), read_options, soptions,
                                 cfd_->internal_comparator(),
                                 cfd_->internal_stats()->GetFileReadHist(level),
                                 false /* for_compaction */,
                                 cfd_->ioptions()->prefix_extractor != nullptr,
                                 IsFilterSkipped(level));
      mem = arena->AllocateAligned(sizeof(LevelFileNumIterator));
      auto* first_level_iter = new (mem) LevelFileNumIterator(
          *cfd_->internal_comparator(), &storage_info_.LevelFilesBrief(level));
      merge_iter_builder->AddIterator(NewTwoLevelIterator(state, first_level_iter, arena, false));
    }
  }
}

namespace {

template <typename IndexIteratorType>
IndexIteratorType* NewIndexIterator(
    const ReadOptions& read_options, TableCache* table_cache,
    TableCache::TableReaderWithHandle* trwh);

template<>
DataBlockAwareIndexInternalIterator* NewIndexIterator<DataBlockAwareIndexInternalIterator>(
    const ReadOptions& read_options, TableCache* table_cache,
    TableCache::TableReaderWithHandle* trwh) {
  return table_cache->NewDataBlockAwareIndexIterator(read_options, trwh);
}

template<>
InternalIterator* NewIndexIterator<InternalIterator>(
    const ReadOptions& read_options, TableCache* table_cache,
    TableCache::TableReaderWithHandle* trwh) {
  return table_cache->NewIndexIterator(read_options, trwh);
}

} // namespace

template<typename MergeIteratorBuilderType>
void Version::AddIndexIterators(
    const ReadOptions& read_options, const EnvOptions& soptions,
    MergeIteratorBuilderType* merge_iter_builder) {
  DCHECK(storage_info_.finalized_);

  if (storage_info_.num_non_empty_levels() == 0) {
    return;
  }

  LOG_IF(FATAL, storage_info_.num_non_empty_levels() != 1)
      << "Only single level is supported for now by Version::AddIndexIterators.";

  // TODO(index_iter): consider using arena.
  AddLevel0Iterators(
      read_options, soptions, merge_iter_builder, /* arena = */ nullptr,
      [this, &read_options](TableCache::TableReaderWithHandle* trwh, size_t i) {
        return NewIndexIterator<typename MergeIteratorBuilderType::IteratorType>(
            read_options, cfd_->table_cache(), trwh);
      });
}

template void Version::AddIndexIterators(
    const ReadOptions& read_options, const EnvOptions& soptions,
    MergeIteratorInHeapBuilder<IteratorWrapperBase<InternalIterator, /* kSkipLastEntry = */ false>>*
        merge_iter_builder);

template void Version::AddIndexIterators(
    const ReadOptions& read_options, const EnvOptions& soptions,
    MergeIteratorInHeapBuilder<IteratorWrapperBase<InternalIterator, /* kSkipLastEntry = */ true>>*
        merge_iter_builder);

template void Version::AddIndexIterators(
    const ReadOptions& read_options, const EnvOptions& soptions,
    MergeIteratorInHeapBuilder<
        IteratorWrapperBase<DataBlockAwareIndexInternalIterator, /* kSkipLastEntry = */ false>>*
        merge_iter_builder);

template void Version::AddIndexIterators(
    const ReadOptions& read_options, const EnvOptions& soptions,
    MergeIteratorInHeapBuilder<
        IteratorWrapperBase<DataBlockAwareIndexInternalIterator, /* kSkipLastEntry = */ true>>*
        merge_iter_builder);

VersionStorageInfo::VersionStorageInfo(
    const InternalKeyComparatorPtr& internal_comparator,
    const Comparator* user_comparator, int levels,
    CompactionStyle compaction_style, VersionStorageInfo* ref_vstorage)
    : internal_comparator_(internal_comparator),
      user_comparator_(user_comparator),
      // cfd is nullptr if Version is dummy
      num_levels_(levels),
      num_non_empty_levels_(0),
      file_indexer_(user_comparator),
      compaction_style_(compaction_style),
      files_(new std::vector<FileMetaData*>[num_levels_]),
      base_level_(num_levels_ == 1 ? -1 : 1),
      files_by_compaction_pri_(num_levels_),
      level0_non_overlapping_(false),
      next_file_to_compact_by_size_(num_levels_),
      compaction_score_(num_levels_),
      compaction_level_(num_levels_),
      l0_delay_trigger_count_(0),
      accumulated_file_size_(0),
      accumulated_raw_key_size_(0),
      accumulated_raw_value_size_(0),
      accumulated_num_non_deletions_(0),
      accumulated_num_deletions_(0),
      current_num_non_deletions_(0),
      current_num_deletions_(0),
      current_num_samples_(0),
      estimated_compaction_needed_bytes_(0),
      finalized_(false) {
  if (ref_vstorage != nullptr) {
    accumulated_file_size_ = ref_vstorage->accumulated_file_size_;
    accumulated_raw_key_size_ = ref_vstorage->accumulated_raw_key_size_;
    accumulated_raw_value_size_ = ref_vstorage->accumulated_raw_value_size_;
    accumulated_num_non_deletions_ =
        ref_vstorage->accumulated_num_non_deletions_;
    accumulated_num_deletions_ = ref_vstorage->accumulated_num_deletions_;
    current_num_non_deletions_ = ref_vstorage->current_num_non_deletions_;
    current_num_deletions_ = ref_vstorage->current_num_deletions_;
    current_num_samples_ = ref_vstorage->current_num_samples_;
  }
}

Version::Version(ColumnFamilyData* column_family_data, VersionSet* vset,
                 uint64_t version_number)
    : env_(vset->env_),
      cfd_(column_family_data),
      info_log_((cfd_ == nullptr) ? nullptr : cfd_->ioptions()->info_log),
      db_statistics_((cfd_ == nullptr) ? nullptr
                                       : cfd_->ioptions()->statistics),
      table_cache_((cfd_ == nullptr) ? nullptr : cfd_->table_cache()),
      merge_operator_((cfd_ == nullptr) ? nullptr
                                        : cfd_->ioptions()->merge_operator),
      storage_info_((cfd_ == nullptr) ? nullptr : cfd_->internal_comparator(),
                    (cfd_ == nullptr) ? nullptr : cfd_->user_comparator(),
                    cfd_ == nullptr ? 0 : cfd_->NumberLevels(),
                    cfd_ == nullptr ? kCompactionStyleLevel
                                    : cfd_->ioptions()->compaction_style,
                    (cfd_ == nullptr || cfd_->current() == nullptr)
                        ? nullptr
                        : cfd_->current()->storage_info()),
      vset_(vset),
      next_(this),
      prev_(this),
      refs_(0),
      version_number_(version_number) {}

void Version::Get(const ReadOptions& read_options, const LookupKey& k,
                  std::string* value, Status* status,
                  MergeContext* merge_context, bool* value_found,
                  bool* key_exists, SequenceNumber* seq) {
  Slice ikey = k.internal_key();
  Slice user_key = k.user_key();

  assert(status->ok() || status->IsMergeInProgress());

  if (key_exists != nullptr) {
    // will falsify below if not found
    *key_exists = true;
  }

  GetContext get_context(
      user_comparator(), merge_operator_, info_log_, db_statistics_,
      status->ok() ? GetContext::kNotFound : GetContext::kMerge, user_key,
      value, value_found, merge_context, this->env_, seq);

  FilePicker fp(
      storage_info_.files_, user_key, ikey, &storage_info_.level_files_brief_,
      storage_info_.num_non_empty_levels_, &storage_info_.file_indexer_,
      user_comparator(), internal_comparator().get());
  FdWithBoundaries* f = fp.GetNextFile();
  while (f != nullptr) {
    *status = table_cache_->Get(
        read_options, internal_comparator(), f->fd, ikey, &get_context,
        cfd_->internal_stats()->GetFileReadHist(fp.GetHitFileLevel()),
        IsFilterSkipped(static_cast<int>(fp.GetHitFileLevel()),
                        fp.IsHitFileLastInLevel()));
    // TODO: examine the behavior for corrupted key
    if (!status->ok()) {
      return;
    }

    switch (get_context.State()) {
      case GetContext::kNotFound:
        // Keep searching in other files
        break;
      case GetContext::kFound:
        if (fp.GetHitFileLevel() == 0) {
          RecordTick(db_statistics_, GET_HIT_L0);
        } else if (fp.GetHitFileLevel() == 1) {
          RecordTick(db_statistics_, GET_HIT_L1);
        } else if (fp.GetHitFileLevel() >= 2) {
          RecordTick(db_statistics_, GET_HIT_L2_AND_UP);
        }
        return;
      case GetContext::kDeleted:
        // Use empty error message for speed
        *status = STATUS(NotFound, "");
        return;
      case GetContext::kCorrupt:
        *status = STATUS(Corruption, "corrupted key for ", user_key);
        return;
      case GetContext::kMerge:
        break;
    }
    f = fp.GetNextFile();
  }

  if (GetContext::kMerge == get_context.State()) {
    if (!merge_operator_) {
      *status =  STATUS(InvalidArgument,
          "merge_operator is not properly initialized.");
      return;
    }
    // merge_operands are in saver and we hit the beginning of the key history
    // do a final merge of nullptr and operands;
    if (merge_operator_->FullMerge(user_key, nullptr,
                                   merge_context->GetOperands(), value,
                                   info_log_)) {
      *status = Status::OK();
    } else {
      RecordTick(db_statistics_, NUMBER_MERGE_FAILURES);
      *status = STATUS(Corruption, "could not perform end-of-key merge for ",
                                   user_key);
    }
  } else {
    if (key_exists != nullptr) {
      *key_exists = false;
    }
    *status = STATUS(NotFound, ""); // Use an empty error message for speed
  }
}

bool Version::IsFilterSkipped(int level, bool is_file_last_in_level) {
  // Reaching the bottom level implies misses at all upper levels, so we'll
  // skip checking the filters when we predict a hit.
  return cfd_->ioptions()->optimize_filters_for_hits &&
         (level > 0 || is_file_last_in_level) &&
         level == storage_info_.num_non_empty_levels() - 1;
}

void VersionStorageInfo::GenerateLevelFilesBrief() {
  level_files_brief_.resize(num_non_empty_levels_);
  for (int level = 0; level < num_non_empty_levels_; level++) {
    DoGenerateLevelFilesBrief(
        &level_files_brief_[level], files_[level], &arena_);
  }
}

void Version::PrepareApply(
    const MutableCFOptions& mutable_cf_options,
    bool update_stats) {
  UpdateAccumulatedStats(update_stats);
  storage_info_.UpdateNumNonEmptyLevels();
  storage_info_.CalculateBaseBytes(*cfd_->ioptions(), mutable_cf_options);
  storage_info_.UpdateFilesByCompactionPri(mutable_cf_options);
  storage_info_.GenerateFileIndexer();
  storage_info_.GenerateLevelFilesBrief();
  storage_info_.GenerateLevel0NonOverlapping();
}

bool Version::MaybeInitializeFileMetaData(FileMetaData* file_meta) {
  if (file_meta->init_stats_from_file ||
      file_meta->compensated_file_size > 0) {
    return false;
  }
  std::shared_ptr<const TableProperties> tp;
  Status s = GetTableProperties(&tp, file_meta);
  file_meta->init_stats_from_file = true;
  if (!s.ok()) {
    RLOG(InfoLogLevel::ERROR_LEVEL, vset_->db_options_->info_log,
        "Unable to load table properties for file %" PRIu64 " --- %s\n",
        file_meta->fd.GetNumber(), s.ToString().c_str());
    return false;
  }
  if (tp.get() == nullptr) return false;
  file_meta->num_entries = tp->num_entries;
  file_meta->num_deletions = GetDeletedKeys(tp->user_collected_properties);
  file_meta->raw_value_size = tp->raw_value_size;
  file_meta->raw_key_size = tp->raw_key_size;

  return true;
}

void VersionStorageInfo::UpdateAccumulatedStats(FileMetaData* file_meta) {
  assert(file_meta->init_stats_from_file);
  accumulated_file_size_ += file_meta->fd.GetTotalFileSize();
  accumulated_raw_key_size_ += file_meta->raw_key_size;
  accumulated_raw_value_size_ += file_meta->raw_value_size;
  accumulated_num_non_deletions_ +=
      file_meta->num_entries - file_meta->num_deletions;
  accumulated_num_deletions_ += file_meta->num_deletions;

  current_num_non_deletions_ +=
      file_meta->num_entries - file_meta->num_deletions;
  current_num_deletions_ += file_meta->num_deletions;
  current_num_samples_++;
}

void VersionStorageInfo::RemoveCurrentStats(FileMetaData* file_meta) {
  if (file_meta->init_stats_from_file) {
    current_num_non_deletions_ -=
        file_meta->num_entries - file_meta->num_deletions;
    current_num_deletions_ -= file_meta->num_deletions;
    current_num_samples_--;
  }
}

void Version::UpdateAccumulatedStats(bool update_stats) {
  if (update_stats) {
    // maximum number of table properties loaded from files.
    const int kMaxInitCount = 20;
    int init_count = 0;
    // here only the first kMaxInitCount files which haven't been
    // initialized from file will be updated with num_deletions.
    // The motivation here is to cap the maximum I/O per Version creation.
    // The reason for choosing files from lower-level instead of higher-level
    // is that such design is able to propagate the initialization from
    // lower-level to higher-level:  When the num_deletions of lower-level
    // files are updated, it will make the lower-level files have accurate
    // compensated_file_size, making lower-level to higher-level compaction
    // will be triggered, which creates higher-level files whose num_deletions
    // will be updated here.
    for (int level = 0;
         level < storage_info_.num_levels_ && init_count < kMaxInitCount;
         ++level) {
      for (auto* file_meta : storage_info_.files_[level]) {
        if (MaybeInitializeFileMetaData(file_meta)) {
          // each FileMeta will be initialized only once.
          storage_info_.UpdateAccumulatedStats(file_meta);
          if (++init_count >= kMaxInitCount) {
            break;
          }
        }
      }
    }
    // In case all sampled-files contain only deletion entries, then we
    // load the table-property of a file in higher-level to initialize
    // that value.
    for (int level = storage_info_.num_levels_ - 1;
         storage_info_.accumulated_raw_value_size_ == 0 && level >= 0;
         --level) {
      for (int i = static_cast<int>(storage_info_.files_[level].size()) - 1;
           storage_info_.accumulated_raw_value_size_ == 0 && i >= 0; --i) {
        if (MaybeInitializeFileMetaData(storage_info_.files_[level][i])) {
          storage_info_.UpdateAccumulatedStats(storage_info_.files_[level][i]);
        }
      }
    }
  }

  storage_info_.ComputeCompensatedSizes();
}

void VersionStorageInfo::ComputeCompensatedSizes() {
  static const int kDeletionWeightOnCompaction = 2;
  uint64_t average_value_size = GetAverageValueSize();

  // compute the compensated size
  for (int level = 0; level < num_levels_; level++) {
    for (auto* file_meta : files_[level]) {
      // Here we only compute compensated_file_size for those file_meta
      // which compensated_file_size is uninitialized (== 0). This is true only
      // for files that have been created right now and no other thread has
      // access to them. That's why we can safely mutate compensated_file_size.
      if (file_meta->compensated_file_size == 0) {
        file_meta->compensated_file_size = file_meta->fd.GetTotalFileSize();
        // Here we only boost the size of deletion entries of a file only
        // when the number of deletion entries is greater than the number of
        // non-deletion entries in the file.  The motivation here is that in
        // a stable workload, the number of deletion entries should be roughly
        // equal to the number of non-deletion entries.  If we compensate the
        // size of deletion entries in a stable workload, the deletion
        // compensation logic might introduce unwanted effet which changes the
        // shape of LSM tree.
        if (file_meta->num_deletions * 2 >= file_meta->num_entries) {
          file_meta->compensated_file_size +=
              (file_meta->num_deletions * 2 - file_meta->num_entries) *
              average_value_size * kDeletionWeightOnCompaction;
        }
      }
    }
  }
}

int VersionStorageInfo::MaxInputLevel() const {
  if (compaction_style_ == kCompactionStyleLevel) {
    return num_levels() - 2;
  }
  return 0;
}

void VersionStorageInfo::EstimateCompactionBytesNeeded(
    const MutableCFOptions& mutable_cf_options) {
  // Only implemented for level-based compaction
  if (compaction_style_ != kCompactionStyleLevel) {
    estimated_compaction_needed_bytes_ = 0;
    return;
  }

  // Start from Level 0, if level 0 qualifies compaction to level 1,
  // we estimate the size of compaction.
  // Then we move on to the next level and see whether it qualifies compaction
  // to the next level. The size of the level is estimated as the actual size
  // on the level plus the input bytes from the previous level if there is any.
  // If it exceeds, take the exceeded bytes as compaction input and add the size
  // of the compaction size to tatal size.
  // We keep doing it to Level 2, 3, etc, until the last level and return the
  // accumulated bytes.

  uint64_t bytes_compact_to_next_level = 0;
  // Level 0
  bool level0_compact_triggered = false;
  if (static_cast<int>(files_[0].size()) >
      mutable_cf_options.level0_file_num_compaction_trigger) {
    level0_compact_triggered = true;
    for (auto* f : files_[0]) {
      bytes_compact_to_next_level += f->fd.GetTotalFileSize();
    }
    estimated_compaction_needed_bytes_ = bytes_compact_to_next_level;
  } else {
    estimated_compaction_needed_bytes_ = 0;
  }

  // Level 1 and up.
  for (int level = base_level(); level <= MaxInputLevel(); level++) {
    uint64_t level_size = 0;
    for (auto* f : files_[level]) {
      level_size += f->fd.GetTotalFileSize();
    }
    if (level == base_level() && level0_compact_triggered) {
      // Add base level size to compaction if level0 compaction triggered.
      estimated_compaction_needed_bytes_ += level_size;
    }
    // Add size added by previous compaction
    level_size += bytes_compact_to_next_level;
    bytes_compact_to_next_level = 0;
    uint64_t level_target = MaxBytesForLevel(level);
    if (level_size > level_target) {
      bytes_compact_to_next_level = level_size - level_target;
      // Simplify to assume the actual compaction fan-out ratio is always
      // mutable_cf_options.max_bytes_for_level_multiplier.
      estimated_compaction_needed_bytes_ +=
          bytes_compact_to_next_level *
          (1 + mutable_cf_options.max_bytes_for_level_multiplier);
    }
  }
}

void VersionStorageInfo::ComputeCompactionScore(
    const MutableCFOptions& mutable_cf_options,
    const CompactionOptionsFIFO& compaction_options_fifo) {
  for (int level = 0; level <= MaxInputLevel(); level++) {
    double score;
    if (level == 0) {
      // We treat level-0 specially by bounding the number of files
      // instead of number of bytes for two reasons:
      //
      // (1) With larger write-buffer sizes, it is nice not to do too
      // many level-0 compactions.
      //
      // (2) The files in level-0 are merged on every read and
      // therefore we wish to avoid too many files when the individual
      // file size is small (perhaps because of a small write-buffer
      // setting, or very high compression ratios, or lots of
      // overwrites/deletions).
      int num_sorted_runs = 0;
      uint64_t total_size = 0;
      for (auto* f : files_[level]) {
        if (!f->being_compacted) {
          total_size += f->compensated_file_size;
          num_sorted_runs++;
        }
      }
      if (compaction_style_ == kCompactionStyleUniversal) {
        // For universal compaction, we use level0 score to indicate
        // compaction score for the whole DB. Adding other levels as if
        // they are L0 files.
        for (int i = 1; i < num_levels(); i++) {
          if (!files_[i].empty() && !files_[i][0]->being_compacted) {
            num_sorted_runs++;
          }
        }
      }

      if (compaction_style_ == kCompactionStyleFIFO) {
        score = static_cast<double>(total_size) /
                compaction_options_fifo.max_table_files_size;
      } else {
        score = static_cast<double>(num_sorted_runs) /
                mutable_cf_options.level0_file_num_compaction_trigger;
      }
    } else {
      // Compute the ratio of current size to size limit.
      uint64_t level_bytes_no_compacting = 0;
      for (auto f : files_[level]) {
        if (!f->being_compacted) {
          level_bytes_no_compacting += f->compensated_file_size;
        }
      }
      score = static_cast<double>(level_bytes_no_compacting) /
              MaxBytesForLevel(level);
    }
    compaction_level_[level] = level;
    compaction_score_[level] = score;
  }

  // sort all the levels based on their score. Higher scores get listed
  // first. Use bubble sort because the number of entries are small.
  for (int i = 0; i < num_levels() - 2; i++) {
    for (int j = i + 1; j < num_levels() - 1; j++) {
      if (compaction_score_[i] < compaction_score_[j]) {
        double score = compaction_score_[i];
        int level = compaction_level_[i];
        compaction_score_[i] = compaction_score_[j];
        compaction_level_[i] = compaction_level_[j];
        compaction_score_[j] = score;
        compaction_level_[j] = level;
      }
    }
  }
  ComputeFilesMarkedForCompaction();
  EstimateCompactionBytesNeeded(mutable_cf_options);
}

void VersionStorageInfo::ComputeFilesMarkedForCompaction() {
  files_marked_for_compaction_.clear();
  int last_qualify_level = 0;

  // Do not include files from the last level with data
  // If table properties collector suggests a file on the last level,
  // we should not move it to a new level.
  for (int level = num_levels() - 1; level >= 1; level--) {
    if (!files_[level].empty()) {
      last_qualify_level = level - 1;
      break;
    }
  }

  for (int level = 0; level <= last_qualify_level; level++) {
    for (auto* f : files_[level]) {
      if (!f->being_compacted && f->marked_for_compaction) {
        files_marked_for_compaction_.emplace_back(level, f);
      }
    }
  }
}

namespace {

// used to sort files by size
struct Fsize {
  size_t index;
  FileMetaData* file;
};

// Compator that is used to sort files based on their size
// In normal mode: descending size
bool CompareCompensatedSizeDescending(const Fsize& first, const Fsize& second) {
  return (first.file->compensated_file_size >
      second.file->compensated_file_size);
}
} // anonymous namespace

void VersionStorageInfo::AddFile(int level, FileMetaData* f, Logger* info_log) {
  auto* level_files = &files_[level];
  // Must not overlap
#ifndef NDEBUG
  if (level > 0 && !level_files->empty() &&
      internal_comparator_->Compare(
          (*level_files)[level_files->size() - 1]->largest.key, f->smallest.key) >= 0) {
    auto* f2 = (*level_files)[level_files->size() - 1];
    if (info_log != nullptr) {
      RERROR(info_log, "Adding new file %" PRIu64
                      " range (%s, %s) to level %d but overlapping "
                      "with existing file %" PRIu64 " %s %s",
            f->fd.GetNumber(),
            f->smallest.key.DebugString(true).c_str(),
            f->largest.key.DebugString(true).c_str(),
            level,
            f2->fd.GetNumber(),
            f2->smallest.key.DebugString(true).c_str(),
            f2->largest.key.DebugString(true).c_str());
      LogFlush(info_log);
    }
    assert(false);
  }
#endif
  f->refs++;
  level_files->push_back(f);
}

// Version::PrepareApply() need to be called before calling the function, or
// following functions called:
// 1. UpdateNumNonEmptyLevels();
// 2. CalculateBaseBytes();
// 3. UpdateFilesByCompactionPri();
// 4. GenerateFileIndexer();
// 5. GenerateLevelFilesBrief();
// 6. GenerateLevel0NonOverlapping();
void VersionStorageInfo::SetFinalized() {
  finalized_ = true;
#ifndef NDEBUG
  if (compaction_style_ != kCompactionStyleLevel) {
    // Not level based compaction.
    return;
  }
  assert(base_level_ < 0 || num_levels() == 1 ||
         (base_level_ >= 1 && base_level_ < num_levels()));
  // Verify all levels newer than base_level are empty except L0
  for (int level = 1; level < base_level(); level++) {
    assert(NumLevelBytes(level) == 0);
  }
  uint64_t max_bytes_prev_level = 0;
  for (int level = base_level(); level < num_levels() - 1; level++) {
    if (LevelFiles(level).size() == 0) {
      continue;
    }
    assert(MaxBytesForLevel(level) >= max_bytes_prev_level);
    max_bytes_prev_level = MaxBytesForLevel(level);
  }
  for (int level = 0; level < num_levels(); level++) {
    assert(LevelFiles(level).size() == 0 ||
           LevelFiles(level).size() == LevelFilesBrief(level).num_files);
    if (LevelFiles(level).size() > 0) {
      assert(level < num_non_empty_levels());
    }
  }
  assert(compaction_level_.size() > 0);
  assert(compaction_level_.size() == compaction_score_.size());
#endif
}

void VersionStorageInfo::UpdateNumNonEmptyLevels() {
  num_non_empty_levels_ = num_levels_;
  for (int i = num_levels_ - 1; i >= 0; i--) {
    if (files_[i].size() != 0) {
      return;
    } else {
      num_non_empty_levels_ = i;
    }
  }
}

namespace {
// Sort `temp` based on ratio of overlapping size over file size
void SortFileByOverlappingRatio(
    const InternalKeyComparator& icmp, const std::vector<FileMetaData*>& files,
    const std::vector<FileMetaData*>& next_level_files,
    std::vector<Fsize>* temp) {
  std::unordered_map<uint64_t, uint64_t> file_to_order;
  auto next_level_it = next_level_files.begin();

  for (auto& file : files) {
    uint64_t overlapping_bytes = 0;
    // Skip files in next level that is smaller than current file
    while (next_level_it != next_level_files.end() &&
           icmp.Compare((*next_level_it)->largest.key, file->smallest.key) < 0) {
      next_level_it++;
    }

    while (next_level_it != next_level_files.end() &&
           icmp.Compare((*next_level_it)->smallest.key, file->largest.key) < 0) {
      overlapping_bytes += (*next_level_it)->fd.total_file_size;

      if (icmp.Compare((*next_level_it)->largest.key, file->largest.key) > 0) {
        // next level file cross large boundary of current file.
        break;
      }
      next_level_it++;
    }

    assert(file->fd.total_file_size != 0);
    file_to_order[file->fd.GetNumber()] =
        overlapping_bytes * 1024u / file->fd.total_file_size;
  }

  std::sort(temp->begin(), temp->end(),
            [&](const Fsize& f1, const Fsize& f2) -> bool {
              return file_to_order[f1.file->fd.GetNumber()] <
                     file_to_order[f2.file->fd.GetNumber()];
            });
}
}  // namespace

void VersionStorageInfo::UpdateFilesByCompactionPri(
    const MutableCFOptions& mutable_cf_options) {
  if (compaction_style_ == kCompactionStyleFIFO ||
      compaction_style_ == kCompactionStyleUniversal) {
    // don't need this
    return;
  }
  // No need to sort the highest level because it is never compacted.
  for (int level = 0; level < num_levels() - 1; level++) {
    const std::vector<FileMetaData*>& files = files_[level];
    auto& files_by_compaction_pri = files_by_compaction_pri_[level];
    assert(files_by_compaction_pri.size() == 0);

    // populate a temp vector for sorting based on size
    std::vector<Fsize> temp(files.size());
    for (size_t i = 0; i < files.size(); i++) {
      temp[i].index = i;
      temp[i].file = files[i];
    }

    // sort the top number_of_files_to_sort_ based on file size
    size_t num = VersionStorageInfo::kNumberFilesToSort;
    if (num > temp.size()) {
      num = temp.size();
    }
    switch (mutable_cf_options.compaction_pri) {
      case kByCompensatedSize:
        std::partial_sort(temp.begin(), temp.begin() + num, temp.end(),
                          CompareCompensatedSizeDescending);
        break;
      case kOldestLargestSeqFirst:
        std::sort(temp.begin(), temp.end(),
                  [](const Fsize& f1, const Fsize& f2) -> bool {
                    return f1.file->largest.seqno < f2.file->largest.seqno;
                  });
        break;
      case kOldestSmallestSeqFirst:
        std::sort(temp.begin(), temp.end(),
                  [](const Fsize& f1, const Fsize& f2) -> bool {
                    return f1.file->smallest.seqno < f2.file->smallest.seqno;
                  });
        break;
      case kMinOverlappingRatio:
        SortFileByOverlappingRatio(*internal_comparator_, files_[level],
                                   files_[level + 1], &temp);
        break;
      default:
        assert(false);
    }
    assert(temp.size() == files.size());

    // initialize files_by_compaction_pri_
    for (size_t i = 0; i < temp.size(); i++) {
      files_by_compaction_pri.push_back(static_cast<int>(temp[i].index));
    }
    next_file_to_compact_by_size_[level] = 0;
    assert(files_[level].size() == files_by_compaction_pri_[level].size());
  }
}

void VersionStorageInfo::GenerateLevel0NonOverlapping() {
  assert(!finalized_);
  level0_non_overlapping_ = true;
  if (level_files_brief_.size() == 0) {
    return;
  }

  // A copy of L0 files sorted by smallest key
  std::vector<FdWithBoundaries> level0_sorted_file(
      level_files_brief_[0].files,
      level_files_brief_[0].files + level_files_brief_[0].num_files);
  sort(level0_sorted_file.begin(), level0_sorted_file.end(),
       [this](const FdWithBoundaries& f1, const FdWithBoundaries& f2)->bool {
    return (internal_comparator_->Compare(f1.smallest.key, f2.smallest.key) < 0);
  });

  for (size_t i = 1; i < level0_sorted_file.size(); ++i) {
    auto& f = level0_sorted_file[i];
    auto& prev = level0_sorted_file[i - 1];
    if (internal_comparator_->Compare(prev.largest.key, f.smallest.key) >= 0) {
      level0_non_overlapping_ = false;
      break;
    }
  }
}

void Version::Ref() {
  ++refs_;
}

bool Version::Unref() {
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
    return true;
  }
  return false;
}

bool VersionStorageInfo::OverlapInLevel(int level,
                                        const Slice* smallest_user_key,
                                        const Slice* largest_user_key) {
  if (level >= num_non_empty_levels_) {
    // empty level, no overlap
    return false;
  }
  return SomeFileOverlapsRange(*internal_comparator_, (level > 0),
                               level_files_brief_[level], smallest_user_key,
                               largest_user_key);
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
// If hint_index is specified, then it points to a file in the
// overlapping range.
// The file_index returns a pointer to any file in an overlapping range.
void VersionStorageInfo::GetOverlappingInputs(
    int level, const InternalKey* begin, const InternalKey* end,
    std::vector<FileMetaData*>* inputs, int hint_index, int* file_index,
    bool expand_range) const {
  if (level >= num_non_empty_levels_) {
    // this level is empty, no overlapping inputs
    return;
  }

  inputs->clear();
  Slice user_begin, user_end;
  if (begin != nullptr) {
    user_begin = begin->user_key();
  }
  if (end != nullptr) {
    user_end = end->user_key();
  }
  if (file_index) {
    *file_index = -1;
  }
  const Comparator* user_cmp = user_comparator_;
  if (begin != nullptr && end != nullptr && level > 0) {
    GetOverlappingInputsBinarySearch(level, user_begin, user_end, inputs,
      hint_index, file_index);
    return;
  }
  for (size_t i = 0; i < level_files_brief_[level].num_files; ) {
    auto* f = &(level_files_brief_[level].files[i++]);
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      inputs->push_back(files_[level][i-1]);
      if (level == 0 && expand_range) {
        // Level-0 files may overlap each other.  So check if the newly
        // added file has expanded the range.  If so, restart search.
        if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != nullptr
            && user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      } else if (file_index) {
        *file_index = static_cast<int>(i) - 1;
      }
    }
  }
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
// Employ binary search to find at least one file that overlaps the
// specified range. From that file, iterate backwards and
// forwards to find all overlapping files.
void VersionStorageInfo::GetOverlappingInputsBinarySearch(
    int level, const Slice& user_begin, const Slice& user_end,
    std::vector<FileMetaData*>* inputs, int hint_index, int* file_index) const {
  assert(level > 0);
  int min = 0;
  int mid = 0;
  int max = static_cast<int>(files_[level].size()) - 1;
  bool foundOverlap = false;
  const Comparator* user_cmp = user_comparator_;

  // if the caller already knows the index of a file that has overlap,
  // then we can skip the binary search.
  if (hint_index != -1) {
    mid = hint_index;
    foundOverlap = true;
  }

  while (!foundOverlap && min <= max) {
    mid = (min + max)/2;
    FdWithBoundaries* f = level_files_brief_[level].files + mid;
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (user_cmp->Compare(file_limit, user_begin) < 0) {
      min = mid + 1;
    } else if (user_cmp->Compare(user_end, file_start) < 0) {
      max = mid - 1;
    } else {
      foundOverlap = true;
      break;
    }
  }

  // If there were no overlapping files, return immediately.
  if (!foundOverlap) {
    return;
  }
  // returns the index where an overlap is found
  if (file_index) {
    *file_index = mid;
  }
  ExtendOverlappingInputs(level, user_begin, user_end, inputs, mid);
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
// The midIndex specifies the index of at least one file that
// overlaps the specified range. From that file, iterate backward
// and forward to find all overlapping files.
// Use FileLevel in searching, make it faster
void VersionStorageInfo::ExtendOverlappingInputs(
    int level, const Slice& user_begin, const Slice& user_end,
    std::vector<FileMetaData*>* inputs, unsigned int midIndex) const {
  const Comparator* user_cmp = user_comparator_;
  const FdWithBoundaries* files = level_files_brief_[level].files;
#ifndef NDEBUG
  {
    // assert that the file at midIndex overlaps with the range
    assert(midIndex < level_files_brief_[level].num_files);
    const auto* f = &files[midIndex];
    const Slice fstart = f->smallest.user_key();
    const Slice flimit = f->largest.user_key();
    if (user_cmp->Compare(fstart, user_begin) >= 0) {
      assert(user_cmp->Compare(fstart, user_end) <= 0);
    } else {
      assert(user_cmp->Compare(flimit, user_begin) >= 0);
    }
  }
#endif
  int startIndex = midIndex + 1;
  int endIndex = midIndex;
  int count __attribute__((unused)) = 0;

  // check backwards from 'mid' to lower indices
  for (int i = midIndex; i >= 0 ; i--) {
    const auto* f = &files[i];
    const Slice file_limit = f->largest.user_key();
    if (user_cmp->Compare(file_limit, user_begin) >= 0) {
      startIndex = i;
      assert((count++, true));
    } else {
      break;
    }
  }
  // check forward from 'mid+1' to higher indices
  for (unsigned int i = midIndex+1;
       i < level_files_brief_[level].num_files; i++) {
    const auto* f = &files[i];
    const Slice file_start = f->smallest.user_key();
    if (user_cmp->Compare(file_start, user_end) <= 0) {
      assert((count++, true));
      endIndex = i;
    } else {
      break;
    }
  }
  assert(count == endIndex - startIndex + 1);

  // insert overlapping files into vector
  for (int i = startIndex; i <= endIndex; i++) {
    FileMetaData* f = files_[level][i];
    inputs->push_back(f);
  }
}

// Returns true iff the first or last file in inputs contains
// an overlapping user key to the file "just outside" of it (i.e.
// just after the last file, or just before the first file)
// REQUIRES: "*inputs" is a sorted list of non-overlapping files
bool VersionStorageInfo::HasOverlappingUserKey(
    const std::vector<FileMetaData*>* inputs, int level) {

  // If inputs empty, there is no overlap.
  // If level == 0, it is assumed that all needed files were already included.
  if (inputs->empty() || level == 0) {
    return false;
  }

  const Comparator* user_cmp = user_comparator_;
  const rocksdb::LevelFilesBrief& file_level = level_files_brief_[level];
  const FdWithBoundaries* files = level_files_brief_[level].files;
  const size_t kNumFiles = file_level.num_files;

  // Check the last file in inputs against the file after it
  size_t last_file = FindFile(*internal_comparator_, file_level,
                              inputs->back()->largest.key.Encode());
  assert(last_file < kNumFiles);  // File should exist!
  if (last_file < kNumFiles-1) {                    // If not the last file
    const Slice last_key_in_input = files[last_file].largest.user_key();
    const Slice first_key_after = files[last_file+1].smallest.user_key();
    if (user_cmp->Equal(last_key_in_input, first_key_after)) {
      // The last user key in input overlaps with the next file's first key
      return true;
    }
  }

  // Check the first file in inputs against the file just before it
  size_t first_file = FindFile(*internal_comparator_, file_level,
                               inputs->front()->smallest.key.Encode());
  assert(first_file <= last_file);   // File should exist!
  if (first_file > 0) {                                 // If not first file
    const Slice& first_key_in_input = files[first_file].smallest.user_key();
    const Slice& last_key_before = files[first_file-1].largest.user_key();
    if (user_cmp->Equal(first_key_in_input, last_key_before)) {
      // The first user key in input overlaps with the previous file's last key
      return true;
    }
  }

  return false;
}

uint64_t VersionStorageInfo::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < num_levels());
  return TotalFileSize(files_[level]);
}

const char* VersionStorageInfo::LevelSummary(
    LevelSummaryStorage* scratch) const {
  int len = 0;
  if (compaction_style_ == kCompactionStyleLevel && num_levels() > 1) {
    assert(base_level_ < static_cast<int>(level_max_bytes_.size()));
    len = snprintf(scratch->buffer, sizeof(scratch->buffer),
                   "base level %d max bytes base %" PRIu64 " ", base_level_,
                   level_max_bytes_[base_level_]);
  }
  len +=
      snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len, "files[");
  for (int i = 0; i < num_levels(); i++) {
    int sz = sizeof(scratch->buffer) - len;
    int ret = snprintf(scratch->buffer + len, sz, "%zd ", files_[i].size());
    if (ret < 0 || ret >= sz) break;
    len += ret;
  }
  if (len > 0) {
    // overwrite the last space
    --len;
  }
  len += snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len,
                  "] max score %.2f", compaction_score_[0]);

  if (!files_marked_for_compaction_.empty()) {
    snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len,
             " (%" ROCKSDB_PRIszt " files need compaction)",
             files_marked_for_compaction_.size());
  }

  return scratch->buffer;
}

const char* VersionStorageInfo::LevelFileSummary(FileSummaryStorage* scratch,
                                                 int level) const {
  int len = snprintf(scratch->buffer, sizeof(scratch->buffer), "files_size[");
  for (const auto& f : files_[level]) {
    int sz = sizeof(scratch->buffer) - len;
    char sztxt[16];
    AppendHumanBytes(f->fd.GetTotalFileSize(), sztxt, sizeof(sztxt));
    int ret = snprintf(scratch->buffer + len, sz,
                       "#%" PRIu64 "(seq=%" PRIu64 ",sz=%s,%d) ",
                       f->fd.GetNumber(), f->smallest.seqno, sztxt,
                       static_cast<int>(f->being_compacted));
    if (ret < 0 || ret >= sz)
      break;
    len += ret;
  }
  // overwrite the last space (only if files_[level].size() is non-zero)
  if (files_[level].size() && len > 0) {
    --len;
  }
  snprintf(scratch->buffer + len, sizeof(scratch->buffer) - len, "]");
  return scratch->buffer;
}

int64_t VersionStorageInfo::MaxNextLevelOverlappingBytes() {
  uint64_t result = 0;
  std::vector<FileMetaData*> overlaps;
  for (int level = 1; level < num_levels() - 1; level++) {
    for (const auto& f : files_[level]) {
      GetOverlappingInputs(level + 1, &f->smallest.key, &f->largest.key, &overlaps);
      const uint64_t sum = TotalFileSize(overlaps);
      if (sum > result) {
        result = sum;
      }
    }
  }
  return result;
}

uint64_t VersionStorageInfo::MaxBytesForLevel(int level) const {
  // Note: the result for level zero is not really used since we set
  // the level-0 compaction threshold based on number of files.
  assert(level >= 0);
  assert(level < static_cast<int>(level_max_bytes_.size()));
  return level_max_bytes_[level];
}

void VersionStorageInfo::CalculateBaseBytes(const ImmutableCFOptions& ioptions,
                                            const MutableCFOptions& options) {
  // Special logic to set number of sorted runs.
  // It is to match the previous behavior when all files are in L0.
  int num_l0_count = 0;
  if (options.MaxFileSizeForCompaction() == std::numeric_limits<uint64_t>::max()) {
    num_l0_count = static_cast<int>(files_[0].size());
  } else {
    for (const auto& file : files_[0]) {
      if (file->fd.GetTotalFileSize() <= options.MaxFileSizeForCompaction()) {
        ++num_l0_count;
      }
    }
  }
  if (compaction_style_ == kCompactionStyleUniversal) {
    // For universal compaction, we use level0 score to indicate
    // compaction score for the whole DB. Adding other levels as if
    // they are L0 files.
    for (int i = 1; i < num_levels(); i++) {
      if (!files_[i].empty()) {
        num_l0_count++;
      }
    }
  }
  set_l0_delay_trigger_count(num_l0_count);

  level_max_bytes_.resize(ioptions.num_levels);
  if (!ioptions.level_compaction_dynamic_level_bytes) {
    base_level_ = (ioptions.compaction_style == kCompactionStyleLevel) ? 1 : -1;

    // Calculate for static bytes base case
    for (int i = 0; i < ioptions.num_levels; ++i) {
      if (i == 0 && ioptions.compaction_style == kCompactionStyleUniversal) {
        level_max_bytes_[i] = options.max_bytes_for_level_base;
      } else if (i > 1) {
        level_max_bytes_[i] = MultiplyCheckOverflow(
            MultiplyCheckOverflow(level_max_bytes_[i - 1],
                                  options.max_bytes_for_level_multiplier),
            options.MaxBytesMultiplerAdditional(i - 1));
      } else {
        level_max_bytes_[i] = options.max_bytes_for_level_base;
      }
    }
  } else {
    uint64_t max_level_size = 0;

    int first_non_empty_level = -1;
    // Find size of non-L0 level of most data.
    // Cannot use the size of the last level because it can be empty or less
    // than previous levels after compaction.
    for (int i = 1; i < num_levels_; i++) {
      uint64_t total_size = 0;
      for (const auto& f : files_[i]) {
        total_size += f->fd.GetTotalFileSize();
      }
      if (total_size > 0 && first_non_empty_level == -1) {
        first_non_empty_level = i;
      }
      if (total_size > max_level_size) {
        max_level_size = total_size;
      }
    }

    // Prefill every level's max bytes to disallow compaction from there.
    for (int i = 0; i < num_levels_; i++) {
      level_max_bytes_[i] = std::numeric_limits<uint64_t>::max();
    }

    if (max_level_size == 0) {
      // No data for L1 and up. L0 compacts to last level directly.
      // No compaction from L1+ needs to be scheduled.
      base_level_ = num_levels_ - 1;
    } else {
      uint64_t base_bytes_max = options.max_bytes_for_level_base;
      uint64_t base_bytes_min =
          base_bytes_max / options.max_bytes_for_level_multiplier;

      // Try whether we can make last level's target size to be max_level_size
      uint64_t cur_level_size = max_level_size;
      for (int i = num_levels_ - 2; i >= first_non_empty_level; i--) {
        // Round up after dividing
        cur_level_size /= options.max_bytes_for_level_multiplier;
      }

      // Calculate base level and its size.
      uint64_t base_level_size;
      if (cur_level_size <= base_bytes_min) {
        // Case 1. If we make target size of last level to be max_level_size,
        // target size of the first non-empty level would be smaller than
        // base_bytes_min. We set it be base_bytes_min.
        base_level_size = base_bytes_min + 1U;
        base_level_ = first_non_empty_level;
        RWARN(ioptions.info_log,
            "More existing levels in DB than needed. "
                "max_bytes_for_level_multiplier may not be guaranteed.");
      } else {
        // Find base level (where L0 data is compacted to).
        base_level_ = first_non_empty_level;
        while (base_level_ > 1 && cur_level_size > base_bytes_max) {
          --base_level_;
          cur_level_size =
              cur_level_size / options.max_bytes_for_level_multiplier;
        }
        if (cur_level_size > base_bytes_max) {
          // Even L1 will be too large
          assert(base_level_ == 1);
          base_level_size = base_bytes_max;
        } else {
          base_level_size = cur_level_size;
        }
      }

      uint64_t level_size = base_level_size;
      for (int i = base_level_; i < num_levels_; i++) {
        if (i > base_level_) {
          level_size = MultiplyCheckOverflow(
              level_size, options.max_bytes_for_level_multiplier);
        }
        level_max_bytes_[i] = level_size;
      }
    }
  }
}

uint64_t VersionStorageInfo::EstimateLiveDataSize() const {
  // Estimate the live data size by adding up the size of the last level for all
  // key ranges. Note: Estimate depends on the ordering of files in level 0
  // because files in level 0 can be overlapping.
  uint64_t size = 0;

  auto ikey_lt = [this](InternalKey* x, InternalKey* y) {
    return internal_comparator_->Compare(*x, *y) < 0;
  };
  // (Ordered) map of largest keys in non-overlapping files
  std::map<InternalKey*, FileMetaData*, decltype(ikey_lt)> ranges(ikey_lt);

  for (int l = num_levels_ - 1; l >= 0; l--) {
    bool found_end = false;
    for (auto file : files_[l]) {
      // Find the first file where the largest key is larger than the smallest
      // key of the current file. If this file does not overlap with the
      // current file, none of the files in the map does. If there is
      // no potential overlap, we can safely insert the rest of this level
      // (if the level is not 0) into the map without checking again because
      // the elements in the level are sorted and non-overlapping.
      auto lb = (found_end && l != 0) ?
        ranges.end() : ranges.lower_bound(&file->smallest.key);
      found_end = (lb == ranges.end());
      if (found_end || internal_comparator_->Compare(
            file->largest.key, (*lb).second->smallest.key) < 0) {
          ranges.emplace_hint(lb, &file->largest.key, file);
          size += file->fd.total_file_size;
      }
    }
  }
  return size;
}


void Version::AddLiveFiles(std::vector<FileDescriptor>* live) {
  for (int level = 0; level < storage_info_.num_levels(); level++) {
    const std::vector<FileMetaData*>& files = storage_info_.files_[level];
    for (const auto& file : files) {
      live->push_back(file->fd);
    }
  }
}

std::string Version::DebugString(bool hex) const {
  std::string r;
  for (int level = 0; level < storage_info_.num_levels_; level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' .. 'd']
    //   20:43['e' .. 'g']
    r.append("--- level ");
    AppendNumberTo(&r, level);
    r.append(" --- version# ");
    AppendNumberTo(&r, version_number_);
    r.append(" ---\n");
    const std::vector<FileMetaData*>& files = storage_info_.files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.append(files[i]->ToString());
    }
  }
  return r;
}

namespace {

struct MiddleKeyWithSize {
  std::string middle_key;
  uint64_t size;
};

static bool compareKeys(MiddleKeyWithSize f1,
                        MiddleKeyWithSize f2) {
  return f1.middle_key.compare(f2.middle_key) > 0;
}

} // namespace

Result<std::string> Version::GetMiddleOfMiddleKeys() {
  const auto level = storage_info_.num_levels_ - 1;
  // Largest files are at lowest level.
  std::vector <MiddleKeyWithSize> sst_files;
  sst_files.reserve(storage_info_.files_[level].size());
  uint64_t total_size = 0;
  // Get middle key and file size for every file
  for (const auto* file : storage_info_.files_[level]) {
    TableCache::TableReaderWithHandle trwh = VERIFY_RESULT(table_cache_->GetTableReader(
        vset_->env_options_, cfd_->internal_comparator(), file->fd, kDefaultQueryId,
        /* no_io = */ false, cfd_->internal_stats()->GetFileReadHist(level),
        IsFilterSkipped(level, /* is_file_last_in_level = */ true)));

    const auto result_mkey = trwh.table_reader->GetMiddleKey();
    if (!result_mkey.ok()) {
      if (result_mkey.status().IsIncomplete()) {
        continue;
      }
      return result_mkey;
    }

    const auto file_size = file->fd.GetTotalFileSize();
    sst_files.push_back({*result_mkey, file_size});
    total_size += file_size;
  }

  if (sst_files.size() == 0) {
    return STATUS(Incomplete, "Either no SST file or too small SST files.");
  }

  std::sort(sst_files.begin(), sst_files.end(), compareKeys);
  uint64_t sorted_size = 0;
  // Weighted middle of middle based on file size
  for (const auto& sst_file : sst_files) {
    sorted_size += sst_file.size;
    if (sorted_size > total_size/2) {
      return sst_file.middle_key;
    }
  }
  return STATUS(InternalError, "Unexpected error state.");
}

Result<TableCache::TableReaderWithHandle> Version::GetLargestSstTableReader() {
  // Largest files are at lowest level.
  const auto level = storage_info_.num_levels_ - 1;
  const FileMetaData* largest_sst_meta = nullptr;
  for (const auto* file : storage_info_.files_[level]) {
    if (!largest_sst_meta ||
        file->fd.GetTotalFileSize() > largest_sst_meta->fd.GetTotalFileSize()) {
      largest_sst_meta = file;
    }
  }
  if (!largest_sst_meta) {
    return STATUS(Incomplete, "No SST files.");
  }

  return table_cache_->GetTableReader(
      vset_->env_options_, cfd_->internal_comparator(), largest_sst_meta->fd, kDefaultQueryId,
      /* no_io = */ false, cfd_->internal_stats()->GetFileReadHist(level),
      IsFilterSkipped(level, /* is_file_last_in_level = */ true));
}

Result<std::string> Version::GetMiddleKey() {
  return GetMiddleOfMiddleKeys();
}

Result<TableReader*> Version::TEST_GetLargestSstTableReader() {
  const auto trwh = VERIFY_RESULT(GetLargestSstTableReader());
  return trwh.table_reader;
}

// this is used to batch writes to the manifest file
struct VersionSet::ManifestWriter {
  Status status;
  bool done;
  InstrumentedCondVar cv;
  ColumnFamilyData* cfd;
  VersionEdit* edit;

  explicit ManifestWriter(InstrumentedMutex* mu, ColumnFamilyData* _cfd,
                          VersionEdit* e)
      : done(false), cv(mu), cfd(_cfd), edit(e) {}
};

constexpr uint64_t VersionSet::kInitialNextFileNumber;

VersionSet::VersionSet(const std::string& dbname, const DBOptions* db_options,
                       const EnvOptions& storage_options, Cache* table_cache,
                       WriteBuffer* write_buffer,
                       WriteController* write_controller)
    : column_family_set_(new ColumnFamilySet(
          dbname, db_options, storage_options, table_cache,
          write_buffer, write_controller)),
      env_(db_options->env),
      dbname_(dbname),
      db_options_(db_options),
      env_options_(storage_options),
      env_options_compactions_(env_options_) {}

VersionSet::~VersionSet() {
  // we need to delete column_family_set_ because its destructor depends on
  // VersionSet
  column_family_set_.reset();
  for (auto file : obsolete_files_) {
    delete file;
  }
  obsolete_files_.clear();
}

void VersionSet::AppendVersion(ColumnFamilyData* column_family_data,
                               Version* v) {
  // compute new compaction score
  v->storage_info()->ComputeCompactionScore(
      *column_family_data->GetLatestMutableCFOptions(),
      column_family_data->ioptions()->compaction_options_fifo);

  // Mark v finalized
  v->storage_info_.SetFinalized();

  // Make "v" current
  assert(v->refs_ == 0);
  Version* current = column_family_data->current();
  assert(v != current);
  if (current != nullptr) {
    assert(current->refs_ > 0);
    current->Unref();
  }
  column_family_data->SetCurrent(v);
  v->Ref();

  // Append to linked list
  v->prev_ = column_family_data->dummy_versions()->prev_;
  v->next_ = column_family_data->dummy_versions();
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(ColumnFamilyData* column_family_data,
                               const MutableCFOptions& mutable_cf_options,
                               VersionEdit* edit, InstrumentedMutex* mu,
                               Directory* db_directory, bool new_descriptor_log,
                               const ColumnFamilyOptions* new_cf_options) {
  mu->AssertHeld();

  // column_family_data can be nullptr only if this is column_family_add.
  // in that case, we also need to specify ColumnFamilyOptions
  if (column_family_data == nullptr) {
    assert(edit->column_family_name_);
    assert(new_cf_options != nullptr);
  }

  // queue our request
  ManifestWriter w(mu, column_family_data, edit);
  manifest_writers_.push_back(&w);
  while (!w.done && &w != manifest_writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }
  if (column_family_data != nullptr && column_family_data->IsDropped()) {
    // if column family is dropped by the time we get here, no need to write
    // anything to the manifest
    manifest_writers_.pop_front();
    // Notify new head of write queue
    if (!manifest_writers_.empty()) {
      YB_PROFILE(manifest_writers_.front()->cv.Signal());
    }
    // we steal this code to also inform about cf-drop
    return STATUS(ShutdownInProgress, "");
  }

  std::vector<VersionEdit*> batch_edits;
  Version* v = nullptr;
  std::unique_ptr<BaseReferencedVersionBuilder> builder_guard(nullptr);

  // Process all requests in the queue.
  ManifestWriter* last_writer = &w;
  assert(!manifest_writers_.empty());
  assert(manifest_writers_.front() == &w);

  UserFrontierPtr flushed_frontier_override;
  if (edit->IsColumnFamilyManipulation()) {
    // No group commits for column family add or drop.
    LogAndApplyCFHelper(edit);
    batch_edits.push_back(edit);
  } else {
    v = new Version(column_family_data, this, current_version_number_++);
    builder_guard.reset(new BaseReferencedVersionBuilder(column_family_data));
    auto* builder = builder_guard->version_builder();
    for (const auto& writer : manifest_writers_) {
      if (writer->edit->IsColumnFamilyManipulation() ||
          writer->cfd->GetID() != column_family_data->GetID()) {
        // No group commits for column family add or drop.
        // Also, group commits across column families are not supported.
        break;
      }
      FrontierModificationMode frontier_mode = FrontierModificationMode::kUpdate;
      const bool force_flushed_frontier = writer->edit->force_flushed_frontier_;
      if (force_flushed_frontier) {
        if (writer != &w) {
          // No group commit for edits that force a particular value of flushed frontier, either.
          // (Also see the logic at the end of the for loop body.)
          break;
        }
        new_descriptor_log = true;
        flushed_frontier_override = edit->flushed_frontier_;
      }
      last_writer = writer;
      LogAndApplyHelper(column_family_data, builder, last_writer->edit, mu);
      batch_edits.push_back(last_writer->edit);

      if (force_flushed_frontier) {
        // This is also needed to disable group commit for flushed-frontier-forcing edits.
        break;
      }
    }

    builder->SaveTo(v->storage_info());
  }

  // Initialize new descriptor log file if necessary by creating
  // a temporary file that contains a snapshot of the current version.
  uint64_t new_manifest_file_size = 0;
  Status s;

  assert(pending_manifest_file_number_ == 0);
  if (!descriptor_log_ ||
      manifest_file_size_ > db_options_->max_manifest_file_size) {
    pending_manifest_file_number_ = NewFileNumber();
    batch_edits.back()->SetNextFile(next_file_number_.load());
    new_descriptor_log = true;
  } else {
    pending_manifest_file_number_ = manifest_file_number_;
  }

  if (new_descriptor_log) {
    // If we're writing out new snapshot make sure to persist max column family.
    if (column_family_set_->GetMaxColumnFamily() > 0) {
      edit->SetMaxColumnFamily(column_family_set_->GetMaxColumnFamily());
    }
  }

  // Unlock during expensive operations. New writes cannot get here
  // because &w is ensuring that all new writes get queued.
  {
    // Before releasing mutex, make a copy of mutable_cf_options and pass to `PrepareApply` to
    // avoid a potential data race.
    MutableCFOptions mutable_cf_options_copy(mutable_cf_options);
    mu->Unlock();

    DEBUG_ONLY_TEST_SYNC_POINT("VersionSet::LogAndApply:WriteManifest");
    if (!edit->IsColumnFamilyManipulation() &&
        db_options_->max_open_files == -1) {
      // unlimited table cache. Pre-load table handle now.
      // Need to do it out of the mutex.
      builder_guard->version_builder()->LoadTableHandlers(
          column_family_data->internal_stats(),
          column_family_data->ioptions()->optimize_filters_for_hits);
    }

    // This is fine because everything inside of this block is serialized --
    // only one thread can be here at the same time.
    if (new_descriptor_log) {
      // Create a new manifest file.
      RLOG(InfoLogLevel::INFO_LEVEL, db_options_->info_log,
          "Creating manifest %" PRIu64 "\n", pending_manifest_file_number_);
      unique_ptr<WritableFile> descriptor_file;
      EnvOptions opt_env_opts = env_->OptimizeForManifestWrite(env_options_);
      descriptor_log_file_name_ = DescriptorFileName(dbname_, pending_manifest_file_number_);
      s = NewWritableFile(
          env_, descriptor_log_file_name_,
          &descriptor_file, opt_env_opts);
      if (s.ok()) {
        descriptor_file->SetPreallocationBlockSize(
            db_options_->manifest_preallocation_size);

        unique_ptr<WritableFileWriter> file_writer(
            new WritableFileWriter(std::move(descriptor_file), opt_env_opts));
        descriptor_log_.reset(new log::Writer(
            std::move(file_writer), /* log_number */ 0, /* recycle_log_files */ false));
        // This will write a snapshot containing metadata for all files in this DB. If we are
        // forcing a particular value of the flushed frontier, we need to set it in this snapshot
        // version edit as well.
        s = WriteSnapshot(descriptor_log_.get(), flushed_frontier_override);
      } else {
        descriptor_log_file_name_ = "";
      }
    }

    if (!edit->IsColumnFamilyManipulation()) {
      // This is cpu-heavy operations, which should be called outside mutex.
      v->PrepareApply(mutable_cf_options_copy, true);
    }

    // Write new records to MANIFEST log.
    if (s.ok()) {
      for (auto& e : batch_edits) {
        std::string record;
        if (!e->AppendEncodedTo(&record)) {
          s = STATUS(Corruption,
              "Unable to Encode VersionEdit:" + e->DebugString(true));
          break;
        }
        TEST_KILL_RANDOM("VersionSet::LogAndApply:BeforeAddRecord",
                         test_kill_odds * REDUCE_ODDS2);
        s = descriptor_log_->AddRecord(record);
        if (!s.ok()) {
          break;
        }
      }
      if (s.ok()) {
        s = SyncManifest(env_, db_options_, descriptor_log_->file());
      }
      if (!s.ok()) {
        RLOG(InfoLogLevel::ERROR_LEVEL, db_options_->info_log,
            "MANIFEST write: %s\n", s.ToString().c_str());
      }
    }

    std::string obsolete_manifest;
    // If we just created a new descriptor file, install it by writing a
    // new CURRENT file that points to it.
    if (s.ok() && new_descriptor_log) {
      s = SetCurrentFile(env_, dbname_, pending_manifest_file_number_, db_directory,
        db_options_->disableDataSync);
      // Leave the old file behind since PurgeObsoleteFiles will take care of it
      // later. It's unsafe to delete now since file deletion may be disabled.
      obsolete_manifest = DescriptorFileName("", manifest_file_number_);
    }

    if (s.ok()) {
      // find offset in manifest file where this version is stored.
      s = db_options_->get_checkpoint_env()->GetFileSize(
          descriptor_log_file_name_, &new_manifest_file_size);
    }

    if (edit->is_column_family_drop_) {
      DEBUG_ONLY_TEST_SYNC_POINT("VersionSet::LogAndApply::ColumnFamilyDrop:0");
      DEBUG_ONLY_TEST_SYNC_POINT("VersionSet::LogAndApply::ColumnFamilyDrop:1");
      DEBUG_ONLY_TEST_SYNC_POINT("VersionSet::LogAndApply::ColumnFamilyDrop:2");
    }

    LogFlush(db_options_->info_log);
    DEBUG_ONLY_TEST_SYNC_POINT("VersionSet::LogAndApply:WriteManifestDone");
    mu->Lock();

    if (!obsolete_manifest.empty()) {
      obsolete_manifests_.push_back(std::move(obsolete_manifest));
    }
  }

  // Install the new version
  if (s.ok()) {
    if (edit->column_family_name_) {
      // no group commit on column family add
      assert(batch_edits.size() == 1);
      assert(new_cf_options != nullptr);
      CreateColumnFamily(*new_cf_options, edit);
    } else if (edit->is_column_family_drop_) {
      assert(batch_edits.size() == 1);
      column_family_data->SetDropped();
      if (column_family_data->Unref()) {
        delete column_family_data;
      }
    } else {
      uint64_t max_log_number_in_batch  = 0;
      for (auto& e : batch_edits) {
        if (e->log_number_) {
          max_log_number_in_batch =
              std::max(max_log_number_in_batch, *e->log_number_);
        }
      }
      if (max_log_number_in_batch != 0) {
        assert(column_family_data->GetLogNumber() <= max_log_number_in_batch);
        column_family_data->SetLogNumber(max_log_number_in_batch);
      }
      AppendVersion(column_family_data, v);
    }

    manifest_file_number_ = pending_manifest_file_number_;
    manifest_file_size_ = new_manifest_file_size;
    prev_log_number_ = edit->prev_log_number_.get_value_or(0);
    if (flushed_frontier_override) {
      flushed_frontier_ = flushed_frontier_override;
    } else if (edit->flushed_frontier_) {
      UpdateFlushedFrontier(edit->flushed_frontier_);
    }
  } else {
    RLOG(InfoLogLevel::ERROR_LEVEL, db_options_->info_log,
        "Error in committing version %" PRIu64 " to [%s]",
        v->GetVersionNumber(),
        column_family_data ? column_family_data->GetName().c_str()
                           : "<null>");
    delete v;
    if (new_descriptor_log) {
      RLOG(InfoLogLevel::INFO_LEVEL, db_options_->info_log,
        "Deleting manifest %" PRIu64 " current manifest %" PRIu64 "\n",
        manifest_file_number_, pending_manifest_file_number_);
      descriptor_log_.reset();
      env_->CleanupFile(
          DescriptorFileName(dbname_, pending_manifest_file_number_));
    }
  }
  pending_manifest_file_number_ = 0;

  // wake up all the waiting writers
  while (true) {
    ManifestWriter* ready = manifest_writers_.front();
    manifest_writers_.pop_front();
    if (ready != &w) {
      ready->status = s;
      ready->done = true;
      YB_PROFILE(ready->cv.Signal());
    }
    if (ready == last_writer) break;
  }
  // Notify new head of write queue
  if (!manifest_writers_.empty()) {
    YB_PROFILE(manifest_writers_.front()->cv.Signal());
  }
  return s;
}

void VersionSet::LogAndApplyCFHelper(VersionEdit* edit) {
  assert(edit->IsColumnFamilyManipulation());
  edit->SetNextFile(next_file_number_.load());
  edit->SetLastSequence(LastSequence());
  if (edit->is_column_family_drop_) {
    // if we drop column family, we have to make sure to save max column family,
    // so that we don't reuse existing ID
    edit->SetMaxColumnFamily(column_family_set_->GetMaxColumnFamily());
  }
}

void VersionSet::LogAndApplyHelper(
    ColumnFamilyData* cfd,
    VersionBuilder* builder,
    VersionEdit* edit,
    InstrumentedMutex* mu) {
  mu->AssertHeld();
  assert(!edit->IsColumnFamilyManipulation());

  if (edit->log_number_) {
    assert(edit->log_number_ >= cfd->GetLogNumber());
    assert(edit->log_number_ < next_file_number_.load());
  }

  if (!edit->prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }
  edit->SetNextFile(next_file_number_.load());
  edit->SetLastSequence(LastSequence());

  if (flushed_frontier_ && !edit->force_flushed_frontier_) {
    edit->UpdateFlushedFrontier(flushed_frontier_);
  }

  builder->Apply(edit);
}

namespace {

struct LogReporter : public log::Reader::Reporter {
  Status* status;
  virtual void Corruption(size_t bytes, const Status& s) override {
    if (this->status->ok()) *this->status = s;
  }
};

class ManifestReader {
 public:
  ManifestReader(Env* env, Env* checkpoint_env, const EnvOptions& env_options,
                 BoundaryValuesExtractor* extractor, const std::string& dbname)
      : env_(env), checkpoint_env_(checkpoint_env), env_options_(env_options),
        extractor_(extractor), dbname_(dbname) {}

  Status OpenManifest() {
    auto status = ReadManifestFilename();
    if (!status.ok()) {
      return status;
    }
    FileType type;
    bool parse_ok = ParseFileName(manifest_filename_, &manifest_file_number_, &type);
    if (!parse_ok || type != kDescriptorFile) {
      return STATUS(Corruption, "CURRENT file corrupted");
    }

    manifest_filename_ = dbname_ + "/" + manifest_filename_;
    std::unique_ptr<SequentialFileReader> manifest_file_reader;
    {
      std::unique_ptr<SequentialFile> manifest_file;
      status = env_->NewSequentialFile(manifest_filename_, &manifest_file, env_options_);
      if (!status.ok()) {
        return status;
      }
      manifest_file_reader.reset(new SequentialFileReader(std::move(manifest_file)));
    }
    status = checkpoint_env_->GetFileSize(manifest_filename_, &current_manifest_file_size_);
    if (!status.ok()) {
      return status;
    }

    reader_.emplace(nullptr, std::move(manifest_file_reader), &reporter_, true /*checksum*/,
                    0 /*initial_offset*/, 0);
    reporter_.status = &status_;

    return Status::OK();
  }

  Status Next() {
    Slice record;
    if (!reader_->ReadRecord(&record, &scratch_)) {
      return STATUS(EndOfFile, "");
    }
    if (!status_.ok()) {
      return status_;
    }
    return edit_.DecodeFrom(extractor_, record);
  }

  VersionEdit& operator*() {
    return edit_;
  }

  uint64_t manifest_file_number() const { return manifest_file_number_; }
  uint64_t current_manifest_file_size() const { return current_manifest_file_size_; }
  const std::string& manifest_filename() const { return manifest_filename_; }
 private:
  Status ReadManifestFilename() {
    // Read "CURRENT" file, which contains a pointer to the current manifest file
    Status s = ReadFileToString(env_, CurrentFileName(dbname_), &manifest_filename_);
    if (!s.ok()) {
      return s;
    }
    if (manifest_filename_.empty() || manifest_filename_.back() != '\n') {
      return STATUS(Corruption, "CURRENT file does not end with newline");
    }
    // remove the trailing '\n'
    manifest_filename_.resize(manifest_filename_.size() - 1);
    return Status::OK();
  }

  // In plaintext cluster, this is a default env, but in encrypted cluster, this encrypts on write
  // and decrypts on read.
  Env* const env_;
  // Default env used to checkpoint files. In encrypted cluster, we don't want to decrypt
  // checkpointed files, so using the default env preserves file encryption.
  Env* const checkpoint_env_;
  const EnvOptions& env_options_;
  BoundaryValuesExtractor* extractor_;
  std::string dbname_;
  std::string manifest_filename_;
  uint64_t manifest_file_number_ = 0;
  uint64_t current_manifest_file_size_ = 0;
  LogReporter reporter_;
  boost::optional<log::Reader> reader_;
  std::string scratch_;
  Status status_;
  VersionEdit edit_;
};

} // namespace

Status VersionSet::Recover(
    const std::vector<ColumnFamilyDescriptor>& column_families,
    bool read_only) {
  std::unordered_map<std::string, ColumnFamilyOptions> cf_name_to_options;
  for (auto cf : column_families) {
    cf_name_to_options.insert({cf.name, cf.options});
  }
  // keeps track of column families in manifest that were not found in
  // column families parameters. if those column families are not dropped
  // by subsequent manifest records, Recover() will return failure status
  std::unordered_map<int, std::string> column_families_not_found;

  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  UserFrontierPtr flushed_frontier;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t previous_log_number = 0;
  uint32_t max_column_family = 0;
  std::unordered_map<uint32_t, std::unique_ptr<BaseReferencedVersionBuilder>> builders;

  // add default column family
  auto default_cf_iter = cf_name_to_options.find(kDefaultColumnFamilyName);
  if (default_cf_iter == cf_name_to_options.end()) {
    return STATUS(InvalidArgument, "Default column family not specified");
  }
  VersionEdit default_cf_edit;
  default_cf_edit.AddColumnFamily(kDefaultColumnFamilyName);
  default_cf_edit.SetColumnFamily(0);
  ColumnFamilyData* default_cfd =
      CreateColumnFamily(default_cf_iter->second, &default_cf_edit);
  builders.emplace(0, std::make_unique<BaseReferencedVersionBuilder>(default_cfd));

  Status s;
  uint64_t current_manifest_file_size;
  std::string current_manifest_filename;
  {
    ManifestReader manifest_reader(env_, db_options_->get_checkpoint_env(), env_options_,
                                   db_options_->boundary_extractor.get(), dbname_);
    auto status = manifest_reader.OpenManifest();
    if (!status.ok()) {
      return status;
    }
    current_manifest_file_size = manifest_reader.current_manifest_file_size();
    current_manifest_filename = manifest_reader.manifest_filename();
    manifest_file_number_ = manifest_reader.manifest_file_number();

    for (;;) {
      s = manifest_reader.Next();
      if (!s.ok()) {
        break;
      }
      auto& edit = *manifest_reader;
      // Not found means that user didn't supply that column
      // family option AND we encountered column family add
      // record. Once we encounter column family drop record,
      // we will delete the column family from
      // column_families_not_found.
      bool cf_in_not_found =
          column_families_not_found.find(edit.column_family_) !=
          column_families_not_found.end();
      // in builders means that user supplied that column family
      // option AND that we encountered column family add record
      bool cf_in_builders =
          builders.find(edit.column_family_) != builders.end();

      // they can't both be true
      assert(!(cf_in_not_found && cf_in_builders));

      ColumnFamilyData* cfd = nullptr;

      if (edit.column_family_name_) {
        if (cf_in_builders || cf_in_not_found) {
          s = STATUS(Corruption,
              "Manifest adding the same column family twice");
          break;
        }
        auto cf_options = cf_name_to_options.find(*edit.column_family_name_);
        if (cf_options == cf_name_to_options.end()) {
          column_families_not_found.emplace(edit.column_family_, *edit.column_family_name_);
        } else {
          cfd = CreateColumnFamily(cf_options->second, &edit);
          builders.emplace(edit.column_family_,
                           std::make_unique<BaseReferencedVersionBuilder>(cfd));
        }
      } else if (edit.is_column_family_drop_) {
        if (cf_in_builders) {
          auto builder = builders.find(edit.column_family_);
          assert(builder != builders.end());
          builders.erase(builder);
          cfd = column_family_set_->GetColumnFamily(edit.column_family_);
          if (cfd->Unref()) {
            delete cfd;
            cfd = nullptr;
          } else {
            // who else can have reference to cfd!?
            assert(false);
          }
        } else if (cf_in_not_found) {
          column_families_not_found.erase(edit.column_family_);
        } else {
          s = STATUS(Corruption,
              "Manifest - dropping non-existing column family");
          break;
        }
      } else if (!cf_in_not_found) {
        if (!cf_in_builders) {
          s = STATUS(Corruption,
              "Manifest record referencing unknown column family");
          break;
        }

        cfd = column_family_set_->GetColumnFamily(edit.column_family_);
        // this should never happen since cf_in_builders is true
        assert(cfd != nullptr);
        if (edit.max_level_ >= cfd->current()->storage_info()->num_levels()) {
          s = STATUS(InvalidArgument,
              "db has more levels than options.num_levels");
          break;
        }

        // if it is not column family add or column family drop,
        // then it's a file add/delete, which should be forwarded
        // to builder
        auto builder = builders.find(edit.column_family_);
        assert(builder != builders.end());
        builder->second->version_builder()->Apply(&edit);
      }

      if (cfd != nullptr) {
        if (edit.log_number_) {
          if (cfd->GetLogNumber() > edit.log_number_) {
            RLOG(InfoLogLevel::WARN_LEVEL, db_options_->info_log,
                "MANIFEST corruption detected, but ignored - Log numbers in "
                "records NOT monotonically increasing");
          } else {
            cfd->SetLogNumber(*edit.log_number_);
            have_log_number = true;
          }
        }
        if (edit.comparator_ &&
            *edit.comparator_ != cfd->user_comparator()->Name()) {
          s = STATUS(InvalidArgument,
              cfd->user_comparator()->Name(),
              "does not match existing comparator " + *edit.comparator_);
          break;
        }
      }

      if (edit.prev_log_number_) {
        previous_log_number = *edit.prev_log_number_;
        have_prev_log_number = true;
      }

      if (edit.next_file_number_) {
        next_file = *edit.next_file_number_;
        have_next_file = true;
      }

      if (edit.max_column_family_) {
        max_column_family = *edit.max_column_family_;
      }

      if (edit.last_sequence_) {
        last_sequence = *edit.last_sequence_;
        have_last_sequence = true;
      }

      if (edit.flushed_frontier_) {
        UpdateUserFrontier(
            &flushed_frontier, edit.flushed_frontier_, UpdateUserValueType::kLargest);
        VLOG(1) << "Updating flushed frontier with that from edit: "
                << edit.flushed_frontier_->ToString()
                << ", new flushed frontier: " << flushed_frontier->ToString();
      } else {
        VLOG(1) << "No flushed frontier found in edit";
      }
    }
    if (s.IsEndOfFile()) {
      s = Status::OK();
    }
  }

  if (s.ok()) {
    if (!have_next_file) {
      s = STATUS(Corruption, "no meta-nextfile entry in descriptor");
    } else if (!have_log_number) {
      s = STATUS(Corruption, "no meta-lognumber entry in descriptor");
    } else if (!have_last_sequence) {
      s = STATUS(Corruption, "no last-sequence-number entry in descriptor");
    }

    if (!have_prev_log_number) {
      previous_log_number = 0;
    }

    column_family_set_->UpdateMaxColumnFamily(max_column_family);

    MarkFileNumberUsedDuringRecovery(previous_log_number);
    MarkFileNumberUsedDuringRecovery(log_number);
  }

  // there were some column families in the MANIFEST that weren't specified
  // in the argument. This is OK in read_only mode
  if (read_only == false && !column_families_not_found.empty()) {
    std::string list_of_not_found;
    for (const auto& cf : column_families_not_found) {
      list_of_not_found += ", " + cf.second;
    }
    list_of_not_found = list_of_not_found.substr(2);
    s = STATUS(InvalidArgument,
        "You have to open all column families. Column families not opened: " +
        list_of_not_found);
  }

  if (s.ok()) {
    for (auto cfd : *column_family_set_) {
      if (cfd->IsDropped()) {
        continue;
      }
      auto builders_iter = builders.find(cfd->GetID());
      assert(builders_iter != builders.end());
      auto* builder = builders_iter->second->version_builder();

      if (db_options_->max_open_files == -1) {
        // unlimited table cache. Pre-load table handle now.
        // Need to do it out of the mutex.
        builder->LoadTableHandlers(cfd->internal_stats(),
                                   db_options_->max_file_opening_threads);
      }

      Version* v = new Version(cfd, this, current_version_number_++);
      builder->SaveTo(v->storage_info());

      // Install recovered version
      v->PrepareApply(*cfd->GetLatestMutableCFOptions(),
          !(db_options_->skip_stats_update_on_db_open));
      AppendVersion(cfd, v);
    }

    manifest_file_size_ = current_manifest_file_size;
    next_file_number_.store(next_file + 1);
    SetLastSequenceNoSanityChecking(last_sequence);
    prev_log_number_ = previous_log_number;
    if (flushed_frontier) {
      UpdateFlushedFrontierNoSanityChecking(std::move(flushed_frontier));
    }

    RLOG(InfoLogLevel::INFO_LEVEL, db_options_->info_log,
        "Recovered from manifest file:%s succeeded,"
        "manifest_file_number is %" PRIu64 ", next_file_number is %lu, "
        "last_sequence is %" PRIu64 ", log_number is %" PRIu64 ","
        "prev_log_number is %" PRIu64 ","
        "max_column_family is %u, flushed_values is %s\n",
        current_manifest_filename.c_str(), manifest_file_number_,
        next_file_number_.load(), LastSequence(),
        log_number, prev_log_number_,
        column_family_set_->GetMaxColumnFamily(),
        yb::ToString(flushed_frontier_).c_str());

    for (auto cfd : *column_family_set_) {
      if (cfd->IsDropped()) {
        continue;
      }
      RLOG(InfoLogLevel::INFO_LEVEL, db_options_->info_log,
          "Column family [%s] (ID %u), log number is %" PRIu64 "\n",
          cfd->GetName().c_str(), cfd->GetID(), cfd->GetLogNumber());
    }
  }

  return s;
}

Status VersionSet::Import(const std::string& source_dir,
                          SequenceNumber seqno,
                          VersionEdit* edit) {
  ManifestReader manifest_reader(env_, db_options_->get_checkpoint_env(), env_options_,
                                 db_options_->boundary_extractor.get(), source_dir);
  auto status = manifest_reader.OpenManifest();
  if (!status.ok()) {
    return status;
  }
  std::vector<FileMetaData> files;
  std::vector<std::pair<SequenceNumber, SequenceNumber>> segments;
  for (;;) {
    status = manifest_reader.Next();
    if (!status.ok()) {
      break;
    }
    auto& current = *manifest_reader;
    if (!current.GetDeletedFiles().empty()) {
      return STATUS(Corruption, "Deleted files should be empty");
    }
    for (const auto& file : current.GetNewFiles()) {
      auto filemeta = file.second;
      filemeta.largest.user_frontier.reset();
      filemeta.smallest.user_frontier.reset();
      filemeta.imported = true;
      if (filemeta.largest.seqno >= seqno) {
        return STATUS_FORMAT(InvalidArgument,
                             "Imported DB contains seqno ($0) greater than active seqno ($1)",
                             filemeta.largest.seqno,
                             seqno);
      }
      files.push_back(filemeta);
      segments.emplace_back(filemeta.smallest.seqno, filemeta.largest.seqno);
    }
  }
  if (!status.IsEndOfFile()) {
    return status;
  }

  if (files.empty()) {
    return STATUS_FORMAT(NotFound, "Imported DB is empty: $0", source_dir);
  }

  std::vector<LiveFileMetaData> live_files;
  GetLiveFilesMetaData(&live_files);
  for (const auto& file : live_files) {
    segments.emplace_back(file.smallest.seqno, file.largest.seqno);
  }

  std::sort(segments.begin(), segments.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  auto prev = segments.front();
  for (size_t i = 1; i != segments.size(); ++i) {
    const auto& segment = segments[i];
    if (segment.first <= prev.second) {
      return STATUS_FORMAT(Corruption,
                           "Overlapping seqno ranges: [$0, $1] and [$2, $3]",
                           prev.first,
                           prev.second,
                           segment.first,
                           segment.second);
    }
    prev = segment;
  }

  std::vector<std::string> revert_list;
  for (auto file : files) {
    auto source_base = MakeTableFileName(source_dir, file.fd.GetNumber());
    auto source_data = TableBaseToDataFileName(source_base);
    auto new_number = NewFileNumber();
    auto dest_base = MakeTableFileName(dbname_, new_number);
    auto dest_data = TableBaseToDataFileName(dest_base);
    LOG(INFO) << "Importing: " << source_base << " => " << dest_base;
    status = env_->LinkFile(source_base, dest_base);
    if (!status.ok()) {
      break;
    }
    revert_list.push_back(dest_base);
    status = env_->LinkFile(source_data, dest_data);
    if (!status.ok()) {
      break;
    }
    revert_list.push_back(dest_data);
    file.fd.packed_number_and_path_id = new_number; // path is 0
    file.marked_for_compaction = false;
    edit->AddCleanedFile(0, file);
  }

  if (!status.ok()) {
    for (const auto& file : revert_list) {
      ERROR_NOT_OK(env_->DeleteFile(file), yb::Format("Failed to delete file $0", file));
    }
    return status;
  }

  return Status::OK();
}

Status VersionSet::ListColumnFamilies(std::vector<std::string>* column_families,
                                      const std::string& dbname,
                                      BoundaryValuesExtractor* extractor,
                                      Env* env) {
  // these are just for performance reasons, not correctness,
  // so we're fine using the defaults
  EnvOptions soptions;
  // Read "CURRENT" file, which contains a pointer to the current manifest file
  std::string current;
  Status s = ReadFileToString(env, CurrentFileName(dbname), &current);
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current[current.size()-1] != '\n') {
    return STATUS(Corruption, "CURRENT file does not end with newline");
  }
  current.resize(current.size() - 1);

  std::string dscname = dbname + "/" + current;

  unique_ptr<SequentialFileReader> file_reader;
  {
    unique_ptr<SequentialFile> file;
    s = env->NewSequentialFile(dscname, &file, soptions);
    if (!s.ok()) {
      return s;
    }
    file_reader.reset(new SequentialFileReader(std::move(file)));
  }

  std::map<uint32_t, std::string> column_family_names;
  // default column family is always implicitly there
  column_family_names.insert({0, kDefaultColumnFamilyName});
  LogReporter reporter;
  reporter.status = &s;
  log::Reader reader(NULL, std::move(file_reader), &reporter, true /*checksum*/,
                     0 /*initial_offset*/, 0);
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch) && s.ok()) {
    VersionEdit edit;
    s = edit.DecodeFrom(extractor, record);
    if (!s.ok()) {
      break;
    }
    if (edit.column_family_name_) {
      if (column_family_names.find(edit.column_family_) !=
          column_family_names.end()) {
        s = STATUS(Corruption, "Manifest adding the same column family twice");
        break;
      }
      column_family_names.emplace(edit.column_family_, *edit.column_family_name_);
    } else if (edit.is_column_family_drop_) {
      if (column_family_names.find(edit.column_family_) ==
          column_family_names.end()) {
        s = STATUS(Corruption,
            "Manifest - dropping non-existing column family");
        break;
      }
      column_family_names.erase(edit.column_family_);
    }
  }

  column_families->clear();
  if (s.ok()) {
    for (const auto& iter : column_family_names) {
      column_families->push_back(iter.second);
    }
  }

  return s;
}

Status VersionSet::ReduceNumberOfLevels(const std::string& dbname,
                                        const Options* options,
                                        const EnvOptions& env_options,
                                        int new_levels) {
  if (new_levels <= 1) {
    return STATUS(InvalidArgument,
        "Number of levels needs to be bigger than 1");
  }

  ColumnFamilyOptions cf_options(*options);
  std::shared_ptr<Cache> tc(NewLRUCache(options->max_open_files - 10,
                                        options->table_cache_numshardbits));
  WriteController wc(options->delayed_write_rate);
  WriteBuffer wb(options->db_write_buffer_size);
  VersionSet versions(dbname, options, env_options, tc.get(), &wb, &wc);
  Status status;

  std::vector<ColumnFamilyDescriptor> dummy;
  ColumnFamilyDescriptor dummy_descriptor(kDefaultColumnFamilyName,
                                          ColumnFamilyOptions(*options));
  dummy.push_back(dummy_descriptor);
  status = versions.Recover(dummy);
  if (!status.ok()) {
    return status;
  }

  Version* current_version =
      versions.GetColumnFamilySet()->GetDefault()->current();
  auto* vstorage = current_version->storage_info();
  int current_levels = vstorage->num_levels();

  if (current_levels <= new_levels) {
    return Status::OK();
  }

  // Make sure there are file only on one level from
  // (new_levels-1) to (current_levels-1)
  int first_nonempty_level = -1;
  int first_nonempty_level_filenum = 0;
  for (int i = new_levels - 1; i < current_levels; i++) {
    int file_num = vstorage->NumLevelFiles(i);
    if (file_num != 0) {
      if (first_nonempty_level < 0) {
        first_nonempty_level = i;
        first_nonempty_level_filenum = file_num;
      } else {
        char msg[255];
        snprintf(msg, sizeof(msg),
                 "Found at least two levels containing files: "
                 "[%d:%d],[%d:%d].\n",
                 first_nonempty_level, first_nonempty_level_filenum, i,
                 file_num);
        return STATUS(InvalidArgument, msg);
      }
    }
  }

  // we need to allocate an array with the old number of levels size to
  // avoid SIGSEGV in WriteSnapshot()
  // however, all levels bigger or equal to new_levels will be empty
  std::vector<FileMetaData*>* new_files_list =
      new std::vector<FileMetaData*>[current_levels];
  for (int i = 0; i < new_levels - 1; i++) {
    new_files_list[i] = vstorage->LevelFiles(i);
  }

  if (first_nonempty_level > 0) {
    new_files_list[new_levels - 1] = vstorage->LevelFiles(first_nonempty_level);
  }

  delete[] vstorage -> files_;
  vstorage->files_ = new_files_list;
  vstorage->num_levels_ = new_levels;

  MutableCFOptions mutable_cf_options(*options, ImmutableCFOptions(*options));
  VersionEdit ve;
  InstrumentedMutex dummy_mutex;
  InstrumentedMutexLock l(&dummy_mutex);
  return versions.LogAndApply(
      versions.GetColumnFamilySet()->GetDefault(),
      mutable_cf_options, &ve, &dummy_mutex, nullptr, true);
}

Status VersionSet::DumpManifest(const Options& options, const std::string& dscname,
                                bool verbose, bool hex) {
  // Open the specified manifest file.
  unique_ptr<SequentialFileReader> file_reader;
  Status s;
  {
    unique_ptr<SequentialFile> file;
    s = options.env->NewSequentialFile(dscname, &file, env_options_);
    if (!s.ok()) {
      return s;
    }
    file_reader.reset(new SequentialFileReader(std::move(file)));
  }

  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t previous_log_number = 0;
  UserFrontierPtr flushed_frontier;
  int count __attribute__((unused)) = 0;
  std::unordered_map<uint32_t, std::string> comparators;
  std::unordered_map<uint32_t, BaseReferencedVersionBuilder*> builders;

  // add default column family
  VersionEdit default_cf_edit;
  default_cf_edit.AddColumnFamily(kDefaultColumnFamilyName);
  default_cf_edit.SetColumnFamily(0);
  ColumnFamilyData* default_cfd =
      CreateColumnFamily(ColumnFamilyOptions(options), &default_cf_edit);
  builders.insert({0, new BaseReferencedVersionBuilder(default_cfd)});

  {
    LogReporter reporter;
    reporter.status = &s;
    log::Reader reader(NULL, std::move(file_reader), &reporter,
                       true /*checksum*/, 0 /*initial_offset*/, 0);
    Slice record;
    std::string scratch;
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(db_options_->boundary_extractor.get(), record);
      if (!s.ok()) {
        break;
      }

      // Write out each individual edit
      if (verbose) {
        printf("%s\n", edit.DebugString(hex).c_str());
      }
      count++;

      bool cf_in_builders =
          builders.find(edit.column_family_) != builders.end();

      if (edit.comparator_) {
        comparators.emplace(edit.column_family_, *edit.comparator_);
      }

      ColumnFamilyData* cfd = nullptr;

      if (edit.column_family_name_) {
        if (cf_in_builders) {
          s = STATUS(Corruption,
              "Manifest adding the same column family twice");
          break;
        }
        cfd = CreateColumnFamily(ColumnFamilyOptions(options), &edit);
        builders.insert(
            {edit.column_family_, new BaseReferencedVersionBuilder(cfd)});
      } else if (edit.is_column_family_drop_) {
        if (!cf_in_builders) {
          s = STATUS(Corruption,
              "Manifest - dropping non-existing column family");
          break;
        }
        auto builder_iter = builders.find(edit.column_family_);
        delete builder_iter->second;
        builders.erase(builder_iter);
        comparators.erase(edit.column_family_);
        cfd = column_family_set_->GetColumnFamily(edit.column_family_);
        assert(cfd != nullptr);
        cfd->Unref();
        delete cfd;
        cfd = nullptr;
      } else {
        if (!cf_in_builders) {
          s = STATUS(Corruption,
              "Manifest record referencing unknown column family");
          break;
        }

        cfd = column_family_set_->GetColumnFamily(edit.column_family_);
        // this should never happen since cf_in_builders is true
        assert(cfd != nullptr);

        // if it is not column family add or column family drop,
        // then it's a file add/delete, which should be forwarded
        // to builder
        auto builder = builders.find(edit.column_family_);
        assert(builder != builders.end());
        builder->second->version_builder()->Apply(&edit);
      }

      if (cfd != nullptr && edit.log_number_) {
        cfd->SetLogNumber(*edit.log_number_);
      }

      if (edit.prev_log_number_) {
        previous_log_number = *edit.prev_log_number_;
        have_prev_log_number = true;
      }

      if (edit.next_file_number_) {
        next_file = *edit.next_file_number_;
        have_next_file = true;
      }

      if (edit.last_sequence_) {
        last_sequence = *edit.last_sequence_;
        have_last_sequence = true;
      }

      if (edit.flushed_frontier_) {
        flushed_frontier = edit.flushed_frontier_;
      }

      if (edit.max_column_family_) {
        column_family_set_->UpdateMaxColumnFamily(*edit.max_column_family_);
      }
    }
  }
  file_reader.reset();

  if (s.ok()) {
    if (!have_next_file) {
      s = STATUS(Corruption, "no meta-nextfile entry in descriptor");
      printf("no meta-nextfile entry in descriptor");
    } else if (!have_last_sequence) {
      printf("no last-sequence-number entry in descriptor");
      s = STATUS(Corruption, "no last-sequence-number entry in descriptor");
    }

    if (!have_prev_log_number) {
      previous_log_number = 0;
    }
  }

  if (s.ok()) {
    for (auto cfd : *column_family_set_) {
      if (cfd->IsDropped()) {
        continue;
      }
      auto builders_iter = builders.find(cfd->GetID());
      assert(builders_iter != builders.end());
      auto builder = builders_iter->second->version_builder();

      Version* v = new Version(cfd, this, current_version_number_++);
      builder->SaveTo(v->storage_info());
      v->PrepareApply(*cfd->GetLatestMutableCFOptions(), false);

      printf("--------------- Column family \"%s\"  (ID %u) --------------\n",
             cfd->GetName().c_str(), (unsigned int)cfd->GetID());
      printf("log number: %" PRIu64 "\n", cfd->GetLogNumber());
      auto comparator = comparators.find(cfd->GetID());
      if (comparator != comparators.end()) {
        printf("comparator: %s\n", comparator->second.c_str());
      } else {
        printf("comparator: <NO COMPARATOR>\n");
      }
      printf("%s \n", v->DebugString(hex).c_str());
      delete v;
    }

    // Free builders
    for (auto& builder : builders) {
      delete builder.second;
    }

    next_file_number_.store(next_file + 1);
    SetLastSequenceNoSanityChecking(last_sequence);
    if (flushed_frontier && FlushedFrontier()) {
      DCHECK_EQ(*flushed_frontier, *FlushedFrontier());
    }
    prev_log_number_ = previous_log_number;

    printf(
        "next_file_number %" PRIu64 " last_sequence "
        "%" PRIu64 " prev_log_number %" PRIu64 " max_column_family %u flushed_values %s\n",
        next_file_number_.load(), last_sequence, previous_log_number,
        column_family_set_->GetMaxColumnFamily(),
        yb::ToString(flushed_frontier.get()).c_str());
  }

  return s;
}

// Set the last sequence number to s.
void VersionSet::SetLastSequence(SequenceNumber s) {
#ifndef NDEBUG
  EnsureNonDecreasingLastSequence(LastSequence(), s);
#endif
  SetLastSequenceNoSanityChecking(s);
}

// Set last sequence number without verifying that it always keeps increasing.
void VersionSet::SetLastSequenceNoSanityChecking(SequenceNumber s) {
  last_sequence_.store(s, std::memory_order_release);
}

// Set the last flushed op id / hybrid time / history cutoff to the specified set of values.
void VersionSet::UpdateFlushedFrontier(UserFrontierPtr values) {
  EnsureNonDecreasingFlushedFrontier(FlushedFrontier(), *values);
  UpdateFlushedFrontierNoSanityChecking(std::move(values));
}

void VersionSet::MarkFileNumberUsedDuringRecovery(uint64_t number) {
  // only called during recovery which is single threaded, so this works because
  // there can't be concurrent calls
  if (next_file_number_.load(std::memory_order_relaxed) <= number) {
    next_file_number_.store(number + 1, std::memory_order_relaxed);
  }
}

namespace {

Status AddEdit(const VersionEdit& edit, const DBOptions* db_options, log::Writer* log) {
  std::string record;
  if (!edit.AppendEncodedTo(&record)) {
    return STATUS(Corruption,
        "Unable to Encode VersionEdit:" + edit.DebugString(true));
  }
  if (FLAGS_log_version_edits) {
    RLOG(InfoLogLevel::INFO_LEVEL, db_options->info_log,
         "Writing version edit: %s\n", edit.DebugString().c_str());
  }
  return log->AddRecord(record);
}

} // namespace

Status VersionSet::WriteSnapshot(log::Writer* log, UserFrontierPtr flushed_frontier_override) {
  // TODO: Break up into multiple records to reduce memory usage on recovery?

  // WARNING: This method doesn't hold a mutex!

  // This is done without DB mutex lock held, but only within single-threaded
  // LogAndApply. Column family manipulations can only happen within LogAndApply
  // (the same single thread), so we're safe to iterate.
  for (auto cfd : *column_family_set_) {
    if (cfd->IsDropped()) {
      continue;
    }
    {
      // Store column family info
      VersionEdit edit;
      if (cfd->GetID() != 0) {
        // default column family is always there,
        // no need to explicitly write it
        edit.AddColumnFamily(cfd->GetName());
        edit.SetColumnFamily(cfd->GetID());
      }
      edit.SetComparatorName(
          cfd->internal_comparator()->user_comparator()->Name());
      RETURN_NOT_OK(AddEdit(edit, db_options_, log));
    }

    {
      // Save files
      VersionEdit edit;
      edit.SetColumnFamily(cfd->GetID());

      for (int level = 0; level < cfd->NumberLevels(); level++) {
        for (const auto& f : cfd->current()->storage_info()->LevelFiles(level)) {
          edit.AddCleanedFile(level, *f);
        }
      }
      edit.SetLogNumber(cfd->GetLogNumber());
      if (flushed_frontier_override) {
        edit.flushed_frontier_ = flushed_frontier_override;
      } else {
        edit.flushed_frontier_ = flushed_frontier_;
      }
      RETURN_NOT_OK(AddEdit(edit, db_options_, log));
    }
  }

  return Status::OK();
}

// TODO(aekmekji): in CompactionJob::GenSubcompactionBoundaries(), this
// function is called repeatedly with consecutive pairs of slices. For example
// if the slice list is [a, b, c, d] this function is called with arguments
// (a,b) then (b,c) then (c,d). Knowing this, an optimization is possible where
// we avoid doing binary search for the keys b and c twice and instead somehow
// maintain state of where they first appear in the files.
uint64_t VersionSet::ApproximateSize(Version* v, const Slice& start,
                                     const Slice& end, int start_level,
                                     int end_level) {
  // pre-condition
  assert(v->cfd_->internal_comparator()->Compare(start, end) <= 0);

  uint64_t size = 0;
  const auto* vstorage = v->storage_info();
  end_level = end_level == -1
                  ? vstorage->num_non_empty_levels()
                  : std::min(end_level, vstorage->num_non_empty_levels());

  assert(start_level <= end_level);

  for (int level = start_level; level < end_level; level++) {
    const LevelFilesBrief& files_brief = vstorage->LevelFilesBrief(level);
    if (!files_brief.num_files) {
      // empty level, skip exploration
      continue;
    }

    if (!level) {
      // level 0 data is sorted order, handle the use case explicitly
      size += ApproximateSizeLevel0(v, files_brief, start, end);
      continue;
    }

    assert(level > 0);
    assert(files_brief.num_files > 0);

    // identify the file position for starting key
    const uint64_t idx_start = FindFileInRange(
        *v->cfd_->internal_comparator(), files_brief, start,
        /*start=*/0, static_cast<uint32_t>(files_brief.num_files - 1));
    assert(idx_start < files_brief.num_files);

    // scan all files from the starting position until the ending position
    // inferred from the sorted order
    for (uint64_t i = idx_start; i < files_brief.num_files; i++) {
      uint64_t val;
      val = ApproximateSize(v, files_brief.files[i], end);
      if (!val) {
        // the files after this will not have the range
        break;
      }

      size += val;

      if (i == idx_start) {
        // subtract the bytes needed to be scanned to get to the starting
        // key
        val = ApproximateSize(v, files_brief.files[i], start);
        assert(size >= val);
        size -= val;
      }
    }
  }

  return size;
}

uint64_t VersionSet::ApproximateSizeLevel0(Version* v,
                                           const LevelFilesBrief& files_brief,
                                           const Slice& key_start,
                                           const Slice& key_end) {
  // level 0 files are not in sorted order, we need to iterate through
  // the list to compute the total bytes that require scanning
  uint64_t size = 0;
  for (size_t i = 0; i < files_brief.num_files; i++) {
    const uint64_t start = ApproximateSize(v, files_brief.files[i], key_start);
    const uint64_t end = ApproximateSize(v, files_brief.files[i], key_end);
    assert(end >= start);
    size += end - start;
  }
  return size;
}

uint64_t VersionSet::ApproximateSize(Version* v, const FdWithBoundaries& f, const Slice& key) {
  // pre-condition
  assert(v);

  uint64_t result = 0;
  if (v->cfd_->internal_comparator()->Compare(f.largest.key, key) <= 0) {
    // Entire file is before "key", so just add the file size
    result = f.fd.GetTotalFileSize();
  } else if (v->cfd_->internal_comparator()->Compare(f.smallest.key, key) > 0) {
    // Entire file is after "key", so ignore
    result = 0;
  } else {
    // "key" falls in the range for this table.  Add the
    // approximate offset of "key" within the table.
    TableReader* table_reader_ptr;
    InternalIterator* iter = v->cfd_->table_cache()->NewIterator(
        ReadOptions(), env_options_, v->cfd_->internal_comparator(), f.fd,
        Slice() /* filter */,
        &table_reader_ptr);
    if (table_reader_ptr != nullptr) {
      result = table_reader_ptr->ApproximateOffsetOf(key);
    }
    delete iter;
  }
  return result;
}

void VersionSet::AddLiveFiles(std::vector<FileDescriptor>* live_list) {
  // pre-calculate space requirement
  int64_t total_files = 0;
  for (auto cfd : *column_family_set_) {
    Version* dummy_versions = cfd->dummy_versions();
    for (Version* v = dummy_versions->next_; v != dummy_versions;
         v = v->next_) {
      const auto* vstorage = v->storage_info();
      for (int level = 0; level < vstorage->num_levels(); level++) {
        total_files += vstorage->LevelFiles(level).size();
      }
    }
  }

  // just one time extension to the right size
  live_list->reserve(live_list->size() + static_cast<size_t>(total_files));

  for (auto cfd : *column_family_set_) {
    auto* current = cfd->current();
    bool found_current = false;
    Version* dummy_versions = cfd->dummy_versions();
    for (Version* v = dummy_versions->next_; v != dummy_versions;
         v = v->next_) {
      v->AddLiveFiles(live_list);
      if (v == current) {
        found_current = true;
      }
    }
    if (!found_current && current != nullptr) {
      // Should never happen unless it is a bug.
      assert(false);
      current->AddLiveFiles(live_list);
    }
  }
}

InternalIterator* VersionSet::MakeInputIterator(Compaction* c) {
  auto cfd = c->column_family_data();
  ReadOptions read_options;
  read_options.verify_checksums =
    c->mutable_cf_options()->verify_checksums_in_compaction;
  read_options.fill_cache = false;
  if (c->ShouldFormSubcompactions()) {
    read_options.total_order_seek = true;
  }

  // Level-0 files have to be merged together.  For other levels,
  // we will make a concatenating iterator per level.
  // TODO(opt): use concatenating iterator for level-0 if there is no overlap
  const size_t space = (c->level() == 0 ? c->input_levels(0)->num_files +
                                              c->num_input_levels() - 1
                                        : c->num_input_levels());
  InternalIterator** list = new InternalIterator* [space];
  size_t num = 0;
  for (size_t which = 0; which < c->num_input_levels(); which++) {
    if (c->input_levels(which)->num_files != 0) {
      if (c->level(which) == 0) {
        const LevelFilesBrief* flevel = c->input_levels(which);
        for (size_t i = 0; i < flevel->num_files; i++) {
          FileMetaData* fmd = c->input(which, i);
          if (c->input(which, i)->delete_after_compaction()) {
            RLOG(
                InfoLogLevel::INFO_LEVEL, db_options_->info_log,
                yb::Format(
                    "[$0] File marked for deletion, will be removed after compaction. file: $1",
                    c->column_family_data()->GetName(), fmd->ToString()).c_str());
            RecordTick(cfd->ioptions()->statistics, COMPACTION_FILES_FILTERED);
            continue;
          }
          RecordTick(cfd->ioptions()->statistics, COMPACTION_FILES_NOT_FILTERED);
          list[num++] = cfd->table_cache()->NewIterator(
              read_options, env_options_compactions_, cfd->internal_comparator(),
              flevel->files[i].fd, flevel->files[i].user_filter_data,
              /* table_reader_ptr = */ nullptr,
              /* file_read_hist = */ nullptr /* no per level latency histogram */,
              /* for compaction = */ true, /* arena = */ nullptr,
              /* skip_filters = */ false, c->skip_corrupt_data_blocks_unsafe());
        }
      } else {
        // Create concatenating iterator for the files from this level
        list[num++] = NewTwoLevelIterator(
            new LevelFileIteratorState(
                cfd->table_cache(), read_options, env_options_, cfd->internal_comparator(),
                nullptr /* no per level latency histogram */, true /* for_compaction */,
                false /* prefix enabled */, false /* skip_filters */),
            new LevelFileNumIterator(*cfd->internal_comparator(), c->input_levels(which)),
            /* arena = */ nullptr, /* need_free_iter_and_state = */ true,
            c->skip_corrupt_data_blocks_unsafe());
      }
    }
  }
  assert(num <= space);
  InternalIterator* result =
      NewMergingIterator(c->column_family_data()->internal_comparator().get(), list,
                         static_cast<int>(num));
  delete[] list;
  return result;
}

// verify that the files listed in this compaction are present
// in the current version
bool VersionSet::VerifyCompactionFileConsistency(Compaction* c) {
#ifndef NDEBUG
  Version* version = c->column_family_data()->current();
  const VersionStorageInfo* vstorage = version->storage_info();
  if (c->input_version_number() != version->GetVersionNumber()) {
    RLOG(
        InfoLogLevel::INFO_LEVEL, db_options_->info_log,
        yb::Format(
            "[$0] compaction output being applied to a different base version ($1) from input "
            "version ($2)",
            c->column_family_data()->GetName(), version->GetVersionNumber(),
            c->input_version_number())
            .c_str());

    if (vstorage->compaction_style_ == kCompactionStyleLevel &&
        c->start_level() == 0 && c->num_input_levels() > 2U) {
      // We are doing a L0->base_level compaction. The assumption is if
      // base level is not L1, levels from L1 to base_level - 1 is empty.
      // This is ensured by having one compaction from L0 going on at the
      // same time in level-based compaction. So that during the time, no
      // compaction/flush can put files to those levels.
      for (int l = c->start_level() + 1; l < c->output_level(); l++) {
        if (vstorage->NumLevelFiles(l) != 0) {
          return false;
        }
      }
    }
  }

  for (size_t input = 0; input < c->num_input_levels(); ++input) {
    int level = c->level(input);
    for (size_t i = 0; i < c->num_input_files(input); ++i) {
      const auto& fd = c->input(input, i)->fd;
      uint64_t number = fd.GetNumber();
      bool found = false;
      for (size_t j = 0; j < vstorage->files_[level].size(); j++) {
        FileMetaData* f = vstorage->files_[level][j];
        if (f->fd.GetNumber() == number) {
          found = true;
          break;
        }
      }
      if (!found) {
        RLOG(InfoLogLevel::INFO_LEVEL, db_options_->info_log,
            yb::Format("[$0] compaction input file $1 not found in current version",
            c->column_family_data()->GetName(), fd).c_str());
        return false;  // input files non existent in current version
      }
    }
  }
#endif
  return true;     // everything good
}

Status VersionSet::GetMetadataForFile(uint64_t number, int* filelevel,
                                      FileMetaData** meta,
                                      ColumnFamilyData** cfd) {
  for (auto cfd_iter : *column_family_set_) {
    Version* version = cfd_iter->current();
    const auto* vstorage = version->storage_info();
    for (int level = 0; level < vstorage->num_levels(); level++) {
      for (const auto& file : vstorage->LevelFiles(level)) {
        if (file->fd.GetNumber() == number) {
          *meta = file;
          *filelevel = level;
          *cfd = cfd_iter;
          return Status::OK();
        }
      }
    }
  }
  return STATUS(NotFound, "File not present in any level");
}

void VersionSet::GetLiveFilesMetaData(std::vector<LiveFileMetaData>* metadata) {
  for (auto cfd : *column_family_set_) {
    if (cfd->IsDropped()) {
      continue;
    }
    for (int level = 0; level < cfd->NumberLevels(); level++) {
      for (const auto& file :
           cfd->current()->storage_info()->LevelFiles(level)) {
        const auto path_id = file->fd.GetPathId();
        const auto& db_path = path_id < db_options_->db_paths.size()
                                  ? db_options_->db_paths[path_id].path
                                  : db_options_->db_paths.back().path;
        // TODO: replace base_size with an accurate metadata size for uncompressed data.
        // Look into: BlockBasedTableBuilder
        const auto uncompressed_size =
            file->fd.GetBaseFileSize() + file->raw_key_size + file->raw_value_size;
        metadata->emplace_back(
            cfd->GetName(), level, file->fd.GetNumber(), db_path, file->fd.GetTotalFileSize(),
            file->fd.GetBaseFileSize(), uncompressed_size, ConvertBoundaryValues(file->smallest),
            ConvertBoundaryValues(file->largest), file->imported, file->being_compacted);
      }
    }
  }
}

void VersionSet::GetObsoleteFiles(const FileNumbersProvider& pending_outputs,
                                  std::vector<FileMetaData*>* files,
                                  std::vector<std::string>* manifest_filenames) {
  assert(manifest_filenames->empty());
  obsolete_manifests_.swap(*manifest_filenames);
  std::vector<FileMetaData*> pending_files;
  for (auto f : obsolete_files_) {
    if (!pending_outputs.HasFileNumber(f->fd.GetNumber())) {
      files->push_back(f);
    } else {
      pending_files.push_back(f);
    }
  }
  obsolete_files_.swap(pending_files);
}

ColumnFamilyData* VersionSet::CreateColumnFamily(
    const ColumnFamilyOptions& cf_options, VersionEdit* edit) {
  assert(edit->column_family_name_);

  Version* dummy_versions = new Version(nullptr, this);
  // Ref() dummy version once so that later we can call Unref() to delete it
  // by avoiding calling "delete" explicitly (~Version is private)
  dummy_versions->Ref();
  auto new_cfd = column_family_set_->CreateColumnFamily(
      *edit->column_family_name_,
      edit->column_family_,
      dummy_versions,
      cf_options);

  Version* v = new Version(new_cfd, this, current_version_number_++);

  // Fill level target base information.
  v->storage_info()->CalculateBaseBytes(*new_cfd->ioptions(),
                                        *new_cfd->GetLatestMutableCFOptions());
  AppendVersion(new_cfd, v);
  // GetLatestMutableCFOptions() is safe here without mutex since the
  // cfd is not available to client
  new_cfd->CreateNewMemtable(*new_cfd->GetLatestMutableCFOptions(),
                             LastSequence());
  new_cfd->SetLogNumber(edit->log_number_.get_value_or(0));
  return new_cfd;
}

void VersionSet::UnrefFile(ColumnFamilyData* cfd, FileMetaData* f) {
  if (f->Unref(cfd->table_cache())) {
    obsolete_files_.push_back(f);
  }
}

uint64_t VersionSet::GetNumLiveVersions(Version* dummy_versions) {
  uint64_t count = 0;
  for (Version* v = dummy_versions->next_; v != dummy_versions; v = v->next_) {
    count++;
  }
  return count;
}

uint64_t VersionSet::GetTotalSstFilesSize(Version* dummy_versions) {
  std::unordered_set<uint64_t> unique_files;
  uint64_t total_files_size = 0;
  for (Version* v = dummy_versions->next_; v != dummy_versions; v = v->next_) {
    VersionStorageInfo* storage_info = v->storage_info();
    for (int level = 0; level < storage_info->num_levels_; level++) {
      for (const auto& file_meta : storage_info->LevelFiles(level)) {
        if (unique_files.find(file_meta->fd.packed_number_and_path_id) ==
            unique_files.end()) {
          unique_files.insert(file_meta->fd.packed_number_and_path_id);
          total_files_size += file_meta->fd.GetTotalFileSize();
        }
      }
    }
  }
  return total_files_size;
}

void VersionSet::EnsureNonDecreasingLastSequence(
    SequenceNumber prev_last_seq,
    SequenceNumber new_last_seq) {
  if (new_last_seq < prev_last_seq) {
    LOG(DFATAL) << "New last sequence id " << new_last_seq << " is lower than "
                << "the previous last sequence " << prev_last_seq;
  }
}

void VersionSet::EnsureNonDecreasingFlushedFrontier(
    const UserFrontier* prev_value,
    const UserFrontier& new_value) {
  if (!prev_value) {
    return;
  }
  if (!prev_value->IsUpdateValid(new_value, UpdateUserValueType::kLargest)) {
    LOG(DFATAL) << "Attempt to decrease flushed frontier " << prev_value->ToString() << " to "
                << new_value.ToString();
  }
}

void VersionSet::UpdateFlushedFrontierNoSanityChecking(UserFrontierPtr value) {
  UpdateUserFrontier(&flushed_frontier_, std::move(value), UpdateUserValueType::kLargest);
}

}  // namespace rocksdb
