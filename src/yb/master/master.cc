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

#include "yb/master/master.h"

#include <algorithm>
#include <list>
#include <memory>
#include <vector>

#include <glog/logging.h>

#include "yb/client/auto_flags_manager.h"
#include "yb/client/async_initializer.h"
#include "yb/client/client.h"

#include "yb/common/wire_protocol.h"

#include "yb/consensus/consensus_meta.h"

#include "yb/gutil/bind.h"

#include "yb/master/auto_flags_orchestrator.h"
#include "yb/master/master_fwd.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/flush_manager.h"
#include "yb/master/master-path-handlers.h"
#include "yb/master/master_cluster.proxy.h"
#include "yb/master/master_service.h"
#include "yb/master/master_tablet_service.h"
#include "yb/master/master_util.h"
#include "yb/master/sys_catalog_constants.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/service_if.h"
#include "yb/rpc/service_pool.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/server/rpc_server.h"

#include "yb/tablet/maintenance_manager.h"

#include "yb/tserver/pg_client_service.h"
#include "yb/tserver/remote_bootstrap_service.h"
#include "yb/tserver/tablet_service.h"
#include "yb/tserver/tserver_shared_mem.h"

#include "yb/util/flags.h"
#include "yb/util/metrics.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/shared_lock.h"
#include "yb/util/status.h"
#include "yb/util/threadpool.h"

#include "yb/yql/pggate/ybc_pg_typedefs.h"

DEFINE_UNKNOWN_int32(master_rpc_timeout_ms, 1500,
             "Timeout for retrieving master registration over RPC.");
TAG_FLAG(master_rpc_timeout_ms, experimental);

DEFINE_UNKNOWN_int32(master_yb_client_default_timeout_ms, 60000,
             "Default timeout for the YBClient embedded into the master.");

METRIC_DEFINE_entity(cluster);

using namespace std::literals;
using std::min;
using std::vector;
using std::string;

using yb::consensus::RaftPeerPB;
using yb::rpc::ServiceIf;
using yb::tserver::ConsensusServiceImpl;
using strings::Substitute;

DEFINE_UNKNOWN_int32(master_tserver_svc_num_threads, 10,
             "Number of RPC worker threads to run for the master tserver service");
TAG_FLAG(master_tserver_svc_num_threads, advanced);

DEFINE_UNKNOWN_int32(master_svc_num_threads, 10,
             "Number of RPC worker threads to run for the master service");
TAG_FLAG(master_svc_num_threads, advanced);

DEFINE_UNKNOWN_int32(master_consensus_svc_num_threads, 10,
             "Number of RPC threads for the master consensus service");
TAG_FLAG(master_consensus_svc_num_threads, advanced);

DEFINE_UNKNOWN_int32(master_remote_bootstrap_svc_num_threads, 10,
             "Number of RPC threads for the master remote bootstrap service");
TAG_FLAG(master_remote_bootstrap_svc_num_threads, advanced);

DEFINE_UNKNOWN_int32(master_tserver_svc_queue_length, 1000,
             "RPC queue length for master tserver service");
TAG_FLAG(master_tserver_svc_queue_length, advanced);

DEFINE_UNKNOWN_int32(master_svc_queue_length, 1000,
             "RPC queue length for master service");
TAG_FLAG(master_svc_queue_length, advanced);

DEFINE_UNKNOWN_int32(master_consensus_svc_queue_length, 1000,
             "RPC queue length for master consensus service");
TAG_FLAG(master_consensus_svc_queue_length, advanced);

DEFINE_UNKNOWN_int32(master_remote_bootstrap_svc_queue_length, 50,
             "RPC queue length for master remote bootstrap service");
TAG_FLAG(master_remote_bootstrap_svc_queue_length, advanced);

DEFINE_test_flag(string, master_extra_list_host_port, "",
                 "Additional host port used in list masters");

DECLARE_int64(inbound_rpc_memory_limit);

DECLARE_int32(master_ts_rpc_timeout_ms);

