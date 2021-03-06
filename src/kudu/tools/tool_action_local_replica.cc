// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tools/tool_action.h"

#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include "kudu/cfile/cfile_reader.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/row_changelist.h"
#include "kudu/common/row_operations.h"
#include "kudu/common/rowblock.h"
#include "kudu/common/schema.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus_meta.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/log_index.h"
#include "kudu/consensus/log_reader.h"
#include "kudu/consensus/log_util.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/human_readable.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/stringpiece.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/master/sys_catalog.h"
#include "kudu/rpc/messenger.h"
#include "kudu/tablet/cfile_set.h"
#include "kudu/tablet/deltafile.h"
#include "kudu/tablet/rowset_metadata.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tools/tool_action_common.h"
#include "kudu/tserver/tablet_copy_client.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/util/env.h"
#include "kudu/util/env_util.h"
#include "kudu/util/logging.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/metrics.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"

DEFINE_bool(metadata_only, false,
            "Only dump the block metadata when printing blocks.");
DEFINE_int64(nrows, 0, "Number of rows to dump");
DEFINE_int64(rowset_index, -1,
             "Index of the rowset in local replica, default value(-1) "
             "will dump all the rowsets of the local replica");
DEFINE_bool(verbose, false,
            "Print additional information (if any)");

namespace kudu {
namespace tools {

using cfile::CFileIterator;
using cfile::CFileReader;
using cfile::DumpIterator;
using cfile::ReaderOptions;
using consensus::ConsensusMetadata;
using consensus::RaftConfigPB;
using consensus::RaftPeerPB;
using consensus::ReplicateMsg;
using fs::ReadableBlock;
using log::LogIndex;
using log::LogReader;
using log::ReadableLogSegment;
using log::SegmentSequence;
using rpc::Messenger;
using rpc::MessengerBuilder;
using std::cout;
using std::endl;
using std::list;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Split;
using strings::Substitute;
using tablet::CFileSet;
using tablet::DeltaFileReader;
using tablet::DeltaIterator;
using tablet::DeltaKeyAndUpdate;
using tablet::DeltaType;
using tablet::MvccSnapshot;
using tablet::RowSetMetadata;
using tablet::Tablet;
using tablet::TabletMetadata;
using tserver::TabletCopyClient;
using tserver::WriteRequestPB;

namespace {

static const char* const kSeparatorLine =
  "----------------------------------------------------------------------\n";

string Indent(int indent) {
  return string(indent, ' ');
}

Status FsInit(unique_ptr<FsManager>* fs_manager) {
  FsManagerOpts fs_opts;
  fs_opts.read_only = true;
  unique_ptr<FsManager> fs_ptr(new FsManager(Env::Default(), fs_opts));
  RETURN_NOT_OK(fs_ptr->Open());
  fs_manager->swap(fs_ptr);
  return Status::OK();
}

// Parses a colon-delimited string containing a hostname or IP address and port
// into its respective parts. For example, "localhost:12345" parses into
// hostname=localhost, and port=12345.
//
// Does not allow a port with value 0.
Status ParseHostPortString(const string& hostport_str, HostPort* hostport) {
  HostPort hp;
  Status s = hp.ParseString(hostport_str, 0);
  if (!s.ok()) {
    return s.CloneAndPrepend(Substitute(
        "error while parsing peer '$0'", hostport_str));
  }
  if (hp.port() == 0) {
    return Status::InvalidArgument(
        Substitute("peer '$0' has port of 0", hostport_str));
  }
  *hostport = hp;
  return Status::OK();
}

// Parses a colon-delimited string containing a uuid, hostname or IP address,
// and port into its respective parts. For example,
// "1c7f19e7ecad4f918c0d3d23180fdb18:localhost:12345" parses into
// uuid=1c7f19e7ecad4f918c0d3d23180fdb18, hostname=localhost, and port=12345.
Status ParsePeerString(const string& peer_str,
                       string* uuid,
                       HostPort* hostport) {
  string::size_type first_colon_idx = peer_str.find(":");
  if (first_colon_idx == string::npos) {
    return Status::InvalidArgument(Substitute("bad peer '$0'", peer_str));
  }
  string hostport_str = peer_str.substr(first_colon_idx + 1);
  RETURN_NOT_OK(ParseHostPortString(hostport_str, hostport));
  *uuid = peer_str.substr(0, first_colon_idx);
  return Status::OK();
}

Status PrintReplicaUuids(const RunnerContext& context) {
  unique_ptr<FsManager> fs_manager;
  RETURN_NOT_OK(FsInit(&fs_manager));
  string tablet_id = FindOrDie(context.required_args, "tablet_id");

  // Load the cmeta file and print all peer uuids.
  unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK(ConsensusMetadata::Load(fs_manager.get(), tablet_id,
                                        fs_manager->uuid(), &cmeta));
  cout << JoinMapped(cmeta->committed_config().peers(),
                     [](const RaftPeerPB& p){ return p.permanent_uuid(); },
                     " ") << endl;
  return Status::OK();
}

Status RewriteRaftConfig(const RunnerContext& context) {
  // Parse tablet ID argument.
  string tablet_id = FindOrDie(context.required_args, "tablet_id");
  if (tablet_id != master::SysCatalogTable::kSysCatalogTabletId) {
    LOG(WARNING) << "Master will not notice rewritten Raft config of regular "
                 << "tablets. A regular Raft config change must occur.";
  }

  // Parse peer arguments.
  vector<pair<string, HostPort>> peers;
  for (const auto& arg : context.variadic_args) {
    pair<string, HostPort> parsed_peer;
    RETURN_NOT_OK(ParsePeerString(arg,
                                  &parsed_peer.first, &parsed_peer.second));
    peers.push_back(parsed_peer);
  }
  DCHECK(!peers.empty());

  // Make a copy of the old file before rewriting it.
  Env* env = Env::Default();
  FsManager fs_manager(env, FsManagerOpts());
  RETURN_NOT_OK(fs_manager.Open());
  string cmeta_filename = fs_manager.GetConsensusMetadataPath(tablet_id);
  string backup_filename = Substitute("$0.pre_rewrite.$1",
                                      cmeta_filename, env->NowMicros());
  WritableFileOptions opts;
  opts.mode = Env::CREATE_NON_EXISTING;
  opts.sync_on_close = true;
  RETURN_NOT_OK(env_util::CopyFile(env, cmeta_filename,
                                   backup_filename, opts));
  LOG(INFO) << "Backed up current config to " << backup_filename;

  // Load the cmeta file and rewrite the raft config.
  unique_ptr<ConsensusMetadata> cmeta;
  RETURN_NOT_OK(ConsensusMetadata::Load(&fs_manager, tablet_id,
                                        fs_manager.uuid(), &cmeta));
  RaftConfigPB current_config = cmeta->committed_config();
  RaftConfigPB new_config = current_config;
  new_config.clear_peers();
  for (const auto& p : peers) {
    RaftPeerPB new_peer;
    new_peer.set_member_type(RaftPeerPB::VOTER);
    new_peer.set_permanent_uuid(p.first);
    HostPortPB new_peer_host_port_pb;
    RETURN_NOT_OK(HostPortToPB(p.second, &new_peer_host_port_pb));
    new_peer.mutable_last_known_addr()->CopyFrom(new_peer_host_port_pb);
    new_config.add_peers()->CopyFrom(new_peer);
  }
  cmeta->set_committed_config(new_config);
  return cmeta->Flush();
}

Status CopyFromRemote(const RunnerContext& context) {
  // Parse the tablet ID and source arguments.
  string tablet_id = FindOrDie(context.required_args, "tablet_id");
  string rpc_address = FindOrDie(context.required_args, "source");

  HostPort hp;
  RETURN_NOT_OK(ParseHostPortString(rpc_address, &hp));

  // Copy the tablet over.
  FsManager fs_manager(Env::Default(), FsManagerOpts());
  RETURN_NOT_OK(fs_manager.Open());
  MessengerBuilder builder("tablet_copy_client");
  shared_ptr<Messenger> messenger;
  builder.Build(&messenger);
  TabletCopyClient client(tablet_id, &fs_manager, messenger);
  RETURN_NOT_OK(client.Start(hp, nullptr));
  RETURN_NOT_OK(client.FetchAll(nullptr));
  return client.Finish();
}

Status DumpWals(const RunnerContext& context) {
  unique_ptr<FsManager> fs_manager;
  RETURN_NOT_OK(FsInit(&fs_manager));
  string tablet_id = FindOrDie(context.required_args, "tablet_id");

  shared_ptr<LogReader> reader;
  RETURN_NOT_OK(LogReader::Open(fs_manager.get(),
                                scoped_refptr<LogIndex>(),
                                tablet_id,
                                scoped_refptr<MetricEntity>(),
                                &reader));

  SegmentSequence segments;
  RETURN_NOT_OK(reader->GetSegmentsSnapshot(&segments));

  for (const scoped_refptr<ReadableLogSegment>& segment : segments) {
    RETURN_NOT_OK(PrintSegment(segment));
  }

  return Status::OK();
}

Status ListBlocksInRowSet(const Schema& schema,
                          const RowSetMetadata& rs_meta) {
  RowSetMetadata::ColumnIdToBlockIdMap col_blocks =
      rs_meta.GetColumnBlocksById();
  for (const RowSetMetadata::ColumnIdToBlockIdMap::value_type& e :
      col_blocks) {
    ColumnId col_id = e.first;
    const BlockId& block_id = e.second;
    cout << "Column block for column ID " << col_id;
    int col_idx = schema.find_column_by_id(col_id);
    if (col_idx != -1) {
      cout << " (" << schema.column(col_idx).ToString() << ")";
    }
    cout << ": ";
    cout << block_id.ToString() << endl;
  }

  for (const BlockId& block : rs_meta.undo_delta_blocks()) {
    cout << "UNDO: " << block.ToString() << endl;
  }

  for (const BlockId& block : rs_meta.redo_delta_blocks()) {
    cout << "REDO: " << block.ToString() << endl;
  }

  return Status::OK();
}

Status DumpBlockIdsForLocalReplica(const RunnerContext& context) {
  unique_ptr<FsManager> fs_manager;
  RETURN_NOT_OK(FsInit(&fs_manager));
  string tablet_id = FindOrDie(context.required_args, "tablet_id");

  scoped_refptr<TabletMetadata> meta;
  RETURN_NOT_OK(TabletMetadata::Load(fs_manager.get(), tablet_id, &meta));

  if (meta->rowsets().empty()) {
    cout << "No rowsets found on disk for tablet "
         << tablet_id << endl;
    return Status::OK();
  }

  cout << "Listing all data blocks in tablet "
       << tablet_id << ":" << endl;

  Schema schema = meta->schema();

  size_t idx = 0;
  for (const shared_ptr<RowSetMetadata>& rs_meta : meta->rowsets())  {
    cout << "Rowset " << idx++ << endl;
    RETURN_NOT_OK(ListBlocksInRowSet(schema, *rs_meta));
  }

  return Status::OK();
}

Status DumpTabletMeta(FsManager* fs_manager,
                       const string& tablet_id, int indent) {
  scoped_refptr<TabletMetadata> meta;
  RETURN_NOT_OK(TabletMetadata::Load(fs_manager, tablet_id, &meta));

  const Schema& schema = meta->schema();

  cout << Indent(indent) << "Partition: "
       << meta->partition_schema().PartitionDebugString(meta->partition(),
                                                       meta->schema())
       << endl;
  cout << Indent(indent) << "Table name: " << meta->table_name()
       << " Table id: " << meta->table_id() << endl;
  cout << Indent(indent) << "Schema (version="
       << meta->schema_version() << "): "
       << schema.ToString() << endl;

  tablet::TabletSuperBlockPB pb;
  RETURN_NOT_OK_PREPEND(meta->ToSuperBlock(&pb), "Could not get superblock");
  cout << "Superblock:\n" << pb.DebugString() << endl;

  return Status::OK();
}

Status ListLocalReplicas(const RunnerContext& context) {
  unique_ptr<FsManager> fs_manager;
  RETURN_NOT_OK(FsInit(&fs_manager));

  vector<string> tablets;
  RETURN_NOT_OK(fs_manager->ListTabletIds(&tablets));
  for (const string& tablet : tablets) {
    if (FLAGS_verbose) {
      cout << "Tablet: " << tablet << endl;
      RETURN_NOT_OK(DumpTabletMeta(fs_manager.get(), tablet, 2));
    } else {
      cout << tablet << endl;
    }
  }
  return Status::OK();
}

Status DumpCFileBlockInternal(FsManager* fs_manager,
                              const BlockId& block_id,
                              int indent) {
  gscoped_ptr<ReadableBlock> block;
  RETURN_NOT_OK(fs_manager->OpenBlock(block_id, &block));
  gscoped_ptr<CFileReader> reader;
  RETURN_NOT_OK(CFileReader::Open(std::move(block), ReaderOptions(), &reader));

  cout << Indent(indent) << "CFile Header: "
       << reader->header().ShortDebugString() << endl;
  if (!FLAGS_verbose) {
    return Status::OK();
  }
  cout << Indent(indent) << reader->footer().num_values()
       << " values:" << endl;

  gscoped_ptr<CFileIterator> it;
  RETURN_NOT_OK(reader->NewIterator(&it, CFileReader::DONT_CACHE_BLOCK));
  RETURN_NOT_OK(it->SeekToFirst());
  return DumpIterator(*reader, it.get(), &cout, FLAGS_nrows, indent + 2);
}

Status DumpDeltaCFileBlockInternal(FsManager* fs_manager,
                                   const Schema& schema,
                                   const shared_ptr<RowSetMetadata>& rs_meta,
                                   const BlockId& block_id,
                                   DeltaType delta_type,
                                   int indent) {
  // Open the delta reader
  gscoped_ptr<ReadableBlock> readable_block;
  RETURN_NOT_OK(fs_manager->OpenBlock(block_id, &readable_block));
  shared_ptr<DeltaFileReader> delta_reader;
  RETURN_NOT_OK(DeltaFileReader::Open(std::move(readable_block),
                                      block_id,
                                      &delta_reader,
                                      delta_type));

  cout << Indent(indent) << "Delta stats: "
       << delta_reader->delta_stats().ToString() << endl;
  if (FLAGS_metadata_only) {
    return Status::OK();
  }

  // Create the delta iterator.
  // TODO: see if it's worth re-factoring NewDeltaIterator to return a
  // gscoped_ptr that can then be released if we need a raw or shared
  // pointer.
  DeltaIterator* raw_iter;

  MvccSnapshot snap_all;
  if (delta_type == tablet::REDO) {
    snap_all = MvccSnapshot::CreateSnapshotIncludingAllTransactions();
  } else if (delta_type == tablet::UNDO) {
    snap_all = MvccSnapshot::CreateSnapshotIncludingNoTransactions();
  }

  Status s = delta_reader->NewDeltaIterator(&schema, snap_all, &raw_iter);

  if (s.IsNotFound()) {
    cout << "Empty delta block." << endl;
    return Status::OK();
  }
  RETURN_NOT_OK(s);

  // NewDeltaIterator returns Status::OK() iff a new DeltaIterator is created. Thus,
  // it's safe to have a unique_ptr take possesion of 'raw_iter' here.
  unique_ptr<DeltaIterator> delta_iter(raw_iter);
  RETURN_NOT_OK(delta_iter->Init(NULL));
  RETURN_NOT_OK(delta_iter->SeekToOrdinal(0));

  // TODO: it's awkward that whenever we want to iterate over deltas we also
  // need to open the CFileSet for the rowset. Ideally, we should use
  // information stored in the footer/store additional information in the
  // footer as to make it feasible iterate over all deltas using a
  // DeltaFileIterator alone.
  shared_ptr<CFileSet> cfileset(new CFileSet(rs_meta));
  RETURN_NOT_OK(cfileset->Open());
  gscoped_ptr<CFileSet::Iterator> cfileset_iter(cfileset->NewIterator(&schema));

  RETURN_NOT_OK(cfileset_iter->Init(NULL));

  const size_t kRowsPerBlock  = 100;
  size_t nrows = 0;
  size_t ndeltas = 0;
  Arena arena(32 * 1024, 128 * 1024);
  RowBlock block(schema, kRowsPerBlock, &arena);

  // See tablet/delta_compaction.cc to understand why this loop is structured the way
  // it is.
  while (cfileset_iter->HasNext()) {
    size_t n;
    if (FLAGS_nrows > 0) {
      // Note: number of deltas may not equal the number of rows, but
      // since this is a CLI tool (and the nrows option exists
      // primarily to limit copious output) it's okay not to be
      // exact here.
      size_t remaining = FLAGS_nrows - nrows;
      if (remaining == 0) break;
      n = std::min(remaining, kRowsPerBlock);
    } else {
      n = kRowsPerBlock;
    }

    arena.Reset();
    cfileset_iter->PrepareBatch(&n);

    block.Resize(n);

    RETURN_NOT_OK(delta_iter->PrepareBatch(
        n, DeltaIterator::PREPARE_FOR_COLLECT));
    vector<DeltaKeyAndUpdate> out;
    RETURN_NOT_OK(
        delta_iter->FilterColumnIdsAndCollectDeltas(vector<ColumnId>(),
                                                              &out,
                                                              &arena));
    for (const DeltaKeyAndUpdate& upd : out) {
      if (FLAGS_verbose) {
        cout << Indent(indent) << upd.key.ToString() << " "
             << RowChangeList(upd.cell).ToString(schema) << endl;
        ++ndeltas;
      }
    }
    RETURN_NOT_OK(cfileset_iter->FinishBatch());

    nrows += n;
  }

  VLOG(1) << "Processed " << ndeltas << " deltas, for total of "
          << nrows << " possible rows.";
  return Status::OK();
}

Status DumpRowSetInternal(FsManager* fs_manager,
                          const Schema& schema,
                          const shared_ptr<RowSetMetadata>& rs_meta,
                          int indent) {
  tablet::RowSetDataPB pb;
  rs_meta->ToProtobuf(&pb);

  cout << Indent(indent) << "RowSet metadata: " << pb.DebugString()
       << endl << endl;

  RowSetMetadata::ColumnIdToBlockIdMap col_blocks =
      rs_meta->GetColumnBlocksById();
  for (const RowSetMetadata::ColumnIdToBlockIdMap::value_type& e :
      col_blocks) {
    ColumnId col_id = e.first;
    const BlockId& block_id = e.second;

    cout << Indent(indent) << "Dumping column block " << block_id
         << " for column id " << col_id;
    int col_idx = schema.find_column_by_id(col_id);
    if (col_idx != -1) {
      cout << "( " << schema.column(col_idx).ToString() <<  ")";
    }
    cout << ":" << endl;
    cout << Indent(indent) << kSeparatorLine;
    if (FLAGS_metadata_only) continue;
    RETURN_NOT_OK(DumpCFileBlockInternal(fs_manager, block_id, indent));
    cout << endl;
  }

  for (const BlockId& block : rs_meta->undo_delta_blocks()) {
    cout << Indent(indent) << "Dumping undo delta block " << block << ":"
         << endl << Indent(indent) << kSeparatorLine;
    RETURN_NOT_OK(DumpDeltaCFileBlockInternal(fs_manager,
                                              schema,
                                              rs_meta,
                                              block,
                                              tablet::UNDO,
                                              indent));
    cout << endl;
  }

  for (const BlockId& block : rs_meta->redo_delta_blocks()) {
    cout << Indent(indent) << "Dumping redo delta block " << block << ":"
         << endl << Indent(indent) << kSeparatorLine;
    RETURN_NOT_OK(DumpDeltaCFileBlockInternal(fs_manager,
                                              schema,
                                              rs_meta,
                                              block,
                                              tablet::REDO,
                                              indent));
    cout << endl;
  }

  return Status::OK();
}

Status DumpRowSet(const RunnerContext& context) {
  unique_ptr<FsManager> fs_manager;
  RETURN_NOT_OK(FsInit(&fs_manager));
  string tablet_id = FindOrDie(context.required_args, "tablet_id");

  scoped_refptr<TabletMetadata> meta;
  RETURN_NOT_OK(TabletMetadata::Load(fs_manager.get(), tablet_id, &meta));
  if (meta->rowsets().empty()) {
    cout << Indent(0) << "No rowsets found on disk for tablet "
         << tablet_id << endl;
    return Status::OK();
  }

  // If rowset index is provided, only dump that rowset.
  if (FLAGS_rowset_index != -1) {
    bool found = false;
    for (const shared_ptr<RowSetMetadata>& rs_meta : meta->rowsets())  {
      if (rs_meta->id() == FLAGS_rowset_index) {
        found = true;
        return DumpRowSetInternal(fs_manager.get(), meta->schema(),
                                  rs_meta, 0);
      }
    }
    if (!found) {
      return Status::InvalidArgument(
          Substitute("Could not find rowset $0 in tablet id $1",
                     FLAGS_rowset_index, tablet_id));
    }
  } else {
    // Rowset index not provided, dump all rowsets
    size_t idx = 0;
    for (const shared_ptr<RowSetMetadata>& rs_meta : meta->rowsets())  {
      cout << endl << "Dumping rowset " << idx++ << endl << kSeparatorLine;
      RETURN_NOT_OK(DumpRowSetInternal(fs_manager.get(), meta->schema(),
                                       rs_meta, 2));
    }
  }
  return Status::OK();
}

Status DumpMeta(const RunnerContext& context) {
  unique_ptr<FsManager> fs_manager;
  RETURN_NOT_OK(FsInit(&fs_manager));
  string tablet_id = FindOrDie(context.required_args, "tablet_id");
  RETURN_NOT_OK(DumpTabletMeta(fs_manager.get(), tablet_id, 0));
  return Status::OK();
}

unique_ptr<Mode> BuildDumpMode() {
  unique_ptr<Action> dump_block_ids =
      ActionBuilder("block_ids", &DumpBlockIdsForLocalReplica)
      .Description("Dump the IDs of all blocks belonging to a local replica")
      .AddRequiredParameter({ "tablet_id", "tablet identifier" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .Build();

  unique_ptr<Action> dump_meta =
      ActionBuilder("meta", &DumpMeta)
      .Description("Dump the metadata of a local replica")
      .AddRequiredParameter({ "tablet_id", "tablet identifier" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .Build();

  unique_ptr<Action> dump_rowset =
      ActionBuilder("rowset", &DumpRowSet)
      .Description("Dump the rowset contents of a local replica")
      .AddRequiredParameter({ "tablet_id", "tablet identifier" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .AddOptionalParameter("metadata_only")
      .AddOptionalParameter("nrows")
      .AddOptionalParameter("rowset_index")
      .AddOptionalParameter("verbose")
      .Build();

  unique_ptr<Action> dump_wals =
      ActionBuilder("wals", &DumpWals)
      .Description("Dump all WAL (write-ahead log) segments of "
        "a local replica")
      .AddRequiredParameter({ "tablet_id", "Tablet identifier" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .AddOptionalParameter("print_entries")
      .AddOptionalParameter("print_meta")
      .AddOptionalParameter("truncate_data")
      .Build();

  return ModeBuilder("dump")
      .Description("Dump a Kudu filesystem")
      .AddAction(std::move(dump_block_ids))
      .AddAction(std::move(dump_meta))
      .AddAction(std::move(dump_rowset))
      .AddAction(std::move(dump_wals))
      .Build();
}

} // anonymous namespace

unique_ptr<Mode> BuildLocalReplicaMode() {
  unique_ptr<Action> print_replica_uuids =
      ActionBuilder("print_replica_uuids", &PrintReplicaUuids)
      .Description("Print all replica UUIDs found in a "
        "tablet's Raft configuration")
      .AddRequiredParameter({ "tablet_id", "Tablet identifier" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .Build();

  unique_ptr<Action> rewrite_raft_config =
      ActionBuilder("rewrite_raft_config", &RewriteRaftConfig)
      .Description("Rewrite a replica's Raft configuration")
      .AddRequiredParameter({ "tablet_id", "Tablet identifier" })
      .AddRequiredVariadicParameter({
        "peers", "List of peers where each peer is of "
        "form 'uuid:hostname:port'" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .Build();

  unique_ptr<Mode> cmeta =
      ModeBuilder("cmeta")
      .Description("Operate on a local Kudu replica's consensus "
        "metadata file")
      .AddAction(std::move(print_replica_uuids))
      .AddAction(std::move(rewrite_raft_config))
      .Build();

  unique_ptr<Action> copy_from_remote =
      ActionBuilder("copy_from_remote", &CopyFromRemote)
      .Description("Copy a replica from a remote server")
      .AddRequiredParameter({ "tablet_id", "Tablet identifier" })
      .AddRequiredParameter({ "source", "Source RPC address of "
        "form hostname:port" })
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .Build();

  unique_ptr<Action> list =
      ActionBuilder("list", &ListLocalReplicas)
      .Description("Show list of Kudu replicas in the local filesystem")
      .AddOptionalParameter("fs_wal_dir")
      .AddOptionalParameter("fs_data_dirs")
      .AddOptionalParameter("verbose")
      .Build();

  return ModeBuilder("local_replica")
      .Description("Operate on local Kudu replicas via the local filesystem")
      .AddMode(std::move(cmeta))
      .AddAction(std::move(copy_from_remote))
      .AddAction(std::move(list))
      .AddMode(BuildDumpMode())
      .Build();
}

} // namespace tools
} // namespace kudu

