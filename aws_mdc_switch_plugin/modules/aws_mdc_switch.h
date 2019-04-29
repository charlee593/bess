// Copyright (c) 2014-2017, The Regents of the University of California.
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

#ifndef BESS_MODULES_LABELLOOKUP_H_
#define BESS_MODULES_LABELLOOKUP_H_


#include "module.h"
#include "utils/bits.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/ip.h"
#include "utils/udp.h"
#include "utils/exact_match_table.h"

#include "pb/aws_mdc_switch_msg.pb.h"

using bess::utils::Error;

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::be64_t;
using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;

#define AWS_MAX_INTF_COUNT 8

class AwsMdcSwitch final : public Module {
public:
  static const Commands cmds;
  static const gate_idx_t kNumIGates = AWS_MAX_INTF_COUNT;
  static const gate_idx_t kNumOGates = AWS_MAX_INTF_COUNT;

  AwsMdcSwitch() : Module(),
                   active_agent_id_(0), label_gates_(),
                   switch_ips_(), switch_macs_(),
                   agent_ips_(), agent_macs_() {
      max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const sample::aws_mdc_switch::pb::AwsMdcSwitchArg &arg);
  void DeInit();

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const sample::aws_mdc_switch::pb::AwsMdcSwitchCommandAddArg &arg);
  CommandResponse CommandClear(const bess::pb::EmptyArg &arg);

private:
    gate_idx_t active_agent_id_;
    uint8_t label_gates_[AWS_MAX_INTF_COUNT];

    be32_t switch_ips_[AWS_MAX_INTF_COUNT];
    Ethernet::Address switch_macs_[AWS_MAX_INTF_COUNT];

    be32_t agent_ips_[AWS_MAX_INTF_COUNT];
    Ethernet::Address agent_macs_[AWS_MAX_INTF_COUNT];
};

#endif // BESS_MODULES_LABELLOOKUP_H_