DECLARE_bool(TEST_enable_db_catalog_version_mode);

namespace yb {
namespace master {

Master::Master(const MasterOptions& opts)
    : DbServerBase("Master", opts, "yb.master", server::CreateMemTrackerForServer()),
      state_(kStopped),
      auto_flags_manager_(new AutoFlagsManager("yb-master", fs_manager_.get())),
      ts_manager_(new TSManager()),
      catalog_manager_(new enterprise::CatalogManager(this)),
      path_handlers_(new MasterPathHandlers(this)),
      flush_manager_(new FlushManager(this, catalog_manager())),
      init_future_(init_status_.get_future()),
      opts_(opts),
      maintenance_manager_(new MaintenanceManager(MaintenanceManager::DEFAULT_OPTIONS)),
      metric_entity_cluster_(
          METRIC_ENTITY_cluster.Instantiate(metric_registry_.get(), "yb.cluster")),
      master_tablet_server_(new MasterTabletServer(this, metric_entity())) {
  SetConnectionContextFactory(rpc::CreateConnectionContextFactory<rpc::YBInboundConnectionContext>(
      GetAtomicFlag(&FLAGS_inbound_rpc_memory_limit),
      mem_tracker()));

  LOG(INFO) << "yb::master::Master created at " << this;
  LOG(INFO) << "yb::master::TSManager created at " << ts_manager_.get();
  LOG(INFO) << "yb::master::CatalogManager created at " << catalog_manager_.get();
}

Master::~Master() {
  Shutdown();
}

string Master::ToString() const {
  if (state_.load() != kRunning) {
    return "Master (stopped)";
  }
  return strings::Substitute("Master@$0", yb::ToString(first_rpc_address()));
}

Status Master::Init() {
  CHECK_EQ(kStopped, state_.load());

  RETURN_NOT_OK(ThreadPoolBuilder("init").set_max_threads(1).Build(&init_pool_));

  RETURN_NOT_OK(DbServerBase::Init());

  RETURN_NOT_OK(fs_manager_->ListTabletIds());

  RETURN_NOT_OK(path_handlers_->Register(web_server_.get()));

  auto bound_addresses = rpc_server()->GetBoundAddresses();
  if (!bound_addresses.empty()) {
    shared_object().SetHostEndpoint(bound_addresses.front(), get_hostname());
  }

  cdc_state_client_init_ = std::make_unique<client::AsyncClientInitialiser>(
      "cdc_state_client",
      default_client_timeout(),
      "" /* tserver_uuid */,
      &options(),
      metric_entity(),
      mem_tracker(),
      messenger());
  cdc_state_client_init_->builder()
      .set_master_address_flag_name("master_addresses")
      .default_admin_operation_timeout(MonoDelta::FromMilliseconds(FLAGS_master_ts_rpc_timeout_ms))
      .AddMasterAddressSource([this] {
    return catalog_manager_->GetMasterAddresses();
  });
  cdc_state_client_init_->Start();

  state_ = kInitialized;
  return Status::OK();
}

Status Master::InitAutoFlags() {
  if (!VERIFY_RESULT(auto_flags_manager_->LoadFromFile())) {
    if (fs_manager_->LookupTablet(kSysCatalogTabletId)) {
      // Pre-existing cluster
      RETURN_NOT_OK(CreateEmptyAutoFlagsConfig(auto_flags_manager_.get()));
    } else if (!opts().AreMasterAddressesProvided()) {
      // New master in Shell mode
      LOG(INFO) << "AutoFlags initialization delayed as master is in Shell mode.";
    } else {
      // New cluster
      RETURN_NOT_OK(CreateAutoFlagsConfigForNewCluster(auto_flags_manager_.get()));
    }
  }

  return Status::OK();
}

Status Master::InitAutoFlagsFromMasterLeader(const HostPort& leader_address) {
  SCHECK(
      opts().IsShellMode(), IllegalState,
      "Cannot load AutoFlags from another master when not in shell mode.");

  return auto_flags_manager_->LoadFromMaster(
      options_.HostsString(), {{leader_address}}, ApplyNonRuntimeAutoFlags::kTrue);
}

MonoDelta Master::default_client_timeout() {
  return std::chrono::milliseconds(FLAGS_master_yb_client_default_timeout_ms);
}

const std::string& Master::permanent_uuid() const {
  static std::string empty_uuid;
  return empty_uuid;
}

void Master::SetupAsyncClientInit(client::AsyncClientInitialiser* async_client_init) {
  async_client_init->builder()
      .set_master_address_flag_name("master_addresses")
      .default_admin_operation_timeout(MonoDelta::FromMilliseconds(FLAGS_master_rpc_timeout_ms))
      .AddMasterAddressSource([this] {
        return catalog_manager_->GetMasterAddresses();
  });
}

Status Master::Start() {
  RETURN_NOT_OK(StartAsync());
  RETURN_NOT_OK(WaitForCatalogManagerInit());
  google::FlushLogFiles(google::INFO); // Flush the startup messages.
  return Status::OK();
}

Status Master::RegisterServices() {
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterAdminService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterClientService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterClusterService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterDclService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterDdlService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterEncryptionService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterHeartbeatService(this)));
  RETURN_NOT_OK(RegisterService(FLAGS_master_svc_queue_length, MakeMasterReplicationService(this)));

