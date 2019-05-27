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


#include <cstdlib>

#include "../utils/format.h"
#include "../utils/cuckoo_map.h"

#define DEFAULT_BUFFEREDQUEUE_SIZE 1024

enum {
  ATTR_W_DATA_ID,
  ATTR_W_DATA_SIZE
};

struct MDCData
{
    char name[50];
    int age;
    float salary;
};

const Commands FileWriter::cmds = {
    {"set_burst", "FileWriterCommandSetBurstArg",
     MODULE_CMD_FUNC(&FileWriter::CommandSetBurst), Command::THREAD_SAFE},
    {"set_size", "FileWriterCommandSetSizeArg",
     MODULE_CMD_FUNC(&FileWriter::CommandSetSize), Command::THREAD_UNSAFE},
    {"get_status", "FileWriterCommandGetStatusArg",
     MODULE_CMD_FUNC(&FileWriter::CommandGetStatus), Command::THREAD_SAFE}};

RecverState * CreateRecverState(uint8_t data_id, int64_t data_size) {
  RecverState * recv_p = (RecverState *) malloc(sizeof(RecverState));
  bzero(recv_p, sizeof(RecverState));

  recv_p->data_id = data_id;
  recv_p->data_size = data_size;
  recv_p->is_finished = 0;
  recv_p->num_recv_ed = 0;

  std::string str = "/tmp/mdc_data_" +  std::to_string(data_id);
  char *cstr = &str[0u];

  if ((recv_p->fd_p = fopen(cstr, "w")) == NULL) {
   free(recv_p);
   std::cout << "Not good!!!!!!!!!" << std::endl;
   return NULL;
  }

  return recv_p;
}

int FileWriter::Resize(int slots) {
  struct llring *old_queue = queue_;
  struct llring *new_queue;

  int bytes = llring_bytes_with_slots(slots);

  new_queue =
      reinterpret_cast<llring *>(std::aligned_alloc(alignof(llring), bytes));
  if (!new_queue) {
    std::cout << "Resize: no enough memory: " + std::to_string(ENOMEM) << std::endl;
    return -ENOMEM;
  }

  int ret = llring_init(new_queue, slots, 0, 1);
  if (ret) {
    std::free(new_queue);
    std::cout << "Resize: invalid: " + std::to_string(EINVAL) << std::endl;
    return -EINVAL;
  }

  /* migrate packets from the old queue */
  if (old_queue) {
    bess::Packet *pkt;

    while (llring_sc_dequeue(old_queue, (void **)&pkt) == 0) {
      ret = llring_sp_enqueue(new_queue, pkt);
      if (ret == -LLRING_ERR_NOBUF) {
        bess::Packet::Free(pkt);
      }
    }

    std::free(old_queue);
  }

  queue_ = new_queue;
  size_ = slots;

  if (backpressure_) {
    AdjustWaterLevels();
  }

  return 0;
}

CommandResponse FileWriter::Init(const sample::file_writer::pb::FileWriterArg &arg) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  AddMetadataAttr("data_id", 1, AccessMode::kRead);
  AddMetadataAttr("data_size", 1, AccessMode::kRead);

  data_ready_ = false;
  data_receiving_ = false;
  data_size_ = 0;
  curr_data_size_ = 0;
  data_requested_ = false;

  task_id_t tid;
  CommandResponse err;

  tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "Task creation failed");
  }

  burst_ = bess::PacketBatch::kMaxBurst;

  if (arg.backpressure()) {
    VLOG(1) << "Backpressure enabled for " << name() << "::FileWriter";
    backpressure_ = true;
  }

  if (arg.size() != 0) {
    err = SetSize(arg.size());
    if (err.error().code() != 0) {
      return err;
    }
  } else {
    int ret = Resize(DEFAULT_BUFFEREDQUEUE_SIZE);
    if (ret) {
      return CommandFailure(-ret);
    }
  }

  if (arg.prefetch()) {
    prefetch_ = true;
  }

  return CommandSuccess();
}

void FileWriter::DeInit() {
  bess::Packet *pkt;

  if (queue_) {
    while (llring_sc_dequeue(queue_, (void **)&pkt) == 0) {
      bess::Packet::Free(pkt);
    }
    std::free(queue_);
  }
}

std::string FileWriter::GetDesc() const {
  const struct llring *ring = queue_;

  return bess::utils::Format("%u/%u", llring_count(ring), ring->common.slots);
}

int FileWriter::Enqueue(bess::Packet *pkt) {
  if (llring_enqueue(queue_, (void *)pkt) != 0){
    return 0;
  }

  if (backpressure_ && llring_count(queue_) > high_water_) {
    SignalOverload();
  }

  stats_.enqueued += 1;

  return 1;
}

