#include "ila.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <stdexcept>

// TODO: Check if Tcl inclusion is allowed in xdp
// I'm not sure if <tcl.h> will be allowed (inclusion of Tcl interpreter)
// We may need to c++ify the code that currently is using Tcl in this file.
// This causes inclusion of -ltcl in the makefiles, which could be a problem when
// we start supporting windows. Also check this on Ubuntu...
#include <tcl8.6/tcl.h>
//~~~~~~~~~~~~~~

// TODO: Change couts to a message manager output conditional on some verbosity param
// TODO: LTX handling - need to get or compute LTX filename (currently hardcoded)

namespace bfs = boost::filesystem;

namespace {
  // ------------------------------------------------
  // UNNAMED NAMESPACE - HELPER CLASSES AND FUNCTIONS
  // ------------------------------------------------

  // Given the unique management device number, locate the xvc_pcie
  // char device driver.
  const std::string get_xvc_driver(unsigned int deviceNumber)
  {
    std::string driverName = "/dev/xvc_pub.m" + std::to_string(deviceNumber);
    if (!bfs::exists(driverName))
      throw std::runtime_error("Kernel driver file '" + driverName + "' not found");
    return driverName;
  }


  // Given long path like /a/b/c/d/e.txt, return e.txt
  const std::string base_filename(const std::string& path)
  {
    return path.substr(path.find_last_of("/\\") + 1);
  }


  const std::string get_xdp_debug_dir()
  {
    // Debug files are located in $XILINX_XRT/share/debug
    const char *xrt_env = getenv("XILINX_XRT");
    if (! xrt_env) {
      throw std::runtime_error("get_debug_dir() - XILINX_XRT not correctly set");
    }
    bfs::path xrt_path(xrt_env);
    bfs::path debug_path = xrt_path / "share/debug";
    if (!bfs::is_directory(debug_path))
      throw std::runtime_error("XILINX_XRT: No such directory '" + debug_path.string() + "'");
    return debug_path.string();
  }

#if 0
  const std::string get_vivado_lab_cmd()
  {
    // Returns the full path to vivado_lab or vivado
    // TODO: Create a smart system to find vivado or vivado_lab on the system
    const std::string cmd = "/proj/xbuilds/2018.3_daily_latest/labtools_installs/lin64/Vivado_Lab/2018.3/bin/vivado_lab";
    return cmd;
  }


  const std::string get_xvc_pcie_cmd()
  {
    // TODO: Find xvc_pcie or thow an exception
    const std::string cmd = "/proj/xbuilds/2018.3_daily_latest/labtools_installs/lin64/Vivado_Lab/2018.3/bin/xvc_pcie";
    return cmd;
  }
#endif

  const std::string get_viewwave_tcl_file()
  {
    bfs::path debugDir(get_xdp_debug_dir());
    bfs::path tclPath = debugDir / "cs_viewwave.tcl";
    if (!bfs::exists(tclPath))
      throw std::runtime_error("File not found '" + tclPath.string() + "'");
    return tclPath.string();
  }


  const std::string get_server_tcl_file()
  {
    bfs::path debugDir(get_xdp_debug_dir());
    bfs::path tclPath = debugDir / "cs_server.tcl";
    if (!bfs::exists(tclPath))
      throw std::runtime_error("File not found '" + tclPath.string() + "'");
    return tclPath.string();
  }


  const std::string get_client_tcl_file()
  {
    bfs::path debugDir(get_xdp_debug_dir());
    bfs::path tclPath = debugDir / "cs_client.tcl";
    if (!bfs::exists(tclPath))
      throw std::runtime_error("File not found '" + tclPath.string() + "'");
    return tclPath.string();
  }



  // Copy the given source file to the destination directory
  //    Used to copy the trigger and ltx files to the vivado working directory
  void copy_to_dir(const std::string& srcFile, const std::string& dstDir)
  {
    if (!bfs::exists(srcFile))
      throw std::runtime_error("copy_to_dir: file '" + srcFile + "' not found");
    bfs::path dstPath = bfs::path(dstDir) / base_filename(srcFile);
    std::string dstFile = dstPath.string();
    // TODO: Use bfs::copy_file (but not currently working - need cmake update)
    // bfs::copy_file(srcFile, dstFile, bfs::copy_option::overwrite_if_exists);
    // Quick and dirty workaround below:
    std::ifstream src(srcFile, std::ios::binary);
    std::ofstream dst(dstFile, std::ios::binary);
    dst << src.rdbuf();
    src.close();
    dst.close();
  }
}