  std::unique_ptr<ServiceIf> master_tablet_service(
      new MasterTabletServiceImpl(master_tablet_server_.get(), this));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_tserver_svc_queue_length,
                                                     std::move(master_tablet_service)));

  std::unique_ptr<ServiceIf> consensus_service(
      new ConsensusServiceImpl(metric_entity(), catalog_manager_.get()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_consensus_svc_queue_length,
                                                     std::move(consensus_service),
                                                     rpc::ServicePriority::kHigh));

  std::unique_ptr<ServiceIf> remote_bootstrap_service(new tserver::RemoteBootstrapServiceImpl(
      fs_manager_.get(), catalog_manager_.get(), metric_entity(), opts_.MakeCloudInfoPB(),
      &this->proxy_cache()));
  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(FLAGS_master_remote_bootstrap_svc_queue_length,
                                                     std::move(remote_bootstrap_service)));

  RETURN_NOT_OK(RpcAndWebServerBase::RegisterService(
      FLAGS_master_svc_queue_length,
      std::make_unique<tserver::PgClientServiceImpl>(
          *master_tablet_server_,
          client_future(), clock(), std::bind(&Master::TransactionPool, this), metric_entity(),
          &messenger()->scheduler(), nullptr /* xcluster_safe_time_map */)));

  return Status::OK();
}

void Master::DisplayGeneralInfoIcons(std::stringstream* output) {
  server::RpcAndWebServerBase::DisplayGeneralInfoIcons(output);
  // Tasks.
  DisplayIconTile(output, "fa-check", "Tasks", "/tasks");
  DisplayIconTile(output, "fa-clone", "Replica Info", "/tablet-replication");
  DisplayIconTile(output, "fa-clock-o", "TServer Clocks", "/tablet-server-clocks");
  DisplayIconTile(output, "fa-tasks", "Load Balancer", "/load-distribution");
}

Status Master::StartAsync() {
  CHECK_EQ(kInitialized, state_.load());

  RETURN_NOT_OK(maintenance_manager_->Init());
  RETURN_NOT_OK(RegisterServices());
  RETURN_NOT_OK(DbServerBase::Start());

  // Now that we've bound, construct our ServerRegistrationPB.
  RETURN_NOT_OK(InitMasterRegistration());

  // Start initializing the catalog manager.
  RETURN_NOT_OK(init_pool_->SubmitClosure(Bind(&Master::InitCatalogManagerTask,
                                               Unretained(this))));

  state_ = kRunning;
  return Status::OK();
}

void Master::InitCatalogManagerTask() {
  Status s = InitCatalogManager();
  if (!s.ok()) {
    LOG(ERROR) << ToString() << ": Unable to init master catalog manager: " << s.ToString();
  }
  init_status_.set_value(s);
}

