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
//
// Repairer does best effort recovery to recover as much data as possible after
// a disaster without compromising consistency. It does not guarantee bringing
// the database to a time consistent state.
//
// Repair process is broken into 4 phases:
// (a) Find files
// (b) Convert logs to tables
// (c) Extract metadata
// (d) Write Descriptor
//
// (a) Find files
//
// The repairer goes through all the files in the directory, and classifies them
// based on their file name. Any file that cannot be identified by name will be
// ignored.
//
// (b) Convert logs to table
//
// Every log file that is active is replayed. All sections of the file where the
// checksum does not match is skipped over. We intentionally give preference to
// data consistency.
//
// (c) Extract metadata
//
// We scan every table to compute
// (1) smallest/largest for the table
// (2) largest sequence number in the table
//
// If we are unable to scan the file, then we ignore the table.
//
// (d) Write Descriptor
//
// We generate descriptor contents:
//  - log number is set to zero
//  - next-file-number is set to 1 + largest file number we found
//  - last-sequence-number is set to largest sequence# found across
//    all tables (see 2c)
//  - compaction pointers are cleared
//  - every table file is added at level 0
//
// Possible optimization 1:
//   (a) Compute total size and use to pick appropriate max-level M
//   (b) Sort tables by largest sequence# in the table
//   (c) For each table: if it overlaps earlier table, place in level-0,
//       else place in level-M.
//   (d) We can provide options for time consistent recovery and unsafe recovery
//       (ignore checksum failure when applicable)
// Possible optimization 2:
//   Store per-table metadata (smallest, largest, largest-seq#, ...)
//   in the table's meta section to speed up ScanTable.


#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include "yb/rocksdb/db/builder.h"
#include "yb/rocksdb/db/db_impl.h"
#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/filename.h"
#include "yb/rocksdb/db/log_reader.h"
#include "yb/rocksdb/db/log_writer.h"
#include "yb/rocksdb/db/memtable.h"
#include "yb/rocksdb/db/table_cache.h"
#include "yb/rocksdb/db/version_edit.h"
#include "yb/rocksdb/db/writebuffer.h"
#include "yb/rocksdb/db/write_batch_internal.h"
#include "yb/rocksdb/comparator.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/options.h"
#include "yb/rocksdb/immutable_options.h"
#include "yb/rocksdb/table/scoped_arena_iterator.h"
#include "yb/rocksdb/table/table_reader.h"
#include "yb/rocksdb/util/file_reader_writer.h"
#include "yb/rocksdb/util/logging.h"

#include "yb/util/status_log.h"

using std::unique_ptr;

namespace rocksdb {

namespace {

class Repairer {
 public:
  Repairer(const std::string& dbname, const Options& options)
      : dbname_(dbname),
        env_(options.env),
        icmp_(std::make_shared<InternalKeyComparator>(options.comparator)),
        options_(SanitizeOptions(dbname, icmp_.get(), options)),
        ioptions_(options_),
        raw_table_cache_(
            // TableCache can be small since we expect each table to be opened
            // once.
            NewLRUCache(10, options_.table_cache_numshardbits)),
        next_file_number_(1) {
    GetIntTblPropCollectorFactory(options, &int_tbl_prop_collector_factories_);

    table_cache_ =
        new TableCache(ioptions_, env_options_, raw_table_cache_.get());
    edit_ = new VersionEdit();
  }

  ~Repairer() {
    delete table_cache_;
    raw_table_cache_.reset();
    delete edit_;
  }

  Status Run() {
    Status status = FindFiles();
    if (status.ok()) {
      ConvertLogFilesToTables();
      ExtractMetaData();
      status = WriteDescriptor();
    }
    if (status.ok()) {
      uint64_t bytes = 0;
      for (size_t i = 0; i < tables_.size(); i++) {
        bytes += tables_[i].meta.fd.GetTotalFileSize();
      }
      RLOG(InfoLogLevel::WARN_LEVEL, options_.info_log,
          "**** Repaired rocksdb %s; "
          "recovered %" ROCKSDB_PRIszt " files; %" PRIu64
          "bytes. "
          "Some data may have been lost. "
          "****",
          dbname_.c_str(), tables_.size(), bytes);
    }
    return status;
  }

