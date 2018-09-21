#include "ila.h"

namespace XCL
{

  void LabtoolController::init(std::string& workspace, unsigned port, unsigned instance, std::string& optional) {
    workspace_root = workspace;
    hardware_server_port = port;
    driver_instance = instance;
    optional_ini_parameters = optional;
  }

  std::string LabtoolController::getID() {
    return ID;
  }

  void LabtoolController::cleanup() {
    return;
  }

  void LabtoolController::launch() {
    std::cout << "launch labtool in: " << workspace_root << std::endl;
    std::cout << "\twith port: " << hardware_server_port << std::endl;
    std::cout << "\twith mgmt_instance: " << driver_instance << std::endl;
    std::cout << "\tand optional argument: " << optional_ini_parameters << std::endl;
  }

}
