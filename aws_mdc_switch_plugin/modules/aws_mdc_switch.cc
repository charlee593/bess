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

#include "aws_mdc_switch.h"


//static int numberOfSetBits_32(uint32_t i)
//{
//    // Java: use >>> instead of >>
//    // C or C++: use uint32_t
//    i = i - ((i >> 1) & 0x55555555);
//    i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
//    return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
//}

static int numberOfSetBits_8(uint8_t b )
{
    b = b - ((b >> 1) & 0x55);
    b = (b & 0x33) + ((b >> 2) & 0x33);
    return (((b + (b >> 4)) & 0x0F) * 0x01);
}


const Commands AwsMdcSwitch::cmds = {
        {"add",   "AwsMdcSwitchCommandAddArg",
                              MODULE_CMD_FUNC(&AwsMdcSwitch::CommandAdd), Command::THREAD_UNSAFE},
        {"clear", "EmptyArg", MODULE_CMD_FUNC(&AwsMdcSwitch::CommandClear),
                                                                          Command::THREAD_UNSAFE},
};

CommandResponse
AwsMdcSwitch::Init(const sample::aws_mdc_switch::pb::AwsMdcSwitchArg &arg) {
    if (arg.switch_ips_size() <= 0 || arg.switch_ips_size() > 16) {
        return CommandFailure(EINVAL, "no less than 1 and no more than 16 switch IPs");
    }
    if (arg.switch_macs_size() <= 0 || arg.switch_macs_size() > 16) {
        return CommandFailure(EINVAL, "no less than 1 and no more than 16 switch MACs");
    }

    if (arg.agents_ips_size() <= 0 || arg.agents_ips_size() > 16) {
        return CommandFailure(EINVAL, "no less than 1 and no more than 16 agent IPs");
    }
    if (arg.agents_macs_size() <= 0 || arg.agents_macs_size() > 16) {
        return CommandFailure(EINVAL, "no less than 1 and no more than 16 agent MACs");
    }

    if ((arg.agents_macs_size() != arg.agents_ips_size()) ||
        (arg.switch_macs_size() != arg.switch_ips_size()) ||
        (arg.agents_macs_size() != arg.switch_macs_size())) {
        return CommandFailure(EINVAL, "switch needs to have same number of agent and switch IPs and MACs");
    }

    for (int i = 0; i < arg.switch_ips_size(); i++) {
        const std::string & switch_mac_str = arg.switch_macs(i);
        const std::string & agent_mac_str = arg.agents_macs(i);
        const std::string & switch_ip_str = arg.switch_ips(i);
        const std::string & agent_ip_str = arg.agents_ips(i);

        switch_macs_[i] = Ethernet::Address(switch_mac_str);
        agent_macs_[i] = Ethernet::Address(agent_mac_str);

        be32_t switch_ip, agent_ip;
        bool ip_parsed = bess::utils::ParseIpv4Address(switch_ip_str, &switch_ip);
        if(!ip_parsed) {
            return CommandFailure(EINVAL, "cannot parse switch IP %s", switch_ip_str.c_str());
        }

        ip_parsed = bess::utils::ParseIpv4Address(agent_ip_str, &agent_ip);
        if(!ip_parsed) {
            return CommandFailure(EINVAL, "cannot parse agent IP %s", agent_ip_str.c_str());
        }

        switch_ips_[i] = switch_ip;
        agent_ips_[i] = agent_ip;
    }

    label_gates_[0] = 0x01;
    label_gates_[1] = 0x02;
    label_gates_[2] = 0x04;
    label_gates_[3] = 0x08;
    label_gates_[4] = 0x10;
    label_gates_[5] = 0x20;
    label_gates_[6] = 0x40;
    label_gates_[7] = 0x80;

    return CommandSuccess();
}

void AwsMdcSwitch::DeInit() {
}

CommandResponse AwsMdcSwitch::CommandAdd(
        const sample::aws_mdc_switch::pb::AwsMdcSwitchCommandAddArg &) {
    return CommandSuccess();
}

CommandResponse AwsMdcSwitch::CommandClear(const bess::pb::EmptyArg &) {
    return CommandSuccess();
}

void AwsMdcSwitch::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
    int cnt = batch->cnt();

    for (int i = 0; i < cnt; i++) {
        bess::Packet *pkt = batch->pkts()[i];
        Ethernet *eth = pkt->head_data<Ethernet *>();

        if (eth->ether_type != be16_t(Ethernet::Type::kIpv4)) {
            DropPacket(ctx, pkt);
            continue;
        }

        Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);
        if (ip->protocol != Ipv4::Proto::kUdp) {
            DropPacket(ctx, pkt);
            continue;
        }

        int ip_bytes = ip->header_length << 2;
//        Udp *udp = reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);
        // Access UDP payload (i.e., mDC data)
        be32_t *p = pkt->head_data<be32_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp));
//            std::cout << std::hex << static_cast<int>(mode) << std::endl;

        // Data pkts
        uint8_t mode = p->raw_value() & 0x00ff0000;
        if (mode == 0x00) {
            // If mode is 0x00, the data pkt needs to be forwarded to the active agent
            EmitPacket(ctx, pkt, active_agent_id_);
        } else {
            // Let's check the label
            uint8_t label = p->raw_value() & 0xff000000;
            int remaining_gate_count = numberOfSetBits_8(label);

            // We actually shouldn't reach here
            if (remaining_gate_count == 0) {
                DropPacket(ctx, pkt);
                continue;
            }

            for (uint8_t i=0; i < AWS_MAX_INTF_COUNT; i++) {
                if((label_gates_[i] & label) == label_gates_[i]) {
                    if(remaining_gate_count == 1) {
                        eth->dst_addr = agent_macs_[i];
                        ip->dst = agent_ips_[i];

                        eth->src_addr = switch_macs_[i];
                        ip->src = switch_ips_[i];
                        EmitPacket(ctx, pkt, i);
                        break;
                    } else {
                        // copy the pkt only if we have more than one gate to duplicate pkt to
                        bess::Packet *new_pkt = bess::Packet::copy(pkt);
                        if (new_pkt) {
                            Ethernet *new_eth = new_pkt->head_data<Ethernet *>();
                            Ipv4 *new_ip = reinterpret_cast<Ipv4 *>(new_eth + 1);

                            new_eth->dst_addr = agent_macs_[i];
                            new_ip->dst = agent_ips_[i];

                            new_eth->src_addr = switch_macs_[i];
                            new_ip->src = switch_ips_[i];

                            EmitPacket(ctx, new_pkt, i);
                        }
                    }
                    remaining_gate_count--;
                }
            }
        }
    }
}

ADD_MODULE(AwsMdcSwitch, "aws_mdc_switch", "Software mDC switch")
