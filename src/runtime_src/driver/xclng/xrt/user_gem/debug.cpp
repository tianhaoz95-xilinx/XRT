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

/*
 * Copyright (C) 2015 Xilinx, Inc
 * Author: Paul Schumacher
 * Performance Monitoring using PCIe for XDMA HAL Driver
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

#include "shim.h"
#include "../user_common/perfmon_parameters.h"
#include "driver/include/xclbin.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <iostream>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <thread>
#include <vector>
#include <time.h>
#include <string>

#ifndef _WINDOWS
// TODO: Windows build support
//    unistd.h is linux only header file
//    it is included for read, write, close, lseek64
#include <unistd.h>
#endif

#ifdef _WINDOWS
#define __func__ __FUNCTION__
#endif

namespace xocl {
  // ****************
  // Helper functions
  // ****************

  void XOCLShim::readDebugIpLayout()
  {
    if (mIsDebugIpLayoutRead)
      return;

    //
    // Profiling - addresses and names
    // Parsed from debug_ip_layout.rtd contained in xclbin
    if (mLogStream.is_open()) {
      mLogStream << "debug_ip_layout: reading profile addresses and names..." << std::endl;
    }

    mMemoryProfilingNumberSlots = getIPCountAddrNames(AXI_MM_MONITOR, mPerfMonBaseAddress,
      mPerfMonSlotName, mPerfmonProperties, XSPM_MAX_NUMBER_SLOTS);
    
    mAccelProfilingNumberSlots = getIPCountAddrNames(ACCEL_MONITOR, mAccelMonBaseAddress,
      mAccelMonSlotName, mAccelmonProperties, XSAM_MAX_NUMBER_SLOTS);
    
    mIsDeviceProfiling = (mMemoryProfilingNumberSlots > 0 || mAccelProfilingNumberSlots > 0);

    std::string fifoName;
    uint64_t fifoCtrlBaseAddr = mOffsets[XCL_ADDR_SPACE_DEVICE_PERFMON];
    getIPCountAddrNames(AXI_MONITOR_FIFO_LITE, &fifoCtrlBaseAddr, &fifoName, nullptr, 1);
    mPerfMonFifoCtrlBaseAddress = fifoCtrlBaseAddr;

    uint64_t fifoReadBaseAddr = XPAR_AXI_PERF_MON_0_TRACE_OFFSET_AXI_FULL2;
    getIPCountAddrNames(AXI_MONITOR_FIFO_FULL, &fifoReadBaseAddr, &fifoName, nullptr, 1);
    mPerfMonFifoReadBaseAddress = fifoReadBaseAddr;

    uint64_t traceFunnelAddr = 0x0;
    getIPCountAddrNames(AXI_TRACE_FUNNEL, &traceFunnelAddr, nullptr, nullptr, 1);
    mTraceFunnelAddress = traceFunnelAddr;

    // Count accel monitors with stall monitoring turned on
    mStallProfilingNumberSlots = 0;
    for (unsigned int i = 0; i < mAccelProfilingNumberSlots; ++i) {
      if ((mAccelmonProperties[i] >> 2) & 0x1)
        mStallProfilingNumberSlots++;
    }

    if (mLogStream.is_open()) {
      for (unsigned int i = 0; i < mMemoryProfilingNumberSlots; ++i) {
        mLogStream << "debug_ip_layout: AXI_MM_MONITOR slot " << i << ": "
                   << "base address = 0x" << std::hex << mPerfMonBaseAddress[i]
                   << ", name = " << mPerfMonSlotName[i] << std::endl;
      }
      for (unsigned int i = 0; i < mAccelProfilingNumberSlots; ++i) {
        mLogStream << "debug_ip_layout: ACCEL_MONITOR slot " << i << ": "
                   << "base address = 0x" << std::hex << mAccelMonBaseAddress[i]
                   << ", name = " << mAccelMonSlotName[i] << std::endl;
      }
      mLogStream << "debug_ip_layout: AXI_MONITOR_FIFO_LITE: "
                 << "base address = 0x" << std::hex << fifoCtrlBaseAddr << std::endl;
      mLogStream << "debug_ip_layout: AXI_MONITOR_FIFO_FULL: "
                 << "base address = 0x" << std::hex << fifoReadBaseAddr << std::endl;
      mLogStream << "debug_ip_layout: AXI_TRACE_FUNNEL: "
                 << "base address = 0x" << std::hex << traceFunnelAddr << std::endl;
    }

    // Only need to read it once
    mIsDebugIpLayoutRead = true;
  }

  // Gets the information about the specified IP from the sysfs debug_ip_table.
  // The IP types are defined in xclbin.h
  uint32_t XOCLShim::getIPCountAddrNames(int type, uint64_t *baseAddress, std::string * portNames,
                                         uint8_t *properties, size_t size) {
    debug_ip_layout *map;
    std::string path = "/sys/bus/pci/devices/" + mDevUserName + "/debug_ip_layout";
    std::ifstream ifs(path.c_str(), std::ifstream::binary);
    uint32_t count = 0;
    char buffer[65536];
    if( ifs ) {
      //debug_ip_layout max size is 65536
      ifs.read(buffer, 65536);
      if (ifs.gcount() > 0) {
        map = (debug_ip_layout*)(buffer);
        for( unsigned int i = 0; i < map->m_count; i++ ) {
          if (count >= size) break;
          if (map->m_debug_ip_data[i].m_type == type) {
            if(baseAddress)baseAddress[count] = map->m_debug_ip_data[i].m_base_address;
            if(portNames) portNames[count] = (char*)map->m_debug_ip_data[i].m_name;
            if(properties) properties[count] = map->m_debug_ip_data[i].m_properties;
            ++count;
          }
        }
      }
      ifs.close();
    }
    return count;
  }

  // Read APM performance counters
  size_t XOCLShim::xclDebugReadCheckers(xclDebugCheckersResults* aCheckerResults) {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id()
      << ", " << aCheckerResults
      << ", Read protocl checker status..." << std::endl;
    }

    size_t size = 0;

    uint64_t statusRegisters[] = {
        LAPC_OVERALL_STATUS_OFFSET,

        LAPC_CUMULATIVE_STATUS_0_OFFSET, LAPC_CUMULATIVE_STATUS_1_OFFSET,
        LAPC_CUMULATIVE_STATUS_2_OFFSET, LAPC_CUMULATIVE_STATUS_3_OFFSET,

        LAPC_SNAPSHOT_STATUS_0_OFFSET, LAPC_SNAPSHOT_STATUS_1_OFFSET,
        LAPC_SNAPSHOT_STATUS_2_OFFSET, LAPC_SNAPSHOT_STATUS_3_OFFSET
    };

    uint64_t baseAddress[XLAPC_MAX_NUMBER_SLOTS];
    uint32_t numSlots = getIPCountAddrNames(LAPC, baseAddress, nullptr, nullptr, XLAPC_MAX_NUMBER_SLOTS);
    uint32_t temp[XLAPC_STATUS_PER_SLOT];
    aCheckerResults->NumSlots = numSlots;
    snprintf(aCheckerResults->DevUserName, 256, "%s", mDevUserName.c_str());
    for (uint32_t s = 0; s < numSlots; ++s) {
      for (int c=0; c < XLAPC_STATUS_PER_SLOT; c++)
        size += xclRead(XCL_ADDR_SPACE_DEVICE_CHECKER, baseAddress[s]+statusRegisters[c], &temp[c], 4);

      aCheckerResults->OverallStatus[s]      = temp[XLAPC_OVERALL_STATUS];
      std::copy(temp+XLAPC_CUMULATIVE_STATUS_0, temp+XLAPC_SNAPSHOT_STATUS_0, aCheckerResults->CumulativeStatus[s]);
      std::copy(temp+XLAPC_SNAPSHOT_STATUS_0, temp+XLAPC_STATUS_PER_SLOT, aCheckerResults->SnapshotStatus[s]);
    }

    return size;
  }

  size_t XOCLShim::xclDebugReadSAMCounters(xclDebugSAMCounterResults* samResult) {
	  if (mLogStream.is_open()) {
		mLogStream << __func__ << ", " << std::this_thread::get_id()
		<< ", " << XCL_PERF_MON_ACCEL << ", " << samResult
		<< ", Read device counters..." << std::endl;
	  }
	  size_t size = 0;
	  uint64_t sam_offsets[] = {
	  	 XSAM_VERSION_OFFSET,
		 XSAM_ACCEL_EXECUTION_COUNT_OFFSET,
		 XSAM_ACCEL_EXECUTION_CYCLES_OFFSET,
		 XSAM_ACCEL_STALL_INT_OFFSET,
		 XSAM_ACCEL_STALL_STR_OFFSET,
		 XSAM_ACCEL_STALL_EXT_OFFSET,
		 XSAM_ACCEL_MIN_EXECUTION_CYCLES_OFFSET,
		 XSAM_ACCEL_MAX_EXECUTION_CYCLES_OFFSET,
		 XSAM_ACCEL_START_COUNT_OFFSET
	  };
	  uint64_t baseAddress[XSAM_MAX_NUMBER_SLOTS];
	  uint32_t numSlots = getIPCountAddrNames(ACCEL_MONITOR, baseAddress, nullptr, nullptr, XSAM_MAX_NUMBER_SLOTS);
	  uint32_t temp[XSAM_DEBUG_SAMPLE_COUNTERS_PER_SLOT];
	  samResult->NumSlots = numSlots;
	  for (uint32_t s=0; s < numSlots; s++) {
		  uint32_t sampleInterval;
		  size += xclRead(XCL_ADDR_SPACE_DEVICE_PERFMON, baseAddress[s] + XSAM_SAMPLE_OFFSET, &sampleInterval, 4);
		  for (int c=0; c < XSAM_DEBUG_SAMPLE_COUNTERS_PER_SLOT; c++) {
			  size += xclRead(XCL_ADDR_SPACE_DEVICE_PERFMON, baseAddress[s]+sam_offsets[c], &temp[c], 4);
		  }
		  samResult->Version[s] 					= temp[0];
		  samResult->CUExecutionCount[s] 			= temp[1];
		  samResult->TotalCUExecutionCycles[s] 		= temp[2];
		  samResult->TotalIntStallCycles[s] 		= temp[3];
		  samResult->TotalStrStallCycles[s] 		= temp[4];
		  samResult->TotalExtStallCycles[s]			= temp[5];
		  samResult->MinExecutionTime[s] 			= (temp[6] == 4294967295) ? 0 : temp[6];
		  samResult->MaxExecutionTime[s] 			= temp[7];
		  samResult->TotalCUStarts[s] 				= temp[8];
	  }
	  return size;
  }

  size_t XOCLShim::xclDebugReadBarCounters(xclDebugBarCounterResults* barResult) {
	  size_t size = 0;
	  size += xclRead(XCL_ADDR_SPACE_DEVICE_PERFMON, barResult->base, &barResult->buffer[0], barResult->size);
	  return size;
  }

  // Read APM performance counters
  
  size_t XOCLShim::xclDebugReadCounters(xclDebugCountersResults* aCounterResults) {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id()
      << ", " << XCL_PERF_MON_MEMORY << ", " << aCounterResults
      << ", Read device counters..." << std::endl;
    }

    size_t size = 0;

    uint64_t spm_offsets[] = {
        XSPM_SAMPLE_WRITE_BYTES_OFFSET,
        XSPM_SAMPLE_WRITE_TRANX_OFFSET,
        XSPM_SAMPLE_READ_BYTES_OFFSET,
        XSPM_SAMPLE_READ_TRANX_OFFSET,
        XSPM_SAMPLE_OUTSTANDING_COUNTS_OFFSET,
        XSPM_SAMPLE_LAST_WRITE_ADDRESS_OFFSET,
        XSPM_SAMPLE_LAST_WRITE_DATA_OFFSET,
        XSPM_SAMPLE_LAST_READ_ADDRESS_OFFSET,
        XSPM_SAMPLE_LAST_READ_DATA_OFFSET
    };

    // Read all metric counters
    uint64_t baseAddress[XSPM_MAX_NUMBER_SLOTS];
    uint32_t numSlots = getIPCountAddrNames(AXI_MM_MONITOR, baseAddress, nullptr, nullptr, XSPM_MAX_NUMBER_SLOTS);

    uint32_t temp[XSPM_DEBUG_SAMPLE_COUNTERS_PER_SLOT];

    aCounterResults->NumSlots = numSlots;
    snprintf(aCounterResults->DevUserName, 256, "%s", mDevUserName.c_str());
    for (uint32_t s=0; s < numSlots; s++) {
      uint32_t sampleInterval;
      // Read sample interval register to latch the sampled metric counters
      size += xclRead(XCL_ADDR_SPACE_DEVICE_PERFMON,
                    baseAddress[s] + XSPM_SAMPLE_OFFSET,
                    &sampleInterval, 4);

      for (int c=0; c < XSPM_DEBUG_SAMPLE_COUNTERS_PER_SLOT; c++)
        size += xclRead(XCL_ADDR_SPACE_DEVICE_PERFMON, baseAddress[s]+spm_offsets[c], &temp[c], 4);

      aCounterResults->WriteBytes[s]      = temp[0];
      aCounterResults->WriteTranx[s]      = temp[1];

      aCounterResults->ReadBytes[s]       = temp[2];
      aCounterResults->ReadTranx[s]       = temp[3];
      aCounterResults->OutStandCnts[s]    = temp[4];
      aCounterResults->LastWriteAddr[s]   = temp[5];
      aCounterResults->LastWriteData[s]   = temp[6];
      aCounterResults->LastReadAddr[s]    = temp[7];
      aCounterResults->LastReadData[s]    = temp[8];
    }
    return size;
  }
} // namespace xocl_gem

size_t xclDebugReadIPStatus(xclDeviceHandle handle, xclDebugReadType type, void* debugResults)
{
  xocl::XOCLShim *drv = xocl::XOCLShim::handleCheck(handle);
  if (!drv)
    return -1;
  switch (type) {
    case XCL_DEBUG_READ_TYPE_LAPC :
      return drv->xclDebugReadCheckers(reinterpret_cast<xclDebugCheckersResults*>(debugResults));
    case XCL_DEBUG_READ_TYPE_SPM :
      return drv->xclDebugReadCounters(reinterpret_cast<xclDebugCountersResults*>(debugResults));
    case XCL_DEBUG_READ_TYPE_SAM:
      return drv->xclDebugReadSAMCounters(reinterpret_cast<xclDebugSAMCounterResults*>(debugResults));
    case XCL_DEBUG_READ_TYPE_BAR:
      return drv->xclDebugReadBarCounters(reinterpret_cast<xclDebugBarCounterResults*>(debugResults));
    default:
      ;
  };
  return -1;
}