namespace XCL
{
  ////////////////////////////////////////////////////////////////////////////
  // InterpGuard -
  //   Helper - Ensure we property create and delete the Tcl interpreter
  //
  // TODO: TCL DEPENDENCY ALERT -
  // This is code for getting a demo up and running more quickly.
  // Unless we can embed a tcl interpreter in the xdp code, we may
  // need to c++ify this code before checkin...
  //
  class InterpGuard
  {
    public:
      InterpGuard()
        : mp_interp(nullptr)
      {
        mp_interp = Tcl_CreateInterp();
        std::string tclCmd = "source " + get_client_tcl_file();
        int result = Tcl_Eval(mp_interp, tclCmd.c_str());
        if (result != TCL_OK) {
          throw std::runtime_error("Could not create Tcl interpreter");
        }
      }

      ~InterpGuard()
      {
        if (mp_interp) {
          Tcl_DeleteInterp(mp_interp);
        }
        mp_interp = nullptr;
      }

      int exec_tcl(const std::string& cmd)
      {
        if (!mp_interp) {
          throw std::runtime_error("Tcl interpreter does not exist");
        }
        return Tcl_Eval(mp_interp, cmd.c_str());
      }

    private:
      Tcl_Interp *mp_interp;
  };

  ////////////////////////////////////////////////////////////////////////////
  // BackgroundProcess -
  //   Helper that runs <cmd> [<args...>] in the background using fork()
  //   and execv() system calls.
  //
  //   This is used to launch xvc_pcie and vivado_lab in the background without
  //   waiting for them to complete. Optionally stdout and stderr can be sent
  //   to a logfile.
  //
  //   Launched process is tracked and automatically cleaned up when destructor
  //   is called
  //
  //   Usage:
  //       BackgroundProcess p("ls", {"-l"});
  //       p.setLog("logfile.txt) <-- optional stdout/stderr log
  //       p.setDir(directory)    <-- optionally change dir before execution
  //       p.start();             <-- required to start process
  //       ...
  //       p.wait();              <-- optional, block for process to end
  //       p.end();               <-- optional, called in destructor
  //
  //  TODO: Error and exception handler for bad return values
  //  TODO: isRunning() still needs some work for a process that ends early
  //  TODO: interprocess mutex that will throw an exception if a conflicting
  //        process tries to start the same program like xvc_pcie while another
  //        is currently running
  //  TODO: Better way to kill processes than the system call to kill
  //
  class BackgroundProcess
  {
    public:
      BackgroundProcess(const std::string& cmd, const std::vector<std::string>& args = {})

        : m_cmd(cmd)
        , m_args(args)
        , m_pid(0)
        , m_inherit(false)
        , m_logFile("")
        , m_dir("")
      {
      }

      ~BackgroundProcess()
      {
        end();
      }

      void setDir(const std::string& dir)
      {
        m_dir = dir;
      }

      void setLog(const std::string& logFile)
      {
        m_logFile = logFile;
      }

      bool isRunning() const
      {
        // Process is running if we are allowed to kill it.
        // The null signal (0) can check that without actually killing
        // TODO: This is not always returning false when the process finishes before
        //       the kill-9 in end()
        int retval = -1;
        if (m_pid > 0) {
          retval = kill(m_pid, 0 /*sig*/);
        }
        return (retval == 0);
      }

