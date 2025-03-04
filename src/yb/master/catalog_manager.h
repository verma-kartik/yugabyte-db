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

#pragma once

#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/optional/optional_fwd.hpp>
#include <boost/functional/hash.hpp>
#include <gtest/internal/gtest-internal.h>

#include "yb/common/constants.h"
#include "yb/common/entity_ids.h"
#include "yb/common/index.h"
#include "yb/common/partition.h"
#include "yb/common/transaction.h"
#include "yb/client/client_fwd.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/thread_annotations.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager_if.h"
#include "yb/master/catalog_manager_util.h"
#include "yb/master/cdc_split_driver.h"
#include "yb/master/master_dcl.fwd.h"
#include "yb/master/master_defaults.h"
#include "yb/master/master_encryption.fwd.h"
#include "yb/master/scoped_leader_shared_lock.h"
#include "yb/master/sys_catalog.h"
#include "yb/master/sys_catalog_initialization.h"
#include "yb/master/system_tablet.h"
#include "yb/master/table_index.h"
#include "yb/master/tablet_split_candidate_filter.h"
#include "yb/master/tablet_split_driver.h"
#include "yb/master/tablet_split_manager.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/master/ysql_tablespace_manager.h"

#include "yb/rpc/rpc.h"
#include "yb/rpc/scheduler.h"
#include "yb/server/monitored_task.h"
#include "yb/tserver/tablet_peer_lookup.h"

#include "yb/util/async_task_util.h"
#include "yb/util/debug/lock_debug.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/pb_util.h"
#include "yb/util/promise.h"
#include "yb/util/random.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/status_callback.h"
#include "yb/util/status_fwd.h"
#include "yb/util/test_macros.h"
#include "yb/util/version_tracker.h"

namespace yb {

class Schema;
class ThreadPool;
class AddTransactionStatusTabletRequestPB;
class AddTransactionStatusTabletResponsePB;

template<class T>
class AtomicGauge;

#define CALL_GTEST_TEST_CLASS_NAME_(...) GTEST_TEST_CLASS_NAME_(__VA_ARGS__)

#define VERIFY_NAMESPACE_FOUND(expr, resp) \
  RESULT_CHECKER_HELPER( \
      expr, \
      if (!__result.ok()) { return SetupError((resp)->mutable_error(), __result.status()); });

namespace pgwrapper {
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBMarkDeleted));
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBUpdateSysTablet));
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBWithTables));
}

class CALL_GTEST_TEST_CLASS_NAME_(MasterPartitionedTest, VerifyOldLeaderStepsDown);
#undef CALL_GTEST_TEST_CLASS_NAME_

namespace tablet {

struct TableInfo;
enum RaftGroupStatePB;

}

namespace master {

struct DeferredAssignmentActions;
class XClusterSafeTimeService;

using PlacementId = std::string;

typedef std::unordered_map<TabletId, TabletServerId> TabletToTabletServerMap;

typedef std::unordered_set<TableId> TableIdSet;

typedef std::unordered_map<TablespaceId, boost::optional<ReplicationInfoPB>>
  TablespaceIdToReplicationInfoMap;

typedef std::unordered_map<TableId, boost::optional<TablespaceId>> TableToTablespaceIdMap;

typedef std::unordered_map<TableId, std::vector<scoped_refptr<TabletInfo>>> TableToTabletInfos;

// Map[NamespaceId]:xClusterSafeTime
typedef std::unordered_map<NamespaceId, HybridTime> XClusterNamespaceToSafeTimeMap;

constexpr int32_t kInvalidClusterConfigVersion = 0;

// The component of the master which tracks the state and location
// of tables/tablets in the cluster.
//
// This is the master-side counterpart of TSTabletManager, which tracks
// the state of each tablet on a given tablet-server.
//
// Thread-safe.
class CatalogManager : public tserver::TabletPeerLookupIf,
                       public TabletSplitCandidateFilterIf,
                       public TabletSplitDriverIf,
                       public CatalogManagerIf,
                       public CDCSplitDriverIf {
  typedef std::unordered_map<NamespaceName, scoped_refptr<NamespaceInfo> > NamespaceInfoMap;

  class NamespaceNameMapper {
   public:
    NamespaceInfoMap& operator[](YQLDatabase db_type);
    const NamespaceInfoMap& operator[](YQLDatabase db_type) const;
    void clear();

   private:
    std::array<NamespaceInfoMap, 4> typed_maps_;
  };

 public:
  explicit CatalogManager(Master *master);
  virtual ~CatalogManager();

  Status Init();

  bool StartShutdown();
  void CompleteShutdown();

  // Create Postgres sys catalog table.
  // If a non-null value of change_meta_req is passed then it does not
  // add the ysql sys table into the raft metadata but adds it in the request
  // pb. The caller is then responsible for performing the ChangeMetadataOperation.
  Status CreateYsqlSysTable(
      const CreateTableRequestPB* req, CreateTableResponsePB* resp,
      tablet::ChangeMetadataRequestPB* change_meta_req = nullptr,
      SysCatalogWriter* writer = nullptr);

  Status ReplicatePgMetadataChange(const tablet::ChangeMetadataRequestPB* req);

  // Reserve Postgres oids for a Postgres database.
  Status ReservePgsqlOids(const ReservePgsqlOidsRequestPB* req,
                          ReservePgsqlOidsResponsePB* resp,
                          rpc::RpcContext* rpc);

  // Get the info (current only version) for the ysql system catalog.
  Status GetYsqlCatalogConfig(const GetYsqlCatalogConfigRequestPB* req,
                              GetYsqlCatalogConfigResponsePB* resp,
                              rpc::RpcContext* rpc);

  // Copy Postgres sys catalog tables into a new namespace.
  Status CopyPgsqlSysTables(const NamespaceId& namespace_id,
                            const std::vector<scoped_refptr<TableInfo>>& tables);

  // Create a new Table with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status CreateTable(const CreateTableRequestPB* req,
                     CreateTableResponsePB* resp,
                     rpc::RpcContext* rpc) override;

  // Create a new transaction status table.
  Status CreateTransactionStatusTable(const CreateTransactionStatusTableRequestPB* req,
                                      CreateTransactionStatusTableResponsePB* resp,
                                      rpc::RpcContext *rpc);

  // Create a transaction status table with the given name.
  Status CreateTransactionStatusTableInternal(rpc::RpcContext* rpc,
                                              const std::string& table_name,
                                              const TablespaceId* tablespace_id,
                                              const ReplicationInfoPB* replication_info);

  // Add a tablet to a transaction status table.
  Status AddTransactionStatusTablet(const AddTransactionStatusTabletRequestPB* req,
                                    AddTransactionStatusTabletResponsePB* resp,
                                    rpc::RpcContext* rpc);

  // Check if there is a transaction table whose tablespace id matches the given tablespace id.
  bool DoesTransactionTableExistForTablespace(
      const TablespaceId& tablespace_id) EXCLUDES(mutex_);

  // Create a local transaction status table for a tablespace if needed
  // (i.e. if it does not exist already).
  //
  // This is called during CreateTable if the table has transactions enabled and is part
  // of a tablespace with a placement set.
  Status CreateLocalTransactionStatusTableIfNeeded(
      rpc::RpcContext *rpc, const TablespaceId& tablespace_id) EXCLUDES(mutex_);

  // Create the global transaction status table if needed (i.e. if it does not exist already).
  //
  // This is called at the end of CreateTable if the table has transactions enabled.
  Status CreateGlobalTransactionStatusTableIfNeeded(rpc::RpcContext *rpc);

  // Get tablet ids of the global transaction status table.
  Status GetGlobalTransactionStatusTablets(
      GetTransactionStatusTabletsResponsePB* resp) EXCLUDES(mutex_);

  // Get ids of transaction status tables matching a given placement.
  Result<std::vector<TableInfoPtr>> GetPlacementLocalTransactionStatusTables(
      const CloudInfoPB& placement) EXCLUDES(mutex_);

  // Get tablet ids of local transaction status tables matching a given placement.
  Status GetPlacementLocalTransactionStatusTablets(
      const std::vector<TableInfoPtr>& placement_local_tables,
      GetTransactionStatusTabletsResponsePB* resp) EXCLUDES(mutex_);

  // Get tablet ids of the global transaction status table and local transaction status tables
  // matching a given placement.
  Status GetTransactionStatusTablets(const GetTransactionStatusTabletsRequestPB* req,
                                     GetTransactionStatusTabletsResponsePB* resp,
                                     rpc::RpcContext *rpc) EXCLUDES(mutex_);

  // Create the metrics snapshots table if needed (i.e. if it does not exist already).
  //
  // This is called at the end of CreateTable.
  Status CreateMetricsSnapshotsTableIfNeeded(rpc::RpcContext *rpc);

