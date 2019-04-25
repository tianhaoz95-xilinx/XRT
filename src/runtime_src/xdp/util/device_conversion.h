#ifndef XDP_DEVICE_CONVERSION_H_
#define XDP_DEVICE_CONVERSION_H_

#include <vector>
#include <map>

#include "xocl/core/device.h"
#include "xocl/core/platform.h"
#include "xocl/core/object.h"
#include "xocl/core/range.h"
#include "xocl/core/refcount.h"

using device_vector_type = std::vector<xocl::ptr<xocl::device>>;
using device_iterator_type = xocl::ptr_iterator<device_vector_type::iterator>;
using device_const_iterator_type = xocl::ptr_iterator<device_vector_type::const_iterator>;

/**
 * This function should take in a raw list of subdevices, search for
 * a list of unique root devices from the subdevices and output the 
 * list of unique root devices.
 */
std::vector<xocl::device*> get_unique_root_device_range(const xocl::platform* target_platform);

#endif