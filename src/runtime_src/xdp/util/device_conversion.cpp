#include "xdp/util/device_conversion.h"

std::vector<xocl::device*> get_unique_root_device_range(const xocl::platform* target_platform) {
    std::vector<xocl::device*> unique_root_device_list;
    std::map<unsigned int, std::vector<xocl::device*>> device_structure;
    for (auto device : target_platform->get_device_range()) {
        xocl::device* root_device = const_cast<xocl::device*>(device->get_root_device());
        unsigned int device_uid = root_device->get_uid();
        auto iter = device_structure.find(device_uid);
        if (iter == device_structure.end()) {
            unique_root_device_list.push_back(root_device);
            device_structure[device_uid] = std::vector<xocl::device*>();
        }
        device_structure[device_uid].push_back(device);
    }
    return unique_root_device_list;
}