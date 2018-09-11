/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "cybertron/topology/topology.h"

#include "cybertron/common/global_data.h"
#include "cybertron/common/log.h"
#include "cybertron/time/time.h"
#include "cybertron/transport/rtps/participant.h"

namespace apollo {
namespace cybertron {
namespace topology {

Topology::Topology()
    : init_(false),
      node_manager_(nullptr),
      channel_manager_(nullptr),
      service_manager_(nullptr),
      participant_(nullptr),
      participant_listener_(nullptr) {
  Init();
}

Topology::~Topology() { Shutdown(); }

void Topology::Shutdown() {
  ADEBUG << "topology shutdown.";
  // avoid shutdown twice
  if (!init_.exchange(false)) {
    return;
  }

  node_manager_->Shutdown();
  channel_manager_->Shutdown();
  service_manager_->Shutdown();
  participant_->Shutdown();

  delete participant_listener_;
  participant_listener_ = nullptr;

  change_signal_.DisconnectAllSlots();
}

Topology::ChangeConnection Topology::AddChangeListener(const ChangeFunc& func) {
  return change_signal_.Connect(func);
}

void Topology::RemoveChangeListener(const ChangeConnection& conn) {
  auto local_conn = conn;
  local_conn.Disconnect();
}

bool Topology::Init() {
  if (init_.exchange(true)) {
    return true;
  }

  node_manager_ = std::make_shared<NodeManager>();
  channel_manager_ = std::make_shared<ChannelManager>();
  service_manager_ = std::make_shared<ServiceManager>();

  CreateParticipant();

  bool result =
      InitNodeManager() && InitChannelManager() && InitServiceManager();
  if (!result) {
    AERROR << "init manager failed.";
    participant_ = nullptr;
    delete participant_listener_;
    participant_listener_ = nullptr;
    node_manager_ = nullptr;
    channel_manager_ = nullptr;
    service_manager_ = nullptr;
    init_.exchange(false);
    return false;
  }

  return true;
}

bool Topology::InitNodeManager() {
  return node_manager_->Init(participant_->fastrtps_participant());
}

bool Topology::InitChannelManager() {
  return channel_manager_->Init(participant_->fastrtps_participant());
}

bool Topology::InitServiceManager() {
  return service_manager_->Init(participant_->fastrtps_participant());
}

bool Topology::CreateParticipant() {
  std::string participant_name =
      common::GlobalData::Instance()->HostName() + '+' +
      std::to_string(common::GlobalData::Instance()->ProcessId());
  participant_listener_ = new ParticipantListener(
      std::bind(&Topology::OnParticipantChange, this, std::placeholders::_1));
  participant_ = std::make_shared<transport::Participant>(
      participant_name, 11511, participant_listener_);
  return true;
}

void Topology::OnParticipantChange(const PartInfo& info) {
  ChangeMsg msg;
  if (!Convert(info, &msg)) {
    return;
  }

  if (!init_.load()) {
    return;
  }

  if (msg.operate_type() == OperateType::OPT_LEAVE) {
    auto& host_name = msg.role_attr().host_name();
    int process_id = msg.role_attr().process_id();
    node_manager_->OnTopoModuleLeave(host_name, process_id);
    channel_manager_->OnTopoModuleLeave(host_name, process_id);
    service_manager_->OnTopoModuleLeave(host_name, process_id);
  }
  change_signal_(msg);
}

bool Topology::Convert(const PartInfo& info, ChangeMsg* msg) {
  auto guid = info.rtps.m_guid;
  auto status = info.rtps.m_status;
  std::string participant_name("");
  OperateType opt_type = OperateType::OPT_JOIN;

  switch (status) {
    case eprosima::fastrtps::rtps::DISCOVERY_STATUS::DISCOVERED_RTPSPARTICIPANT:
      participant_name = info.rtps.m_RTPSParticipantName;
      participant_names_[guid] = participant_name;
      opt_type = OperateType::OPT_JOIN;
      break;

    case eprosima::fastrtps::rtps::DISCOVERY_STATUS::REMOVED_RTPSPARTICIPANT:
    case eprosima::fastrtps::rtps::DISCOVERY_STATUS::DROPPED_RTPSPARTICIPANT:
      if (participant_names_.find(guid) != participant_names_.end()) {
        participant_name = participant_names_[guid];
        participant_names_.erase(guid);
      }
      opt_type = OperateType::OPT_LEAVE;
      break;

    default:
      break;
  }

  std::string host_name("");
  int process_id = 0;
  if (!ParseParticipantName(participant_name, &host_name, &process_id)) {
    return false;
  }

  msg->set_timestamp(cybertron::Time::Now().ToNanosecond());
  msg->set_change_type(ChangeType::CHANGE_PARTICIPANT);
  msg->set_operate_type(opt_type);
  msg->set_role_type(RoleType::ROLE_PARTICIPANT);
  auto role_attr = msg->mutable_role_attr();
  role_attr->set_host_name(host_name);
  role_attr->set_process_id(process_id);
  return true;
}

bool Topology::ParseParticipantName(const std::string& participant_name,
                                    std::string* host_name, int* process_id) {
  // participant_name format: host_name+process_id
  auto pos = participant_name.find('+');
  if (pos == std::string::npos) {
    ADEBUG << "participant_name [" << participant_name << "] format mismatch.";
    return false;
  }
  *host_name = participant_name.substr(0, pos);
  std::string pid_str = participant_name.substr(pos + 1);
  try {
    *process_id = std::stoi(pid_str);
  } catch (const std::exception& e) {
    AERROR << "invalid process_id:" << e.what();
    return false;
  }
  return true;
}

}  // namespace topology
}  // namespace cybertron
}  // namespace apollo