  // Get the information about an in-progress create operation.
  Status IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                           IsCreateTableDoneResponsePB* resp) override;

  Status IsCreateTableInProgress(const TableId& table_id,
                                 CoarseTimePoint deadline,
                                 bool* create_in_progress);

  Status WaitForCreateTableToFinish(const TableId& table_id, CoarseTimePoint deadline);

  // Check if the transaction status table creation is done.
  //
  // This is called at the end of IsCreateTableDone if the table has transactions enabled.
  Result<bool> IsTransactionStatusTableCreated();

  // Check if the metrics snapshots table creation is done.
  //
  // This is called at the end of IsCreateTableDone.
  Result<bool> IsMetricsSnapshotsTableCreated();

  // Called when transaction associated with table create finishes. Verifies postgres layer present.
  Status VerifyTablePgLayer(scoped_refptr<TableInfo> table, bool txn_query_succeeded);

  // Truncate the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status TruncateTable(const TruncateTableRequestPB* req,
                       TruncateTableResponsePB* resp,
                       rpc::RpcContext* rpc);

  // Get the information about an in-progress truncate operation.
  Status IsTruncateTableDone(const IsTruncateTableDoneRequestPB* req,
                             IsTruncateTableDoneResponsePB* resp);

  // Backfill the specified index.  Currently only supported for YSQL.  YCQL does not need this as
  // master automatically runs backfill according to the DocDB permissions.
  Status BackfillIndex(const BackfillIndexRequestPB* req,
                       BackfillIndexResponsePB* resp,
                       rpc::RpcContext* rpc);

  // Gets the backfill jobs state associated with the requested table.
  Status GetBackfillJobs(const GetBackfillJobsRequestPB* req,
                                      GetBackfillJobsResponsePB* resp,
                                      rpc::RpcContext* rpc);

  // Backfill the indexes for the specified table.
  // Used for backfilling YCQL defered indexes when triggered from yb-admin.
  Status LaunchBackfillIndexForTable(const LaunchBackfillIndexForTableRequestPB* req,
                                     LaunchBackfillIndexForTableResponsePB* resp,
                                     rpc::RpcContext* rpc);

  // Schedules a table deletion to run as a background task.
  Status ScheduleDeleteTable(const scoped_refptr<TableInfo>& table);

  // Delete the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status DeleteTable(const DeleteTableRequestPB* req,
                     DeleteTableResponsePB* resp,
                     rpc::RpcContext* rpc);
  Status DeleteTableInternal(
      const DeleteTableRequestPB* req, DeleteTableResponsePB* resp, rpc::RpcContext* rpc);

  // Get the information about an in-progress delete operation.
  Status IsDeleteTableDone(const IsDeleteTableDoneRequestPB* req,
                           IsDeleteTableDoneResponsePB* resp);

  // Alter the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status AlterTable(const AlterTableRequestPB* req,
                    AlterTableResponsePB* resp,
                    rpc::RpcContext* rpc);

  Status UpdateSysCatalogWithNewSchema(
    const scoped_refptr<TableInfo>& table,
    const std::vector<DdlLogEntry>& ddl_log_entries,
    const std::string& new_namespace_id,
    const std::string& new_table_name,
    AlterTableResponsePB* resp);

  // Get the information about an in-progress alter operation.
  Status IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                          IsAlterTableDoneResponsePB* resp);

  Result<NamespaceId> GetTableNamespaceId(TableId table_id) EXCLUDES(mutex_);

  void ScheduleYsqlTxnVerification(const scoped_refptr<TableInfo>& table,
                                   const TransactionMetadata& txn);

  Status YsqlTableSchemaChecker(scoped_refptr<TableInfo> table,
                                const std::string& txn_id_pb,
                                bool txn_rpc_success);

  Status YsqlDdlTxnCompleteCallback(scoped_refptr<TableInfo> table,
                                    const std::string& txn_id_pb,
                                    bool success);

  // Get the information about the specified table.
  Status GetTableSchema(const GetTableSchemaRequestPB* req,
                        GetTableSchemaResponsePB* resp) override;
  Status GetTableSchemaInternal(const GetTableSchemaRequestPB* req,
                                GetTableSchemaResponsePB* resp,
                                bool get_fully_applied_indexes = false);

  // Get the information about the specified tablegroup.
  Status GetTablegroupSchema(const GetTablegroupSchemaRequestPB* req,
                             GetTablegroupSchemaResponsePB* resp);

  // Get the information about the specified colocated databsae.
  Status GetColocatedTabletSchema(const GetColocatedTabletSchemaRequestPB* req,
                                  GetColocatedTabletSchemaResponsePB* resp);

  // List all the running tables.
  Status ListTables(const ListTablesRequestPB* req,
                    ListTablesResponsePB* resp) override;

  Status GetTableLocations(const GetTableLocationsRequestPB* req,
                           GetTableLocationsResponsePB* resp) override;

  // Lookup tablet by ID, then call GetTabletLocations below.
  Status GetTabletLocations(
      const TabletId& tablet_id,
      TabletLocationsPB* locs_pb,
      IncludeInactive include_inactive) override;

  // Look up the locations of the given tablet. The locations
  // vector is overwritten (not appended to).
  // If the tablet is not found, returns Status::NotFound.
  // If the tablet is not running, returns Status::ServiceUnavailable.
  // Otherwise, returns Status::OK and puts the result in 'locs_pb'.
  // This only returns tablets which are in RUNNING state.
  Status GetTabletLocations(
      scoped_refptr<TabletInfo> tablet_info,
      TabletLocationsPB* locs_pb,
      IncludeInactive include_inactive) override;

  // Returns the system tablet in catalog manager by the id.
  Result<std::shared_ptr<tablet::AbstractTablet>> GetSystemTablet(const TabletId& id) override;

  // Handle a tablet report from the given tablet server.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status ProcessTabletReport(TSDescriptor* ts_desc,
                             const TabletReportPB& report,
                             TabletReportUpdatesPB *report_update,
                             rpc::RpcContext* rpc);

  // Create a new Namespace with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status CreateNamespace(const CreateNamespaceRequestPB* req,
                         CreateNamespaceResponsePB* resp,
                         rpc::RpcContext* rpc) override;
  // Get the information about an in-progress create operation.
  Status IsCreateNamespaceDone(const IsCreateNamespaceDoneRequestPB* req,
                               IsCreateNamespaceDoneResponsePB* resp);

  // Delete the specified Namespace.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status DeleteNamespace(const DeleteNamespaceRequestPB* req,
                         DeleteNamespaceResponsePB* resp,
                         rpc::RpcContext* rpc);
  // Get the information about an in-progress delete operation.
  Status IsDeleteNamespaceDone(const IsDeleteNamespaceDoneRequestPB* req,
                               IsDeleteNamespaceDoneResponsePB* resp);

  // Alter the specified Namespace.
  Status AlterNamespace(const AlterNamespaceRequestPB* req,
                        AlterNamespaceResponsePB* resp,
                        rpc::RpcContext* rpc);

  // User API to Delete YSQL database tables.
  Status DeleteYsqlDatabase(const DeleteNamespaceRequestPB* req,
                            DeleteNamespaceResponsePB* resp,
                            rpc::RpcContext* rpc);

  // Work to delete YSQL database tables, handled asynchronously from the User API call.
  void DeleteYsqlDatabaseAsync(scoped_refptr<NamespaceInfo> database);

  // Work to delete YCQL database, handled asynchronously from the User API call.
  void DeleteYcqlDatabaseAsync(scoped_refptr<NamespaceInfo> database);

  // Delete all tables in YSQL database.
  Status DeleteYsqlDBTables(const scoped_refptr<NamespaceInfo>& database);

  // List all the current namespaces.
  Status ListNamespaces(const ListNamespacesRequestPB* req,
                        ListNamespacesResponsePB* resp);

  // Get information about a namespace.
  Status GetNamespaceInfo(const GetNamespaceInfoRequestPB* req,
                          GetNamespaceInfoResponsePB* resp,
                          rpc::RpcContext* rpc);

  // Set Redis Config
  Status RedisConfigSet(const RedisConfigSetRequestPB* req,
                        RedisConfigSetResponsePB* resp,
                        rpc::RpcContext* rpc);

  // Get Redis Config
  Status RedisConfigGet(const RedisConfigGetRequestPB* req,
                        RedisConfigGetResponsePB* resp,
                        rpc::RpcContext* rpc);

  Status CreateTablegroup(const CreateTablegroupRequestPB* req,
                          CreateTablegroupResponsePB* resp,
                          rpc::RpcContext* rpc);

  Status DeleteTablegroup(const DeleteTablegroupRequestPB* req,
                          DeleteTablegroupResponsePB* resp,
                          rpc::RpcContext* rpc);

  // List all the current tablegroups for a namespace.
  Status ListTablegroups(const ListTablegroupsRequestPB* req,
                         ListTablegroupsResponsePB* resp,
                         rpc::RpcContext* rpc);

  // Create a new User-Defined Type with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status CreateUDType(const CreateUDTypeRequestPB* req,
                      CreateUDTypeResponsePB* resp,
                      rpc::RpcContext* rpc);

  // Delete the specified UDType.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  Status DeleteUDType(const DeleteUDTypeRequestPB* req,
                      DeleteUDTypeResponsePB* resp,
                      rpc::RpcContext* rpc);

  // List all user defined types in given namespaces.
  Status ListUDTypes(const ListUDTypesRequestPB* req,
                     ListUDTypesResponsePB* resp);

  // Get the info (id, name, namespace, fields names, field types) of a (user-defined) type.
  Status GetUDTypeInfo(const GetUDTypeInfoRequestPB* req,
                       GetUDTypeInfoResponsePB* resp,
                       rpc::RpcContext* rpc);

  // Disables tablet splitting for a specified amount of time.
  Status DisableTabletSplitting(
      const DisableTabletSplittingRequestPB* req, DisableTabletSplittingResponsePB* resp,
      rpc::RpcContext* rpc);

  void DisableTabletSplittingInternal(const MonoDelta& duration, const std::string& feature);

  // Returns true if there are no outstanding tablets and the tablet split manager is not currently
  // processing tablet splits.
  Status IsTabletSplittingComplete(
      const IsTabletSplittingCompleteRequestPB* req, IsTabletSplittingCompleteResponsePB* resp,
      rpc::RpcContext* rpc);

  bool IsTabletSplittingCompleteInternal(bool wait_for_parent_deletion);

  // Delete CDC streams for a table.
  virtual Status DeleteCDCStreamsForTable(const TableId& table_id) EXCLUDES(mutex_);
  virtual Status DeleteCDCStreamsForTables(const std::vector<TableId>& table_ids)
      EXCLUDES(mutex_);

  // Delete CDC streams metadata for a table.
  virtual Status DeleteCDCStreamsMetadataForTable(const TableId& table_id) EXCLUDES(mutex_);
  virtual Status DeleteCDCStreamsMetadataForTables(const std::vector<TableId>& table_ids)
      EXCLUDES(mutex_);

  // Add new table metadata to all CDCSDK streams of required namespace.
  virtual Status AddNewTableToCDCDKStreamsMetadata(
      const TableId& table_id, const NamespaceId& ns_id) EXCLUDES(mutex_);

  virtual Status ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                                              ChangeEncryptionInfoResponsePB* resp);

  Status UpdateXClusterConsumerOnTabletSplit(
      const TableId& consumer_table_id, const SplitTabletIds& split_tablet_ids) override {
    // Default value.
    return Status::OK();
  }

  Status UpdateCDCProducerOnTabletSplit(
      const TableId& producer_table_id, const SplitTabletIds& split_tablet_ids) override {
    // Default value.
    return Status::OK();
  }

  Result<uint64_t> IncrementYsqlCatalogVersion() override;

  // Records the fact that initdb has succesfully completed.
  Status InitDbFinished(Status initdb_status, int64_t term);

  // Check if the initdb operation has been completed. This is intended for use by whoever wants
  // to wait for the cluster to be fully initialized, e.g. minicluster, YugaWare, etc.
  Status IsInitDbDone(
      const IsInitDbDoneRequestPB* req, IsInitDbDoneResponsePB* resp) override;

  Status GetYsqlCatalogVersion(
      uint64_t* catalog_version, uint64_t* last_breaking_version) override;
  Status GetYsqlAllDBCatalogVersions(DbOidToCatalogVersionMap* versions) override;
  Status GetYsqlDBCatalogVersion(
      uint32_t db_oid, uint64_t* catalog_version, uint64_t* last_breaking_version) override;

  Status InitializeTransactionTablesConfig(int64_t term);

  Status IncrementTransactionTablesVersion();

  uint64_t GetTransactionTablesVersion() override;

  Status WaitForTransactionTableVersionUpdateToPropagate();

  virtual Status FillHeartbeatResponse(const TSHeartbeatRequestPB* req,
                                               TSHeartbeatResponsePB* resp);

  SysCatalogTable* sys_catalog() override { return sys_catalog_.get(); }

  // Tablet peer for the sys catalog tablet's peer.
  std::shared_ptr<tablet::TabletPeer> tablet_peer() const override;

  ClusterLoadBalancer* load_balancer() override { return load_balance_policy_.get(); }

  TabletSplitManager* tablet_split_manager() override { return &tablet_split_manager_; }

  XClusterSafeTimeService* TEST_xcluster_safe_time_service() override {
    return xcluster_safe_time_service_.get();
  }

  // Dump all of the current state about tables and tablets to the
  // given output stream. This is verbose, meant for debugging.
  void DumpState(std::ostream* out, bool on_disk_dump = false) const override;

  void SetLoadBalancerEnabled(bool is_enabled);

  bool IsLoadBalancerEnabled() override;

  // Return the table info for the table with the specified UUID, if it exists.
  TableInfoPtr GetTableInfo(const TableId& table_id) override;
  TableInfoPtr GetTableInfoUnlocked(const TableId& table_id) REQUIRES_SHARED(mutex_);

  // Get Table info given namespace id and table name.
  // Does not work for YSQL tables because of possible ambiguity.
  scoped_refptr<TableInfo> GetTableInfoFromNamespaceNameAndTableName(
      YQLDatabase db_type, const NamespaceName& namespace_name,
      const TableName& table_name) override;

  // Return TableInfos according to specified mode.
  std::vector<TableInfoPtr> GetTables(GetTablesMode mode) override;

  // Return all the available NamespaceInfo. The flag 'includeOnlyRunningNamespaces' determines
  // whether to retrieve all Namespaces irrespective of their state or just 'RUNNING' namespaces.
  // To retrieve all live tables in the system, you should set this flag to true.
  void GetAllNamespaces(std::vector<scoped_refptr<NamespaceInfo> >* namespaces,
                        bool include_only_running_namespaces = false) override;

  // Return all the available (user-defined) types.
  void GetAllUDTypes(std::vector<scoped_refptr<UDTypeInfo>>* types) override;

  // Return the recent tasks.
  std::vector<std::shared_ptr<server::MonitoredTask>> GetRecentTasks() override;

  // Return the recent user-initiated jobs.
  std::vector<std::shared_ptr<server::MonitoredTask>> GetRecentJobs() override;

  NamespaceName GetNamespaceNameUnlocked(const NamespaceId& id) const REQUIRES_SHARED(mutex_);
  NamespaceName GetNamespaceName(const NamespaceId& id) const override;

  NamespaceName GetNamespaceNameUnlocked(const scoped_refptr<TableInfo>& table) const
      REQUIRES_SHARED(mutex_);
  NamespaceName GetNamespaceName(const scoped_refptr<TableInfo>& table) const;

  // Is the table a system table?
  bool IsSystemTable(const TableInfo& table) const override;

  // Is the table a user created table?
  bool IsUserTable(const TableInfo& table) const override;
  bool IsUserTableUnlocked(const TableInfo& table) const REQUIRES_SHARED(mutex_);

  // Is the table a user created index?
  bool IsUserIndex(const TableInfo& table) const override;
  bool IsUserIndexUnlocked(const TableInfo& table) const REQUIRES_SHARED(mutex_);

  // Is the table a special sequences system table?
  bool IsSequencesSystemTable(const TableInfo& table) const;

  // Is the table a materialized view?
  bool IsMatviewTable(const TableInfo& table) const;

  // Is the table created by user?
  // Note that table can be regular table or index in this case.
  bool IsUserCreatedTable(const TableInfo& table) const override;
  bool IsUserCreatedTableUnlocked(const TableInfo& table) const REQUIRES_SHARED(mutex_);

  // Let the catalog manager know that we have received a response for a prepare delete
  // transaction tablet request. This will trigger delete tablet requests on all replicas.
  void NotifyPrepareDeleteTransactionTabletFinished(
      const scoped_refptr<TabletInfo>& tablet, const std::string& msg, HideOnly hide_only) override;

  // Let the catalog manager know that we have received a response for a delete tablet request,
  // and that we either deleted the tablet successfully, or we received a fatal error.
  //
  // Async tasks should call this when they finish. The last such tablet peer notification will
  // trigger trying to transition the table from DELETING to DELETED state.
  void NotifyTabletDeleteFinished(
      const TabletServerId& tserver_uuid, const TabletId& tablet_id,
      const TableInfoPtr& table) override;

  // For a DeleteTable, we first mark tables as DELETING then move them to DELETED once all
  // outstanding tasks are complete and the TS side tablets are deleted.
  // For system tables or colocated tables, we just need outstanding tasks to be done.
  //
  // If all conditions are met, returns a locked write lock on this table.
  // Otherwise lock is default constructed, i.e. not locked.
  TableInfo::WriteLock PrepareTableDeletion(const TableInfoPtr& table);
  bool ShouldDeleteTable(const TableInfoPtr& table);

  // Used by ConsensusService to retrieve the TabletPeer for a system
  // table specified by 'tablet_id'.
  //
  // See also: TabletPeerLookupIf, ConsensusServiceImpl.
  Result<tablet::TabletPeerPtr> GetServingTablet(const TabletId& tablet_id) const override;
  Result<tablet::TabletPeerPtr> GetServingTablet(const Slice& tablet_id) const override;

  const NodeInstancePB& NodeInstance() const override;

  Status GetRegistration(ServerRegistrationPB* reg) const override;

  bool IsInitialized() const;

  virtual Status StartRemoteBootstrap(const consensus::StartRemoteBootstrapRequestPB& req)
      override;

  // Checks that placement info can be accommodated by available ts_descs.
  Status CheckValidPlacementInfo(const PlacementInfoPB& placement_info,
                                 const TSDescriptorVector& ts_descs,
                                 ValidateReplicationInfoResponsePB* resp);

  // Loops through the table's placement infos and populates the corresponding config from
  // each placement.
  Status HandlePlacementUsingReplicationInfo(
      const ReplicationInfoPB& replication_info,
      const TSDescriptorVector& all_ts_descs,
      consensus::RaftConfigPB* config,
      CMPerTableLoadState* per_table_state,
      CMGlobalLoadState* global_state);

  // Handles the config creation for a given placement.
  Status HandlePlacementUsingPlacementInfo(const PlacementInfoPB& placement_info,
                                           const TSDescriptorVector& ts_descs,
                                           consensus::PeerMemberType member_type,
                                           consensus::RaftConfigPB* config,
                                           CMPerTableLoadState* per_table_state,
                                           CMGlobalLoadState* global_state);

  // Populates ts_descs with all tservers belonging to a certain placement.
  void GetTsDescsFromPlacementInfo(const PlacementInfoPB& placement_info,
                                   const TSDescriptorVector& all_ts_descs,
                                   TSDescriptorVector* ts_descs);

    // Set the current committed config.
  Status GetCurrentConfig(consensus::ConsensusStatePB *cpb) const override;

  // Return OK if this CatalogManager is a leader in a consensus configuration and if
  // the required leader state (metadata for tables and tablets) has
  // been successfully loaded into memory. CatalogManager must be
  // initialized before calling this method.
  Status CheckIsLeaderAndReady() const override;

  // Returns this CatalogManager's role in a consensus configuration. CatalogManager
  // must be initialized before calling this method.
  PeerRole Role() const;

  Status PeerStateDump(const std::vector<consensus::RaftPeerPB>& masters_raft,
                       const DumpMasterStateRequestPB* req,
                       DumpMasterStateResponsePB* resp);

  // If we get removed from an existing cluster, leader might ask us to detach ourselves from the
  // cluster. So we enter a shell mode equivalent state, with no bg tasks and no tablet peer
  // nor consensus.
  Status GoIntoShellMode();

  // Setters and getters for the cluster config item.
  //
  // To change the cluster config, a client would need to do a client-side read-modify-write by
  // issuing a get for the latest config, obtaining the current valid config (together with its
  // respective version number), modify the values it wants of said config and issuing a write
  // afterwards, without changing the version number. In case the version number does not match
  // on the server, the change will fail and the client will have to retry the get, as someone
  // must havGetTableInfoe updated the config in the meantime.
  Status GetClusterConfig(GetMasterClusterConfigResponsePB* resp) override;
  Status GetClusterConfig(SysClusterConfigEntryPB* config) override;
  Result<int32_t> GetClusterConfigVersion();

  Status SetClusterConfig(
      const ChangeMasterClusterConfigRequestPB* req,
      ChangeMasterClusterConfigResponsePB* resp) override;


  // Validator for placement information with respect to cluster configuration
  Status ValidateReplicationInfo(
      const ValidateReplicationInfoRequestPB* req, ValidateReplicationInfoResponsePB* resp);

  Status SetPreferredZones(
      const SetPreferredZonesRequestPB* req, SetPreferredZonesResponsePB* resp);

  Result<size_t> GetReplicationFactor() override;
  Result<size_t> GetReplicationFactorForTablet(const scoped_refptr<TabletInfo>& tablet);

  void GetExpectedNumberOfReplicas(int* num_live_replicas, int* num_read_replicas);

  // Get the percentage of tablets that have been moved off of the black-listed tablet servers.
  Status GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp);

  // Get the percentage of leaders that have been moved off of the leader black-listed tablet
  // servers.
  Status GetLeaderBlacklistCompletionPercent(GetLoadMovePercentResponsePB* resp);

  // Get the percentage of leaders/tablets that have been moved off of the (leader) black-listed
  // tablet servers.
  Status GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp,
      bool blacklist_leader);

  // API to check if all the live tservers have similar tablet workload.
  Status IsLoadBalanced(const IsLoadBalancedRequestPB* req,
                        IsLoadBalancedResponsePB* resp) override;

  MonoTime LastLoadBalancerRunTime() const;

  Status IsLoadBalancerIdle(const IsLoadBalancerIdleRequestPB* req,
                            IsLoadBalancerIdleResponsePB* resp);

  // API to check that all tservers that shouldn't have leader load do not.
  Status AreLeadersOnPreferredOnly(const AreLeadersOnPreferredOnlyRequestPB* req,
                                   AreLeadersOnPreferredOnlyResponsePB* resp);

  // Return the placement uuid of the primary cluster containing this master.
  Result<std::string> placement_uuid() const;

  // Clears out the existing metadata ('table_names_map_', 'table_ids_map_',
  // and 'tablet_map_'), loads tables metadata into memory and if successful
  // loads the tablets metadata.
  Status VisitSysCatalog(int64_t term) override;
  virtual Status RunLoaders(int64_t term) REQUIRES(mutex_);

  // Waits for the worker queue to finish processing, returns OK if worker queue is idle before
  // the provided timeout, TimedOut Status otherwise.
  Status WaitForWorkerPoolTests(
      const MonoDelta& timeout = MonoDelta::FromSeconds(10)) const override;

  // Get the disk size of tables (Used for YSQL \d+ command)
  Status GetTableDiskSize(
      const GetTableDiskSizeRequestPB* req, GetTableDiskSizeResponsePB* resp, rpc::RpcContext* rpc);

  Result<scoped_refptr<UDTypeInfo>> FindUDTypeById(
      const UDTypeId& udt_id) const EXCLUDES(mutex_);

  Result<scoped_refptr<UDTypeInfo>> FindUDTypeByIdUnlocked(
      const UDTypeId& udt_id) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespaceUnlocked(
      const NamespaceIdentifierPB& ns_identifier) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespace(
      const NamespaceIdentifierPB& ns_identifier) const EXCLUDES(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespaceById(
      const NamespaceId& id) const override EXCLUDES(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespaceByIdUnlocked(
      const NamespaceId& id) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<TableInfo>> FindTableUnlocked(
      const TableIdentifierPB& table_identifier) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<TableInfo>> FindTable(
      const TableIdentifierPB& table_identifier) const override EXCLUDES(mutex_);

  Result<scoped_refptr<TableInfo>> FindTableById(
      const TableId& table_id) const override EXCLUDES(mutex_);

  Result<scoped_refptr<TableInfo>> FindTableByIdUnlocked(
      const TableId& table_id) const REQUIRES_SHARED(mutex_);

  Result<bool> TableExists(
      const std::string& namespace_name, const std::string& table_name) const EXCLUDES(mutex_);

  Result<TableDescription> DescribeTable(
      const TableIdentifierPB& table_identifier, bool succeed_if_create_in_progress);

  Result<TableDescription> DescribeTable(
      const TableInfoPtr& table_info, bool succeed_if_create_in_progress);

  Result<std::string> GetPgSchemaName(const TableInfoPtr& table_info) REQUIRES_SHARED(mutex_);

  Result<std::unordered_map<std::string, uint32_t>> GetPgAttNameTypidMap(
      const TableInfoPtr& table_info) REQUIRES_SHARED(mutex_);

  Result<std::unordered_map<uint32_t, PgTypeInfo>> GetPgTypeInfo(
      const scoped_refptr<NamespaceInfo>& namespace_info, std::vector<uint32_t>* type_oids)
      REQUIRES_SHARED(mutex_);

  void AssertLeaderLockAcquiredForReading() const override {
    leader_lock_.AssertAcquiredForReading();
  }

  std::string GenerateId() override {
    return GenerateId(boost::none);
  }

  std::string GenerateId(boost::optional<const SysRowEntryType> entity_type);
  std::string GenerateIdUnlocked(boost::optional<const SysRowEntryType> entity_type = boost::none)
      REQUIRES_SHARED(mutex_);

  ThreadPool* AsyncTaskPool() override { return async_task_pool_.get(); }

  PermissionsManager* permissions_manager() override {
    return permissions_manager_.get();
  }

  intptr_t tablets_version() const override NO_THREAD_SAFETY_ANALYSIS {
    // This method should not hold the lock, because Version method is thread safe.
    return tablet_map_.Version() + tables_.Version();
  }

  intptr_t tablet_locations_version() const override {
    return tablet_locations_version_.load(std::memory_order_acquire);
  }

  EncryptionManager& encryption_manager() {
    return *encryption_manager_;
  }

  client::UniverseKeyClient& universe_key_client() {
    return *universe_key_client_;
  }

  Status FlushSysCatalog(const FlushSysCatalogRequestPB* req,
                         FlushSysCatalogResponsePB* resp,
                         rpc::RpcContext* rpc);

  Status CompactSysCatalog(const CompactSysCatalogRequestPB* req,
                           CompactSysCatalogResponsePB* resp,
                           rpc::RpcContext* rpc);

  Status SplitTablet(const TabletId& tablet_id, ManualSplit is_manual_split) override;

  // Splits tablet specified in the request using middle of the partition as a split point.
  Status SplitTablet(
      const SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext* rpc);

  // Deletes a tablet that is no longer serving user requests. This would require that the tablet
  // has been split and both of its children are now in RUNNING state and serving user requests
  // instead.
  Status DeleteNotServingTablet(
      const DeleteNotServingTabletRequestPB* req, DeleteNotServingTabletResponsePB* resp,
      rpc::RpcContext* rpc);

  Status DdlLog(
      const DdlLogRequestPB* req, DdlLogResponsePB* resp, rpc::RpcContext* rpc);

  // Test wrapper around protected DoSplitTablet method.
  Status TEST_SplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info,
      docdb::DocKeyHash split_hash_code) override;

  Status TEST_SplitTablet(
      const TabletId& tablet_id, const std::string& split_encoded_key,
      const std::string& split_partition_key) override;

  Status TEST_IncrementTablePartitionListVersion(const TableId& table_id) override;

  Status TEST_SendTestRetryRequest(
      const PeerId& peer_id, int32_t num_retries, StdStatusCallback callback);

  // Schedule a task to run on the async task thread pool.
  Status ScheduleTask(std::shared_ptr<RetryingTSRpcTask> task) override;

  // Time since this peer became master leader. Caller should verify that it is leader before.
  MonoDelta TimeSinceElectedLeader();

  Result<std::vector<TableDescription>> CollectTables(
      const google::protobuf::RepeatedPtrField<TableIdentifierPB>& table_identifiers,
      bool add_indexes,
      bool include_parent_colocated_table = false) override;

  Result<std::vector<TableDescription>> CollectTables(
      const google::protobuf::RepeatedPtrField<TableIdentifierPB>& table_identifiers,
      CollectFlags flags,
      std::unordered_set<NamespaceId>* namespaces = nullptr);

  // Returns 'table_replication_info' itself if set. Else looks up placement info for its
  // 'tablespace_id'. If neither is set, returns the cluster level replication info.
  Result<ReplicationInfoPB> GetTableReplicationInfo(
      const ReplicationInfoPB& table_replication_info,
      const TablespaceId& tablespace_id) override;

  Result<ReplicationInfoPB> GetTableReplicationInfo(
      const scoped_refptr<const TableInfo>& table) const;

  Result<size_t> GetTableReplicationFactor(const TableInfoPtr& table) const override;

  Result<boost::optional<TablespaceId>> GetTablespaceForTable(
      const scoped_refptr<TableInfo>& table) override;

  void ProcessTabletStorageMetadata(
      const std::string& ts_uuid,
      const TabletDriveStorageMetadataPB& storage_metadata);

  virtual Status ProcessTabletReplicationStatus(
      const TabletReplicationStatusPB& replication_state) EXCLUDES(mutex_);

  void CheckTableDeleted(const TableInfoPtr& table) override;

  Status ShouldSplitValidCandidate(
      const TabletInfo& tablet_info, const TabletReplicaDriveInfo& drive_info) const override;

  Status GetAllAffinitizedZones(std::vector<AffinitizedZonesSet>* affinitized_zones) override;
  Result<std::vector<BlacklistSet>> GetAffinitizedZoneSet();
  Result<BlacklistSet> BlacklistSetFromPB(bool leader_blacklist = false) const override;

  std::vector<std::string> GetMasterAddresses();

  // Returns true if there is at-least one snapshot schedule on any database/keyspace
  // in the cluster.
  Status CheckIfPitrActive(
    const CheckIfPitrActiveRequestPB* req, CheckIfPitrActiveResponsePB* resp);

  // Get the parent table id for a colocated table. The table parameter must be colocated and
  // not satisfy IsColocationParentTableId.
  Result<TableId> GetParentTableIdForColocatedTable(const scoped_refptr<TableInfo>& table);

  Result<std::optional<cdc::ConsumerRegistryPB>> GetConsumerRegistry();
  Result<XClusterNamespaceToSafeTimeMap> GetXClusterNamespaceToSafeTimeMap();
  Status SetXClusterNamespaceToSafeTimeMap(
      const int64_t leader_term, const XClusterNamespaceToSafeTimeMap& safe_time_map);

  Status GetXClusterEstimatedDataLoss(
      const GetXClusterEstimatedDataLossRequestPB* req,
      GetXClusterEstimatedDataLossResponsePB* resp);

  Status GetXClusterSafeTime(
      const GetXClusterSafeTimeRequestPB* req, GetXClusterSafeTimeResponsePB* resp);

  Status SubmitToSysCatalog(std::unique_ptr<tablet::Operation> operation);

  Status PromoteAutoFlags(const PromoteAutoFlagsRequestPB* req, PromoteAutoFlagsResponsePB* resp);

 protected:
  // TODO Get rid of these friend classes and introduce formal interface.
  friend class TableLoader;
  friend class TabletLoader;
  friend class NamespaceLoader;
  friend class UDTypeLoader;
  friend class ClusterConfigLoader;
  friend class RoleLoader;
  friend class RedisConfigLoader;
  friend class SysConfigLoader;
  friend class XClusterSafeTimeLoader;
  friend class ::yb::master::ScopedLeaderSharedLock;
  friend class PermissionsManager;
  friend class MultiStageAlterTable;
  friend class BackfillTable;
  friend class BackfillTablet;

  FRIEND_TEST(SysCatalogTest, TestCatalogManagerTasksTracker);
  FRIEND_TEST(SysCatalogTest, TestPrepareDefaultClusterConfig);
  FRIEND_TEST(SysCatalogTest, TestSysCatalogTablesOperations);
  FRIEND_TEST(SysCatalogTest, TestSysCatalogTabletsOperations);
  FRIEND_TEST(SysCatalogTest, TestTableInfoCommit);

  FRIEND_TEST(MasterTest, TestTabletsDeletedWhenTableInDeletingState);
  FRIEND_TEST(yb::MasterPartitionedTest, VerifyOldLeaderStepsDown);

  // Called by SysCatalog::SysCatalogStateChanged when this node
  // becomes the leader of a consensus configuration.
  //
  // Executes LoadSysCatalogDataTask below and marks the current time as time since leader.
  Status ElectedAsLeaderCb();

  // Loops and sleeps until one of the following conditions occurs:
  // 1. The current node is the leader master in the current term
  //    and at least one op from the current term is committed. Returns OK.
  // 2. The current node is not the leader master.
  //    Returns IllegalState.
  // 3. The provided timeout expires. Returns TimedOut.
  //
  // This method is intended to ensure that all operations replicated by
  // previous masters are committed and visible to the local node before
  // reading that data, to ensure consistency across failovers.
  Status WaitUntilCaughtUpAsLeader(const MonoDelta& timeout) override;

  // This method is submitted to 'leader_initialization_pool_' by
  // ElectedAsLeaderCb above. It:
  // 1) Acquired 'lock_'
  // 2) Runs the various Visitors defined below
  // 3) Releases 'lock_' and if successful, updates 'leader_ready_term_'
  // to true (under state_lock_).
  void LoadSysCatalogDataTask();

  // This method checks that resource such as keyspace is available for GrantRevokePermission
  // request.
  // Since this method takes lock on mutex_, it is separated out of permissions manager
  // so that the thread safety relationship between the two managers is easy to reason about.
  Status CheckResource(const GrantRevokePermissionRequestPB* req,
                       GrantRevokePermissionResponsePB* resp);

  // Generated the default entry for the cluster config, that is written into sys_catalog on very
  // first leader election of the cluster.
  //
  // Sets the version field of the SysClusterConfigEntryPB to 0.
  Status PrepareDefaultClusterConfig(int64_t term) REQUIRES(mutex_);

  // Sets up various system configs.
  Status PrepareDefaultSysConfig(int64_t term) REQUIRES(mutex_);

  // Starts an asynchronous run of initdb. Errors are handled in the callback. Returns true
  // if started running initdb, false if decided that it is not needed.
  Result<bool> StartRunningInitDbIfNeeded(int64_t term) REQUIRES_SHARED(mutex_);

  Status PrepareDefaultNamespaces(int64_t term) REQUIRES(mutex_);

  Status PrepareSystemTables(int64_t term) REQUIRES(mutex_);

  Status PrepareSysCatalogTable(int64_t term) REQUIRES(mutex_);

  template <class T>
  Status PrepareSystemTableTemplate(const TableName& table_name,
                                    const NamespaceName& namespace_name,
                                    const NamespaceId& namespace_id,
                                    int64_t term) REQUIRES(mutex_);

  Status PrepareSystemTable(const TableName& table_name,
                            const NamespaceName& namespace_name,
                            const NamespaceId& namespace_id,
                            const Schema& schema,
                            int64_t term,
                            YQLVirtualTable* vtable) REQUIRES(mutex_);

  Status PrepareNamespace(YQLDatabase db_type,
                          const NamespaceName& name,
                          const NamespaceId& id,
                          int64_t term) REQUIRES(mutex_);

  void ProcessPendingNamespace(NamespaceId id,
                               std::vector<scoped_refptr<TableInfo>> template_tables,
                               TransactionMetadata txn);

  // Called when transaction associated with NS create finishes. Verifies postgres layer present.
  Status VerifyNamespacePgLayer(scoped_refptr<NamespaceInfo> ns, bool txn_query_succeeded);

  Status ConsensusStateToTabletLocations(const consensus::ConsensusStatePB& cstate,
                                         TabletLocationsPB* locs_pb);

  // Creates the table and associated tablet objects in-memory and updates the appropriate
  // catalog manager maps.
  Status CreateTableInMemory(const CreateTableRequestPB& req,
                             const Schema& schema,
                             const PartitionSchema& partition_schema,
                             const NamespaceId& namespace_id,
                             const NamespaceName& namespace_name,
                             const std::vector<Partition>& partitions,
                             bool colocated,
                             IndexInfoPB* index_info,
                             TabletInfos* tablets,
                             CreateTableResponsePB* resp,
                             scoped_refptr<TableInfo>* table) REQUIRES(mutex_);

  Result<TabletInfos> CreateTabletsFromTable(const std::vector<Partition>& partitions,
                                             const TableInfoPtr& table) REQUIRES(mutex_);

  // Helper for creating copartitioned table.
  Status CreateCopartitionedTable(const CreateTableRequestPB& req,
                                  CreateTableResponsePB* resp,
                                  rpc::RpcContext* rpc,
                                  Schema schema,
                                  scoped_refptr<NamespaceInfo> ns);

  // Check that local host is present in master addresses for normal master process start.
  // On error, it could imply that master_addresses is incorrectly set for shell master startup
  // or that this master host info was missed in the master addresses and it should be
  // participating in the very first quorum setup.
  Status CheckLocalHostInMasterAddresses();

  // Helper for initializing 'sys_catalog_'. After calling this
  // method, the caller should call WaitUntilRunning() on sys_catalog_
  // WITHOUT holding 'lock_' to wait for consensus to start for
  // sys_catalog_.
  //
  // This method is thread-safe.
  Status InitSysCatalogAsync();

  // Helper for creating the initial TableInfo state
  // Leaves the table "write locked" with the new info in the
  // "dirty" state field.
  scoped_refptr<TableInfo> CreateTableInfo(const CreateTableRequestPB& req,
                                           const Schema& schema,
                                           const PartitionSchema& partition_schema,
                                           const NamespaceId& namespace_id,
                                           const NamespaceName& namespace_name,
                                           bool colocated,
                                           IndexInfoPB* index_info) REQUIRES(mutex_);

  // Helper for creating the initial TabletInfo state.
  // Leaves the tablet "write locked" with the new info in the
  // "dirty" state field.
  TabletInfoPtr CreateTabletInfo(TableInfo* table,
                                 const PartitionPB& partition) REQUIRES(mutex_);

  // Remove the specified entries from the protobuf field table_ids of a TabletInfo.
  Status RemoveTableIdsFromTabletInfo(
      TabletInfoPtr tablet_info, std::unordered_set<TableId> tables_to_remove);

  // Add index info to the indexed table.
  Status AddIndexInfoToTable(const scoped_refptr<TableInfo>& indexed_table,
                             CowWriteLock<PersistentTableInfo>* l_ptr,
                             const IndexInfoPB& index_info,
                             CreateTableResponsePB* resp);

  // Delete index info from the indexed table.
  Status MarkIndexInfoFromTableForDeletion(
      const TableId& indexed_table_id, const TableId& index_table_id, bool multi_stage,
      DeleteTableResponsePB* resp);

  // Delete index info from the indexed table.
  Status DeleteIndexInfoFromTable(
      const TableId& indexed_table_id, const TableId& index_table_id);

  // Builds the TabletLocationsPB for a tablet based on the provided TabletInfo.
  // Populates locs_pb and returns true on success.
  // Returns Status::ServiceUnavailable if tablet is not running.
  // Set include_inactive to true in order to also get information about hidden tablets.
  Status BuildLocationsForTablet(
      const scoped_refptr<TabletInfo>& tablet,
      TabletLocationsPB* locs_pb,
      IncludeInactive include_inactive = IncludeInactive::kFalse);

  // Check whether the tservers in the current replica map differs from those in the cstate when
  // processing a tablet report. Ignore the roles reported by the cstate, just compare the
  // tservers.
  bool ReplicaMapDiffersFromConsensusState(const scoped_refptr<TabletInfo>& tablet,
                                           const consensus::ConsensusStatePB& consensus_state);

  void ReconcileTabletReplicasInLocalMemoryWithReport(
      const scoped_refptr<TabletInfo>& tablet,
      const std::string& sender_uuid,
      const consensus::ConsensusStatePB& consensus_state,
      const ReportedTabletPB& report);

  // Register a tablet server whenever it heartbeats with a consensus configuration. This is
  // needed because we have logic in the Master that states that if a tablet
  // server that is part of a consensus configuration has not heartbeated to the Master yet, we
  // leave it out of the consensus configuration reported to clients.
  // TODO: See if we can remove this logic, as it seems confusing.
  void UpdateTabletReplicaInLocalMemory(TSDescriptor* ts_desc,
                                        const consensus::ConsensusStatePB* consensus_state,
                                        const ReportedTabletPB& report,
                                        const scoped_refptr<TabletInfo>& tablet_to_update);

  static void CreateNewReplicaForLocalMemory(TSDescriptor* ts_desc,
                                             const consensus::ConsensusStatePB* consensus_state,
                                             const ReportedTabletPB& report,
                                             const tablet::RaftGroupStatePB& state,
                                             TabletReplica* new_replica);

  // Extract the set of tablets that can be deleted and the set of tablets
  // that must be processed because not running yet.
  // Returns a map of table_id -> {tablet_info1, tablet_info2, etc.}.
  void ExtractTabletsToProcess(TabletInfos *tablets_to_delete,
                               TableToTabletInfos *tablets_to_process);

  // Determine whether any tables are in the DELETING state.
  bool AreTablesDeleting() override;

  // Task that takes care of the tablet assignments/creations.
  // Loops through the "not created" tablets and sends a CreateTablet() request.
  Status ProcessPendingAssignmentsPerTable(
      const TableId& table_id, const TabletInfos& tablets, CMGlobalLoadState* global_load_state);

  // Select a tablet server from 'ts_descs' on which to place a new replica.
  // Any tablet servers in 'excluded' are not considered.
  // REQUIRES: 'ts_descs' must include at least one non-excluded server.
  std::shared_ptr<TSDescriptor> SelectReplica(
      const TSDescriptorVector& ts_descs,
      std::set<TabletServerId>* excluded,
      CMPerTableLoadState* per_table_state, CMGlobalLoadState* global_state);

  // Select and assign a tablet server as the protege 'config'. This protege is selected from the
  // set of tservers in 'global_state' that have the lowest current protege load.
  Status SelectProtegeForTablet(
      TabletInfo* tablet, consensus::RaftConfigPB *config, CMGlobalLoadState* global_state);

  // Select N Replicas from online tablet servers (as specified by
  // 'ts_descs') for the specified tablet and populate the consensus configuration
  // object. If 'ts_descs' does not specify enough online tablet
  // servers to select the N replicas, return Status::InvalidArgument.
  //
  // This method is called by "ProcessPendingAssignmentsPerTable()".
  Status SelectReplicasForTablet(
      const TSDescriptorVector& ts_descs, TabletInfo* tablet,
      CMPerTableLoadState* per_table_state, CMGlobalLoadState* global_state);

  // Select N Replicas from the online tablet servers that have been chosen to respect the
  // placement information provided. Populate the consensus configuration object with choices and
  // also update the set of selected tablet servers, to not place several replicas on the same TS.
  // member_type indicated what type of replica to select for.
  //
  // This method is called by "SelectReplicasForTablet".
  void SelectReplicas(
      const TSDescriptorVector& ts_descs,
      size_t nreplicas, consensus::RaftConfigPB* config,
      std::set<TabletServerId>* already_selected_ts,
      consensus::PeerMemberType member_type,
      CMPerTableLoadState* per_table_state,
      CMGlobalLoadState* global_state);

  void HandleAssignPreparingTablet(TabletInfo* tablet,
                                   DeferredAssignmentActions* deferred);

  // Assign tablets and send CreateTablet RPCs to tablet servers.
  // The out param 'new_tablets' should have any newly-created TabletInfo
  // objects appended to it.
  void HandleAssignCreatingTablet(TabletInfo* tablet,
                                  DeferredAssignmentActions* deferred,
                                  TabletInfos* new_tablets);

  Status HandleTabletSchemaVersionReport(
      TabletInfo *tablet, uint32_t version,
      const scoped_refptr<TableInfo>& table = nullptr) override;

  // Send the create tablet requests to the selected peers of the consensus configurations.
  // The creation is async, and at the moment there is no error checking on the
  // caller side. We rely on the assignment timeout. If we don't see the tablet
  // after the timeout, we regenerate a new one and proceed with a new
  // assignment/creation.
  //
  // This method is part of the "ProcessPendingAssignmentsPerTable()"
  //
  // This must be called after persisting the tablet state as
  // CREATING to ensure coherent state after Master failover.
  Status SendCreateTabletRequests(const std::vector<TabletInfo*>& tablets);

  // Send the "alter table request" to all tablets of the specified table.
  //
  // Also, initiates the required AlterTable requests to backfill the Index.
  // Initially the index is set to be in a INDEX_PERM_DELETE_ONLY state, then
  // updated to INDEX_PERM_WRITE_AND_DELETE state; followed by backfilling. Once
  // all the tablets have completed backfilling, the index will be updated
  // to be in INDEX_PERM_READ_WRITE_AND_DELETE state.
  Status SendAlterTableRequest(const scoped_refptr<TableInfo>& table,
                               const AlterTableRequestPB* req = nullptr);

  Status SendAlterTableRequestInternal(const scoped_refptr<TableInfo>& table,
                                       const TransactionId& txn_id);

  // Start the background task to send the CopartitionTable() RPC to the leader for this
  // tablet.
  void SendCopartitionTabletRequest(const scoped_refptr<TabletInfo>& tablet,
                                    const scoped_refptr<TableInfo>& table);

  // Starts the background task to send the SplitTablet RPC to the leader for the specified tablet.
  Status SendSplitTabletRequest(
      const scoped_refptr<TabletInfo>& tablet, std::array<TabletId, kNumSplitParts> new_tablet_ids,
      const std::string& split_encoded_key, const std::string& split_partition_key);

  // Send the "truncate table request" to all tablets of the specified table.
  void SendTruncateTableRequest(const scoped_refptr<TableInfo>& table);

  // Start the background task to send the TruncateTable() RPC to the leader for this tablet.
  void SendTruncateTabletRequest(const scoped_refptr<TabletInfo>& tablet);

  // Truncate the specified table/index.
  Status TruncateTable(const TableId& table_id,
                       TruncateTableResponsePB* resp,
                       rpc::RpcContext* rpc);

  struct DeletingTableData {
    TableInfoPtr info;
    TableInfo::WriteLock write_lock;
    RepeatedBytes retained_by_snapshot_schedules;
    bool remove_from_name_map;
  };

  // Delete the specified table in memory. The TableInfo, DeletedTableInfo and lock of the deleted
  // table are appended to the lists. The caller will be responsible for committing the change and
  // deleting the actual table and tablets.
  Status DeleteTableInMemory(
      const TableIdentifierPB& table_identifier,
      bool is_index_table,
      bool update_indexed_table,
      const SnapshotSchedulesToObjectIdsMap& schedules_to_tables_map,
      std::vector<DeletingTableData>* tables,
      DeleteTableResponsePB* resp,
      rpc::RpcContext* rpc);

  // Request tablet servers to delete all replicas of the tablet.
  void DeleteTabletReplicas(
      TabletInfo* tablet, const std::string& msg, HideOnly hide_only, KeepData keep_data) override;

  // Returns error if and only if it is forbidden to both:
  // 1) Delete single tablet from table.
  // 2) Delete the whole table.
  // This is used for pre-checks in both `DeleteTablet` and `DeleteTabletsAndSendRequests`.
  Status CheckIfForbiddenToDeleteTabletOf(const scoped_refptr<TableInfo>& table);

  // Marks each of the tablets in the given table as deleted and triggers requests to the tablet
  // servers to delete them. The table parameter is expected to be given "write locked".
  Status DeleteTabletsAndSendRequests(
      const TableInfoPtr& table, const RepeatedBytes& retained_by_snapshot_schedules);

  // Marks each tablet as deleted and triggers requests to the tablet servers to delete them.
  Status DeleteTabletListAndSendRequests(
      const std::vector<scoped_refptr<TabletInfo>>& tablets, const std::string& deletion_msg,
      const RepeatedBytes& retained_by_snapshot_schedules, bool transaction_status_tablets);

  // Sends a prepare delete transaction tablet request to the leader of the status tablet.
  // This will be followed by delete tablet requests to each replica.
  Status SendPrepareDeleteTransactionTabletRequest(
      const scoped_refptr<TabletInfo>& tablet, const std::string& leader_uuid,
      const std::string& reason, HideOnly hide_only);

  // Send the "delete tablet request" to the specified TS/tablet.
  // The specified 'reason' will be logged on the TS.
  void SendDeleteTabletRequest(const TabletId& tablet_id,
                               tablet::TabletDataState delete_type,
                               const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
                               const scoped_refptr<TableInfo>& table,
                               TSDescriptor* ts_desc,
                               const std::string& reason,
                               HideOnly hide_only = HideOnly::kFalse,
                               KeepData keep_data = KeepData::kFalse);

  // Start a task to request the specified tablet leader to step down and optionally to remove
  // the server that is over-replicated. A new tablet server can be specified to start an election
  // immediately to become the new leader. If new_leader_ts_uuid is empty, the election will be run
  // following the protocol's default mechanism.
  void SendLeaderStepDownRequest(
      const scoped_refptr<TabletInfo>& tablet, const consensus::ConsensusStatePB& cstate,
      const std::string& change_config_ts_uuid, bool should_remove,
      const std::string& new_leader_ts_uuid = "");

  // Start a task to change the config to remove a certain voter because the specified tablet is
  // over-replicated.
  void SendRemoveServerRequest(
      const scoped_refptr<TabletInfo>& tablet, const consensus::ConsensusStatePB& cstate,
      const std::string& change_config_ts_uuid);

  // Start a task to change the config to add an additional voter because the
  // specified tablet is under-replicated.
  void SendAddServerRequest(
      const scoped_refptr<TabletInfo>& tablet, consensus::PeerMemberType member_type,
      const consensus::ConsensusStatePB& cstate, const std::string& change_config_ts_uuid);

  void GetPendingServerTasksUnlocked(const TableId &table_uuid,
                                     TabletToTabletServerMap *add_replica_tasks_map,
                                     TabletToTabletServerMap *remove_replica_tasks_map,
                                     TabletToTabletServerMap *stepdown_leader_tasks)
      REQUIRES_SHARED(mutex_);

  // Abort creation of 'table': abort all mutation for TabletInfo and
  // TableInfo objects (releasing all COW locks), abort all pending
  // tasks associated with the table, and erase any state related to
  // the table we failed to create from the in-memory maps
  // ('table_names_map_', 'table_ids_map_', 'tablet_map_' below).
  Status AbortTableCreation(TableInfo* table,
                            const TabletInfos& tablets,
                            const Status& s,
                            CreateTableResponsePB* resp);

  Status CreateTransactionStatusTablesForTablespaces(
      const TablespaceIdToReplicationInfoMap& tablespace_info,
      const TableToTablespaceIdMap& table_to_tablespace_map);

  void StartTablespaceBgTaskIfStopped();

  std::shared_ptr<YsqlTablespaceManager> GetTablespaceManager() const;

  Result<boost::optional<ReplicationInfoPB>> GetTablespaceReplicationInfoWithRetry(
      const TablespaceId& tablespace_id);

  // Report metrics.
  void ReportMetrics();

  // Reset metrics.
  void ResetMetrics();

  // Conventional "T xxx P yyy: " prefix for logging.
  std::string LogPrefix() const;

  // Removes all tasks from jobs_tracker_ and tasks_tracker_.
  void ResetTasksTrackers();
  // Aborts all tasks belonging to 'tables' and waits for them to finish.
  void AbortAndWaitForAllTasks(const std::vector<scoped_refptr<TableInfo>>& tables);
  void AbortAndWaitForAllTasksUnlocked() REQUIRES_SHARED(mutex_);

  // Can be used to create background_tasks_ field for this master.
  // Used on normal master startup or when master comes out of the shell mode.
  Status EnableBgTasks();

  // Helper function for RebuildYQLSystemPartitions to get the system.partitions tablet.
  Status GetYQLPartitionsVTable(std::shared_ptr<SystemTablet>* tablet);
  // Background task for automatically rebuilding system.partitions every
  // partitions_vtable_cache_refresh_secs seconds.
  void RebuildYQLSystemPartitions();

  // Registers new split tablet with `partition` for the same table as `source_tablet_info` tablet.
  // Does not change any other tablets and their partitions.
  // Returns TabletInfo for registered tablet.
  Result<TabletInfoPtr> RegisterNewTabletForSplit(
      TabletInfo* source_tablet_info, const PartitionPB& partition,
      TableInfo::WriteLock* table_write_lock, TabletInfo::WriteLock* tablet_write_lock)
      REQUIRES(mutex_);

  Result<scoped_refptr<TabletInfo>> GetTabletInfo(const TabletId& tablet_id) override
      EXCLUDES(mutex_);
  Result<scoped_refptr<TabletInfo>> GetTabletInfoUnlocked(const TabletId& tablet_id)
      REQUIRES_SHARED(mutex_);

  Status DoSplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, std::string split_encoded_key,
      std::string split_partition_key, ManualSplit is_manual_split);

  // Splits tablet using specified split_hash_code as a split point.
  Status DoSplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, docdb::DocKeyHash split_hash_code,
      ManualSplit is_manual_split);

  // Calculate the total number of replicas which are being handled by servers in state.
  int64_t GetNumRelevantReplicas(const BlacklistPB& state, bool leaders_only);

  int64_t leader_ready_term() override EXCLUDES(state_lock_) {
    std::lock_guard<simple_spinlock> l(state_lock_);
    return leader_ready_term_;
  }

  // Delete tables from internal map by id, if it has no more active tasks and tablets.
  // This function should only be called from the bg_tasks thread, in a single threaded fashion!
  void CleanUpDeletedTables();

  // Called when a new table id is added to table_ids_map_.
  void HandleNewTableId(const TableId& id);

  // Creates a new TableInfo object.
  scoped_refptr<TableInfo> NewTableInfo(TableId id, bool colocated) override;

  // Register the tablet server with the ts manager using the Raft config. This is called for
  // servers that are part of the Raft config but haven't registered as yet.
  Status RegisterTsFromRaftConfig(const consensus::RaftPeerPB& peer);

  template <class Loader>
  Status Load(const std::string& title, const int64_t term);

  virtual void Started() {}

  virtual void SysCatalogLoaded(int64_t term) { StartXClusterSafeTimeServiceIfStopped(); }

  // Ensure the sys catalog tablet respects the leader affinity and blacklist configuration.
  // Chooses an unblacklisted master in the highest priority affinity location to step down to. If
  // this master is not blacklisted and there is no unblacklisted master in a higher priority
  // affinity location than this one, does nothing.
  // If there is no unblacklisted master in an affinity zone, chooses an arbitrary master to step
  // down to.
  Status SysCatalogRespectLeaderAffinity();

  virtual Result<bool> IsTablePartOfSomeSnapshotSchedule(const TableInfo& table_info) override {
    // Default value.
    return false;
  }

  virtual Result<bool> IsTableUndergoingPitrRestore(const TableInfo& table_info) {
    // Default value.
    return false;
  }

  virtual bool IsCdcEnabled(const TableInfo& table_info) const {
    // Default value.
    return false;
  }

  virtual bool IsCdcEnabledUnlocked(const TableInfo& table_info) const REQUIRES_SHARED(mutex_) {
    // Default value.
    return false;
  }

  virtual bool IsCdcSdkEnabled(const TableInfo& table_info) {
    // Default value.
    return false;
  }

  virtual bool IsTablePartOfBootstrappingCdcStream(const TableInfo& table_info) const {
    // Default value.
    return false;
  }

  virtual bool IsTablePartOfBootstrappingCdcStreamUnlocked(const TableInfo& table_info) const
      REQUIRES_SHARED(mutex_) {
    // Default value.
    return false;
  }

  virtual bool IsTableCdcProducer(const TableInfo& table_info) const REQUIRES_SHARED(mutex_) {
    // Default value.
    return false;
  }

  virtual bool IsTableCdcConsumer(const TableInfo& table_info) const REQUIRES_SHARED(mutex_) {
    // Default value.
    return false;
  }

  virtual bool IsTablePartOfCDCSDK(const TableInfo& table_info) const REQUIRES_SHARED(mutex_) {
    // Default value.
    return false;
  }

  virtual Status ValidateNewSchemaWithCdc(const TableInfo& table_info, const Schema& new_schema)
      const {
    return Status::OK();
  }

  virtual Status ResumeCdcAfterNewSchema(const TableInfo& table_info,
                                         SchemaVersion last_compatible_consumer_schema_version) {
    return Status::OK();
  }

  virtual Result<SnapshotSchedulesToObjectIdsMap> MakeSnapshotSchedulesToObjectIdsMap(
      SysRowEntryType type) {
    return SnapshotSchedulesToObjectIdsMap();
  }

  virtual bool IsPitrActive() {
    return false;
  }

  Result<SnapshotScheduleId> FindCoveringScheduleForObject(
      SysRowEntryType type, const std::string& object_id);

  Status DoDeleteNamespace(const DeleteNamespaceRequestPB* req,
                           DeleteNamespaceResponsePB* resp,
                           rpc::RpcContext* rpc);

  std::shared_ptr<ClusterConfigInfo> ClusterConfig() const;

  Result<TableInfoPtr> GetGlobalTransactionStatusTable();

  Result<bool> IsCreateTableDone(const TableInfoPtr& table);

  // TODO: the maps are a little wasteful of RAM, since the TableInfo/TabletInfo
  // objects have a copy of the string key. But STL doesn't make it
  // easy to make a "gettable set".

  // Lock protecting the various in memory storage structures.
  using MutexType = rw_spinlock;
  using SharedLock = NonRecursiveSharedLock<MutexType>;
  using LockGuard = std::lock_guard<MutexType>;
  mutable MutexType mutex_;

  // Note: Namespaces and tables for YSQL databases are identified by their ids only and therefore
  // are not saved in the name maps below.

  // Data structure containing all tables.
  VersionTracker<TableIndex> tables_ GUARDED_BY(mutex_);

  // Table map: [namespace-id, table-name] -> TableInfo
  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  // Note that this map isn't used for YSQL tables.
  TableInfoByNameMap table_names_map_ GUARDED_BY(mutex_);

  // Set of table ids that are transaction status tables.
  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  TableIdSet transaction_table_ids_set_ GUARDED_BY(mutex_);

  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  // Tablet maps: tablet-id -> TabletInfo
  VersionTracker<TabletInfoMap> tablet_map_ GUARDED_BY(mutex_);

  // Tablets that was hidden instead of deleting, used to cleanup such tablets when time comes.
  std::vector<TabletInfoPtr> hidden_tablets_ GUARDED_BY(mutex_);

  // Split parent tablets that are now hidden and still being replicated by some CDC stream. Keep
  // track of these tablets until their children tablets start being polled, at which point they
  // can be deleted and cdc_state metadata can also be cleaned up. retained_by_xcluster_ is a
  // subset of hidden_tablets_.
  struct HiddenReplicationParentTabletInfo {
    TableId table_id_;
    std::string parent_tablet_id_;
    std::array<TabletId, kNumSplitParts> split_tablets_;
  };
  std::unordered_map<TabletId, HiddenReplicationParentTabletInfo> retained_by_xcluster_
      GUARDED_BY(mutex_);
  std::unordered_map<TabletId, HiddenReplicationParentTabletInfo> retained_by_cdcsdk_
      GUARDED_BY(mutex_);

  // TODO(jhe) Cleanup how we use ScheduledTaskTracker, move is_running and util functions to class.
  // Background task for deleting parent split tablets retained by xCluster streams.
  std::atomic<bool> cdc_parent_tablet_deletion_task_running_{false};
  rpc::ScheduledTaskTracker cdc_parent_tablet_deletion_task_;

  // Namespace maps: namespace-id -> NamespaceInfo and namespace-name -> NamespaceInfo
  NamespaceInfoMap namespace_ids_map_ GUARDED_BY(mutex_);
  NamespaceNameMapper namespace_names_mapper_ GUARDED_BY(mutex_);

  // User-Defined type maps: udtype-id -> UDTypeInfo and udtype-name -> UDTypeInfo
  UDTypeInfoMap udtype_ids_map_ GUARDED_BY(mutex_);
  UDTypeInfoByNameMap udtype_names_map_ GUARDED_BY(mutex_);

  // RedisConfig map: RedisConfigKey -> RedisConfigInfo
  typedef std::unordered_map<RedisConfigKey, scoped_refptr<RedisConfigInfo>> RedisConfigInfoMap;
  RedisConfigInfoMap redis_config_map_ GUARDED_BY(mutex_);

  // Config information.
  // IMPORTANT: The shared pointer that points to the cluster config
  // is only written to with a new object during a catalog load.
  // At all other times, the address pointed to remains the same
  // (thus the value of this shared ptr remains the same), only
  // the underlying object is read or modified via cow read/write lock mechanism.
  // We don't need a lock guard for changing this pointer value since
  // we already acquire the leader write lock during catalog loading,
  // so all concurrent accesses of this shared ptr -- either external via RPCs or
  // internal by the bg threads (bg_tasks and master_snapshot_coordinator threads)
  // are locked out since they grab the scoped leader shared lock that
  // depends on this leader lock.
  std::shared_ptr<ClusterConfigInfo> cluster_config_ = nullptr; // No GUARD, only write on load.

  // YSQL Catalog information.
  scoped_refptr<SysConfigInfo> ysql_catalog_config_ = nullptr; // No GUARD, only write on Load.

  // Transaction tables information.
  scoped_refptr<SysConfigInfo> transaction_tables_config_ =
      nullptr; // No GUARD, only write on Load.

  Master* const master_;
  Atomic32 closing_;

  std::unique_ptr<SysCatalogTable> sys_catalog_;

  // Mutex to avoid concurrent remote bootstrap sessions.
  std::mutex remote_bootstrap_mtx_;

  // Set to true if this master has received at least the superblock from a remote master.
  bool tablet_exists_;

  // Background thread, used to execute the catalog manager tasks
  // like the assignment and cleaner.
  friend class CatalogManagerBgTasks;
  std::unique_ptr<CatalogManagerBgTasks> background_tasks_;

  // Background threadpool, newer features use this (instead of the Background thread)
  // to execute time-lenient catalog manager tasks.
  std::unique_ptr<yb::ThreadPool> background_tasks_thread_pool_;

  // TODO: convert this to YB_DEFINE_ENUM for automatic pretty-printing.
  enum State {
    kConstructed,
    kStarting,
    kRunning,
    kClosing
  };

  // Lock protecting state_, leader_ready_term_
  mutable simple_spinlock state_lock_;
  State state_ GUARDED_BY(state_lock_);

  // Used to defer Master<->TabletServer work from reactor threads onto a thread where
  // blocking behavior is permissible.
  //
  // NOTE: Presently, this thread pool must contain only a single
  // thread (to correctly serialize invocations of ElectedAsLeaderCb
  // upon closely timed consecutive elections).
  std::unique_ptr<ThreadPool> leader_initialization_pool_;

  // Thread pool to do the async RPC task work.
  std::unique_ptr<ThreadPool> async_task_pool_;

  // This field is updated when a node becomes leader master,
  // waits for all outstanding uncommitted metadata (table and tablet metadata)
  // in the sys catalog to commit, and then reads that metadata into in-memory
  // data structures. This is used to "fence" client and tablet server requests
  // that depend on the in-memory state until this master can respond
  // correctly.
  int64_t leader_ready_term_ GUARDED_BY(state_lock_);

  // This field is set to true when the leader master has completed loading
  // metadata into in-memory structures. This can happen in two cases presently:
  // 1. When a new leader is elected
  // 2. When an existing leader executes a restore_snapshot_schedule
  // In case (1), the above leader_ready_term_ is sufficient to indicate
  // the completion of this stage since the new term is only set after load.
  // However, in case (2), since the before/after term is the same, the above
  // check will succeed even when load is not complete i.e. there's a small
  // window when there's a possibility that the master_service sends RPCs
  // to the leader. This window is after the sys catalog has been restored and
  // all records have been updated on disk and before it starts loading them
  // into the in-memory structures.
  bool is_catalog_loaded_ GUARDED_BY(state_lock_) = false;

  // Lock used to fence operations and leader elections. All logical operations
  // (i.e. create table, alter table, etc.) should acquire this lock for
  // reading. Following an election where this master is elected leader, it
  // should acquire this lock for writing before reloading the metadata.
  //
  // Readers should not acquire this lock directly; use ScopedLeadershipLock
  // instead.
  //
  // Always acquire this lock before state_lock_.
  RWMutex leader_lock_;

  // Async operations are accessing some private methods
  // (TODO: this stuff should be deferred and done in the background thread)
  friend class AsyncAlterTable;

  // Number of live tservers metric.
  scoped_refptr<AtomicGauge<uint32_t>> metric_num_tablet_servers_live_;

  // Number of dead tservers metric.
  scoped_refptr<AtomicGauge<uint32_t>> metric_num_tablet_servers_dead_;

  friend class ClusterLoadBalancer;

  // Policy for load balancing tablets on tablet servers.
  std::unique_ptr<ClusterLoadBalancer> load_balance_policy_;

  // Use the Raft config that has been bootstrapped to update the in-memory state of master options
  // and also the on-disk state of the consensus meta object.
  Status UpdateMastersListInMemoryAndDisk();

  // Tablets of system tables on the master indexed by the tablet id.
  std::unordered_map<std::string, std::shared_ptr<tablet::AbstractTablet>> system_tablets_;

  // Tablet of colocated databases indexed by the namespace id.
  std::unordered_map<NamespaceId, scoped_refptr<TabletInfo>> colocated_db_tablets_map_
      GUARDED_BY(mutex_);

  std::unique_ptr<YsqlTablegroupManager> tablegroup_manager_
      GUARDED_BY(mutex_);

  std::unordered_map<TableId, TableId> matview_pg_table_ids_map_
      GUARDED_BY(mutex_);

  boost::optional<std::future<Status>> initdb_future_;
  boost::optional<InitialSysCatalogSnapshotWriter> initial_snapshot_writer_;

  std::unique_ptr<PermissionsManager> permissions_manager_;

  // This is used for tracking that initdb has started running previously.
  std::atomic<bool> pg_proc_exists_{false};

  // Tracks most recent async tasks.
  scoped_refptr<TasksTracker> tasks_tracker_;

  // Tracks most recent user initiated jobs.
  scoped_refptr<TasksTracker> jobs_tracker_;

  std::unique_ptr<EncryptionManager> encryption_manager_;

  std::unique_ptr<client::UniverseKeyClient> universe_key_client_;

  // A pointer to the system.partitions tablet for the RebuildYQLSystemPartitions bg task.
  std::shared_ptr<SystemTablet> system_partitions_tablet_ = nullptr;

  // Handles querying and processing YSQL DDL Transactions as a catalog manager background task.
  std::unique_ptr<YsqlTransactionDdl> ysql_transaction_;

  std::atomic<MonoTime> time_elected_leader_;

  std::unique_ptr<client::YBClient> cdc_state_client_;

  // Mutex to avoid simultaneous creation of transaction tables for a tablespace.
  std::mutex tablespace_transaction_table_creation_mutex_;

  mutable MutexType backfill_mutex_;
  std::unordered_set<TableId> pending_backfill_tables_ GUARDED_BY(backfill_mutex_);

  // XCluster Safe Time information.
  XClusterSafeTimeInfo xcluster_safe_time_info_;

  std::unique_ptr<XClusterSafeTimeService> xcluster_safe_time_service_;

  void StartElectionIfReady(
      const consensus::ConsensusStatePB& cstate, TabletInfo* tablet);

  void StartXClusterSafeTimeServiceIfStopped();

  void CreateXClusterSafeTimeTableAndStartService();

 private:
  // Performs the provided action with the sys catalog shared tablet instance, or sets up an error
  // if the tablet is not found.
  template <class Req, class Resp, class F>
  Status PerformOnSysCatalogTablet(const Req& req, Resp* resp, const F& f);

  virtual bool CDCStreamExistsUnlocked(const CDCStreamId& id) REQUIRES_SHARED(mutex_);

  Status CollectTable(
      const TableDescription& table_description,
      CollectFlags flags,
      std::vector<TableDescription>* all_tables,
      std::unordered_set<TableId>* parent_colocated_table_ids);

  Status SplitTablet(const scoped_refptr<TabletInfo>& tablet, ManualSplit is_manual_split);

  void SplitTabletWithKey(
      const scoped_refptr<TabletInfo>& tablet, const std::string& split_encoded_key,
      const std::string& split_partition_key, ManualSplit is_manual_split);

  Status ValidateSplitCandidateTableCdc(const TableInfo& table) const override;
  Status ValidateSplitCandidateTableCdcUnlocked(const TableInfo& table) const
      REQUIRES_SHARED(mutex_);

  Status ValidateSplitCandidate(
      const scoped_refptr<TabletInfo>& tablet, ManualSplit is_manual_split) EXCLUDES(mutex_);
  Status ValidateSplitCandidateUnlocked(
      const scoped_refptr<TabletInfo>& tablet, ManualSplit is_manual_split)
      REQUIRES_SHARED(mutex_);

  // From the list of TServers in 'ts_descs', return the ones that match any placement policy
  // in 'placement_info'. Returns error if there are insufficient TServers to match the
  // required replication factor in placement_info.
  // NOTE: This function will only check whether the total replication factor can be
  // satisfied, and not the individual min_num_replicas in each placement block.
  Result<TSDescriptorVector> FindTServersForPlacementInfo(
      const PlacementInfoPB& placement_info,
      const TSDescriptorVector& ts_descs) const;

  // Using the TServer info in 'ts_descs', return the TServers that match 'pplacement_block'.
  // Returns error if there aren't enough TServers to fulfill the min_num_replicas requirement
  // outlined in 'placement_block'.
  Result<TSDescriptorVector> FindTServersForPlacementBlock(
      const PlacementBlockPB& placement_block,
      const TSDescriptorVector& ts_descs);

  bool IsReplicationInfoSet(const ReplicationInfoPB& replication_info);

  Status ValidateTableReplicationInfo(const ReplicationInfoPB& replication_info);

  // Return the id of the tablespace associated with a transaction status table, if any.
  boost::optional<TablespaceId> GetTransactionStatusTableTablespace(
      const scoped_refptr<TableInfo>& table) REQUIRES_SHARED(mutex_);

  // Clears tablespace id for a transaction status table, reverting it back to cluster default
  // if no placement has been set explicitly.
  void ClearTransactionStatusTableTablespace(
      const scoped_refptr<TableInfo>& table) REQUIRES(mutex_);

  // Checks if there are any transaction tables with tablespace id set for a tablespace not in
  // the given tablespace info map.
  bool CheckTransactionStatusTablesWithMissingTablespaces(
      const TablespaceIdToReplicationInfoMap& tablespace_info) EXCLUDES(mutex_);

  // Updates transaction tables' tablespace ids for tablespaces that don't exist.
  Status UpdateTransactionStatusTableTablespaces(
      const TablespaceIdToReplicationInfoMap& tablespace_info) EXCLUDES(mutex_);

  // Return the tablespaces in the system and their associated replication info from
  // pg catalog tables.
  Result<std::shared_ptr<TablespaceIdToReplicationInfoMap>> GetYsqlTablespaceInfo();

  // Return the table->tablespace mapping by reading the pg catalog tables.
  Result<std::shared_ptr<TableToTablespaceIdMap>> GetYsqlTableToTablespaceMap(
      const TablespaceIdToReplicationInfoMap& tablespace_info) EXCLUDES(mutex_);

  // Background task that refreshes the in-memory state for YSQL tables with their associated
  // tablespace info.
  // Note: This function should only ever be called by StartTablespaceBgTaskIfStopped().
  void RefreshTablespaceInfoPeriodically();

  // Helper function to schedule the next iteration of the tablespace info task.
  void ScheduleRefreshTablespaceInfoTask(const bool schedule_now = false);

  // Helper function to refresh the tablespace info.
  Status DoRefreshTablespaceInfo();

  // Processes committed consensus state for specified tablet from ts_desc.
  // Returns true if tablet was mutated.
  bool ProcessCommittedConsensusState(
      TSDescriptor* ts_desc,
      bool is_incremental,
      const ReportedTabletPB& report,
      std::map<TableId, TableInfo::WriteLock>* table_write_locks,
      const TabletInfoPtr& tablet,
      const TabletInfo::WriteLock& tablet_lock,
      std::map<TableId, scoped_refptr<TableInfo>>* tables,
      std::vector<RetryingTSRpcTaskPtr>* rpcs);

  struct ReportedTablet {
    TabletId tablet_id;
    TabletInfoPtr info;
    const ReportedTabletPB* report;
    std::map<TableId, scoped_refptr<TableInfo>> tables;
  };
  using ReportedTablets = std::vector<ReportedTablet>;

  // Process tablets batch while processing tablet report.
  Status ProcessTabletReportBatch(
      TSDescriptor* ts_desc,
      bool is_incremental,
      ReportedTablets::iterator begin,
      ReportedTablets::iterator end,
      TabletReportUpdatesPB* full_report_update,
      std::vector<RetryingTSRpcTaskPtr>* rpcs);

  size_t GetNumLiveTServersForPlacement(const PlacementId& placement_id);

  TSDescriptorVector GetAllLiveNotBlacklistedTServers() const;

  const YQLPartitionsVTable& GetYqlPartitionsVtable() const;

  void InitializeTableLoadState(
      const TableId& table_id, TSDescriptorVector ts_descs, CMPerTableLoadState* state);

  void InitializeGlobalLoadState(
      TSDescriptorVector ts_descs, CMGlobalLoadState* state);

  // Send a step down request for the sys catalog tablet to the specified master. If the step down
  // RPC response has an error, returns false. If the step down RPC is successful, returns true.
  // For any other failure, returns a non-OK status.
  Result<bool> SysCatalogLeaderStepDown(const ServerEntryPB& master);

  // Attempts to remove a colocated table from tablegroup.
  // NOOP if the table does not belong to one.
  Status TryRemoveFromTablegroup(const TableId& table_id);

  // Returns an AsyncDeleteReplica task throttler for the given tserver uuid.
  AsyncTaskThrottlerBase* GetDeleteReplicaTaskThrottler(
    const std::string& ts_uuid) EXCLUDES(delete_replica_task_throttler_per_ts_mutex_);

  // Should be bumped up when tablet locations are changed.
  std::atomic<uintptr_t> tablet_locations_version_{0};

  rpc::ScheduledTaskTracker refresh_yql_partitions_task_;

  mutable MutexType tablespace_mutex_;

  // The tablespace_manager_ encapsulates two maps that are periodically updated by a background
  // task that reads tablespace information from the PG catalog tables. The task creates a new
  // manager instance, populates it with the information read from the catalog tables and updates
  // this shared_ptr. The maps themselves are thus never updated (no inserts/deletes/updates)
  // once populated and are garbage collected once all references to them go out of scope.
  // No clients are expected to update the manager, they take a lock merely to copy the
  // shared_ptr and read from it.
  std::shared_ptr<YsqlTablespaceManager> tablespace_manager_ GUARDED_BY(tablespace_mutex_);

  // Whether the periodic job to update tablespace info is running.
  std::atomic<bool> tablespace_bg_task_running_;

  rpc::ScheduledTaskTracker refresh_ysql_tablespace_info_task_;

  ServerRegistrationPB server_registration_;

  TabletSplitManager tablet_split_manager_;

  mutable MutexType delete_replica_task_throttler_per_ts_mutex_;

  // Maps a tserver uuid to the AsyncTaskThrottler instance responsible for throttling outstanding
  // AsyncDeletaReplica tasks per destination.
  std::unordered_map<std::string, std::unique_ptr<DynamicAsyncTaskThrottler>>
    delete_replica_task_throttler_per_ts_ GUARDED_BY(delete_replica_task_throttler_per_ts_mutex_);

  DISALLOW_COPY_AND_ASSIGN(CatalogManager);
};

}  // namespace master
}  // namespace yb
