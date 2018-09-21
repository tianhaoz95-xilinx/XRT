#ifndef __XILINX_ILA
#define __XILINX_ILA

#include <iostream>

namespace XCL
{

  class LabtoolController {
  public:
    LabtoolController(std::string& ID_init) : ID(ID_init) {};
    void init(std::string& workspace, unsigned port, unsigned instance, std::string& optional);
    std::string getID();
    void launch();
    void finish();
    void cleanup();
  private:
    std::string ID;
    std::string workspace_root;
    unsigned hardware_server_port;
    unsigned driver_instance;
    std::string optional_ini_parameters;
  };

}

#endif