 private:
  struct TableInfo {
    FileMetaData meta;
    SequenceNumber min_sequence;
    SequenceNumber max_sequence;
  };

  std::string const dbname_;
  Env* const env_;
  InternalKeyComparatorPtr icmp_;
  IntTblPropCollectorFactories int_tbl_prop_collector_factories_;
  const Options options_;
  const ImmutableCFOptions ioptions_;
  std::shared_ptr<Cache> raw_table_cache_;
  TableCache* table_cache_;
  VersionEdit* edit_;

  std::vector<std::string> manifests_;
  std::vector<FileDescriptor> table_fds_;
  std::vector<uint64_t> logs_;
  std::vector<TableInfo> tables_;
  uint64_t next_file_number_;
  const EnvOptions env_options_;

  Status FindFiles() {
    std::vector<std::string> filenames;
    bool found_file = false;
    for (size_t path_id = 0; path_id < options_.db_paths.size(); path_id++) {
      Status status =
          env_->GetChildren(options_.db_paths[path_id].path, &filenames);
      if (!status.ok()) {
        return status;
      }
      if (!filenames.empty()) {
        found_file = true;
      }

      uint64_t number;
      FileType type;
      for (size_t i = 0; i < filenames.size(); i++) {
        if (ParseFileName(filenames[i], &number, &type)) {
          if (type == kDescriptorFile) {
            assert(path_id == 0);
            manifests_.push_back(filenames[i]);
          } else {
            if (number + 1 > next_file_number_) {
              next_file_number_ = number + 1;
            }
            if (type == kLogFile) {
              assert(path_id == 0);
              logs_.push_back(number);
            } else if (type == kTableFile) {
              table_fds_.emplace_back(number, static_cast<uint32_t>(path_id),
                                      0, 0);
            } else {
              // Ignore other files
            }
          }
        }
      }
    }
    if (!found_file) {
      return STATUS(Corruption, dbname_, "repair found no files");
    }
    return Status::OK();
  }

  void ConvertLogFilesToTables() {
    for (size_t i = 0; i < logs_.size(); i++) {
      std::string logname = LogFileName(dbname_, logs_[i]);
      Status status = ConvertLogToTable(logs_[i]);
      if (!status.ok()) {
        RLOG(InfoLogLevel::WARN_LEVEL, options_.info_log,
            "Log #%" PRIu64 ": ignoring conversion error: %s", logs_[i],
            status.ToString().c_str());
      }
      ArchiveFile(logname);
    }
  }

  Status ConvertLogToTable(uint64_t log) {
    struct LogReporter : public log::Reader::Reporter {
      Env* env;
      std::shared_ptr<Logger> info_log;
      uint64_t lognum;
      void Corruption(size_t bytes, const Status& s) override {
        // We print error messages for corruption, but continue repairing.
        RLOG(InfoLogLevel::ERROR_LEVEL, info_log,
            "Log #%" PRIu64 ": dropping %d bytes; %s", lognum,
            static_cast<int>(bytes), s.ToString().c_str());
      }
    };

    // Open the log file
    std::string logname = LogFileName(dbname_, log);
    unique_ptr<SequentialFile> lfile;
    Status status = env_->NewSequentialFile(logname, &lfile, env_options_);
    if (!status.ok()) {
      return status;
    }
    unique_ptr<SequentialFileReader> lfile_reader(
        new SequentialFileReader(std::move(lfile)));

    // Create the log reader.
    LogReporter reporter;
    reporter.env = env_;
    reporter.info_log = options_.info_log;
    reporter.lognum = log;
    // We intentially make log::Reader do checksumming so that
    // corruptions cause entire commits to be skipped instead of
    // propagating bad information (like overly large sequence
    // numbers).
    log::Reader reader(options_.info_log, std::move(lfile_reader), &reporter,
                       true /*enable checksum*/, 0 /*initial_offset*/, log);

    // Read all the records and add to a memtable
    std::string scratch;
    Slice record;
    WriteBatch batch;
    WriteBuffer wb(options_.db_write_buffer_size);
    MemTable* mem =
        new MemTable(*icmp_, ioptions_, MutableCFOptions(options_, ioptions_),
                     &wb, kMaxSequenceNumber);
    auto cf_mems_default = new ColumnFamilyMemTablesDefault(mem);
    mem->Ref();
    int counter = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      if (record.size() < 12) {
        reporter.Corruption(
            record.size(), STATUS(Corruption, "log record too small"));
        continue;
      }
      WriteBatchInternal::SetContents(&batch, record);
      status = WriteBatchInternal::InsertInto(&batch, cf_mems_default, nullptr);
      if (status.ok()) {
        counter += WriteBatchInternal::Count(&batch);
      } else {
        RLOG(InfoLogLevel::WARN_LEVEL,
            options_.info_log, "Log #%" PRIu64 ": ignoring %s", log,
            status.ToString().c_str());
        status = Status::OK();  // Keep going with rest of file
      }
    }

