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
#define FILENAME_LEN (6)

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
using bess::utils::CuckooMap;
using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;

struct RecverState
{
  uint8_t data_id;
  int64_t data_size;
  int64_t num_recv_ed;

  FILE *fd_p;

  uint8_t is_finished;
};

struct McReply
{
  uint8_t data_id;
  int64_t data_size;

  FILE *fd_p;
};

class Controller : public Module
{
public:
  Controller()
      : Module()
  {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::IPEncapArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

private:
  // Send a request packet
  void SendReq(uint8_t code, uint8_t lrange, uint8_t rrange,
               uint8_t app_id, uint8_t data_id, uint8_t mode, uint8_t label, uint16_t addr, Context *ctx);

  CuckooMap<uint8_t, RecverState> cuckoo;
};

#endif // BESS_MODULES_