Status Master::InitCatalogManager() {
  if (catalog_manager_->IsInitialized()) {
    return STATUS(IllegalState, "Catalog manager is already initialized");
  }
  RETURN_NOT_OK_PREPEND(catalog_manager_->Init(),
                        "Unable to initialize catalog manager");
  return Status::OK();
}

Status Master::WaitForCatalogManagerInit() {
  CHECK_EQ(state_.load(), kRunning);

  return init_future_.get();
}

Status Master::WaitUntilCatalogManagerIsLeaderAndReadyForTests(const MonoDelta& timeout) {
  RETURN_NOT_OK(catalog_manager_->WaitForWorkerPoolTests(timeout));
  Status s;
  MonoTime start = MonoTime::Now();
  int backoff_ms = 1;
  const int kMaxBackoffMs = 256;
  do {
    SCOPED_LEADER_SHARED_LOCK(l, catalog_manager_.get());
    if (l.IsInitializedAndIsLeader()) {
      return Status::OK();
    }
    l.Unlock();

    SleepFor(MonoDelta::FromMilliseconds(backoff_ms));
    backoff_ms = min(backoff_ms << 1, kMaxBackoffMs);
  } while (MonoTime::Now().GetDeltaSince(start).LessThan(timeout));
  return STATUS(TimedOut, "Maximum time exceeded waiting for master leadership",
                          s.ToString());
}

void Master::Shutdown() {
  if (state_.load() == kRunning) {
    string name = ToString();
    LOG(INFO) << name << " shutting down...";
    maintenance_manager_->Shutdown();
    // We shutdown RpcAndWebServerBase here in order to shutdown messenger and reactor threads
    // before shutting down catalog manager. This is needed to prevent async calls callbacks
    // (running on reactor threads) from trying to use catalog manager thread pool which would be
    // already shutdown.
    auto started = catalog_manager_->StartShutdown();
    LOG_IF(DFATAL, !started) << name << " catalog manager shutdown already in progress";
    async_client_init_->Shutdown();
    cdc_state_client_init_->Shutdown();
    RpcAndWebServerBase::Shutdown();
    if (init_pool_) {
      init_pool_->Shutdown();
    }
    catalog_manager_->CompleteShutdown();
    LOG(INFO) << name << " shutdown complete.";
  } else {
    LOG(INFO) << ToString() << " did not start, shutting down all that started...";
    RpcAndWebServerBase::Shutdown();
  }
  state_ = kStopped;
}

Status Master::GetMasterRegistration(ServerRegistrationPB* reg) const {
  auto* registration = registration_.get();
  if (!registration) {
    return STATUS(ServiceUnavailable, "Master startup not complete");
  }
  reg->CopyFrom(*registration);
  return Status::OK();
}

Status Master::InitMasterRegistration() {
  CHECK(!registration_.get());

  auto reg = std::make_unique<ServerRegistrationPB>();
  RETURN_NOT_OK(GetRegistration(reg.get()));
  registration_.reset(reg.release());

  return Status::OK();
}

Status Master::ResetMemoryState(const consensus::RaftConfigPB& config) {
  LOG(INFO) << "Memory state set to config: " << config.ShortDebugString();

  auto master_addr = std::make_shared<server::MasterAddresses>();
  for (const RaftPeerPB& peer : config.peers()) {
    master_addr->push_back({HostPortFromPB(DesiredHostPort(peer, opts_.MakeCloudInfoPB()))});
  }

  SetMasterAddresses(std::move(master_addr));

  return Status::OK();
}

void Master::DumpMasterOptionsInfo(std::ostream* out) {
  *out << "Master options : ";
  auto master_addresses_shared_ptr = opts_.GetMasterAddresses();  // ENG-285
  bool first = true;
  for (const auto& list : *master_addresses_shared_ptr) {
    if (first) {
      first = false;
    } else {
      *out << ", ";
    }
    bool need_comma = false;
    for (const HostPort& hp : list) {
      if (need_comma) {
        *out << "/ ";
      }
      need_comma = true;
      *out << hp.ToString();
    }
  }
  *out << "\n";
}

