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

enum {
  ATTR_R_FILE_D,
  ATTR_R_HEADER_SIZE,
  ATTR_R_DATA_SIZE
};

CommandResponse FileWriter::Init(const bess::pb::IPEncapArg &arg[[maybe_unused]]) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  AddMetadataAttr("file_d", sizeof(FILE *), AccessMode::kRead);
  AddMetadataAttr("header_size", 1, AccessMode::kRead);
  AddMetadataAttr("data_size", 1, AccessMode::kRead);

  return CommandSuccess();
}


/* from upstream */
void FileWriter::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::Packet *pkt = batch->pkts()[i];

    Ethernet *eth = pkt->head_data<Ethernet *>(); // Ethernet
    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1); // IP
    int ip_bytes = ip->header_length << 2;

    // Access UDP payload (i.e., mDC data)
    uint8_t offset = sizeof(Ethernet) + ip_bytes + sizeof(Udp);

    FILE * fd_d = get_attr<FILE *>(this, ATTR_R_FILE_D, pkt);
    uint8_t header_size_ = get_attr<uint8_t>(this, ATTR_R_HEADER_SIZE, pkt);
    uint8_t data_size_ = get_attr<uint8_t>(this, ATTR_R_DATA_SIZE, pkt);

    // fwrite to fd_d
    uint8_t data_written = fwrite( pkt->head_data<void *>(offset + header_size_) , sizeof(char), data_size_, fd_d);
    fclose (fd_d);

    // Reply with data id, amount of data written,
    // Rewrite mode field to 0x14
    char *head = pkt->head_data<char *>(offset);
    uint8_t *mode_p = reinterpret_cast<uint8_t *>(head + 2);
    uint8_t *data_size_p = reinterpret_cast<uint8_t *>(head + 8);
    *mode_p = (*mode_p & 0x00) | 0x14;
    *data_size_p = (*data_size_p & 0x00) | data_written;

    EmitPacket(ctx, pkt, 0);

  }
}

ADD_MODULE(FileWriter, "file_writer",
           "terminates current task and enqueue packets for new task")
