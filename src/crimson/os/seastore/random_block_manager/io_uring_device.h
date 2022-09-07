//-*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <string.h>

#include "liburing.h"

#include "nvmedevice.h"

namespace ceph {
  namespace buffer {
    class bufferptr;
  }
}

namespace crimson::os::seastore::nvme_device {
class IOUringNVMeDevice : public NVMeBlockDevice {
  io_uring ring;
  static const unsigned QUEUE_DEPTH = 1024;
  struct block_uring_cmd {
    __u32   ioctl_cmd;
    __u32   unused1;
    __u64   unused2[4];
  };

  struct data_info {
    nvme_io_command_t io_cmd;
    uring_completion uring_cpl;
    bool done;
    int err;
  };
  int fd;
  bool isChardev = false;
  bool isBlockdev = false;
  int flag;

public:

  IOUringNVMeDevice() {}
  ~IOUringNVMeDevice() = default;

  open_ertr::future<> open(
    const std::string &in_path,
    seastar::open_flags mode) override;

  write_ertr::future<> write(
    uint64_t offset,
    bufferptr &bptr,
    uint16_t stream = 0) override;

  read_ertr::future<> read(
    uint64_t offset,
    bufferptr &bptr) override;

  seastar::future<> close() override;

  discard_ertr::future<> discard(
    uint64_t offset,
    uint64_t len) override{ return seastar::now();};

  nvme_command_ertr::future<int> pass_admin(
    nvme_admin_command_t& admin_cmd) override{ return seastar::make_ready_future<int>(1);};
  nvme_command_ertr::future<uring_completion*> uring_pass_through_io(
    nvme_io_command_t& io_cmd);

  virtual append_ertr::future<uint64_t> append(
    uint32_t zone, bufferptr &bptr) { return seastar::make_ready_future<uint64_t>(0); };

private:
  bool io_uring_supported() { return true; };
  static void _create_pass_through_command(
   io_uring_sqe *sqe, nvme_io_command_t& io_cmd, 
   int fd, data_info *di, block_uring_cmd *blk_cmd);  
  static void _initialize_data_info(data_info *di, nvme_io_command_t io_cmd);
  nvme_io_command_t _create_write_command(uint64_t offset, bufferptr &bptr);
  nvme_io_command_t _create_read_command(uint64_t offset, bufferptr &bptr);
};
}