      void start()
      {
        // Use fork() and execv() to start a background process
        m_pid = fork();
        if (m_pid < 0) {
          // I am the parent
          // Fork error in parent
          // TODO: throw exception or something here...
        }
        else if (m_pid == 0) {
          // I am the forked child...
          // Replace this process image with a new one using execv.

          if (m_dir != "") {
            // new working directory
            if(chdir(m_dir.c_str())) {
              std::cout << "Cannot swith dir, return" << std::endl;
              return;
            }
          }

          // Stdout/Stderr log file - we log results from the child to a file
          // if requested
          if (m_logFile != "") {
            int fd = open(m_logFile.c_str(), \
                          O_WRONLY | O_TRUNC | O_CREAT, \
                          S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR );
            if (fd == -1) {
              throw std::runtime_error("Could not open log file");
            }
            else {
              if (dup2(fd,1) == -1) {
                throw std::runtime_error("Could not redirect stdout");
              }
              if (dup2(fd,2) == -1) {
                throw std::runtime_error("Could not redirect stderr");
              }
              close(fd);
            }
          }

          if (!m_inherit) {
            for (int fd = 3; fd < 1024; ++fd) {
              int tmp = dup(fd);
              if (tmp >= 0) {
                close(tmp);
                close(fd);
              }
            }
          }

          char *p_cmd = strdup(m_cmd.c_str());
          std::vector<char*> args_vec;
          args_vec.push_back(p_cmd);
          for (auto arg : m_args) {
            args_vec.push_back(strdup(arg.c_str()));
          }
          args_vec.push_back(nullptr);
          char** p_args = &args_vec[0];

          // This goes to the process stdout log file
          std::cout << "=========================================\n";
          std::cout << "STDOUT/STDERR LOG\n";
          std::cout << "COMMAND:\n";
          std::cout << p_args[0];
          for (char **p = &p_args[1]; *p != nullptr; ++p) {
            std::cout << " " << *p;
          }
          std::cout << "\n=========================================\n";
          std::cout << std::endl;

          setsid();
          // comment this out to clean unused variable error
          execv(p_cmd, p_args);
          // Never should get here... execv does not return
          throw std::runtime_error("Error during execv");
        }
        else {
          // If we get here we are a happy parent of a healthy new child
          // Add anything here to do when the child starts
        }
      }

      void end(int sig = 9)
      {
        if (m_pid > 0) {
          // Kill all processes associated with this
          // session id. Required to kill all 3 processes in a vivado
          // type loader. Otherwise we get zombies.
          // TODO: There must be a better way to kill a group of processes
          kill((-1)*(m_pid), sig);
#if 0
          std::string cmd = "ps -s " + std::to_string(m_pid) + " -o pid=";
          FILE *pipe = popen(cmd.c_str(), "r");
          if (!pipe) {
            throw std::runtime_error("Error killing process");
          }
          char pidStr[256];
          std::vector<pid_t> pidList;
          while (fscanf(pipe, "%s", pidStr) != EOF) {
            pidList.push_back(atoi(pidStr));
          }
          pclose(pipe);
          for (auto pid: pidList) {
            // This did not work: kill(m_pid, sig);
            // Instead I use a system call to kill and it does work
            cmd = "kill -" + std::to_string(sig) + " " + std::to_string(pid);
            system(cmd.c_str());
          }
          m_pid = 0;
#endif
        }
      }

      void wait()
      {
        int status;
        waitpid(m_pid, &status, 0);
        m_pid = 0;
      }

      pid_t get_pid()
      {
        return m_pid;
      }

    private:
      std::string m_cmd;
      std::vector<std::string> m_args;
      pid_t m_pid;
      bool m_inherit;
      std::string m_logFile;
      std::string m_dir;
  };
  ////////////////////////////////////////////////////////////////////////////
}


namespace XCL
{

  LabtoolController::LabtoolController(const std::string& ID_init)
    : ID(ID_init)
    , xvc_pcie_port(0)
    , driver_instance(0)
    , m_timeout(120)
    , mp_vivado(nullptr)
    , mp_xvcpcie(nullptr)
    , m_interactive(false)
    , mp_interp(nullptr)
    ,vivado_lab_available(false)
    ,vivado_available(false)
    ,xvc_pcie_available(false)
  {
  }


  void LabtoolController::init(std::string& workspace, unsigned port, unsigned timeout,
                               unsigned instance, std::string& vivado_lab_path,
                               std::string& optional)
  {
    workspace_root = workspace;
    xvc_pcie_port = port;
    driver_instance = instance;
    optional_ini_parameters = optional;
    vivado_lab_location = vivado_lab_path;
    m_timeout = timeout;
  }


  std::string LabtoolController::getID()
  {
    return ID;
  }


