//-*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "io_uring_device.h"

namespace crimson::os::seastore::nvme_device {
class ZonedNVMeDevice : public IOUringNVMeDevice {
  size_t zone_size = 0;
  uint32_t nr_zones = 0;
  int fd;

public:
  ZonedNVMeDevice() {}
  ~ZonedNVMeDevice() = default;

   append_ertr::future<uint64_t> append(
   uint32_t zone, bufferptr &bptr) override;

private:
  nvme_io_command_t _create_append_cmd(uint32_t zone, bufferptr &bptr);

};
}