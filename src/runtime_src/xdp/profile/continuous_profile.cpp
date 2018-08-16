#include "continuous_profile.h"

#include <stdlib.h>
#include <thread>
#include <chrono>

namespace XCL {

ThreadMonitor::~ThreadMonitor() {
	if (monitor_thread.joinable()) {
		terminate();
	}
}

void ThreadMonitor::launch() {
	willLaunch();
	setLaunch();
	monitor_thread = std::thread(&ThreadMonitor::thread_func, this, 0);
	didLaunch();
}

void ThreadMonitor::terminate() {
	willTerminate();
	setTerminate();
	monitor_thread.join();
	didTerminate();
}

void SamplingMonitor::setLaunch() {
	status_guard.lock();
	should_continue = true;
	status_guard.unlock();
}

void SamplingMonitor::setTerminate() {
	status_guard.lock();
	should_continue = false;
	status_guard.unlock();
}

void SamplingMonitor::thread_func(int id) {
	willSample();
	int interval = 1e6 / sample_freq;
	while (true) {
		status_guard.lock();
		bool continue_sample = should_continue;
		status_guard.unlock();
		if (continue_sample && !shoudEarlyTerminate()) {
			willSampleOnce();
			sampleOnce();
			didSampleOnce();
		} else {
			break;
		}
		willPause();
		std::this_thread::sleep_for (std::chrono::microseconds(interval));
		didPause();
	}
	didSample();
}

PowerMonitor::PowerMonitor(int freq_in, xocl::device* device_in)
:SamplingMonitor(freq_in) {
	device = device_in;
}

void PowerMonitor::sampleOnce() {
	auto status = readPowerStatus();
	outputPowerStatus(status);
}

void PowerMonitor::didTerminate() {
	power_dump_file.close();
}

void PowerMonitor::willLaunch() {
	std::string dump_filename = "power-trace-"+device->get_unique_name()+".csv";
	std::cout << "open " << dump_filename << std::endl;
	power_dump_file.open(dump_filename, std::ofstream::out | std::ofstream::trunc);
	power_dump_file.close();
	power_dump_file.open(dump_filename, std::ofstream::app);
	power_dump_file << "Timestamp,FPGA Power Consumption,Board Power Consumption" << std::endl;
	power_dump_file.flush();
}

XclPowerInfo PowerMonitor::readPowerStatus() {
	XclPowerInfo powerInfo = device->get_power_info();
	return powerInfo;
}

void PowerMonitor::outputPowerStatus(XclPowerInfo& status) {
	auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
	power_dump_file << timestamp << ",";
	power_dump_file << status.m3v3Pex;
	power_dump_file << std::endl;
	power_dump_file.flush();
}

PowerProfile::PowerProfile(std::shared_ptr<xocl::platform> platform) {
	for (auto device : platform->get_device_range()) {
      	std::string deviceName = device->get_unique_name();
		BaseMonitor* monitor = new PowerMonitor(10, device);
		powerMonitors.push_back(monitor);
    }
}

void PowerProfile::launch() {
	std::cout << "launching power profile ..." << std::endl;
	for (auto monitor : powerMonitors) {
		monitor->launch();
	}
}

void PowerProfile::terminate() {
	for (auto monitor : powerMonitors) {
		monitor->terminate();
	}
}

}
