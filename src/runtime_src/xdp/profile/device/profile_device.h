#ifndef XDP_PROFILE_DEVICE_PROFILE_DEVICE_H_
#define XDP_PROFILE_DEVICE_PROFILE_DEVICE_H_

#include <string>
#include <iostream>
#include <memory>
#include <vector>
#include <fstream>
#include "xclhal2.h"
#include "xdp/profile/core/rt_util.h"

namespace xdp {

/**
 * The ProfileDevice class is responsible for
 * all the device related operations xdp needs.
 * 
 * Examples:
 *  - access debug_ip_layout
 *  - access ip_layout
 *  - read device registers
 *  - get device timestamp
 * 
 * This class internally use (make sure
 * only hal layer APIs are involved): 
 *  - xclGetSysfsPath
 *  - xclRead
 *  - xclWrite
 *  - xclunmgdPRead
 *  - xclunmgedPWrite
 */
class ProfileDevice {
public:
    /**
     * The hal device handle represents all the hardware 
     * related aspects of the device, which makes it sufficient 
     * to construct a profile device representation.
     */
    ProfileDevice(xclDeviceHandle handle);
    
    /**
     * The update_debug_ip_layout API fetch debug_ip_layout data
     * using fetch method(s) and overwrites the internal structure
     * that represent the debug and profile IPs.
     * 
     * \note
     * This API should be called at initiation and also can be
     * called at any random time within the lifetime of the hal
     * device handle.
     */
    void update_debug_ip_layout();

    /**
     * The update_device_info API reads the xclDeviceInfo2 through
     * hal API with the underlying device handle and saves a copy
     * of the device information internally.
     * 
     * \note
     * This API should be called at initiation and also can be
     * called at any random time within the lifetime of the hal
     * device handle.
     */
    void update_device_info();

    /**
     * The fetch_from_sysfs API will fetch the debug IP data through
     * sysfs hal API using the a device handle. It is made as a static
     * method because it can be used standalone for one time query 
     * use case.
     * 
     * \note
     * Given that debug_ip_layout can exist in many different places
     * depending on the platform and the mode. This method is only 
     * responsible for fetching the data from the hal defined sysfs 
     * location.
     */
    static std::vector<debug_ip_data> fetch_debug_ip_layout_from_sysfs(xclDeviceHandle handle);

    /**
     * The fetch_clock_freq_from_sysfs API will fetch the kernel clock
     * information through sysfs using the hal API with a hal device handle.
     * Normally, this method should only be called internally. However, given
     * that it is reading from sysfs which doesn not necessarily have a binding 
     * to an instant of this class, it is made static.
     */
    static std::vector<clock_freq> fetch_clock_freq_topology_from_sysfs(xclDeviceHandle handle);

    /**
     * The get_device_name API returns the name of the device
     * retrieved from the xclDeviceInfo2 structure.
     */
    std::string get_device_name();

    /**
     * The get_ip_config_by_type API queries the underlying 
     * ip_config object and retrieves a list containing all
     * the ip configs of the type specified.
     */
    std::vector<debug_ip_data> get_ip_config_by_type(xclPerfMonType type);

    /**
     * The get_ip_name_by_index API will get the m_name of
     * the IP with matching type and index in the form of 
     * a standard string.
     */
    std::string get_ip_name_by_index(xclPerfMonType type, unsigned index);

    /**
     * The get_kernel_clock_frequency API should retrieve the
     * kernel clock frequency of the current bitstream 
     * configuration.
     */
    unsigned get_kernel_clock_frequency();

    /**
     * The get_max_bandwith_in_mdps API should retrieve the 
     * theoretical maximum memory bandwidth of the device.
     */
    double get_max_bandwith_in_mbps();

    /**
     * The get_timestamp API should retrievfe the on-device
     * timestamp through hal.
     */
    size_t get_timestamp();

private:
    std::vector<debug_ip_data> ip_list; /**< list of debug and profile IPs found */

    /**
     * As discussed with Jason, it's cleaner to dump the information per xclbin loading
     * It can be:
     *  1. dump multiple csv files and leave the concat to sdx_analyze
     *  2. keep separate file streams and append to the event file stream
     * so this map might not be needed.
     */
    std::map<std::string, std::vector<debug_ip_data>> layout_history; /**< a history of the xclbins loaded onto the device */

    xclDeviceHandle device_handle; /**< the hal handle for all the device operations */
    xclDeviceInfo2 device_info; /**< the hal device information */
};

} //  xdp

#endif