Status Master::ListRaftConfigMasters(std::vector<RaftPeerPB>* masters) const {
  consensus::ConsensusStatePB cpb;
  RETURN_NOT_OK(catalog_manager_->GetCurrentConfig(&cpb));
  if (cpb.has_config()) {
    for (RaftPeerPB peer : cpb.config().peers()) {
      masters->push_back(peer);
    }
    return Status::OK();
  } else {
    return STATUS(NotFound, "No raft config found.");
  }
}

Status Master::ListMasters(std::vector<ServerEntryPB>* masters) const {
  if (IsShellMode()) {
    ServerEntryPB local_entry;
    local_entry.mutable_instance_id()->CopyFrom(catalog_manager_->NodeInstance());
    RETURN_NOT_OK(GetMasterRegistration(local_entry.mutable_registration()));
    local_entry.set_role(IsShellMode() ? PeerRole::NON_PARTICIPANT : PeerRole::LEADER);
    masters->push_back(local_entry);
    return Status::OK();
  }

  consensus::ConsensusStatePB cpb;
  RETURN_NOT_OK(catalog_manager_->GetCurrentConfig(&cpb));
  if (!cpb.has_config()) {
      return STATUS(NotFound, "No raft config found.");
  }

  for (const RaftPeerPB& peer : cpb.config().peers()) {
    // Get all network addresses associated with this peer master
    std::vector<HostPort> addrs;
    for (const auto& hp : peer.last_known_private_addr()) {
      addrs.push_back(HostPortFromPB(hp));
    }
    for (const auto& hp : peer.last_known_broadcast_addr()) {
      addrs.push_back(HostPortFromPB(hp));
    }
    if (!FLAGS_TEST_master_extra_list_host_port.empty()) {
      addrs.push_back(VERIFY_RESULT(HostPort::FromString(
          FLAGS_TEST_master_extra_list_host_port, 0)));
    }

    // Make GetMasterRegistration calls for peer master info.
    ServerEntryPB peer_entry;
    Status s = GetMasterEntryForHosts(
        proxy_cache_.get(), addrs, MonoDelta::FromMilliseconds(FLAGS_master_rpc_timeout_ms),
        &peer_entry);
    if (!s.ok()) {
      // In case of errors talking to the peer master,
      // fill in fields from our catalog best as we can.
      s = s.CloneAndPrepend(
        Format("Unable to get registration information for peer ($0) id ($1)",
              addrs, peer.permanent_uuid()));
      YB_LOG_EVERY_N_SECS(WARNING, 5) << "ListMasters: " << s;
      StatusToPB(s, peer_entry.mutable_error());
      peer_entry.mutable_instance_id()->set_permanent_uuid(peer.permanent_uuid());
      peer_entry.mutable_instance_id()->set_instance_seqno(0);
      auto reg = peer_entry.mutable_registration();
      reg->mutable_private_rpc_addresses()->CopyFrom(peer.last_known_private_addr());
      reg->mutable_broadcast_addresses()->CopyFrom(peer.last_known_broadcast_addr());
    }
    masters->push_back(peer_entry);
  }

  return Status::OK();
}

Status Master::InformRemovedMaster(const HostPortPB& hp_pb) {
  HostPort hp(hp_pb.host(), hp_pb.port());
  MasterClusterProxy proxy(proxy_cache_.get(), hp);
  RemovedMasterUpdateRequestPB req;
  RemovedMasterUpdateResponsePB resp;
  rpc::RpcController controller;
  controller.set_timeout(MonoDelta::FromMilliseconds(FLAGS_master_rpc_timeout_ms));
  RETURN_NOT_OK(proxy.RemovedMasterUpdate(req, &resp, &controller));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  return Status::OK();
}

