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

#include "file_writer.h"
#include <bitset>
#include <cstdlib>

#include "../utils/format.h"
#include "../utils/cuckoo_map.h"

#define DEFAULT_BUFFEREDQUEUE_SIZE 1024

enum
{
  ATTR_R_FILE_D,
  ATTR_R_HEADER_SIZE,
  ATTR_R_DATA_SIZE
};

CommandResponse FileWriter::Init(const bess::pb::IPEncapArg &arg[[maybe_unused]])
{
  using AccessMode = bess::metadata::Attribute::AccessMode;
  AddMetadataAttr("file_d", sizeof(FILE *), AccessMode::kRead);
  AddMetadataAttr("header_size", 1, AccessMode::kRead);
  AddMetadataAttr("data_size", 1, AccessMode::kRead);

  return CommandSuccess();
}

/* from upstream */
void FileWriter::ProcessBatch(Context *ctx, bess::PacketBatch *batch)
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

    FILE *fd_d = get_attr<FILE *>(this, ATTR_R_FILE_D, pkt);
    uint8_t header_size_ = get_attr<uint8_t>(this, ATTR_R_HEADER_SIZE, pkt);
    uint8_t data_size_ = get_attr<uint8_t>(this, ATTR_R_DATA_SIZE, pkt);

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
    std::cout << "FileWriter ProcessBatch batch size: " + std::to_string(cnt) + " pkt: " + std::to_string(i) << std::endl;
    std::cout << std::hex << addr << std::endl;
    std::cout << std::hex << static_cast<int>(mode) << std::endl;
    std::cout << std::hex << static_cast<int>(label) << std::endl;
    std::cout << std::hex << static_cast<int>(code) << std::endl;
    std::cout << std::hex << static_cast<int>(app_id) << std::endl;
    std::cout << std::hex << static_cast<int>(data_id) << std::endl;
    std::cout << std::hex << std::to_string(sn) << std::endl;
    std::cout << std::hex << std::to_string(data_size) << std::endl;

    // fwrite to fd_d
    uint8_t data_written = fwrite(pkt->head_data<void *>(offset + header_size_), sizeof(char), data_size_, fd_d);
    fclose(fd_d);

    // Reply with data id, amount of data written,
    // Rewrite mode field to 0x14
    char *head = pkt->head_data<char *>(offset);
    uint8_t *code_p = reinterpret_cast<uint8_t *>(head + 4);
    uint8_t *data_size_p = reinterpret_cast<uint8_t *>(head + 8);
    *code_p = (*code_p & 0x00) | 0x06;
    *data_size_p = (*data_size_p & 0x00) | data_written;

    be64_t *mdc_p1_ = pkt->head_data<be64_t *>(offset);     // first 8 bytes
    be64_t *mdc_p2_ = pkt->head_data<be64_t *>(offset + 8); // second 8 bytes

    uint8_t code_ = (mdc_p1_->raw_value() & 0xff00000000) >> 32;
    uint8_t data_size_1 = (mdc_p2_->raw_value() & 0xff);

    std::cout << "FileWriter send reply: " << std::endl;
    std::cout << std::hex << static_cast<int>(code_) << std::endl;
    std::cout << std::hex << std::to_string(data_size_1) << std::endl;

    EmitPacket(ctx, pkt, 0);
  }
}

ADD_MODULE(FileWriter, "file_writer",
           "description of file_writer")