    // Do not record a version edit for this conversion to a Table
    // since ExtractMetaData() will also generate edits.
    FileMetaData meta;
    meta.fd = FileDescriptor(next_file_number_++, 0, 0, 0);
    {
      ReadOptions ro;
      ro.total_order_seek = true;
      Arena arena;
      ScopedArenaIterator iter(mem->NewIterator(ro, &arena));
      status = BuildTable(dbname_,
                          env_,
                          ioptions_,
                          env_options_,
                          table_cache_,
                          iter.get(),
                          &meta,
                          icmp_,
                          int_tbl_prop_collector_factories_,
                          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily,
                          {},
                          kMaxSequenceNumber,
                          kNoCompression,
                          CompressionOptions(),
                          /* paranoid_file_checks */ false,
                          /* internal_stats */ nullptr,
                          options_.boundary_extractor.get());
    }
    delete mem->Unref();
    delete cf_mems_default;
    mem = nullptr;
    if (status.ok()) {
      if (meta.fd.GetTotalFileSize() > 0) {
        table_fds_.push_back(meta.fd);
      }
    }
    RLOG(InfoLogLevel::INFO_LEVEL, options_.info_log,
        "Log #%" PRIu64 ": %d ops saved to Table #%" PRIu64 " %s",
        log, counter, meta.fd.GetNumber(), status.ToString().c_str());
    return status;
  }

  void ExtractMetaData() {
    for (size_t i = 0; i < table_fds_.size(); i++) {
      TableInfo t;
      t.meta.fd = table_fds_[i];
      Status status = ScanTable(&t);
      if (!status.ok()) {
        std::string fname = TableFileName(
            options_.db_paths, t.meta.fd.GetNumber(), t.meta.fd.GetPathId());
        char file_num_buf[kFormatFileNumberBufSize];
        FormatFileNumber(t.meta.fd.GetNumber(), t.meta.fd.GetPathId(),
                         file_num_buf, sizeof(file_num_buf));
        RLOG(InfoLogLevel::WARN_LEVEL, options_.info_log,
            "Table #%s: ignoring %s", file_num_buf,
            status.ToString().c_str());
        ArchiveFile(fname);
      } else {
        tables_.push_back(t);
      }
    }
  }

