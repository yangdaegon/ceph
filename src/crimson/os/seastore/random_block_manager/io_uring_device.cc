// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <sys/stat.h>
#include <string.h>
using namespace std;

#include "crimson/common/log.h"
#include "include/buffer.h"

#include "io_uring_device.h"
#include "nvmedevice.h"

#define IORING_OP_URING_CMD 40 

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_filestore);
  }
}

namespace crimson::os::seastore::nvme_device {
  open_ertr::future<> IOUringNVMeDevice::open(
      const std::string& path,
      seastar::open_flags mode) {

    if (io_uring_supported() == false) {
      logger().error("open: io uring is not supported");
      return crimson::ct_error::input_output_error::make();
    }
    flag = 0;
    // Submission queue poll
    // With SQPOLL, kernel thread polls submisison queue and submit commands
    // accumulated while polling period. Without SQPOLL, Every command submission
    // incurs interrupt to device. This feature is for submitting command to
    // device with less interrupts.

    //flag |= IORING_SETUP_SQPOLL;

    // IO poll
    // With IOPOLL, user should poll completion queue for command completion
    // instead of getting completion from interrupt. Without IOPOLL, every command
    // completion incurs interrupt from device to host. This feature is for
    // command completions with less interrupts.

    //flag |= IORING_SETUP_IOPOLL;
    int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, flag);
    if (ret != 0) {
      logger().error("open: io uring queue is not initilized");
      return crimson::ct_error::input_output_error::make();
    }
    // IOUring library requires POSIX file descriptor which is not provided by
    // Seastar library. Therefore, open block device via open system call to get
    // POSIX file descriptor.
    string isCdev = "ng";
    string isBdev = "nvme";
    // For Zoned Namespace SSDs which are not supported for block devices(nvme#n#), 
    // they would be described as character devices like "ng#n#"
    if( strstr(path.c_str(), isCdev.c_str()) != NULL){
	isChardev = true;
    } else if( strstr(path.c_str(), isBdev.c_str()) != NULL){
	isBlockdev = true;
    }
    fd = ::open(path.c_str(), static_cast<int>(mode));
    if (fd < 0) {
      logger().error("open: device open failed");
      return crimson::ct_error::input_output_error::make();
    }
    struct stat stat;
    ret = fstat(fd, &stat);
    if (ret < 0) {
      logger().error("open: fstat failed");
      return crimson::ct_error::input_output_error::make();
    }
    block_size = stat.st_blksize;
    size = stat.st_size;
    ret = io_uring_register_files(&ring, &fd, 1);
    if (ret != 0) {
      logger().error("open: linking device file to io uring is failed");
      return crimson::ct_error::input_output_error::make();
    }
    return open_ertr::now();
  }

  seastar::future<> IOUringNVMeDevice::close() {
    io_uring_unregister_files(&ring);
    io_uring_queue_exit(&ring);
    ::close(fd);
    return seastar::now();
  }

  void IOUringNVMeDevice::_create_pass_through_command(
  io_uring_sqe *sqe, nvme_io_command_t& io_cmd, 
  int fd, data_info *di, block_uring_cmd *blk_cmd) { 
    sqe->opcode = IORING_OP_URING_CMD;
    // To use passthrough command with iouring, you need IORING_OP_URING_CMD opcode
    // until now, it's only suppported on kernel v5.15.0
    // but it would be supported on new released version of kernel                           
    sqe->addr = 4;                                               
    sqe->len = io_cmd.common.data_len;                           
    sqe->off = reinterpret_cast<unsigned long>(di);              
    sqe->flags = 0;                                              
    sqe->ioprio = 0;                                             
    sqe->user_data = reinterpret_cast<uint64_t>(di);             
    sqe->fd = fd;                                                
    sqe->rw_flags = 0;                                           
    sqe->__pad2[0] = sqe->__pad2[1] = sqe->__pad2[2] =0;         
    blk_cmd = reinterpret_cast<block_uring_cmd *>(&sqe->len);    
#ifdef NVME_IOCTL_IO64_CMD                                       
    blk_cmd->ioctl_cmd = NVME_IOCTL_IO64_CMD;                    
#else                                                            
    blk_cmd->ioctl_cmd = NVME_IOCTL_IO_CMD;                      
#endif
  } 

  nvme_io_command_t IOUringNVMeDevice::_create_write_command(
  uint64_t offset, bufferptr &bptr){
    nvme_io_command_t io_cmd;
    memset(&io_cmd, 0, sizeof(io_cmd));
    io_cmd.common.opcode = io_cmd.OPCODE_WRITE;
    io_cmd.common.nsid = 1;
    if (flag == IORING_SETUP_IOPOLL){
      io_cmd.rw.fuse = 1;
    }
    io_cmd.rw.s_lba = offset / block_size;
    io_cmd.rw.nlb = 0; //zero-based
    io_cmd.common.addr = (uint64_t)bptr.c_str();
    io_cmd.common.data_len = bptr.length();

    return io_cmd;
  }

  nvme_io_command_t IOUringNVMeDevice::_create_read_command(
  uint64_t offset, bufferptr &bptr){
    nvme_io_command_t io_cmd;
    memset(&io_cmd, 0, sizeof(io_cmd));
    io_cmd.common.opcode = io_cmd.OPCODE_READ; 
    io_cmd.common.nsid = 1;
    if (flag == IORING_SETUP_IOPOLL){
      io_cmd.rw.fuse = 1;
    }
    io_cmd.rw.s_lba = offset / block_size;
    io_cmd.rw.nlb = 0; //zero-based
    io_cmd.common.addr = (uint64_t)bptr.c_str();
    io_cmd.common.data_len = bptr.length();

    return io_cmd;
  }

  void IOUringNVMeDevice::_initialize_data_info(
  data_info *di, nvme_io_command_t io_cmd){
    uring_completion ucpl;
    memset(&ucpl, 0, sizeof(ucpl));
    di->io_cmd = io_cmd;
    di->done = false;
    di->uring_cpl = ucpl; 
    di->err = 0;
  }

  write_ertr::future<> IOUringNVMeDevice::write(                                                               
      uint64_t offset, bufferptr &bptr, uint16_t stream) {                                                                      
    logger().debug("Block: write offset {} len {}", offset, bptr.length());                                   
    struct block_uring_cmd  *blk_cmd = NULL;
    struct io_uring_sqe *sqe = NULL;                                                                         
    int ret = 0;                                                                                             
    sqe = io_uring_get_sqe(&ring);                                                                           
    if (!sqe){                                                                                               
      logger().error("FAILED: io_uring_get_sqe");                                                          
      return crimson::ct_error::input_output_error::make();                                                
    }                  
    nvme_io_command_t io_cmd = _create_write_command(offset, bptr);
    data_info *di = new data_info();
    _initialize_data_info(di, io_cmd);
    if (isBlockdev) { // if block device(ex. nvme0n1)              
      io_uring_prep_write(sqe, fd, bptr.c_str(), bptr.length(), offset);                                                           
      sqe->user_data = reinterpret_cast<uint64_t>(di);
    } else if (isChardev) { // else if char device(ex. ng0n1)
      _create_pass_through_command(sqe, io_cmd, fd, di, blk_cmd);                                                          
    } else {
      return crimson::ct_error::input_output_error::make();                                                
    }
    ret = io_uring_submit(&ring);                                                                            
    if (ret < 0){                                                                                            
      logger().error("IOUringNVMeDevice::write: \
      submit io_uring submission queue failed ({})", ret);
      return crimson::ct_error::input_output_error::make();                                                
    }                                                                                                        
    return seastar::do_until(
    [di](){
      if(di->done == true){
        return true;
      }
      return false;
    },
    [this, di](){
      struct io_uring_cqe *cqe;
      int ret = 0;
      ret = io_uring_peek_cqe(&ring, &cqe); 
      // To use iouring_write, flag should be 0                                                                    
      if(ret == -EAGAIN){
        return seastar::later();
      }
      if (ret < 0){                                                                                            
        logger().error("IOUringNVMeDevice::write: \
        wait io_uring completion queue failed ({})", ret);
        di->uring_cpl.err = ret;
        di->done = true;
        return seastar::now();
      }
      data_info *completed_di = reinterpret_cast<data_info *>(io_uring_cqe_get_data(cqe));
      if (cqe->res < 0){                                                                             
        logger().error("IOUringNVMeDevice::write: \
        get data from completion queue failed ({})", strerror(-cqe->res));
        completed_di->uring_cpl.err  = cqe->res;
      }                                                                                                        
      completed_di->done = true;
      io_uring_cqe_seen(&ring, cqe);                                                                           
      return seastar::now();
    }
    ).then([di](){
      delete di;
      return write_ertr::now();});
    }

  read_ertr::future<> IOUringNVMeDevice::read(                                                               
      uint64_t offset, bufferptr &bptr) {                                                                      
    logger().debug("Block: read offset {} len {}", offset, bptr.length());                                   
    struct block_uring_cmd  *blk_cmd = NULL;
    struct io_uring_sqe *sqe = NULL;                                                                         
    int ret = 0;                                                                                             
    sqe = io_uring_get_sqe(&ring);                                                                           
    if (!sqe){                                                                                               
      logger().error("FAILED: io_uring_get_sqe");                                                          
      return crimson::ct_error::input_output_error::make();                                                
    }                             
    nvme_io_command_t io_cmd = _create_read_command(offset, bptr);
    data_info *di = new data_info();
    _initialize_data_info(di, io_cmd);
    if (isBlockdev) { // if block device(ex. nvme0n1)              
      io_uring_prep_read(sqe, fd, bptr.c_str(), bptr.length(), offset);                                                           
      sqe->user_data = reinterpret_cast<uint64_t>(di);
    } else if (isChardev) { // else if char device(ex. ng0n1)
      _create_pass_through_command(sqe, io_cmd, fd, di, blk_cmd);                                                          
    } else {
      return crimson::ct_error::input_output_error::make();                                                
    }
    ret = io_uring_submit(&ring);                                                                            
    if (ret < 0){                                                                                            
      logger().error("IOUringNVMeDevice::read: \
      submit io_uring submission queue failed ({})", ret);
      return crimson::ct_error::input_output_error::make();                                                
    }                                                                                                        
    return seastar::do_until(
        [di](){
        if(di->done == true){
          return true;
        }
        return false;
        },
        [this, di](){
        struct io_uring_cqe *cqe;
        int ret = 0;
        ret = io_uring_peek_cqe(&ring, &cqe); 
        // To use iouring_read, flag should be 0                                                                   
        if(ret == -EAGAIN){
          return seastar::later();
        }
        if (ret < 0){                                                                                            
          logger().error("IOUringNVMeDevice::read: \
          wait io_uring completion queue failed ({})", ret);
          di->uring_cpl.err = ret;
          di->done = true;
          return seastar::now();                                                
        }
        data_info *completed_di = reinterpret_cast<data_info *>(io_uring_cqe_get_data(cqe));
        if (cqe->res < 0){                                                                             
          logger().error("IOUringNVMeDevice::read: \
          get data from completion queue failed ({})", strerror(-cqe->res));
          completed_di->uring_cpl.err  = cqe->res;
        }                                                                                                        
        completed_di->done = true;
        io_uring_cqe_seen(&ring, cqe);                                                                           
        return seastar::now();
        }
    ).then([di](){
      delete di;
      return read_ertr::now();});
  }

  nvme_command_ertr::future<uring_completion*> IOUringNVMeDevice::uring_pass_through_io(
    nvme_io_command_t& io_cmd) {
    struct io_uring_sqe *sqe = NULL;
    struct block_uring_cmd *blk_cmd = NULL;
    int ret =0;
    sqe = io_uring_get_sqe(&ring);
    memset(sqe, 0, sizeof(*sqe));
    if (!sqe){
      logger().error("uring_pass_through_io: \
      getting the io_uring submission queue is failed ({})", ret);
      return crimson::ct_error::input_output_error::make();
    }
    data_info *di = new data_info();
    _initialize_data_info(di, io_cmd);
    if (isBlockdev) { // if block device(ex. nvme0n1)              
      logger().error("uring_pass_through_io: \
      this function only supports CharDevice now :({})", EOPNOTSUPP);
      di->uring_cpl.err = EOPNOTSUPP;
      return seastar::make_exception_future<uring_completion*>(di->uring_cpl);
    } else if (isChardev) { // else if char device(ex. ng0n1)
      _create_pass_through_command(sqe, io_cmd, fd, di, blk_cmd);                                                          
    }
    ret = io_uring_submit(&ring);
    if(ret < 0){
      logger().error("uring_pass_through_io: \
      submitting io_uring submission queue is failed ({})", ret);
      di->uring_cpl.err = ret;
      return seastar::make_exception_future<uring_completion*>(di->uring_cpl);
    }
    return seastar::do_until(
        [di]() {   
          if (di->done == true){
            return true;
          }
          return false;
        },
        [this, di]() {
          struct io_uring_cqe *cqe;
          int ret = 0;
          if (flag == IORING_SETUP_IOPOLL){
            ret = io_uring_wait_cqe(&ring, &cqe);
          }else if (flag == 0){
            ret = io_uring_peek_cqe(&ring, &cqe);
          }
          if(ret == -EAGAIN){
            return seastar::later();
          }
          if(ret < 0){
            logger().error("uring_pass_through_io: \
            waiting io_uring completion queue is failed ({})", ret);
            di->uring_cpl.err = ret;
            di->done = true;
            return seastar::now();
          }
          data_info *completed_di = reinterpret_cast<data_info *>(io_uring_cqe_get_data(cqe));
          if(cqe->res < 0){
            logger().error("uring_pass_through_io: \
            getting data from completion queue is failed ({})", strerror(-cqe->res));
            completed_di->uring_cpl.err = cqe->res;
          }
          completed_di->done = true;
          completed_di->uring_cpl.alba = completed_di->io_cmd.common.result;
          io_uring_cqe_seen(&ring, cqe);
          return seastar::now(); 
        }
    ).then([di]() {
      uring_completion *cpl_ptr = new uring_completion;
      cpl_ptr->alba = di->uring_cpl.alba;
      cpl_ptr->err = di->uring_cpl.err;
      delete di;
      return seastar::make_ready_future<uring_completion*>(cpl_ptr);
    });
  }
}