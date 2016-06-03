/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <eventql/db/LSMPartitionReplication.h>
#include <eventql/db/LSMPartitionReader.h>
#include <eventql/db/LSMPartitionWriter.h>
#include <eventql/db/ReplicationScheme.h>
#include <eventql/util/logging.h>
#include <eventql/util/io/fileutil.h>
#include <eventql/util/protobuf/msg.h>
#include <eventql/util/protobuf/MessageEncoder.h>
#include <eventql/io/cstable/RecordMaterializer.h>
#include <eventql/db/metadata_operations.pb.h>
#include <eventql/db/metadata_coordinator.h>

#include "eventql/eventql.h"

namespace eventql {

const size_t LSMPartitionReplication::kMaxBatchSizeRows = 8192;
const size_t LSMPartitionReplication::kMaxBatchSizeBytes = 1024 * 1024 * 50; // 50 MB

LSMPartitionReplication::LSMPartitionReplication(
    RefPtr<Partition> partition,
    RefPtr<ReplicationScheme> repl_scheme,
    ConfigDirectory* cdir,
    http::HTTPConnectionPool* http) :
    PartitionReplication(partition, repl_scheme, http), cdir_(cdir) {}

bool LSMPartitionReplication::needsReplication() const {
  // check if we have seen the latest metadata transaction, otherwise enqueue
  if (snap_->state.last_metadata_txnid().empty()) {
    return true;
  }

  SHA1Hash last_seen_txid(
      snap_->state.last_metadata_txnid().data(),
      snap_->state.last_metadata_txnid().size());

  auto last_txid = partition_->getTable()->getLastMetadataTransaction();
  if (last_txid.getTransactionID() != last_seen_txid) {
    return true;
  }

  // check if we are splitting
  if (snap_->state.is_splitting()) {
    return true;
  }

  // check if all replicas named in the current metadata transaction have seen
  // the latest sequence, otherwise enqueue
  auto& writer = dynamic_cast<LSMPartitionWriter&>(*partition_->getWriter());
  auto repl_state = writer.fetchReplicationState();
  auto head_offset = snap_->state.lsm_sequence();
  for (const auto& r : snap_->state.replication_targets()) {
    auto replica_offset = replicatedOffsetFor(repl_state, r);
    if (replica_offset < head_offset) {
      return true;
    }
  }

  return false;
}

void LSMPartitionReplication::replicateTo(
    const ReplicationTarget& replica,
    uint64_t replicated_offset) {
  auto server_cfg = cdir_->getServerConfig(replica.server_id());
  if (server_cfg.server_status() != SERVER_UP) {
    RAISE(kRuntimeError, "server is down");
  }

  SHA1Hash target_partition_id(
      replica.partition_id().data(),
      replica.partition_id().size());

  size_t batch_size = 0;
  RecordEnvelopeList batch;
  batch.set_sync_commit(true); // fixme only on last batch?
  fetchRecords(
      replicated_offset,
      replica.keyrange_begin(),
      replica.keyrange_end(),
      [
          this,
          &batch,
          &replica,
          &replicated_offset,
          &batch_size,
          &server_cfg,
          &target_partition_id
        ] (
          const SHA1Hash& record_id,
          const uint64_t record_version,
          const void* record_data,
          size_t record_size) {
    auto rec = batch.add_records();
    rec->set_tsdb_namespace(snap_->state.tsdb_namespace());
    rec->set_table_name(snap_->state.table_key());
    rec->set_partition_sha1(target_partition_id.toString());
    rec->set_record_id(record_id.toString());
    rec->set_record_version(record_version);
    rec->set_record_data(record_data, record_size);

    batch_size += record_size;

    if (batch_size > kMaxBatchSizeBytes ||
        batch.records().size() > kMaxBatchSizeRows) {
      uploadBatchTo(server_cfg.server_addr(), batch);
      batch.mutable_records()->Clear();
      batch_size = 0;
    }
  });

  if (batch.records().size() > 0) {
    uploadBatchTo(server_cfg.server_addr(), batch);
  }
}

bool LSMPartitionReplication::replicate() {
  logDebug(
      "z1.replication",
      "Replicating partition $0/$1/$2",
      snap_->state.tsdb_namespace(),
      snap_->state.table_key(),
      snap_->key.toString());

  // if there is a new metadata transaction, fetch and apply it
  auto rc = fetchAndApplyMetadataTransaction();
  if (!rc.isSuccess()) {
    RAISEF(
        kRuntimeError,
        "error while applying metadata transactionL $0",
        rc.message());
  }

  // get the list of other replicaas for this partition
  auto& writer = dynamic_cast<LSMPartitionWriter&>(*partition_->getWriter());
  auto repl_state = writer.fetchReplicationState();
  auto head_offset = snap_->state.lsm_sequence();
  bool dirty = false;
  bool success = true;

  for (const auto& r : snap_->state.replication_targets()) {
    auto replica_offset = replicatedOffsetFor(repl_state, r);

    if (replica_offset >= head_offset) {
      continue;
    }

    logDebug(
        "z1.replication",
        "Replicating partition $0/$1/$2 to $3 (replicated_seq: $4, head_seq: $5, $6 records)",
        snap_->state.tsdb_namespace(),
        snap_->state.table_key(),
        snap_->key.toString(),
        r.server_id(),
        replica_offset,
        head_offset,
        head_offset - replica_offset);

    try {
      replicateTo(r, replica_offset);
      setReplicatedOffsetFor(&repl_state, r, head_offset);
      dirty = true;
    } catch (const std::exception& e) {
      success = false;

      logError(
        "z1.replication",
        e,
        "Error while replicating partition $0/$1/$2 to $3",
        snap_->state.tsdb_namespace(),
        snap_->state.table_key(),
        snap_->key.toString(),
        r.server_id());
    }
  }

  if (dirty) {
    auto& writer = dynamic_cast<LSMPartitionWriter&>(*partition_->getWriter());
    writer.commitReplicationState(repl_state);
  }

  return success;
}

void LSMPartitionReplication::uploadBatchTo(
    const String& host,
    const RecordEnvelopeList& batch) {
  auto body = msg::encode(batch);
  URI uri(StringUtil::format("http://$0/tsdb/replicate", host));
  http::HTTPRequest req(http::HTTPMessage::M_POST, uri.pathAndQuery());
  req.addHeader("Host", uri.hostAndPort());
  req.addHeader("Content-Type", "application/fnord-msg");
  req.addBody(body->data(), body->size());

  auto res = http_->executeRequest(req);
  res.wait();

  const auto& r = res.get();
  if (r.statusCode() != 201) {
    RAISEF(kRuntimeError, "received non-201 response: $0", r.body().toString());
  }
}

void LSMPartitionReplication::fetchRecords(
    size_t start_sequence,
    const String& keyrange_begin,
    const String& keyrange_end,
    Function<void (
        const SHA1Hash& record_id,
        uint64_t record_version,
        const void* record_data,
        size_t record_size)> fn) {
  auto schema = partition_->getTable()->schema();
  auto pkey_fieldname = partition_->getTable()->getPartitionKey();
  auto pkey_fieldid = schema->fieldId(pkey_fieldname);
  auto keyspace = partition_->getTable()->getKeyspaceType();

  const auto& tables = snap_->state.lsm_tables();
  for (const auto& tbl : tables) {
    if (tbl.last_sequence() < start_sequence) {
      continue;
    }

    auto cstable_file = FileUtil::joinPaths(
        snap_->base_path,
        tbl.filename() + ".cst");
    auto cstable = cstable::CSTableReader::openFile(cstable_file);
    cstable::RecordMaterializer materializer(schema.get(), cstable.get());
    auto id_col = cstable->getColumnReader("__lsm_id");
    auto version_col = cstable->getColumnReader("__lsm_version");
    auto sequence_col = cstable->getColumnReader("__lsm_sequence");

    auto nrecs = cstable->numRecords();
    for (size_t i = 0; i < nrecs; ++i) {
      uint64_t rlvl;
      uint64_t dlvl;

      uint64_t sequence;
      sequence_col->readUnsignedInt(&rlvl, &dlvl, &sequence);

      if (sequence < start_sequence) {
        materializer.skipRecord();
        continue;
      }

      String id_str;
      id_col->readString(&rlvl, &dlvl, &id_str);
      SHA1Hash id(id_str.data(), id_str.size());

      uint64_t version;
      version_col->readUnsignedInt(&rlvl, &dlvl, &version);

      msg::MessageObject record;
      materializer.nextRecord(&record);

      String pkey_value;
      switch (keyspace) {
        case KEYSPACE_STRING: {
          pkey_value = record.getString(pkey_fieldid);
          break;
        }
        case KEYSPACE_UINT64: {
          uint64_t pkey_value_uint = record.getUInt64(pkey_fieldid);
          pkey_value = String((const char*) &pkey_value_uint, sizeof(uint64_t));
          break;
        }
      }

      if (!keyrange_begin.empty()) {
        if (comparePartitionKeys(keyspace, pkey_value, keyrange_begin) < 0) {
          continue;
        }
      }

      if (!keyrange_end.empty()) {
        if (comparePartitionKeys(keyspace, pkey_value, keyrange_end) >= 0) {
          continue;
        }
      }

      Buffer record_buf;
      msg::MessageEncoder::encode(record, *schema, &record_buf);

      fn(id, version, record_buf.data(), record_buf.size());
    }
  }
}

Status LSMPartitionReplication::fetchAndApplyMetadataTransaction() {
  auto last_txid = partition_->getTable()->getLastMetadataTransaction();
  bool has_new_metadata_txn =
      snap_->state.last_metadata_txnid().empty() ||
      SHA1Hash(
          snap_->state.last_metadata_txnid().data(),
          snap_->state.last_metadata_txnid().size()) != last_txid.getTransactionID();

  if (has_new_metadata_txn) {
    auto rc = fetchAndApplyMetadataTransaction(last_txid);
    if (!rc.isSuccess()) {
      return rc;
    }

    snap_ = partition_->getSnapshot();

    has_new_metadata_txn =
        snap_->state.last_metadata_txnid().empty() ||
        SHA1Hash(
            snap_->state.last_metadata_txnid().data(),
            snap_->state.last_metadata_txnid().size()) != last_txid.getTransactionID();
  }

  if (has_new_metadata_txn) {
    return Status(
        eRuntimeError,
        StringUtil::format(
            "error while applying metadata transaction $0",
            last_txid.getTransactionID().toString()));
  }

  return Status::success();
}

Status LSMPartitionReplication::fetchAndApplyMetadataTransaction(
    MetadataTransaction txn) {
  PartitionDiscoveryRequest discovery_request;
  discovery_request.set_db_namespace(snap_->state.tsdb_namespace());
  discovery_request.set_table_id(snap_->state.table_key());
  discovery_request.set_min_txnseq(txn.getSequenceNumber());
  discovery_request.set_partition_id(snap_->state.partition_key());
  discovery_request.set_keyrange_begin(snap_->state.partition_keyrange_begin());
  discovery_request.set_keyrange_end(snap_->state.partition_keyrange_end());

  MetadataCoordinator coordinator(cdir_);
  PartitionDiscoveryResponse discovery_response;
  auto rc = coordinator.discoverPartition(
      discovery_request,
      &discovery_response);

  if (!rc.isSuccess()) {
    return rc;
  }

  auto writer = partition_->getWriter().asInstanceOf<LSMPartitionWriter>();
  return writer->applyMetadataChange(discovery_response);
}

bool LSMPartitionReplication::shouldDropPartition() const {
  if (snap_->state.lifecycle_state() == PDISCOVERY_UNLOAD &&
      !needsReplication()) {
    return true;
  } else {
    return false;
  }
}

} // namespace tdsb

