#ifndef __XILINX_ILA
#define __XILINX_ILA

#include <string>

namespace XCL
{
  // Forward
  class BackgroundProcess;
  class InterpGuard;


  class LabtoolController {
  public:
    LabtoolController(const std::string& ID_init);
    void init(std::string& workspace, unsigned port, unsigned instance, std::string& vivado_lab_path, std::string& optional);
    std::string getID();
    void launch();
    void finish();
    void cleanup();

  private:
    const std::string get_user_tcl_file() const;
    const std::string get_ltx_file() const;
    const std::string get_working_dir() const;

    // Alter settings as needed based on passed in ini params
    void process_params();

    // Throws an exception if we are missing tools on the host
    // Need to have xvc_pcie and vivado (or vivado_lab).
    void verify_tools_installed_or_error();

    // Copy the trigger template settings file to the current
    // directory for the user.
    // On the first debug iteration, the file will not exist and
    // we copy to the current directory.
    // Subsequent iterations pick the file up from the current dir.
    // This allows the user to modify the file and iterate with the
    // debug flow.
    void copy_user_tcl_template() const;

    // Creates the unique working directory for this process launch
    // This is the working directory for vivado and xvc_pcie and all their
    // input/output files
    void setup_working_directory();

    // Launch background xvc_pcie process. This must be launched before
    // vivado (or vivado_lab) is launched.
    // Changes to the given dir before launch.
    // This will throw an exception if the port is already in use
    void launch_xvc_pcie();

    // Launch background vivado (or vivado_lab) batch process.
    // Changes to the given dir before launch.
    void launch_vivado_lab();

    void launch_vivado();

    // Launch vivado in interactive GUI mode to view waveform capture
    void launch_vivado_lab_interactive();

    // Need to give vivado process time to load the tcl script
    // If we exceed a timeout threshold, a runtime exception is
    // thrown
    void wait_until_ready();

    // Communicate with the running vivado process - when the
    // ILA is armed, allow the host process to continue
    void arm_ila_trigger();

    // Write ILA data to the capture file
    void capture_ila();

    // Shut down the vivado and xvc_pcie servers
    void shutdown_servers();

    // Delete leftovers in the working directory when our flow completes.
    // Copy any interesting artifacts (like the waveform.ila file) up
    // and then delete the directory.
    void cleanup_working_directory();

    bool check_vivado_lab_availability();
    bool check_vivado_availability();
    bool check_xvc_pcie_availability();

  private:
    std::string ID;
    std::string workspace_root;
    std::string vivado_lab_location;
    unsigned int xvc_pcie_port;
    unsigned int driver_instance;
    std::string optional_ini_parameters;
    unsigned int m_timeout;
    BackgroundProcess *mp_vivado;
    BackgroundProcess *mp_xvcpcie;
    bool m_interactive;
    InterpGuard *mp_interp;

    bool vivado_lab_available;
    bool vivado_available;
    bool xvc_pcie_available;
  };

}

#endif
