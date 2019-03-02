#ifndef XDP_PROFILE_DEVICE_PROFILE_PLATFORM_H_
#define XDP_PROFILE_DEVICE_PROFILE_PLATFORM_H_

#include <map>
#include <string>
#include <chrono>
#include "xdp/profile/device/profile_device.h"

namespace xdp {

typedef std::chrono::duration<uint64_t, std::ratio<1, 1000000000>> duration_ns;

/**
 * The ProfilePlatform class is responsible for managing
 * the underlying devices and expose the APIs of the devices.
 * It will appear as a member variable of the xdp core
 * that can provide full visibility and accessibility into
 * all the Xilinx devices.
 * 
 * \note
 * In our case, ProfilePlatform can only exist once as the
 * Xilinx platform, we should consider making it into a 
 * singleton structure if found being more preferrable in
 * the future.
 */
class ProfilePlatform {
public:
    ProfilePlatform();

    /**
     * The register_profile_device_with_name registers a profile
     * device instance into the interanl device pool. Normally, this
     * should only be called internally for code sharing between
     * different hal device registration methods.
     */
    void register_profile_device_with_name(std::shared_ptr<ProfileDevice> profile_device, const std::string& name);

    /**
     * The register_device_with_name should register a hal device
     * into the platform with a specified name. This API is made
     * for backward compatibility with OpenCL xrt devices with a
     * xrt conventional names.
     */
    void register_device_with_name(xclDeviceHandle handle, const std::string& name);

    /**
     * The register_device should register a hal device and assign
     * the device with a name according to the hal device name and
     * then return that generated name back to the user.
     */
    std::string register_device(xclDeviceHandle handle);

    /**
     * The device_registered will check whether a device with a certain
     * name has been registered. It is for the convenience of other API
     * implementations.
     */
    bool device_registered(const std::string& name);

    /**
     * The get_device_ip_config_by_type checks for a device specified by
     * the name and the retrieve the ip configurations filtered by the type
     * through the API provided by ProfileDevice.
     */
    unsigned get_device_ip_config_by_type(xclPerfMonType type, const std::string& device_name);

    std::string get_device_ip_name_by_index(xclPerfMonType type, unsigned index, const std::string& device_name);

    unsigned get_device_kernel_clock_frequency(const std::string& deviceName);

    size_t get_device_timestamp(const std::string& deviceName);

    /**
     * The set_flow_mode sets the execution mode for the platform which
     * is shared by all the devices. Since it comes from the environment
     * variable so there can only be one mode per platform.
     * 
     * \note
     * Theoretically this is only directly relevent to the device itself
     * which means there should not be a set method. However, so far it
     * is determined in a higher layer, so it has to be passed in from the
     * plugins. When that changes, this set method should be removed.
     */
    void set_flow_mode(RTUtil::e_flow_mode mode) {flow_mode = mode;}

    /**
     * The get_flow_mode returns the execution mode this platform is
     * running on. So far, it has to be set first. Otherwise, it will
     * default to hardware device execution.
     */
    RTUtil::e_flow_mode get_flow_mode() {return flow_mode;}

    double get_trace_time();

private:
    int device_count; /**< the number of devices registered, used for assigning unique id */
    std::map<std::string, std::shared_ptr<ProfileDevice>> device_pool; /**< a dictionary for all registered devices */
    RTUtil::e_flow_mode flow_mode; /**< flow mode indicate which mode the application running on */
};

} //  xdp

#endif