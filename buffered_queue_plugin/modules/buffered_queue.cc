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

#include "buffered_queue.h"

#include <cstdlib>

#include "../utils/format.h"

#define DEFAULT_BUFFEREDQUEUE_SIZE 1024

struct MDCData
{
    char name[50];
    int age;
    float salary;
};

const Commands BufferedQueue::cmds = {
    {"set_burst", "BufferedQueueCommandSetBurstArg",
     MODULE_CMD_FUNC(&BufferedQueue::CommandSetBurst), Command::THREAD_SAFE},
    {"set_size", "BufferedQueueCommandSetSizeArg",
     MODULE_CMD_FUNC(&BufferedQueue::CommandSetSize), Command::THREAD_UNSAFE},
    {"get_status", "BufferedQueueCommandGetStatusArg",
     MODULE_CMD_FUNC(&BufferedQueue::CommandGetStatus), Command::THREAD_SAFE}};

int BufferedQueue::Resize(int slots) {
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

CommandResponse BufferedQueue::Init(const sample::buffered_queue::pb::BufferedQueueArg &arg) {
  sendto_ = false;
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
    VLOG(1) << "Backpressure enabled for " << name() << "::BufferedQueue";
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

void BufferedQueue::DeInit() {
  bess::Packet *pkt;

  if (queue_) {
    while (llring_sc_dequeue(queue_, (void **)&pkt) == 0) {
      bess::Packet::Free(pkt);
    }
    std::free(queue_);
  }
}

std::string BufferedQueue::GetDesc() const {
  const struct llring *ring = queue_;

  return bess::utils::Format("%u/%u", llring_count(ring), ring->common.slots);
}

/* from upstream */
void BufferedQueue::ProcessBatch(Context *, bess::PacketBatch *batch) {
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
    uint8_t appID = (mdc_p1->raw_value() & 0xff0000000000) >> 40;
    uint8_t dataID = (mdc_p1->raw_value() & 0xff000000000000) >> 48;
    uint8_t sn = (mdc_p1->raw_value() & 0xff00000000000000) >> 56;
    uint8_t dataSize = (mdc_p2->raw_value() & 0xff);

    std::cout << "ProcessBatch ID";
    std::cout << std::hex << addr << std::endl;
    std::cout << std::hex << mode << std::endl;
    std::cout << std::hex << label << std::endl;
    std::cout << std::hex << code << std::endl;
    std::cout << std::hex << appID << std::endl;
    std::cout << std::hex << dataID << std::endl;
    std::cout << std::hex << sn << std::endl;
    std::cout << std::hex << dataSize << std::endl;
  
  }


//     // Data pkts
//     uint8_t mul_type = (p->raw_value() & 0xff);
//     uint8_t sn = (p->raw_value() & 0xff00)  >> 8;
//     uint8_t data_size = (p->raw_value() & 0xff0000)  >> 16;
    
//     uint16_t address = (p->raw_value() & 0x0000ffff);
//     uint8_t appID = (p->raw_value() & 0xff00000000) >> 32;



//     /* Recv Request from Receiver */
//     if(data_size_ != 0 && data_size_ == curr_data_size_){
//       data_ready_ = true;
//       data_receiving_ = false;
//       data_requested_ = true;
//     }

//     /* Recv Data from Sender - intial*/
//     /* Recv Data from Sender - case 1*/
//     /* Recv Data from Sender - case 2*/
//     /* Recv Data from Sender - case 3*/
//     /* Recv Data from Sender - DATA_FNSD*/
//     /* Recv Data from Sender - PTCH_DATAs*/

//   }




//   for (int i = 0; i < cnt; i++) {

//     bess::Packet *pkt = batch->pkts()[i];
//     Ethernet *eth = pkt->head_data<Ethernet *>();
//     Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);

//     int ip_bytes = ip->header_length << 2;
// //        Udp *udp = reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);
//     // Access UDP payload (i.e., mDC data)
//     be64_t *p = pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp)+8);


//       std::cout << "SWITCH";
//        std::cout << std::hex << static_cast<int>(p->value()) << std::endl;
//        std::cout << std::hex << static_cast<int>(p->raw_value()) << std::endl;
//        std::cout << sizeof(Ethernet) + ip_bytes + sizeof(Udp) << std::endl;


//     // Data pkts
//     uint8_t mul_type = (p->raw_value() & 0xff);
//     uint8_t sn = (p->raw_value() & 0xff00)  >> 8;
//     uint8_t data_size = (p->raw_value() & 0xff0000)  >> 16;
    
//     uint16_t address = (p->raw_value() & 0x0000ffff);
//     uint8_t appID = (p->raw_value() & 0xff00000000) >> 32;



//     std::cout << "BufferedQueue address :::::" << std::endl;
//     std::cout << std::hex << static_cast<int>(address) << std::endl;
//     std::cout << "BufferedQueue appID :::::" << std::endl;
//     std::cout << std::hex << static_cast<int>(appID) << std::endl;
//     std::cout << "BufferedQueue sn :::::" << std::endl;
//     std::cout << std::hex << static_cast<int>(sn) << std::endl;
//     std::cout << "BufferedQueue type :::::" << std::endl;
//     std::cout << std::hex << static_cast<int>(mul_type) << std::endl;
//     std::cout << "BufferedQueue type: " + std::to_string(mul_type)<< std::endl;
//     std::cout << "BufferedQueue size: " + std::to_string(data_size)<< std::endl;
//     std::cout << std::hex << static_cast<int>(data_size) << std::endl;
//     std::cout << std::hex << static_cast<int>(data_size+1) << std::endl;


//     std::cout << "BufferedQueue pkt size: " + std::to_string(pkt->total_len())<< std::endl;
//     std::cout << "BufferedQueue pkt header size: " + std::to_string(sizeof(Ethernet) + ip_bytes + sizeof(Udp))<< std::endl;


//     if(mul_type == 1 && !data_requested_){
//       data_requested_ = true;
//       return;
//     }


//     curr_ = sn;
//     if(!data_receiving_){
//       initial_ = sn;
//       prior_ = sn;
//       data_receiving_ = true;
//       data_size_ = data_size;
//       std::cout << "BufferedQueue initial"  + std::to_string(data_receiving_)<< std::endl;
//     }
//     else{
//       if(curr_ == (prior_+1)%data_size_ ){
//         prior_ = curr_;
//         curr_data_size_++;

//         if(curr_data_size_ == data_size_){
//             data_ready_ = true;
//         }

//       }
//       else if(curr_ > (prior_+1)%data_size_  || curr_ <= initial_){
//         std::cout << "Case 2" << std::endl;

//         bess ::Packet *new_pkt = current_worker.packet_pool()->Alloc(sizeof(Ethernet) + ip_bytes + sizeof(Udp) +11);

//         if (new_pkt) {
//             be32_t *new_p = new_pkt->head_data<be32_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp) + 8);

//             uint32_t code = 0x022bc5;

//             bess::utils::Copy(new_p, reinterpret_cast<uint32_t *>(&code), 6);

//             be32_t *p4 = new_pkt->head_data<be32_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp)+ 8);
//             std::cout << "BufferedQueue new packet "  << p4->raw_value() << std::endl;

//             // EmitPacket(ctx, new_pkt, i);
//         }

//         return;

//       }
//       else{
//         std::cout << "Case 3" << std::endl;

//         bess ::Packet *new_pkt = current_worker.packet_pool()->Alloc(sizeof(Ethernet) + ip_bytes + sizeof(Udp) +11);

//         if (new_pkt) {
//             be32_t *new_p = new_pkt->head_data<be32_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp) + 8);

//             uint32_t code = 0x022bc5;

//             bess::utils::Copy(new_p, reinterpret_cast<uint32_t *>(&code), 6);

//             be32_t *p4 = new_pkt->head_data<be32_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp)+ 8);
//             std::cout << "BufferedQueue new packet "  << p4->raw_value() << std::endl;

//             // EmitPacket(ctx, new_pkt, i);
//         }
//         return;

//       }
//     }

//     std::cout << "BufferedQueue initial_: " + std::to_string(initial_)<< std::endl;
//     std::cout << "BufferedQueue prior_: " + std::to_string(prior_)<< std::endl;
//     std::cout << "BufferedQueue curr_: " + std::to_string(curr_)<< std::endl;

//   }

//   int queued =
//       llring_mp_enqueue_burst(queue_, (void **)batch->pkts(), batch->cnt());

//   std::cout << "Queued value in llring_mp_enqueue_burst: " + std::to_string(queued) << std::endl;

//   std::cout << "Queued value in llring_count: " + std::to_string(llring_count(queue_)) << std::endl;

//   if (backpressure_ && llring_count(queue_) > high_water_) {
//     SignalOverload();
//   }

//   stats_.enqueued += queued; // Total enqueued stat

//   // Drop if batch size is larger than queued packet in this round
//   if (queued < batch->cnt()) {
//     int to_drop = batch->cnt() - queued;
//     stats_.dropped += to_drop;
//     bess::Packet::Free(batch->pkts() + queued, to_drop);
//   }

}

/* to downstream */
struct task_result BufferedQueue::RunTask(Context *ctx, bess::PacketBatch *batch,
                                  void *) {

  // std::cout << "BufferedQueue RunTask: " + std::to_string(llring_count(queue_)) << std::endl;

  if (children_overload_ > 0 ) {
    return {
        .block = true, .packets = 0, .bits = 0,
    };
  }

  const int burst = ACCESS_ONCE(burst_);
  const int pkt_overhead = 24;
  uint32_t cnt = 0;
  uint64_t total_bytes = 0;

  if (data_requested_){
    std::cout << "BufferedQueue queue value in before: " + std::to_string(llring_count(queue_)) << std::endl;

    cnt = llring_sc_dequeue_burst(queue_, (void **)batch->pkts(), burst);

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

    std::cout << "BufferedQueue queue batch value in during: " + std::to_string(batch->cnt()) << std::endl;

    RunChooseModule(ctx, 0, batch);

    if(llring_count(queue_) <= 0){
      data_requested_ = false;
    }

    // for (uint32_t i = 0; i < cnt; i++) {
    //   std::cout << "BufferedQueue sending packet: " + std::to_string(i) << std::endl;
    //   EmitPacket(ctx, batch->pkts()[i], 0);
    // }


    if (backpressure_ && llring_count(queue_) < low_water_) {
      SignalUnderload();
    }

    std::cout << "BufferedQueue queue value in after: " + std::to_string(llring_count(queue_)) << std::endl;


  }

  return {.block = false,
          .packets = cnt,
          .bits = (total_bytes + cnt * pkt_overhead) * 8};
}

CommandResponse BufferedQueue::CommandSetBurst(
    const sample::buffered_queue::pb::BufferedQueueCommandSetBurstArg &arg) {
  uint64_t burst = arg.burst();

  if (burst > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "burst size must be [0,%zu]",
                          bess::PacketBatch::kMaxBurst);
  }

  burst_ = burst;
  return CommandSuccess();
}

