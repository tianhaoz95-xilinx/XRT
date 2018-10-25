#include "xcl_macros.h"

//----------xclPerfMonReadCounters------------
#define xclPerfMonReadCounters_SET_PROTOMESSAGE_AWS() \
    if(simulator_started == false) \
    {\
      RELEASE_MUTEX();\
      return 0; \
    }\
    c_msg.set_slotname(slotname); \
    c_msg.set_accel(accel); \

#define xclPerfMonReadCounters_SET_PROTO_RESPONSE_AWS() \
    wr_byte_count    = r_msg.wr_byte_count(); \
    wr_trans_count   = r_msg.wr_trans_count(); \
    total_wr_latency = r_msg.total_wr_latency(); \
    rd_byte_count    = r_msg.rd_byte_count(); \
    rd_trans_count   = r_msg.rd_trans_count(); \
    total_rd_latency = r_msg.total_rd_latency();


#define xclPerfMonReadCounters_RETURN_AWS()

#define xclPerfMonReadCounters_RPC_CALL_AWS(func_name,wr_byte_count,wr_trans_count,total_wr_latency,rd_byte_count,rd_trans_count,total_rd_latency,sampleIntervalUsec,slotname,accel) \
    RPC_PROLOGUE(func_name); \
    xclPerfMonReadCounters_SET_PROTOMESSAGE_AWS(); \
    SERIALIZE_AND_SEND_MSG(func_name)\
    xclPerfMonReadCounters_SET_PROTO_RESPONSE_AWS(); \
    FREE_BUFFERS(); \
    xclPerfMonReadCounters_RETURN_AWS();

//----------xclPerfMonGetTraceCount------------
#define xclPerfMonGetTraceCount_SET_PROTOMESSAGE_AWS() \
    if(simulator_started == false) \
    {\
      RELEASE_MUTEX();\
      return 0; \
    }\
  c_msg.set_ack(ack); \
  c_msg.set_slotname(slotname); \
  c_msg.set_accel(accel);

#define xclPerfMonGetTraceCount_SET_PROTO_RESPONSE_AWS() \
    no_of_samples = r_msg.no_of_samples();


#define xclPerfMonGetTraceCount_RPC_CALL_AWS(func_name,ack,no_of_samples,slotname,accel) \
    RPC_PROLOGUE(func_name); \
    xclPerfMonGetTraceCount_SET_PROTOMESSAGE_AWS(); \
    SERIALIZE_AND_SEND_MSG(func_name)\
    xclPerfMonGetTraceCount_SET_PROTO_RESPONSE_AWS(); \
    FREE_BUFFERS();

//----------xclPerfMonReadTrace------------
#define xclPerfMonReadTrace_SET_PROTOMESSAGE_AWS() \
    if(simulator_started == false) \
    {\
      RELEASE_MUTEX();\
      return 0; \
    }\
    c_msg.set_ack(ack); \
    c_msg.set_slotname(slotname); \
    c_msg.set_accel(accel);

#define xclPerfMonReadTrace_SET_PROTO_RESPONSE_AWS() \
    samplessize = r_msg.output_data_size(); \

#define xclPerfMonReadTrace_RPC_CALL_AWS(func_name,ack,samplessize,slotname,accel) \
    RPC_PROLOGUE(func_name); \
    xclPerfMonReadTrace_SET_PROTOMESSAGE_AWS(); \
    SERIALIZE_AND_SEND_MSG(func_name)\
    xclPerfMonReadTrace_SET_PROTO_RESPONSE_AWS(); \
    FREE_BUFFERS();



