//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/flush_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <vector>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/version_set.h"
#include "port/port.h"
#include "port/likely.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/block_based_table_factory.h"
#include "table/merger.h"
#include "table/table_builder.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/log_buffer.h"
#include "util/mutexlock.h"
#include "util/perf_context_imp.h"
#include "util/iostats_context_imp.h"
#include "util/stop_watch.h"
#include "util/sync_point.h"

namespace rocksdb {

FlushJob::FlushJob(const std::string& dbname, ColumnFamilyData* cfd,
                   const DBOptions& db_options,
                   const MutableCFOptions& mutable_cf_options,
                   const EnvOptions& env_options, VersionSet* versions,
                   port::Mutex* db_mutex, std::atomic<bool>* shutting_down,
                   FileNumToPathIdMap* pending_outputs,
                   SequenceNumber newest_snapshot, JobContext* job_context,
                   LogBuffer* log_buffer, Directory* db_directory,
                   CompressionType output_compression, Statistics* stats)
    : dbname_(dbname),
      cfd_(cfd),
      db_options_(db_options),
      mutable_cf_options_(mutable_cf_options),
      env_options_(env_options),
      versions_(versions),
      db_mutex_(db_mutex),
      shutting_down_(shutting_down),
      pending_outputs_(pending_outputs),
      newest_snapshot_(newest_snapshot),
      job_context_(job_context),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_compression_(output_compression),
      stats_(stats) {}

Status FlushJob::Run() {
  // Save the contents of the earliest memtable as a new Table
  uint64_t file_number;
  autovector<MemTable*> mems;
  cfd_->imm()->PickMemtablesToFlush(&mems);
  if (mems.empty()) {
    LogToBuffer(log_buffer_, "[%s] Nothing in memtable to flush",
                cfd_->GetName().c_str());
    return Status::OK();
  }

  // entries mems are (implicitly) sorted in ascending order by their created
  // time. We will use the first memtable's `edit` to keep the meta info for
  // this flush.
  MemTable* m = mems[0];
  VersionEdit* edit = m->GetEdits();
  edit->SetPrevLogNumber(0);
  // SetLogNumber(log_num) indicates logs with number smaller than log_num
  // will no longer be picked up for recovery.
  edit->SetLogNumber(mems.back()->GetNextLogNumber());
  edit->SetColumnFamily(cfd_->GetID());

  // This will release and re-acquire the mutex.
  Status s = WriteLevel0Table(mems, edit, &file_number);

  if (s.ok() &&
      (shutting_down_->load(std::memory_order_acquire) || cfd_->IsDropped())) {
    s = Status::ShutdownInProgress(
        "Database shutdown or Column family drop during flush");
  }

  if (!s.ok()) {
    cfd_->imm()->RollbackMemtableFlush(mems, file_number, pending_outputs_);
  } else {
    // Replace immutable memtable with the generated Table
    s = cfd_->imm()->InstallMemtableFlushResults(
        cfd_, mutable_cf_options_, mems, versions_, db_mutex_, file_number,
        pending_outputs_, &job_context_->memtables_to_free, db_directory_,
        log_buffer_);
  }

  return s;
}

Status FlushJob::WriteLevel0Table(const autovector<MemTable*>& mems,
                                  VersionEdit* edit, uint64_t* filenumber) {
  db_mutex_->AssertHeld();
  const uint64_t start_micros = db_options_.env->NowMicros();
  FileMetaData meta;

  meta.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);
  *filenumber = meta.fd.GetNumber();
  // path 0 for level 0 file.
  pending_outputs_->insert({meta.fd.GetNumber(), 0});

  const SequenceNumber earliest_seqno_in_memtable =
      mems[0]->GetFirstSequenceNumber();
  Version* base = cfd_->current();
  base->Ref();  // it is likely that we do not need this reference
  Status s;
  {
    db_mutex_->Unlock();
    if (log_buffer_) {
      log_buffer_->FlushBufferToLog();
    }
    std::vector<Iterator*> memtables;
    ReadOptions ro;
    ro.total_order_seek = true;
    Arena arena;
    for (MemTable* m : mems) {
      Log(db_options_.info_log,
          "[%s] Flushing memtable with next log file: %" PRIu64 "\n",
          cfd_->GetName().c_str(), m->GetNextLogNumber());
      memtables.push_back(m->NewIterator(ro, &arena));
    }
    {
      ScopedArenaIterator iter(NewMergingIterator(&cfd_->internal_comparator(),
                                                  &memtables[0],
                                                  memtables.size(), &arena));
      Log(db_options_.info_log,
          "[%s] Level-0 flush table #%" PRIu64 ": started",
          cfd_->GetName().c_str(), meta.fd.GetNumber());

      s = BuildTable(dbname_, db_options_.env, *cfd_->ioptions(), env_options_,
                     cfd_->table_cache(), iter.get(), &meta,
                     cfd_->internal_comparator(), newest_snapshot_,
                     earliest_seqno_in_memtable, output_compression_,
                     cfd_->ioptions()->compression_opts, Env::IO_HIGH);
      LogFlush(db_options_.info_log);
    }
    Log(db_options_.info_log,
        "[%s] Level-0 flush table #%" PRIu64 ": %" PRIu64 " bytes %s",
        cfd_->GetName().c_str(), meta.fd.GetNumber(), meta.fd.GetFileSize(),
        s.ToString().c_str());

    if (!db_options_.disableDataSync && db_directory_ != nullptr) {
      db_directory_->Fsync();
    }
    db_mutex_->Lock();
  }
  base->Unref();

  // re-acquire the most current version
  base = cfd_->current();

  // There could be multiple threads writing to its own level-0 file.
  // The pending_outputs cannot be cleared here, otherwise this newly
  // created file might not be considered as a live-file by another
  // compaction thread that is concurrently deleting obselete files.
  // The pending_outputs can be cleared only after the new version is
  // committed so that other threads can recognize this file as a
  // valid one.
  // pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.fd.GetFileSize() > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    if (base != nullptr && db_options_.max_background_compactions <= 1 &&
        db_options_.max_background_flushes == 0 &&
        cfd_->ioptions()->compaction_style == kCompactionStyleLevel) {
      level = base->PickLevelForMemTableOutput(mutable_cf_options_,
                                               min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.fd.GetNumber(), meta.fd.GetPathId(),
                  meta.fd.GetFileSize(), meta.smallest, meta.largest,
                  meta.smallest_seqno, meta.largest_seqno);
  }

  InternalStats::CompactionStats stats(1);
  stats.micros = db_options_.env->NowMicros() - start_micros;
  stats.bytes_written = meta.fd.GetFileSize();
  cfd_->internal_stats()->AddCompactionStats(level, stats);
  cfd_->internal_stats()->AddCFStats(InternalStats::BYTES_FLUSHED,
                                     meta.fd.GetFileSize());
  RecordTick(stats_, COMPACT_WRITE_BYTES, meta.fd.GetFileSize());
  return s;
}

}  // namespace rocksdb