void FileWriter::SendReq(uint8_t code, uint8_t lrange, uint8_t rrange,
  uint8_t app_id, uint8_t data_id, uint8_t mode, uint8_t label, uint16_t addr, Context *ctx) {

  bess ::Packet *new_pkt = current_worker.packet_pool()->Alloc(42 + 9);
  Ethernet *eth = new_pkt->head_data<Ethernet *>(); // Ethernet
  Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1); // IP
  int ip_bytes = ip->header_length << 2;

  if (new_pkt) {
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


      std::cout << "FileWriter new packet mDC"  << std::hex <<  mDC << std::endl;
      bess::utils::Copy(new_p, reinterpret_cast<uint64_t *>(&mDC), 16);




      be64_t *p4 = new_pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp));
      std::cout << "FileWriter new packet "  << std::hex << p4->raw_value() << std::endl;
      be64_t *p3 = new_pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp) + 2);
      std::cout << "FileWriter new packet "  << std::hex << p3->raw_value() << std::endl;

      EmitPacket(ctx, new_pkt, 1);
  }
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
    be64_t *mdc_p1 = pkt->head_data<be64_t *>(offset); // first 8 bytes
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

    auto result = cuckoo.Find(app_id);

    RecverState * recv_s = &(result->second);

    if (result == nullptr) {
      std::cout << "CuckooMap INSSSSSSSSSSSSSIIIIIIIIIDEEEE" << std::endl;
      recv_s = CreateRecverState(data_id, data_size);
      cuckoo.Insert(data_id, *recv_s);
    }


    uint8_t data_id = get_attr<uint8_t>(this, ATTR_W_DATA_ID, pkt);
    uint8_t data_size = get_attr<uint8_t>(this, ATTR_W_DATA_SIZE, pkt);


    fwrite(p + DATA_PAYLOAD_OFFSET, sizeof(char), dh.len, recv_p - > fd_p);

    EmitPacket(ctx, pkt, 0);

    // std::cout << "CuckooMap: " << std::to_string(recv_r->data_id) << std::endl;
    //
    // RecverState * recv_p = CreateRecverState(0xff, 64);
    // std::cout << "CuckooMap: " << std::to_string(recv_p->data_id) << std::to_string(recv_p->data_size) << std::to_string(recv_p->num_recv_ed)  << std::endl;



  }
}

/* to downstream */
struct task_result FileWriter::RunTask(Context *ctx, bess::PacketBatch *batch,
                                  void *) {
  if (!data_requested_ || children_overload_ > 0) {
    return {
        .block = true, .packets = 0, .bits = 0,
    };
  }

  const int burst = ACCESS_ONCE(burst_);
  const int pkt_overhead = 24;

  uint64_t total_bytes = 0;

  uint32_t cnt = llring_sc_dequeue_burst(queue_, (void **)batch->pkts(), burst);

  if (cnt == 0) {
    return {.block = true, .packets = 0, .bits = 0};
  }

  stats_.dequeued += cnt;
  batch->set_cnt(cnt);

  if (prefetch_) {
    for (uint32_t i = 0; i < cnt; i++) {
      total_bytes += batch->pkts()[i]->total_len();
      rte_prefetch0(batch->pkts()[i]->head_data());
    }
  } else {
    for (uint32_t i = 0; i < cnt; i++) {
      total_bytes += batch->pkts()[i]->total_len();
    }
  }

  RunNextModule(ctx, batch);

  if (backpressure_ && llring_count(queue_) < low_water_) {
    SignalUnderload();
  }

  return {.block = false,
          .packets = cnt,
          .bits = (total_bytes + cnt * pkt_overhead) * 8};
}

CommandResponse FileWriter::CommandSetBurst(
    const sample::file_writer::pb::FileWriterCommandSetBurstArg &arg) {
  uint64_t burst = arg.burst();

  if (burst > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "burst size must be [0,%zu]",
                          bess::PacketBatch::kMaxBurst);
  }

  burst_ = burst;
  return CommandSuccess();
}

CommandResponse FileWriter::SetSize(uint64_t size) {
  std::cout << "Here in Setsize: " << std::endl;
  if (size < 4 || size > 16384) {
    return CommandFailure(EINVAL, "must be in [4, 16384]");
  }

  if (size & (size - 1)) {
    return CommandFailure(EINVAL, "must be a power of 2");
  }

  int ret = Resize(size);
  std::cout << "After Here in Setsize: " +  std::to_string(ret) << std::endl;
  if (ret) {
    return CommandFailure(-ret);
  }

  return CommandSuccess();
}

CommandResponse FileWriter::CommandSetSize(
    const sample::file_writer::pb::FileWriterCommandSetSizeArg &arg) {
  return SetSize(arg.size());
}

CommandResponse FileWriter::CommandGetStatus(
    const sample::file_writer::pb::FileWriterCommandGetStatusArg &) {
  sample::file_writer::pb::FileWriterCommandGetStatusResponse resp;
  resp.set_count(llring_count(queue_));
  resp.set_size(size_);
  resp.set_enqueued(stats_.enqueued);
  resp.set_dequeued(stats_.dequeued);
  resp.set_dropped(stats_.dropped);
  return CommandSuccess(resp);
}

void FileWriter::AdjustWaterLevels() {
  high_water_ = static_cast<uint64_t>(size_ * kHighWaterRatio);
  low_water_ = static_cast<uint64_t>(size_ * kLowWaterRatio);
}

CheckConstraintResult FileWriter::CheckModuleConstraints() const {
  CheckConstraintResult status = CHECK_OK;
  if (num_active_tasks() - tasks().size() < 1) {  // Assume multi-producer.
    LOG(ERROR) << "FileWriter has no producers";
    status = CHECK_NONFATAL_ERROR;
  }

  if (tasks().size() > 1) {  // Assume single consumer.
    LOG(ERROR) << "More than one consumer for the queue" << name();
    return CHECK_FATAL_ERROR;
  }

  return status;
}

ADD_MODULE(FileWriter, "file_writer",
           "terminates current task and enqueue packets for new task")