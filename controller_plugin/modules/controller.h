// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_MODULES_QUEUE_H_
#define BESS_MODULES_QUEUE_H_
#define FILENAME_LEN			(6)

#include "../kmod/llring.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "utils/bits.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/udp.h"
#include "utils/exact_match_table.h"
#include "utils/cuckoo_map.h"

#include "pb/controller_msg.pb.h"

using bess::utils::Error;

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::be64_t;
using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;


class Controller : public Module {
  struct RecverState {/* the state variable related to the receiver machine */

    uint8_t data_id;

    /* updated by the zero packet */
    int64_t data_size;		// number of bytes in the data


    /* file descripter for writing the files */
    FILE * fd_p;		// file descriptor for writing from the slow path

    int32_t num_recv_ed;

    /* recver state */
    uint8_t is_finished;		// the status of the receiving: 0->the file has not completely received..

    char bcd_filename[FILENAME_LEN];

  };
  
 public:
  static const Commands cmds;

  Controller()
      : Module(),
        queue_(),
        prefetch_(),
        backpressure_(),
        burst_(),
        size_(),
        high_water_(),
        low_water_(),
        stats_() {
    is_task_ = true;
    propagate_workers_ = false;
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const sample::controller::pb::ControllerArg &arg);

  void DeInit() override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  CommandResponse CommandSetBurst(const sample::controller::pb::ControllerCommandSetBurstArg &arg);
  CommandResponse CommandSetSize(const sample::controller::pb::ControllerCommandSetSizeArg &arg);
  CommandResponse CommandGetStatus(
      const sample::controller::pb::ControllerCommandGetStatusArg &arg);

  CheckConstraintResult CheckModuleConstraints() const override;

 private:
  const double kHighWaterRatio = 0.90;
  const double kLowWaterRatio = 0.15;

  int Resize(int slots);

  RecverState * CreateRecverState(uint8_t data_id, int64_t data_size, char* bcd_filename);

  // Readjusts the water level according to `size_`.
  void AdjustWaterLevels();

  CommandResponse SetSize(uint64_t size);

  struct llring *queue_;
  bool prefetch_;

  // Whether backpressure should be applied or not
  bool backpressure_;

  int burst_;

  // Queue capacity
  uint64_t size_;

  // High water occupancy
  uint64_t high_water_;

  // Low water occupancy
  uint64_t low_water_;

  // Enqueue a packet
  int Enqueue(bess::Packet *pkt);

  // Send a request packet
  void SendReq(uint8_t code, uint8_t lrange, uint8_t rrange,
  uint8_t app_id, uint8_t data_id, uint8_t mode, uint8_t label, uint16_t addr, Context *ctx);


  bool data_ready_;
  bool data_receiving_;
  bool data_requested_;

  uint8_t curr_;
  uint8_t prior_;
  uint8_t initial_;

  uint8_t data_size_;
  uint8_t curr_data_size_;
  uint8_t curr_data_id_;

  uint8_t curr_data_sent_to_receiver;







  // Accumulated statistics counters
  struct {
    uint64_t enqueued;
    uint64_t dequeued;
    uint64_t dropped;
  } stats_;
};

#endif  // BESS_MODULES_QUEUE_H_
