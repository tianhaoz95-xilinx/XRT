#ifndef _OCL_HELP_H_
#define _OCL_HELP_H_

#include <CL/cl.h>

struct oclHardware {
    cl_platform_id mPlatform;
    cl_context mContext;
    cl_device_id mDevice;
    cl_command_queue mQueue;
};

struct oclSoftware {
    cl_program mProgram;
    cl_kernel mKernel;
    char mKernelName[128];
    char mFileName[1024];
    char mCompileOptions[1024];
};

oclHardware getOclHardware(cl_device_type type);

int getOclSoftware(oclSoftware &soft, const oclHardware &hardware);

const char *oclErrorCode(cl_int code);

#endif