  //------------------------------
  // MAIN FLOW IS IN THIS METHOD
  //------------------------------
  void LabtoolController::launch()
  {
    std::cout << "\n";
    std::cout << "================================================\n";
    std::cout << "          CHIPSCOPE DEBUG FLOW ENABLED          \n";
    std::cout << "================================================\n";
    std::cout << "\n";
    std::cout << "chipscope_flow enabled in sdx.ini\n";
    std::cout << "\n";

//    std::cout << "launch labtool in: " << workspace_root << std::endl;
//    std::cout << "\twith port: " << xvc_pcie_port << std::endl;
//    std::cout << "\twith mgmt_instance: " << driver_instance << std::endl;
//    std::cout << "\tand optional argument: " << optional_ini_parameters << std::endl;

    try {
      mp_interp = new InterpGuard();
      process_params();
      verify_tools_installed_or_error();
      copy_user_tcl_template();
      setup_working_directory();
      launch_xvc_pcie();
      launch_vivado();
      wait_until_ready();
      arm_ila_trigger();
    }
    catch (std::exception &e) {
      std::cout << e.what() << "\n";
      std::cout << "Host program will continue without chipscope debug\n";
      std::cout << "\n*** Aborted chipscope debug operation ***\n\n";
    }
    catch (...) {
      std::cout << "Caught unknown exception\n";
      std::cout << "Host program will continue without chipscope debug\n";
      std::cout << "\n*** Aborted chipscope debug operation ***\n\n";
    }

    std::cout << "\nCONTINUING HOST EXECUTION...\n";

  }


  void LabtoolController::finish()
  {
    // Nothing to do here I believe
  }


  // Gracefully shut down any running processes
  void LabtoolController::cleanup()
  {
    bool valid = (mp_vivado) && (mp_xvcpcie);
    if (valid) {
      std::cout << "\n";
      std::cout << "================================================\n";
      std::cout << "       CHIPSCOPE DEBUG FLOW POST PROCESS        \n";
      std::cout << "================================================\n";
      std::cout << "\n";
      if (m_interactive) {
        std::cout << "Interactive (GUI) mode enabled\n";
      }
      else {
        std::cout << "For interactive (GUI) mode, set\n\n";
        std::cout << "    [Debug]\n";
        std::cout << "    chipscope_params = interactive\n";
        std::cout << "\nIn the sdx.ini file\n";
      }
      std::cout << "\n";

      try {
        capture_ila();
        shutdown_servers();
      }
      catch (std::exception &e) {
        valid = false;
        std::cout << e.what() << "\n";
        std::cout << "\n*** Aborted chipscope debug operation ***\n\n";
      }
      catch (...) {
        valid = false;
        std::cout << "Caught unknown exception\n";
        std::cout << "\n*** Aborted chipscope debug operation ***\n\n";
      }
    }

    // Ensure processes are killed before exiting program so our children
    // don't turn to zombies
    if (mp_interp)
      delete mp_interp;
    if (mp_vivado)
      delete mp_vivado;
    if (mp_xvcpcie)
      delete mp_xvcpcie;

    if (valid) {
      cleanup_working_directory();
      if (m_interactive) {
        launch_vivado_interactive();
      }
    }
  }


  const std::string LabtoolController::get_user_tcl_file() const
  {
    // Prefer the trigger file in the current directory to the one
    // in the system area... Allows user to iterate and change
    // the file.
    std::string triggerFile = "cs_trigger.tcl";
    bfs::path cwd = bfs::current_path();
    bfs::path tclPath;
    if (bfs::exists(cwd / triggerFile)) {
      tclPath = cwd / triggerFile;
    }
    else {
      bfs::path debugDir(get_xdp_debug_dir());
      tclPath = debugDir / "cs_trigger.tcl";
    }
    return tclPath.string();
  }


  const std::string LabtoolController::get_ltx_file() const
  {
    // TODO: IMPORTANT: ltx file needs to be the same base name as the xclbin
    // How do we know the name of the xclbin? If we can't figure that out,
    // a second option for 2018.3 is to just assume 1 ltx file.
    // See: CR-1011484
    // TODO: Maybe override ltx with an ini param
    //std::string ltxFile = "pfm_top_wrapper.ltx";
    std::string ltxFile;
    std::string ltxDir = ".";
    for (bfs::directory_iterator itr(ltxDir); itr!=bfs::directory_iterator(); ++itr) {
      if (is_regular_file(itr->status()) && (itr->path().extension() == ".ltx")) {
        // Just grab the first ltx we find for now...
        ltxFile = itr->path().string();
        break;
      }
    }
    if (ltxFile == "") {
      throw std::runtime_error("No ltx file found");
    }
    return ltxFile;
  }


