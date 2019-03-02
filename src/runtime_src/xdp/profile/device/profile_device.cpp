#include "profile_device.h"

namespace xdp {

ProfileDevice::ProfileDevice(xclDeviceHandle handle)
: device_handle(handle) {
    update_debug_ip_layout();
    update_device_info();
}

void ProfileDevice::update_debug_ip_layout() {
    ip_list = fetch_debug_ip_layout_from_sysfs(device_handle);
}

void ProfileDevice::update_device_info() {
    xclDeviceInfo2 info = {0};
    int get_device_info_ret = xclGetDeviceInfo2(device_handle, &info);
    if (get_device_info_ret) {
        std::cout << "fetch device info failed with code: " << get_device_info_ret << std::endl;
    }
    device_info = info;
}

std::vector<debug_ip_data> ProfileDevice::fetch_debug_ip_layout_from_sysfs(xclDeviceHandle handle) {
    std::vector<debug_ip_data> layout_output;
    std::string subdev = "icap";
    std::string entry = "debug_ip_layout";
    size_t max_path_size = 256;
    char raw_debug_ip_layout_path[max_path_size] = {0};
    int get_sysfs_ret = xclGetSysfsPath(handle, subdev.c_str(), entry.c_str(), raw_debug_ip_layout_path, max_path_size);
    if (get_sysfs_ret < 0) {
        return layout_output;
    }
    raw_debug_ip_layout_path[max_path_size - 1] = '\0';
    std::string debug_ip_layout_path(raw_debug_ip_layout_path);
    std::ifstream debug_ip_layout_fs(debug_ip_layout_path.c_str(), std::ifstream::binary);
    size_t max_sysfs_size = 65536;
    char buffer[max_sysfs_size] = {0};
    if (debug_ip_layout_fs) {
        debug_ip_layout_fs.read(buffer, max_sysfs_size);
        if (debug_ip_layout_fs.gcount() > 0) {
            debug_ip_layout* layout = reinterpret_cast<debug_ip_layout*>(buffer);
            int ip_count = layout->m_count;
            for (int ip_index = 0; ip_index < ip_count; ++ip_index) {
                debug_ip_data ip_data = layout->m_debug_ip_data[ip_index];
                layout_output.push_back(ip_data);
            }
        } else {
            return layout_output;
        }
    } else {
        return layout_output;
    }
    return layout_output;
}

std::vector<clock_freq> ProfileDevice::fetch_clock_freq_topology_from_sysfs(xclDeviceHandle handle) {
    std::vector<clock_freq> clock_freq_topology_output;
    std::string subdev = "icap";
    std::string entry = "clock_freq_topology";
    size_t max_path_size = 256;
    char raw_clock_freq_topology_path[max_path_size] = {0};
    int get_sysfs_ret = xclGetSysfsPath(handle, subdev.c_str(), entry.c_str(), raw_clock_freq_topology_path, max_path_size);
    if (get_sysfs_ret < 0) {
        return clock_freq_topology_output;
    }
    raw_clock_freq_topology_path[max_path_size - 1] = '\0';
    std::string clock_freq_topology_path(raw_clock_freq_topology_path);
    std::ifstream clock_freq_topology_fs(clock_freq_topology_path.c_str(), std::ifstream::binary);
    size_t max_sysfs_size = 65536;
    char buffer[max_sysfs_size] = {0};
    if (clock_freq_topology_fs) {
        clock_freq_topology_fs.read(buffer, max_sysfs_size);
        if (clock_freq_topology_fs.gcount() > 0) {
            clock_freq_topology* layout = reinterpret_cast<clock_freq_topology*>(buffer);
            int clock_count = layout->m_count;
            for (int clock_index = 0; clock_index < clock_count; ++clock_index) {
                clock_freq ip_data = layout->m_clock_freq[clock_index];
                clock_freq_topology_output.push_back(ip_data);
            }
        } else {
            return clock_freq_topology_output;
        }
    } else {
        return clock_freq_topology_output;
    }
    return clock_freq_topology_output;
}

std::string ProfileDevice::get_device_name() {
    std::string device_name(device_info.mName);
    return device_name;
}

std::vector<debug_ip_data> ProfileDevice::get_ip_config_by_type(xclPerfMonType type) {
    /**
     * TODO:
     * If this get_ip_config_by_type ends up being called too frequently, please
     * consider caching the different types of IPs using a hash map to make it
     * computationally efficient. For now, in my use case, it should be called 
     * once so there is no performance concerns.
     */
    std::vector<debug_ip_data> ip_config_output;
    for (auto ip : ip_list) {
        if (ip.m_type == type) {
            ip_config_output.push_back(ip);
        }
    }
    return ip_config_output;
}

std::string ProfileDevice::get_ip_name_by_index(xclPerfMonType type, unsigned index) {
    auto target_ip_list = get_ip_config_by_type(type);
    if (index >= target_ip_list.size()) {
        return "";
    }
    auto target_ip = target_ip_list[index];
    std::string target_ip_name(target_ip.m_name);
    return target_ip_name;
}

unsigned ProfileDevice::get_kernel_clock_frequency() {
    return 0.;
}

double ProfileDevice::get_max_bandwith_in_mdps() {
    return 0.0;
}

size_t ProfileDevice::get_timestamp() {
    size_t device_timestamp = xclGetDeviceTimestamp(device_handle);
    return device_timestamp;
}

} //  xdp

