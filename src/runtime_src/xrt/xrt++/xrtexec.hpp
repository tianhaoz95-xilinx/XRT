/**
 * Copyright (C) 2019 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

////////////////////////////////////////////////////////////////
// This is experimental code, subject to disappear without warning
////////////////////////////////////////////////////////////////

#ifndef _XRT_XRTEXEC_H_
#define _XRT_XRTEXEC_H_
#include <memory>
#include "ert.h"

struct xrt_device;

namespace xrtcpp {

namespace exec {

using value_type = uint32_t;
using addr_type = uint32_t;

/**
 * class command : abstraction for commands executed by XRT
 */
class command
{
  struct impl;
protected:
  std::shared_ptr<impl> m_impl;

  command()
    : m_impl(0)
  {}

  command(xrt_device* dev, ert_cmd_opcode opcode);

public:
  void
  execute();

  void
  wait();

  bool
  completed() const;
};

/**
 * class write_command : concrete class for ERT_WRITE
 *
 * The write command allows XRT to values to specific
 * addresses exposed over AXI-lite.
 */
class write_command : public command
{
public:
  write_command(xrt_device* dev);

  /**
   * Add {addr,value} pair to the command
   *
   * @addr: the address that will be written with @value
   * @value: the value to write to @addr
   */
  void
  add(addr_type addr, value_type value);

private:
  size_t offset = 1;
};

}} //exec, xrtcpp
#endif