  const std::string LabtoolController::get_working_dir() const
  {
    return workspace_root;
  }


  void LabtoolController::copy_user_tcl_template() const
  {
    // If the file already exists in the current directory, nothing to do...
    bfs::path cwd = bfs::current_path();
    bfs::path currentLocation = get_user_tcl_file();
    if (currentLocation.parent_path() != cwd) {
      copy_to_dir(get_user_tcl_file(), cwd.string());
    }
    else {
      std::cout << "Reusing trigger tcl file: " << get_user_tcl_file() << "\n";
    }
  }


  void LabtoolController::process_params()
  {
    // TODO: Set run modes based on optional_ini_parameters
    if (optional_ini_parameters == "interactive")
      m_interactive = true;
    // TODO: get timeout value
  }


  void LabtoolController::verify_tools_installed_or_error()
  {
    // Ensures that the correct tools are installed on this system.
    // Throw an exception if we are missing 2018.3 vivado and vivado_lab
    // Throw an exception if we are missing 2018.3 xvc_pcie
    // TODO: Implement Me

    find_set_vivado_exe();
    find_set_xvc_pcie();

    std::cout << "\n";
    std::cout << "server script    : " << get_server_tcl_file() << "\n";
    std::cout << "client script    : " << get_client_tcl_file() << "\n";
    std::cout << "user trigger file: " << get_user_tcl_file() << "\n";
    std::cout << "ltx file         : " << get_ltx_file() << "\n";
    std::cout << "vivado           : " << vivado_exe << "\n";
    std::cout << "xvc_pcie         : " << xvc_pcie_exe << "\n";
    std::cout << "kernel driver    : " << get_xvc_driver(driver_instance) << "\n";
    std::cout << "\n";
  }


  void LabtoolController::setup_working_directory()
  {
    std::string working_dir = get_working_dir();

    // TODO: check filename of this directory
    if (bfs::exists(working_dir)) {
      bfs::remove_all(working_dir);
    }

    if (! bfs::exists(working_dir)) {
      bfs::create_directory(working_dir);
    }
    std::cout << "\nOutput directory is: " << working_dir << "\n\n";
    // For simplicity, we copy files into the working directory where
    // vivado and xvc_pcie will be run. This keeps a user from accidentally
    // overwriting a file in use
    std::cout << "Copying intermediate files to working directory\n";
    copy_to_dir(get_ltx_file(), working_dir);
    copy_to_dir(get_user_tcl_file(), working_dir);
  }

  void LabtoolController::launch_xvc_pcie()
  {
    // TODO: Throw exception if port in use
    std::vector<std::string> args = {
      "-s", std::string("TCP::")+std::to_string(xvc_pcie_port),
      "-d", get_xvc_driver(driver_instance)
    };
    mp_xvcpcie = new BackgroundProcess(xvc_pcie_exe, args);
    mp_xvcpcie->setDir(get_working_dir());
    std::string log = "xvc_pcie_" + std::to_string(driver_instance) + ".log";
    mp_xvcpcie->setLog(log);
    std::cout << "Launching xvc_pcie server in background...\n";
    mp_xvcpcie->start();
  }

  void LabtoolController::launch_vivado()
  {
    // Changes to the given dir before launch.
    // Assumes that ltx and tcl files are already copied into the working dir.

    int hws_port = 3121;

    std::vector<std::string> args;
    args.push_back("-source");
    args.push_back(get_server_tcl_file());
    args.push_back("-mode");
    args.push_back("tcl");
    args.push_back("-tclargs");
    args.push_back("project_1");
    std::string ltxFile = base_filename(get_ltx_file());
    args.push_back(ltxFile);
    args.push_back("localhost");
    args.push_back(std::to_string(xvc_pcie_port));
    args.push_back(std::to_string(hws_port));

    mp_vivado = new BackgroundProcess(vivado_exe, args);
    mp_vivado->setDir(get_working_dir());
    std::string log = "vivado_log.out";
    mp_vivado->setLog(log);
    std::cout << "Launching vivado server in background...\n";
    mp_vivado->start();
  }

