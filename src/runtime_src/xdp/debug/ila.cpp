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


// NOTE TO JAKE: 
// I'm not sure if <tcl.h> will be allowed (inclusion of Tcl interpreter)
// We may need to c++ify the code that currently is using Tcl in this file.
// This causes inclusion of -ltcl in the makefiles, which could be a problem when
// we start supporting windows. Also check this on Ubuntu...
//
#include <tcl.h>
//~~~~~~~~~~~~~~

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
  
  
  const std::string get_vivado_cmd()
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
    // TODO: Use bfs::copy_file (but not currently working - need cmake update)
    if (!bfs::exists(srcFile)) 
      throw std::runtime_error("copy_to_dir: file '" + srcFile + "' not found");
    bfs::path dstPath = bfs::path(dstDir) / base_filename(srcFile);
    std::string dstFile = dstPath.string();
    std::ifstream src(srcFile, std::ios::binary);
    std::ofstream dst(dstFile, std::ios::binary);
    dst << src.rdbuf();
    src.close();
    dst.close();
    std::cout << "copy " << srcFile << " -> " << dstFile << "\n";
  }


  // TODO: DEPENDENCY ALERT - 
  // This is code for getting a demo up and running more quickly. 
  // Unless we can embed a tcl interpreter in the xdp code, we need to c++ify 
  // this code before checkin...
  Tcl_Interp *myinterp = nullptr;

  void create_interp()
  {
    if (myinterp == nullptr) {
      myinterp = Tcl_CreateInterp();
      std::string tclCmd = "source " + get_client_tcl_file();
      int result = Tcl_Eval(myinterp, tclCmd.c_str());
      if (result != TCL_OK) {
        throw std::runtime_error("Could not create Tcl interpreter");
      }
    }
  }

  void delete_interp()
  {
    if (myinterp) {
      Tcl_DeleteInterp(myinterp);
    }
  }

  int run_tcl_cmd(const std::string& cmd)
  {
    return Tcl_Eval(myinterp, cmd.c_str());
  }

}


