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

#include "controller.h"

#include <cstdlib>

#include "../utils/format.h"
#include "../utils/cuckoo_map.h"

enum
{
  ATTR_W_FILE_D,
  ATTR_W_HEADER_SIZE,
  ATTR_W_DATA_SIZE
};

RecverState *CreateRecverState(uint8_t data_id, int64_t data_size)
{
  RecverState *recv_p = (RecverState *)malloc(sizeof(RecverState));
  bzero(recv_p, sizeof(RecverState));

  recv_p->data_id = data_id;
  recv_p->data_size = data_size;
  recv_p->is_finished = 0;
  recv_p->num_recv_ed = 0;

  std::string str = "/tmp/mdc_data_" + std::to_string(data_id);
  char *cstr = &str[0u];

  if ((recv_p->fd_p = fopen(cstr, "a+")) == NULL)
  {
    free(recv_p);
    std::cout << "Not good!!!!!!!!!" << std::endl;
    return NULL;
  }

  return recv_p;
}

McReply *CreateMcReply(uint8_t data_id, int64_t data_size, FILE *fd_p)
{
  McReply *reply_p = (McReply *)malloc(sizeof(McReply));
  bzero(reply_p, sizeof(McReply));

  reply_p->data_id = data_id;
  reply_p->data_size = data_size;
  reply_p->fd_p = fd_p;

  return reply_p;
}

CommandResponse Controller::Init(const bess::pb::IPEncapArg &arg[[maybe_unused]])
{
  using AccessMode = bess::metadata::Attribute::AccessMode;
  AddMetadataAttr("file_d", sizeof(FILE *), AccessMode::kWrite);
  AddMetadataAttr("header_size", 1, AccessMode::kWrite);
  AddMetadataAttr("data_size", 1, AccessMode::kWrite);

  return CommandSuccess();
}

void Controller::SendReq(uint8_t code, uint8_t lrange, uint8_t rrange,
                         uint8_t app_id, uint8_t data_id, uint8_t mode, uint8_t label, uint16_t addr, Context *ctx)
{

  bess ::Packet *new_pkt = current_worker.packet_pool()->Alloc(42 + 9);
  Ethernet *eth = new_pkt->head_data<Ethernet *>(); // Ethernet
  Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);     // IP
  int ip_bytes = ip->header_length << 2;

  if (new_pkt)
  {
    uint64_t mDC = 0xffff & addr;
    mDC = (mDC << 8) | mode;
    mDC = (mDC << 8) | label;
    mDC = (mDC << 8) | code;
    mDC = (mDC << 8) | app_id;
    mDC = (mDC << 8) | data_id;
    mDC = (mDC << 8) | lrange;

    be16_t *new_p2 = new_pkt->head_data<be16_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp));
    bess::utils::Copy(new_p2, &rrange, 2);

    be64_t *new_p = new_pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp) + 1); // First 8 bytes

    std::cout << "Controller new packet mDC" << std::hex << mDC << std::endl;
    bess::utils::Copy(new_p, reinterpret_cast<uint64_t *>(&mDC), 16);

    be64_t *p4 = new_pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp));
    std::cout << "Controller new packet " << std::hex << p4->raw_value() << std::endl;
    be64_t *p3 = new_pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp) + 2);
    std::cout << "Controller new packet " << std::hex << p3->raw_value() << std::endl;

    EmitPacket(ctx, new_pkt, 1);
  }
}

/* from upstream */
void Controller::ProcessBatch(Context *ctx, bess::PacketBatch *batch)
{
  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++)
  {
    bess::Packet *pkt = batch->pkts()[i];

    Ethernet *eth = pkt->head_data<Ethernet *>(); // Ethernet
    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1); // IP
    int ip_bytes = ip->header_length << 2;

    // Access UDP payload (i.e., mDC data)
    uint8_t offset = sizeof(Ethernet) + ip_bytes + sizeof(Udp);
    be64_t *mdc_p1 = pkt->head_data<be64_t *>(offset);     // first 8 bytes
    be64_t *mdc_p2 = pkt->head_data<be64_t *>(offset + 8); // second 8 bytes

    uint16_t addr = (mdc_p1->raw_value() & 0xffff);
    uint8_t mode = (mdc_p1->raw_value() & 0xff0000) >> 16;
    uint8_t label = (mdc_p1->raw_value() & 0xff000000) >> 24;
    uint8_t code = (mdc_p1->raw_value() & 0xff00000000) >> 32;
    uint8_t app_id = (mdc_p1->raw_value() & 0xff0000000000) >> 40;
    uint8_t data_id = (mdc_p1->raw_value() & 0xff000000000000) >> 48;
    uint8_t sn = (mdc_p1->raw_value() & 0xff00000000000000) >> 56;
    uint8_t data_size = (mdc_p2->raw_value() & 0xff);

    std::cout << "Controller ProcessBatch batch size: " + std::to_string(cnt) + " pkt: " + std::to_string(i) << std::endl;
    std::cout << std::hex << addr << std::endl;
    std::cout << std::hex << static_cast<int>(mode) << std::endl;
    std::cout << std::hex << static_cast<int>(label) << std::endl;
    std::cout << std::hex << static_cast<int>(code) << std::endl;
    std::cout << std::hex << static_cast<int>(app_id) << std::endl;
    std::cout << std::hex << static_cast<int>(data_id) << std::endl;
    std::cout << std::hex << std::to_string(sn) << std::endl;
    std::cout << std::hex << std::to_string(data_size) << std::endl;

    if (code == 5 || code == 6 || code == 1)
    {
      auto recv_s = cuckoo.Find(data_id);
      RecverState *recv_p = &(recv_s->second);
      if (code == 5)
      {
        std::cout << "Controller: Got data from sender" << std::endl;
        if (recv_s == nullptr)
        {
          recv_p = CreateRecverState(data_id, data_size);
          cuckoo.Insert(data_id, *recv_p);
        }

        set_attr<FILE *>(this, ATTR_W_FILE_D, pkt, recv_p->fd_p);
        set_attr<uint8_t>(this, ATTR_W_HEADER_SIZE, pkt, 9);
        set_attr<uint8_t>(this, ATTR_W_DATA_SIZE, pkt, data_size);

        EmitPacket(ctx, pkt, 0);
      }
      if (code == 6)
      {
        std::cout << "Controller: Got reply from file writer" << std::endl;
        recv_p->num_recv_ed += data_size;
        if (recv_p->num_recv_ed == recv_p->data_size)
        {
          recv_p->is_finished = 1;
        }
      }
      if (code == 1)
      {
        if (recv_p->is_finished)
        {
          McReply *reply_p = CreateMcReply(recv_p->data_id, recv_p->data_size, recv_p->fd_p);
          bess ::Packet *new_pkt = current_worker.packet_pool()->Alloc(sizeof(McReply));

          if (new_pkt)
          {
            char *head = new_pkt->head_data<char *>();
            bess::utils::Copy(head, reply_p, sizeof(McReply));

            be64_t *ee1 = new_pkt->head_data<be64_t *>(); // first 8 bytes
            uint8_t daa = ee1->raw_value();
            std::cout << "Controller: daa" << std::endl;

            std::cout << std::hex << daa << std::endl;

            EmitPacket(ctx, new_pkt, 1);
          }
        }
      }
    }
  }
}

ADD_MODULE(Controller, "controller",
           "description of controller")
