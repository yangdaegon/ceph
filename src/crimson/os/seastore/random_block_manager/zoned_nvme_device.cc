// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <sys/stat.h>

#include "crimson/common/log.h"
#include "include/buffer.h"

#include "zoned_nvme_device.h"

#define ZONE_SIZE 96*1024*1024

namespace crimson::os::seastore::nvme_device {
  nvme_io_command_t ZonedNVMeDevice::_create_append_cmd(
  uint32_t zone, bufferptr &bptr){
    nvme_io_command_t io_cmd;
    memset(&io_cmd, 0, sizeof(io_cmd));
    io_cmd.common.opcode = io_cmd.OPCODE_APPEND;
    io_cmd.common.nsid = 1; // nsid
    io_cmd.rw.s_lba = (zone * ZONE_SIZE) / block_size; // zslba
    /*if (flag == IORING_SETUP_IOPOLL){
      io_cmd.rw.fuse = 1;
    }*/
    io_cmd.common.addr = (uint64_t)bptr.c_str();
    io_cmd.common.data_len = bptr.length(); 
    return io_cmd;
  }
  append_ertr::future<uint64_t> ZonedNVMeDevice::append(
  uint32_t zone, bufferptr &bptr){
    nvme_io_command_t append_cmd = _create_append_cmd(zone, bptr);
    return IOUringNVMeDevice::uring_pass_through_io(append_cmd).safe_then(
    [](auto completion){
        uint64_t lba = completion->alba;
        delete completion;
        return seastar::make_ready_future<uint64_t>(lba);
        });
  }
}