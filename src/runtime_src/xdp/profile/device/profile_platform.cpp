#include "profile_platform.h"

namespace xdp {

ProfilePlatform::ProfilePlatform() 
: device_count(0),
flow_mode(RTUtil::e_flow_mode::DEVICE) {}

void ProfilePlatform::register_profile_device_with_name(std::shared_ptr<ProfileDevice> profile_device, const std::string& name) {
    /**
     * TODO:
     * We need to think more about name conflict, for now I think if there is a
     * name conflict, I should simply overwrite the previous device because it
     * can be for update purpose. However, If later proven to be not the case, 
     * name conflict checking can be added.
     * 
     * Also how to deal with one actual hal device handle being registered multiple
     * times is another open discussion. Normally, they should never happen, but in
     * rare cases where they do. We will need to decide whether to overwrite, give a 
     * warning or throw an error.
     */
    device_pool[name] = profile_device;
    ++device_count;
}

void ProfilePlatform::register_device_with_name(xclDeviceHandle handle, const std::string& name) {
    auto candidate_device = std::shared_ptr<ProfileDevice>(new ProfileDevice(handle));
    register_profile_device_with_name(candidate_device, name);
}

std::string ProfilePlatform::register_device(xclDeviceHandle handle) {
    auto candidate_device = std::shared_ptr<ProfileDevice>(new ProfileDevice(handle));
    auto device_name = candidate_device->get_device_name();
    auto device_register_name = device_name + std::to_string(device_count);
    register_profile_device_with_name(candidate_device, device_register_name);
    return device_register_name;
}

bool ProfilePlatform::device_registered(const std::string& name) {
    auto it = device_pool.find(name);
    if (it == device_pool.end()) {
        return false;
    }
    return true;
}

unsigned ProfilePlatform::get_device_ip_config_by_type(xclPerfMonType type, const std::string& device_name) {
    if (!device_registered(device_name)) {
        return 0;
    }
    return device_pool[device_name]->get_ip_config_by_type(type).size();
}

double ProfilePlatform::get_trace_time() {
    duration_ns time_span = std::chrono::duration_cast<duration_ns>(std::chrono::high_resolution_clock::now().time_since_epoch());
    uint64_t time_nano_sec = time_span.count();
    return (time_nano_sec / 1.0e6);
}

} //  xdp

