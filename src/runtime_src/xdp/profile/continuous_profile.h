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

#ifndef __XILINX_CONTINUOUS_PROFILING
#define __XILINX_CONTINUOUS_PROFILING

#include "rt_profile_writers.h"
#include "xclperf.h"
#include "xocl/core/device.h"
#include "xocl/core/platform.h"

#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <iostream>

namespace XCL {

class BaseMonitor {
public:
	BaseMonitor() {};
	virtual std::string get_id() = 0;
	virtual void launch() = 0;
	virtual void terminate() = 0;
	virtual ~BaseMonitor() {};
};

class ThreadMonitor : public BaseMonitor {
public:
	std::string get_id() override {return "thread_monitor";};
	void launch();
	void terminate();
	virtual ~ThreadMonitor();
protected:
	virtual void thread_func(int id) {};
	virtual void willLaunch() {};
	virtual void setLaunch() {};
	virtual void didLaunch() {};
	virtual void willTerminate() {};
	virtual void setTerminate() {};
	virtual void didTerminate() {};
private:
	std::thread monitor_thread;
};

class SamplingMonitor : public ThreadMonitor {
public:
	SamplingMonitor(int freq_in=1) : ThreadMonitor(),
			should_continue(false), sample_freq(freq_in) {};
	std::string get_id() override {return "sampling_monitor";};
protected:
	void thread_func(int id) override;
	void setLaunch() override;
	void setTerminate() override;
	virtual bool shoudEarlyTerminate() {return false;};
	virtual void willSampleOnce() {};
	virtual void sampleOnce() {};
	virtual void didSampleOnce() {};
	virtual void willSample() {};
	virtual void didSample() {};
	virtual void willPause() {};
	virtual void didPause() {};
private:
	std::mutex status_guard;
	bool should_continue;
	int sample_freq;
};

class PowerMonitor : public SamplingMonitor {
public:
	PowerMonitor(int freq_in, xocl::device* device_in);
	std::string get_id() {return "power_monitor";};
private:
	XclPowerInfo readPowerStatus();
	void outputPowerStatus(XclPowerInfo& status);
protected:
	void sampleOnce() override;
	void willLaunch() override;
	void didTerminate() override;
private:
	xocl::device* device;
	std::ofstream power_dump_file;
};

class PowerProfile {
public:
	PowerProfile(std::shared_ptr<xocl::platform> platform);
	void launch();
	void terminate();
private:
	std::vector<BaseMonitor*> powerMonitors;
};

}

#endif
