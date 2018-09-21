/**
 * Copyright (C) 2016-2017 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// Copyright 2014 Xilinx, Inc. All rights reserved.

#include "rt_singleton.h"
#include "xdp/appdebug/appdebug.h"
#include "xocl/core/platform.h"
#include "xocl/core/execution_context.h"
#include "xrt/util/config_reader.h"

#include "xdp/profile/profile.h"
#include "xdp/profile/rt_profile.h"
#include "xdp/profile/rt_profile_writers.h"
#include "xdp/profile/rt_profile_xocl.h"

#include <cstdlib>
#include <cstdio>
#include <string>
#include <chrono>
#include <iostream>
#include <thread>

namespace XCL {

  static bool gActive = false;
  static bool gDead = false;

  bool
  active() {
    return gActive;
  }

  RTSingleton*
  RTSingleton::Instance() {
    if (gDead) {
      std::cout << "RTSingleton is dead\n";
      return nullptr;
    }

    static RTSingleton singleton;
    return &singleton;
  }

  RTSingleton::RTSingleton()
  : Status( CL_SUCCESS ),
    ProfileMgr( nullptr ),
    DebugMgr( nullptr ),
    Platform( nullptr ),
	ProfileFlags( 0 )
  {
    ProfileMgr = new RTProfile(ProfileFlags);
    startProfiling();

    DebugMgr = new RTDebug();

    // share ownership of the global platform
    Platform = xocl::get_shared_platform();

    if (xrt::config::get_app_debug()) {
      appdebug::register_xocl_appdebug_callbacks();
    }

    if (xrt::config::get_ila_debug() != "off") {
      XCL::register_xocl_debug_callbacks();
    }

    if (applicationProfilingOn()) {
      XCL::register_xocl_profile_callbacks();
    }

    std::cout << "before constructing power profile" << std::endl;
    powerProfile = new PowerProfile(Platform);
    std::cout << "before launching power profile" << std::endl;
    powerProfile->launch();
    std::cout << "after launching power profile" << std::endl;
#ifdef PMD_OCL
    return;
#endif

    gActive = true;

  };

  RTSingleton::~RTSingleton() {
    cleanupLabtoolPool();

    gActive = false;

    powerProfile->terminate();

    endProfiling();

    gDead = true;

    // Destruct in reverse order of construction
    delete ProfileMgr;
    delete DebugMgr;
    delete powerProfile;
  }

  // Kick off profiling and open writers
  void RTSingleton::startProfiling() {
    if (xrt::config::get_profile() == false)
      return;

    // Find default flow mode
    // NOTE: it will be modified in clCreateProgramWithBinary (if run)
    FlowMode = (std::getenv("XCL_EMULATION_MODE")) ? HW_EM : DEVICE;

    // Turn on application profiling
    turnOnProfile(RTProfile::PROFILE_APPLICATION);

    // Turn on device profiling (as requested)
    std::string data_transfer_trace = xrt::config::get_data_transfer_trace();
    // TEMPORARY - TURN ON DATA TRANSFER TRACE WHEN TIMELINE TRACE IS ON (HW EM ONLY)
    //if ((FlowMode == HW_EM) && xrt::config::get_timeline_trace())
    //  data_transfer_trace = "fine";

    std::string stall_trace = xrt::config::get_stall_trace();
    ProfileMgr->setTransferTrace(data_transfer_trace);
    ProfileMgr->setStallTrace(stall_trace);

    turnOnProfile(RTProfile::PROFILE_DEVICE_COUNTERS);
    // HW trace is controlled at HAL layer
    if ((FlowMode == DEVICE) || xrt::config::get_device_profile() ||
        (data_transfer_trace.find("off") == std::string::npos)) {
      turnOnProfile(RTProfile::PROFILE_DEVICE_TRACE);
    }

    // Issue warning for device_profile setting (not supported after 2018.2)
    if (xrt::config::get_device_profile()) {
      xrt::message::send(xrt::message::severity_level::WARNING,
          "The setting device_profile will be deprecated after 2018.2. Please use data_transfer_trace.");
    }

    std::string profileFile("");
    std::string profileFile2("");
    std::string timelineFile("");
    std::string timelineFile2("");

    if (ProfileMgr->isApplicationProfileOn()) {
      //always on by default.
      ProfileMgr->turnOnFile(RTProfile::FILE_SUMMARY);
      profileFile = "sdaccel_profile_summary";
      profileFile2 = "sdx_profile_summary";
    }

    if (xrt::config::get_timeline_trace()) {
      ProfileMgr->turnOnFile(RTProfile::FILE_TIMELINE_TRACE);
      timelineFile = "sdaccel_timeline_trace";
      timelineFile2 = "sdx_timeline_trace";
    }

    // HTML and CSV writers
    //HTMLWriter* htmlWriter = new HTMLWriter(profileFile, timelineFile, "Xilinx");
    CSVWriter* csvWriter = new CSVWriter(profileFile, timelineFile, "Xilinx");

    //Writers.push_back(htmlWriter);
    Writers.push_back(csvWriter);

    //ProfileMgr->attach(htmlWriter);
    ProfileMgr->attach(csvWriter);

    if (std::getenv("SDX_NEW_PROFILE")) {
      UnifiedCSVWriter* csvWriter2 = new UnifiedCSVWriter(profileFile2, timelineFile2, "Xilinx");
      Writers.push_back(csvWriter2);
      ProfileMgr->attach(csvWriter2);
    }

    // Add functions to callback for profiling kernel/CU scheduling
    xocl::add_command_start_callback(xdp::profile::get_cu_start);
    xocl::add_command_done_callback(xdp::profile::get_cu_done);
  }

  // Wrap up profiling by writing files
  void RTSingleton::endProfiling() {
    if (applicationProfilingOn()) {
      // Write out reports
      ProfileMgr->writeProfileSummary();

      // Close writers
      for (auto& w: Writers) {
        ProfileMgr->detach(w);
        delete w;
      }
    }
  }

  // Log final trace for a given profile type
  // NOTE: this is a bit tricky since trace logging is accessed by multiple
  // threads. We have to wait since this is the only place where we flush.
  void RTSingleton::logFinalTrace(xclPerfMonType type) {
    const unsigned int wait_msec = 1;
    const unsigned int max_iter = 100;
    unsigned int iter = 0;
    cl_int ret = -1;

    while (ret == -1 && iter < max_iter) {
      ret = xdp::profile::platform::log_device_trace(Platform.get(),type, true);
      if (ret == -1)
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_msec));
      iter++;
    }
    XOCL_DEBUGF("Trace logged for type %d after %d iterations\n", type, iter);
  }

  unsigned RTSingleton::getProfileNumberSlots(xclPerfMonType type, std::string& deviceName) {
    unsigned numSlots = xdp::profile::platform::get_profile_num_slots(Platform.get(),
        deviceName, type);
    //XOCL_DEBUG(std::cout,"Profiling: type = "type," slots = ",numSlots,"\n");
    return numSlots;
  }

  DeviceInfo RTSingleton::getDeviceInfo(std::string& deviceName) {
    DeviceInfo deviceInfo = xdp::profile::platform::get_device_info(Platform.get(), deviceName);
    return deviceInfo;
  }

  void RTSingleton::getProfileSlotName(xclPerfMonType type, std::string& deviceName,
                                       unsigned slotnum, std::string& slotName) {
    xdp::profile::platform::get_profile_slot_name(Platform.get(), deviceName,
        type, slotnum, slotName);
    //XOCL_DEBUG(std::cout,"Profiling: type = "type," slot ",slotnum," name = ",slotName.c_str(),"\n");
  }

  void RTSingleton::getProfileKernelName(const std::string& deviceName, const std::string& cuName, std::string& kernelName) {
    xdp::profile::platform::get_profile_kernel_name(Platform.get(), deviceName, cuName, kernelName);
  }

  // Set OCL profile mode based on profile type string
  // NOTE: this corresponds to strings defined in regiongen_new/ipihandler.cxx
  void RTSingleton::setOclProfileMode(unsigned slotnum, std::string type) {
    if (slotnum >= XAPM_MAX_NUMBER_SLOTS)
	  return;

    XOCL_DEBUG(std::cout,"OCL profiling: mode for slot ",slotnum," = ",type.c_str(),"\n");

    if (type.find("stream") != std::string::npos || type.find("STREAM") != std::string::npos)
      OclProfileMode[slotnum] = STREAM;
    else if (type.find("pipe") != std::string::npos || type.find("PIPE") != std::string::npos)
      OclProfileMode[slotnum] = PIPE;
    else if (type.find("memory") != std::string::npos || type.find("MEMORY") != std::string::npos)
      OclProfileMode[slotnum] = MEMORY;
    else if (type.find("activity") != std::string::npos || type.find("ACTIVITY") != std::string::npos)
      OclProfileMode[slotnum] = ACTIVITY;
    else
      OclProfileMode[slotnum] = NONE;
  }

  size_t RTSingleton::getDeviceTimestamp(std::string& deviceName) {
    return xdp::profile::platform::get_device_timestamp(Platform.get(),deviceName);
  }

  double RTSingleton::getReadMaxBandwidthMBps() {
    return xdp::profile::platform::get_device_max_read(Platform.get());
  }

  double RTSingleton::getWriteMaxBandwidthMBps() {
    return xdp::profile::platform::get_device_max_write(Platform.get());
  }

  void RTSingleton::getFlowModeName(std::string& str) {
    if (FlowMode == CPU)
      str = "CPU Emulation";
    else if (FlowMode == COSIM_EM)
      str = "Co-Sim Emulation";
    else if (FlowMode == HW_EM)
      str = "Hardware Emulation";
    else
      str = "System Run";
  }

  void RTSingleton::configDeviceInfo(std::string& deviceName) {
    auto device_info = getDeviceInfo(deviceName);
    DeviceConfig config;
    debug_ip_layout *map;
    config.mgmt_instance = device_info.mDeviceMgmtInstance;
    config.user_instance = device_info.mDeviceMgmtInstance - 1;
    config.user_name = std::string(device_info.mDeviceUserName);
    config.mgmt_name = std::string(device_info.mDeviceMgmtName);
    config.device_name = deviceName;
    config.debugIP[DEBUG_IP_TYPE::UNDEFINED] = {};
    config.debugIP[DEBUG_IP_TYPE::LAPC] = {};
    config.debugIP[DEBUG_IP_TYPE::ILA] = {};
    config.debugIP[DEBUG_IP_TYPE::AXI_MM_MONITOR] = {};
    config.debugIP[DEBUG_IP_TYPE::AXI_TRACE_FUNNEL] = {};
    config.debugIP[DEBUG_IP_TYPE::AXI_MONITOR_FIFO_LITE] = {};
    config.debugIP[DEBUG_IP_TYPE::AXI_MONITOR_FIFO_FULL] = {};
    config.debugIP[DEBUG_IP_TYPE::ACCEL_MONITOR] = {};
    config.debugIP[DEBUG_IP_TYPE::AXI_STREAM_MONITOR] = {};
    std::string debug_ip_layout_path = "/sys/bus/pci/devices/" + config.user_name + "/debug_ip_layout";
    std::ifstream debug_ip_layout_file(debug_ip_layout_path.c_str(), std::ifstream::binary);
    uint32_t count = 0;
    char buffer[65536];
    if( debug_ip_layout_file.good() ) {
        debug_ip_layout_file.read(buffer, 65536);
        if (debug_ip_layout_file.gcount() > 0) {
            map = (debug_ip_layout*)(buffer);
            for( unsigned int i = 0; i < map->m_count; i++ ) {
              auto ip_type = (DEBUG_IP_TYPE)map->m_debug_ip_data[i].m_type;
              if (config.debugIP.find(ip_type) == config.debugIP.end()) {
                config.debugIP[ip_type] = {};
              }
              config.debugIP[ip_type].push_back(map->m_debug_ip_data[i]);
            }
        }
        debug_ip_layout_file.close();
    } else {
      std::cout << "Cannot open debug_ip_layout, considered as no debug IP exist" << std::endl;
    }
    configDict[deviceName] = config;
  }

  DeviceConfig RTSingleton::getDeviceConfig(std::string& deviceName) {
    if (configDict.find(deviceName) == configDict.end()) {
      std::cout << "Device Config for " << deviceName << " not initialized" << std::endl;
      return {};
    }
    return configDict[deviceName];
  }

  void RTSingleton::registerLabtool(LabtoolController* instance) {
    labtoolPool[instance->getID()] = instance;
  }

  LabtoolController* RTSingleton::getLabtool(std::string& ID) {
    auto target = labtoolPool.find(ID);
    if (target == labtoolPool.end()) {
      return NULL;
    }
    return target->second;
  }

  void RTSingleton::removeLabtool(std::string& ID) {
    auto target = labtoolPool.find(ID);
    if (target == labtoolPool.end()) {
      return;
    }
    delete target->second;
    labtoolPool.erase(target);
  }

  int RTSingleton::getLabtoolCount() {
    return labtoolPool.size();
  }

  void RTSingleton::cleanupLabtoolPool() {
    for (auto it = labtoolPool.begin(); it != labtoolPool.end(); ++it) {
      it->second->cleanup();
      delete it->second;
    }
    labtoolPool.clear();
  }

};