CommandResponse BufferedQueue::SetSize(uint64_t size) {
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

CommandResponse BufferedQueue::CommandSetSize(
    const sample::buffered_queue::pb::BufferedQueueCommandSetSizeArg &arg) {
  return SetSize(arg.size());
}

CommandResponse BufferedQueue::CommandGetStatus(
    const sample::buffered_queue::pb::BufferedQueueCommandGetStatusArg &) {
  sample::buffered_queue::pb::BufferedQueueCommandGetStatusResponse resp;
  resp.set_count(llring_count(queue_));
  resp.set_size(size_);
  resp.set_enqueued(stats_.enqueued);
  resp.set_dequeued(stats_.dequeued);
  resp.set_dropped(stats_.dropped);
  return CommandSuccess(resp);
}

void BufferedQueue::AdjustWaterLevels() {
  high_water_ = static_cast<uint64_t>(size_ * kHighWaterRatio);
  low_water_ = static_cast<uint64_t>(size_ * kLowWaterRatio);
}

CheckConstraintResult BufferedQueue::CheckModuleConstraints() const {
  CheckConstraintResult status = CHECK_OK;
  if (num_active_tasks() - tasks().size() < 1) {  // Assume multi-producer.
    LOG(ERROR) << "BufferedQueue has no producers";
    status = CHECK_NONFATAL_ERROR;
  }

  if (tasks().size() > 1) {  // Assume single consumer.
    LOG(ERROR) << "More than one consumer for the queue" << name();
    return CHECK_FATAL_ERROR;
  }

  return status;
}

ADD_MODULE(BufferedQueue, "buffered_queue",
           "terminates current task and enqueue packets for new task")