scoped_refptr<Histogram> Master::GetMetric(
    const std::string& metric_identifier, MasterMetricType type, const std::string& description) {
  std::string temp_metric_identifier = Format("$0_$1", metric_identifier,
      (type == TaskMetric ? "Task" : "Attempt"));
  EscapeMetricNameForPrometheus(&temp_metric_identifier);
  {
    std::lock_guard<std::mutex> lock(master_metrics_mutex_);
    std::map<std::string, scoped_refptr<Histogram>>* master_metrics_ptr = master_metrics();
    auto it = master_metrics_ptr->find(temp_metric_identifier);
    if (it == master_metrics_ptr->end()) {
      std::unique_ptr<HistogramPrototype> histogram = std::make_unique<OwningHistogramPrototype>(
          "server", temp_metric_identifier, description, yb::MetricUnit::kMicroseconds,
          description, yb::MetricLevel::kInfo, 0, 10000000, 2);
      scoped_refptr<Histogram> temp =
          metric_entity()->FindOrCreateHistogram(std::move(histogram));
      (*master_metrics_ptr)[temp_metric_identifier] = temp;
      return temp;
    }
    return it->second;
  }
}

Status Master::GoIntoShellMode() {
  maintenance_manager_->Shutdown();
  RETURN_NOT_OK(catalog_manager_impl()->GoIntoShellMode());
  return Status::OK();
}

scoped_refptr<MetricEntity> Master::metric_entity_cluster() {
  return metric_entity_cluster_;
}

client::LocalTabletFilter Master::CreateLocalTabletFilter() {
  return client::LocalTabletFilter();
}

CatalogManagerIf* Master::catalog_manager() const {
  return catalog_manager_.get();
}

SysCatalogTable& Master::sys_catalog() const {
  return *catalog_manager_->sys_catalog();
}

PermissionsManager& Master::permissions_manager() {
  return *catalog_manager_->permissions_manager();
}

EncryptionManager& Master::encryption_manager() {
  return catalog_manager_->encryption_manager();
}

uint32_t Master::GetAutoFlagConfigVersion() const {
  return auto_flags_manager_->GetConfigVersion();
}

AutoFlagsConfigPB Master::GetAutoFlagsConfig() const { return auto_flags_manager_->GetConfig(); }

Status Master::get_ysql_db_oid_to_cat_version_info_map(
    bool size_only, tserver::GetTserverCatalogVersionInfoResponsePB *resp) const {
  DCHECK(FLAGS_create_initial_sys_catalog_snapshot);
  DCHECK(FLAGS_TEST_enable_db_catalog_version_mode);
  // This function can only be called during initdb time.
  DbOidToCatalogVersionMap versions;
  RETURN_NOT_OK(catalog_manager_->GetYsqlAllDBCatalogVersions(&versions));
  if (size_only) {
    resp->set_num_entries(narrow_cast<uint32_t>(versions.size()));
  } else {
    // We assume that during initdb:
    // (1) we only create databases, not drop databases;
    // (2) databases OIDs are allocated increasingly.
    // Based upon these assumptions, we can have a simple shm_index assignment algorithm by
    // doing shm_index++. As a result, a subsequent call to this function will return either
    // identical or a superset of the result of any previous calls. For example, if the first
    // call sees two DB oids [1, 16384], this function will return (1, 0), (16384, 1). If the
    // next call sees 3 DB oids [1, 16384, 16385], we return (1, 0), (16384, 1), (16385, 2)
    // which is a superset of the result of the first call. This is to ensure that the
    // shm_index assigned to a DB oid remains the same during the lifetime of the DB.
    int shm_index = 0;
    uint32_t db_oid = kInvalidOid;
    for (const auto& it : versions) {
      auto* entry = resp->add_entries();
      CHECK_LT(db_oid, it.first);
      db_oid = it.first;
      entry->set_db_oid(db_oid);
      entry->set_current_version(it.second.first);
      entry->set_shm_index(shm_index++);
    }
  }
  LOG(INFO) << "resp: " << resp->ShortDebugString();
  return Status::OK();
}

} // namespace master
} // namespace yb