  Status ScanTable(TableInfo* t) {
    std::string fname = TableFileName(options_.db_paths, t->meta.fd.GetNumber(),
                                      t->meta.fd.GetPathId());
    int counter = 0;
    uint64_t base_file_size;
    Status status = env_->GetFileSize(fname, &base_file_size);
    t->meta.fd = FileDescriptor(t->meta.fd.GetNumber(), t->meta.fd.GetPathId(),
                                t->meta.fd.total_file_size, base_file_size);
    if (status.ok()) {
      TableReader* reader;
      InternalIterator* iter = table_cache_->NewIterator(
          ReadOptions(), env_options_, icmp_, t->meta.fd, t->meta.UserFilter(), &reader);
      t->meta.fd.total_file_size = base_file_size +
          (reader->IsSplitSst() ? reader->GetTableProperties()->data_size : 0);
      bool empty = true;
      ParsedInternalKey parsed;
      t->min_sequence = 0;
      t->max_sequence = 0;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        if (!ParseInternalKey(key, &parsed)) {
          RLOG(InfoLogLevel::ERROR_LEVEL,
              options_.info_log, "Table #%" PRIu64 ": unparsable key %s",
              t->meta.fd.GetNumber(), EscapeString(key).c_str());
          continue;
        }

        counter++;
        if (empty) {
          empty = false;
          t->meta.smallest.key = InternalKey::DecodeFrom(key);
        }
        t->meta.largest.key = InternalKey::DecodeFrom(key);
        if (parsed.sequence < t->min_sequence) {
          t->min_sequence = parsed.sequence;
        }
        if (parsed.sequence > t->max_sequence) {
          t->max_sequence = parsed.sequence;
        }
      }
      if (!iter->status().ok()) {
        status = iter->status();
      }
      delete iter;
    }
    RLOG(InfoLogLevel::INFO_LEVEL,
        options_.info_log, "Table #%" PRIu64 ": %d entries %s",
        t->meta.fd.GetNumber(), counter, status.ToString().c_str());
    return status;
  }

  Status WriteDescriptor() {
    std::string tmp = TempFileName(dbname_, 1);
    unique_ptr<WritableFile> file;
    EnvOptions env_options = env_->OptimizeForManifestWrite(env_options_);
    Status status = env_->NewWritableFile(tmp, &file, env_options);
    if (!status.ok()) {
      return status;
    }

    SequenceNumber max_sequence = 0;
    for (size_t i = 0; i < tables_.size(); i++) {
      if (max_sequence < tables_[i].max_sequence) {
        max_sequence = tables_[i].max_sequence;
      }
    }

    edit_->SetComparatorName(icmp_->user_comparator()->Name());
    edit_->SetLogNumber(0);
    edit_->SetNextFile(next_file_number_);
    edit_->SetLastSequence(max_sequence);

    for (size_t i = 0; i < tables_.size(); i++) {
      // TODO(opt): separate out into multiple levels
      const TableInfo& t = tables_[i];
      auto meta = t.meta;
      meta.smallest.seqno = t.min_sequence;
      meta.largest.seqno = t.max_sequence;
      edit_->AddCleanedFile(0, meta);
    }

    // fprintf(stderr, "NewDescriptor:\n%s\n", edit_.DebugString().c_str());
    {
      unique_ptr<WritableFileWriter> file_writer(
          new WritableFileWriter(std::move(file), env_options));
      log::Writer log(std::move(file_writer), 0, false);
      std::string record;
      edit_->AppendEncodedTo(&record);
      status = log.AddRecord(record);
    }

    if (status.ok()) {
      // Discard older manifests
      for (size_t i = 0; i < manifests_.size(); i++) {
        ArchiveFile(dbname_ + "/" + manifests_[i]);
      }

      // Install new manifest
      status = env_->RenameFile(tmp, DescriptorFileName(dbname_, 1));
      if (status.ok()) {
        status = SetCurrentFile(env_, dbname_, 1, nullptr, options_.disableDataSync);
      }
    }
    if (!status.ok()) {
      env_->CleanupFile(tmp);
    }
    return status;
  }

  void ArchiveFile(const std::string& fname) {
    // Move into another directory.  E.g., for
    //    dir/foo
    // rename to
    //    dir/lost/foo
    const char* slash = strrchr(fname.c_str(), '/');
    std::string new_dir;
    if (slash != nullptr) {
      new_dir.assign(fname.data(), slash - fname.data());
    }
    new_dir.append("/lost");
    WARN_NOT_OK(env_->CreateDir(new_dir), "Failed to create dir " + new_dir);
    std::string new_file = new_dir;
    new_file.append("/");
    new_file.append((slash == nullptr) ? fname.c_str() : slash + 1);
    Status s = env_->RenameFile(fname, new_file);
    RLOG(InfoLogLevel::INFO_LEVEL,
        options_.info_log, "Archiving %s: %s\n",
        fname.c_str(), s.ToString().c_str());
  }
};
}  // namespace

Status RepairDB(const std::string& dbname, const Options& options) {
  Repairer repairer(dbname, options);
  return repairer.Run();
}

}  // namespace rocksdb