  void LabtoolController::launch_vivado_interactive()
  {
    std::vector<std::string> args = {
        "-source", get_viewwave_tcl_file(),
        "-tclargs", "waveform.ila"
    };
    BackgroundProcess p(vivado_exe, args);
    p.setDir(get_working_dir());
    std::string log = "vivado_interactive_log.out";
    p.setLog(log);
    std::cout << "\nLaunching vivado GUI to view captured wave...\n";
    p.start();
    p.wait();
  }

  void LabtoolController::wait_until_ready()
  {
    std::cout << "\nWaiting for vivado server process to come online...";
    bool ready = false;
    for (unsigned i = 0; i < m_timeout; ++i) {
      int result = mp_interp->exec_tcl("ready localhost");
      if (result == TCL_OK) {
        ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::cout << "." << std::flush;
    }
    std::cout << "\n";
    if (!ready) {
      if (mp_vivado) {
        delete mp_vivado;
        mp_vivado = nullptr;
      }
      if (mp_xvcpcie) {
        delete mp_xvcpcie;
        mp_xvcpcie = nullptr;
      }
      throw std::runtime_error("Timeout while waiting for vivado process communication");
    }
  }


  void LabtoolController::arm_ila_trigger()
  {
    int result = mp_interp->exec_tcl("run_ila localhost");
    if (result != TCL_OK) {
      throw std::runtime_error("Error during run_ila");
    }
  }


  void LabtoolController::capture_ila()
  {
    int result = mp_interp->exec_tcl("capture_ila localhost");
    if (result != TCL_OK) {
      throw std::runtime_error("Error during capture_ila");
    }
  }


  void LabtoolController::shutdown_servers()
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Shutting down vivado background process...\n";
    mp_interp->exec_tcl("close localhost");
    if (mp_vivado) {
      mp_vivado->end();
    }
    if (mp_xvcpcie) {
      std::cout << "Shutting down xvc_pcie background process...\n";
      mp_xvcpcie->end();
    }
  }


  void LabtoolController::cleanup_working_directory()
  {
    // Copy waveform.ila file to the working directory
    // Delete the working directory
    // TODO: Implement me
  }

  bool LabtoolController::find_set_vivado_exe()
  {
    if(find_set_vivado_lab()) {
        return true;
    }
    if(find_set_vivado()) {
        return true;
    }
    if(!vivado_lab_available && !vivado_available) {
      throw std::runtime_error("Neither of Vivado and Vivado Lab is available");
    }
    return false;
  }

  bool LabtoolController::find_set_vivado_lab()
  {
    if(vivado_lab_location.empty()) {
        vivado_lab_available = false;
        return vivado_lab_available;
    }

    std::string vivado_lab_tool = vivado_lab_location + "/vivado_lab";
    if (!bfs::exists(vivado_lab_tool)) {
        vivado_lab_available = false;
        return vivado_lab_available;
    }
    vivado_exe_path = vivado_lab_location;
    vivado_exe = vivado_lab_tool;
    vivado_lab_available = true;
    return vivado_lab_available;
  }

  bool LabtoolController::find_set_vivado()
  {
    std::string vivado_env(getenv("XILINX_VIVADO"));
    if(vivado_env.empty()) {
        vivado_available = false;
        return false;
    }

    std::string vivado_tool = vivado_env + "/bin/vivado";
    if(!bfs::exists(vivado_tool)) {
        vivado_available = false;
        return false;
    }
    vivado_exe_path = vivado_env + "/bin";
    vivado_exe = vivado_tool;
    vivado_available = true;
    return vivado_available;
  }

  bool LabtoolController::find_set_xvc_pcie()
  {
    if(vivado_exe_path.empty()) {
        xvc_pcie_available = false;
        return false;
    }

    std::string xvc_pcie_path = vivado_exe_path + "/xvc_pcie";
    if(!bfs::exists(xvc_pcie_path)) {
        xvc_pcie_available = false;
        throw std::runtime_error("XVC PCIe server not available");
    }

    xvc_pcie_available = true;
    xvc_pcie_exe = xvc_pcie_path;
    return true;
  }

  bool LabtoolController::check_vivado_lab_availability() {
    return true;
  }

  bool LabtoolController::check_vivado_availability() {
    return true;
  }

  bool LabtoolController::check_xvc_pcie_availability() {
    return true;
  }


}