namespace XCL
{
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
  //       p.end(); <-- optional, called in destructor
  //
  //  TODO: Error and exception handler for bad return values
  //  TODO: isRunning() still needs some work for a process that ends early
  //  TODO: need to kill process group (killpg, setpgrp, setsid). I am still getting
  //        lingering processes after exit sometimes.
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
            chdir(m_dir.c_str());
          }

          // Stdout/Stderr log file - we log results from the child to a file
          // if requested
          if (m_logFile != "") {
            int fd = open(m_logFile.c_str(), \
                          O_WRONLY | O_TRUNC | O_CREAT, \
                          S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR );
            if (fd == -1) {
              // TODO: Throw exception - could not open stdout/stderr log file
              std::cout << "ERROR: Could not open log" << std::endl;
              return;
            } 
            else {
              if (dup2(fd,1) == -1) {
                // TODO: Throw exception: Could not redirect stdout
                std::cout << "ERROR: Could not redirect stdout" << std::endl;
                return;
              }
              if (dup2(fd,2) == -1) {
                // TODO: Throw exception: Could not redirect stderr
                std::cout << "ERROR: Could not redirect stderr" << std::endl;
                return;
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

          int retval = execv(p_cmd, p_args);
          // Never should get here... 
          // TODO: throw exception or something
          return;
        }
        else {
          // If we get here we are a happy parent of a healthy new child
        }
      }

      void end(int sig = 9)
      {
        // Maybe there is a more gentle way. But this works...
        // 2 = SIGINT (ctrl-c)
        // 9 = SIGKILL
        if (m_pid > 0) {
          int retval = kill(m_pid, sig);
          m_pid = 0;
        }
      }
      
      void wait()
      {
        int status;
        waitpid(m_pid, &status, 0);
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
    , mp_vivado(nullptr)
    , mp_xvcpcie(nullptr)
    , m_timeout(60)
    , m_interactive(true)
  {
  }


  void LabtoolController::init(std::string& workspace, unsigned port, 
                               unsigned instance, std::string& optional) 
  {
    workspace_root = workspace;
    xvc_pcie_port = port;
    driver_instance = instance;
    optional_ini_parameters = optional;
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
    std::cout << "==        CHIPSCOPE DEBUG FLOW ENABLED        ==\n";
    std::cout << "================================================\n";
    std::cout << "\n";
    std::cout << "chipscope_flow enabled in sdx.ini\n";
    std::cout << "\n";
      
//    std::cout << "launch labtool in: " << workspace_root << std::endl;
//    std::cout << "\twith port: " << xvc_pcie_port << std::endl;
//    std::cout << "\twith mgmt_instance: " << driver_instance << std::endl;
//    std::cout << "\tand optional argument: " << optional_ini_parameters << std::endl;

    try {
      verify_tools_installed_or_error();
      copy_user_tcl_template();
      create_interp();
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
      std::cout << "==     CHIPSCOPE DEBUG FLOW POST PROCESS      ==\n";
      std::cout << "================================================\n";
      std::cout << "\n";
      if (m_interactive) {
        std::cout << "  Interactive (GUI) mode enabled\n";
      }
      else {
        std::cout << "For interactive (GUI) mode, set\n";
        std::cout << "    [Debug]\n";
        std::cout << "    chipscope_params = interactive\n";
        std::cout << "In the sdx.ini file\n";
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
    delete_interp();
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
    // TODO: This needs to be calculated as the same base name as the xclbin
    const std::string ltxFile = "pfm_top_wrapper.ltx";
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


  void LabtoolController::verify_tools_installed_or_error() const
  {
    // Ensures that the correct tools are installed on this system. 
    // Throw an exception if we are missing 2018.3 vivado and vivado_lab
    // Throw an exception if we are missing 2018.3 xvc_pcie
    // TODO: Implement Me
    std::cout << "\n";
    std::cout << "server script    : " << get_server_tcl_file() << "\n";
    std::cout << "client script    : " << get_client_tcl_file() << "\n";
    std::cout << "user trigger file: " << get_user_tcl_file() << "\n";
    std::cout << "vivado           : " << get_vivado_cmd() << "\n";
    std::cout << "xvc_pcie         : " << get_xvc_pcie_cmd() << "\n";
    std::cout << "kernel driver    : " << get_xvc_driver(driver_instance) << "\n";
    std::cout << "\n";
  }


  void LabtoolController::setup_working_directory()
  {
    // TODO: Maybe need the PID here - i don't think this filename is correct...
    if (! bfs::exists(workspace_root)) {
      bfs::create_directory(workspace_root);
    }
    // For simplicity, we copy files into the working directory where
    // vivado and xvc_pcie will be run. This keeps a user from accidentally
    // overwriting a file in use
    copy_to_dir(get_ltx_file(), workspace_root);
    copy_to_dir(get_user_tcl_file(), workspace_root);
  }


  void LabtoolController::launch_xvc_pcie()
  {
    // TODO: Throw exception if port in use
    std::vector<std::string> args = {
      "-s", std::string("TCP::")+std::to_string(xvc_pcie_port),
      "-d", get_xvc_driver(driver_instance)
    };
    mp_xvcpcie = new BackgroundProcess(get_xvc_pcie_cmd(), args);
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
    const std::string cmd = get_vivado_cmd();
    const std::string project = "project_1";
    int hws_port = 3121;  
    std::string host = "localhost";
    
    std::vector<std::string> args;
    args.push_back("-source");
    std::string tclFile = get_server_tcl_file();
    args.push_back(tclFile);
    args.push_back("-mode");
    args.push_back("tcl");
    args.push_back("-tclargs");
    args.push_back(project);
    std::string ltxFile = base_filename(get_ltx_file());
    args.push_back(ltxFile);
    args.push_back(host);
    args.push_back(std::to_string(xvc_pcie_port));
    args.push_back(std::to_string(hws_port));

    mp_vivado = new BackgroundProcess(cmd, args);
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
    BackgroundProcess p(get_vivado_cmd(), args);
    p.setDir(get_working_dir());
    std::string log = "vivado_log.out";
    p.setLog(log);
    std::cout << "\nLaunching vivado GUI to view captured wave...\n";
    p.start();
    p.wait();
  }


  void LabtoolController::wait_until_ready()
  {
    std::cout << "\nWaiting for vivado server process to come online...";
    bool ready = false;
    for (int i = 0; i < m_timeout; ++i) {
      int result = run_tcl_cmd("ready localhost");
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
    int result = run_tcl_cmd("run_ila localhost");
    if (result != TCL_OK) {
      throw std::runtime_error("Error during run_ila");
    }
  }

  
  void LabtoolController::capture_ila()
  {
    int result = run_tcl_cmd("capture_ila localhost");
    if (result != TCL_OK) {
      throw std::runtime_error("Error during capture_ila");
    }
  }


  void LabtoolController::shutdown_servers()
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "Shutting down vivado background process...\n";
    run_tcl_cmd("close localhost");
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

}

