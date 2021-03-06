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

#include "sender_buffered_queue.h"

#include <cstdlib>

#include "../utils/format.h"

#define DEFAULT_SenderBufferedQueue_SIZE 1024

struct MDCData
{
    char name[50];
    int age;
    float salary;
};

const Commands SenderBufferedQueue::cmds = {
    {"set_burst", "SenderBufferedQueueCommandSetBurstArg",
     MODULE_CMD_FUNC(&SenderBufferedQueue::CommandSetBurst), Command::THREAD_SAFE},
    {"set_size", "SenderBufferedQueueCommandSetSizeArg",
     MODULE_CMD_FUNC(&SenderBufferedQueue::CommandSetSize), Command::THREAD_UNSAFE},
    {"get_status", "SenderBufferedQueueCommandGetStatusArg",
     MODULE_CMD_FUNC(&SenderBufferedQueue::CommandGetStatus), Command::THREAD_SAFE}};

int SenderBufferedQueue::Resize(int slots) {
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

CommandResponse SenderBufferedQueue::Init(const sample::sender_buffered_queue::pb::SenderBufferedQueueArg &arg) {
  sendto_ = false;
  data_ready_ = false;
  data_receiving_ = false;
  data_size_ = 0;
  curr_data_size_ = 0;

  task_id_t tid;
  CommandResponse err;

  tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "Task creation failed");
  }

  burst_ = bess::PacketBatch::kMaxBurst;

  if (arg.backpressure()) {
    VLOG(1) << "Backpressure enabled for " << name() << "::SenderBufferedQueue";
    backpressure_ = true;
  }

  if (arg.size() != 0) {
    err = SetSize(arg.size());
    if (err.error().code() != 0) {
      return err;
    }
  } else {
    int ret = Resize(DEFAULT_SenderBufferedQueue_SIZE);
    if (ret) {
      return CommandFailure(-ret);
    }
  }

  if (arg.prefetch()) {
    prefetch_ = true;
  }

  return CommandSuccess();
}

void SenderBufferedQueue::DeInit() {
  bess::Packet *pkt;

  if (queue_) {
    while (llring_sc_dequeue(queue_, (void **)&pkt) == 0) {
      bess::Packet::Free(pkt);
    }
    std::free(queue_);
  }
}

std::string SenderBufferedQueue::GetDesc() const {
  const struct llring *ring = queue_;

  return bess::utils::Format("%u/%u", llring_count(ring), ring->common.slots);
}

/* from upstream */
void SenderBufferedQueue::ProcessBatch(Context *, bess::PacketBatch *batch) {
  // std::cout << "Setsize resize: " + std::to_string(Resize(4)) << std::endl;

  int cnt = batch->cnt();


  for (int i = 0; i < cnt; i++) {

    bess::Packet *pkt = batch->pkts()[i];
    Ethernet *eth = pkt->head_data<Ethernet *>();
    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);

    int ip_bytes = ip->header_length << 2;
//        Udp *udp = reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);
    // Access UDP payload (i.e., mDC data)
    be64_t *p = pkt->head_data<be64_t *>(sizeof(Ethernet) + ip_bytes + sizeof(Udp)+8);


      std::cout << "SWITCH";
       std::cout << std::hex << static_cast<int>(p->value()) << std::endl;
       std::cout << std::hex << static_cast<int>(p->raw_value()) << std::endl;
       std::cout << sizeof(Ethernet) + ip_bytes + sizeof(Udp) << std::endl;


    // Data pkts
    uint8_t sn = (p->raw_value() & 0xff);
    uint8_t mul_type = (p->raw_value() & 0xff00)  >> 8;
    uint8_t data_size = (p->raw_value() & 0xff0000)  >> 16;
    
    uint16_t address = (p->raw_value() & 0x0000ffff);
    uint8_t appID = (p->raw_value() & 0xff00000000) >> 32;



    std::cout << "SenderBufferedQueue address :::::" << std::endl;
    std::cout << std::hex << static_cast<int>(address) << std::endl;
    std::cout << "SenderBufferedQueue appID :::::" << std::endl;
    std::cout << std::hex << static_cast<int>(appID) << std::endl;
    std::cout << "SenderBufferedQueue sn :::::" << std::endl;
    std::cout << std::hex << static_cast<int>(sn) << std::endl;
    std::cout << "SenderBufferedQueue type :::::" << std::endl;
    std::cout << std::hex << static_cast<int>(mul_type) << std::endl;
    std::cout << "SenderBufferedQueue type: " + std::to_string(mul_type)<< std::endl;
    std::cout << "SenderBufferedQueue size: " + std::to_string(data_size)<< std::endl;
    std::cout << std::hex << static_cast<int>(data_size) << std::endl;
    std::cout << std::hex << static_cast<int>(data_size+1) << std::endl;


    curr_ = sn;
    if(!data_receiving_){
      initial_ = sn;
      prior_ = sn;
      data_receiving_ = true;
      data_size_ = data_size;
      std::cout << "SenderBufferedQueue initial"  + std::to_string(data_receiving_)<< std::endl;
    }
    else{
      if(curr_ == (prior_+1)%data_size_ ){
        prior_ = curr_;
        curr_data_size_++;

        if(curr_data_size_ == data_size_){
            data_ready_ = true;
        }

      }
      else if(curr_ > (prior_+1)%data_size_  || curr_ <= initial_){
        std::cout << "Case 2" << std::endl;
        return;

      }
      else{
        std::cout << "Case 3" << std::endl;
        return;

      }
    }

    std::cout << "SenderBufferedQueue initial_: " + std::to_string(initial_)<< std::endl;
    std::cout << "SenderBufferedQueue prior_: " + std::to_string(prior_)<< std::endl;
    std::cout << "SenderBufferedQueue curr_: " + std::to_string(curr_)<< std::endl;

  }







  int queued =
      llring_mp_enqueue_burst(queue_, (void **)batch->pkts(), batch->cnt());

  std::cout << "Queued value in llring_mp_enqueue_burst: " + std::to_string(queued) << std::endl;

  std::cout << "Queued value in llring_count: " + std::to_string(llring_count(queue_)) << std::endl;

  if (backpressure_ && llring_count(queue_) > high_water_) {
    SignalOverload();
  }

  stats_.enqueued += queued; // Total enqueued stat

  // Drop if batch size is larger than queued packet in this round
  if (queued < batch->cnt()) {
    int to_drop = batch->cnt() - queued;
    stats_.dropped += to_drop;
    bess::Packet::Free(batch->pkts() + queued, to_drop);
  }

  if(llring_count(queue_) >= 100){
    sendto_ = true;
  }

}

/* to downstream */
struct task_result SenderBufferedQueue::RunTask(Context *ctx, bess::PacketBatch *batch,
                                  void *) {

  // std::cout << "SenderBufferedQueue RunTask: " + std::to_string(llring_count(queue_)) << std::endl;

  if (children_overload_ > 0 ) {
    return {
        .block = true, .packets = 0, .bits = 0,
    };
  }

  const int burst = ACCESS_ONCE(burst_);
  const int pkt_overhead = 24;
  uint32_t cnt = 0;
  uint64_t total_bytes = 0;

  if (sendto_){
    std::cout << "SenderBufferedQueue queue value in before: " + std::to_string(llring_count(queue_)) << std::endl;

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

    std::cout << "SenderBufferedQueue queue batch value in during: " + std::to_string(batch->cnt()) << std::endl;

    RunChooseModule(ctx, 0, batch);

    if(llring_count(queue_) <= 0){
      sendto_ = false;
    }

    // for (uint32_t i = 0; i < cnt; i++) {
    //   std::cout << "SenderBufferedQueue sending packet: " + std::to_string(i) << std::endl;
    //   EmitPacket(ctx, batch->pkts()[i], 0);
    // }


    if (backpressure_ && llring_count(queue_) < low_water_) {
      SignalUnderload();
    }

    std::cout << "SenderBufferedQueue queue value in after: " + std::to_string(llring_count(queue_)) << std::endl;


  }

  return {.block = false,
          .packets = cnt,
          .bits = (total_bytes + cnt * pkt_overhead) * 8};
}

CommandResponse SenderBufferedQueue::CommandSetBurst(
    const sample::sender_buffered_queue::pb::SenderBufferedQueueCommandSetBurstArg &arg) {
  uint64_t burst = arg.burst();

  if (burst > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "burst size must be [0,%zu]",
                          bess::PacketBatch::kMaxBurst);
  }

  burst_ = burst;
  return CommandSuccess();
}

CommandResponse SenderBufferedQueue::SetSize(uint64_t size) {
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

CommandResponse SenderBufferedQueue::CommandSetSize(
    const sample::sender_buffered_queue::pb::SenderBufferedQueueCommandSetSizeArg &arg) {
  return SetSize(arg.size());
}

CommandResponse SenderBufferedQueue::CommandGetStatus(
    const sample::sender_buffered_queue::pb::SenderBufferedQueueCommandGetStatusArg &) {
  sample::sender_buffered_queue::pb::SenderBufferedQueueCommandGetStatusResponse resp;
  resp.set_count(llring_count(queue_));
  resp.set_size(size_);
  resp.set_enqueued(stats_.enqueued);
  resp.set_dequeued(stats_.dequeued);
  resp.set_dropped(stats_.dropped);
  return CommandSuccess(resp);
}

void SenderBufferedQueue::AdjustWaterLevels() {
  high_water_ = static_cast<uint64_t>(size_ * kHighWaterRatio);
  low_water_ = static_cast<uint64_t>(size_ * kLowWaterRatio);
}

CheckConstraintResult SenderBufferedQueue::CheckModuleConstraints() const {
  CheckConstraintResult status = CHECK_OK;
  if (num_active_tasks() - tasks().size() < 1) {  // Assume multi-producer.
    LOG(ERROR) << "SenderBufferedQueue has no producers";
    status = CHECK_NONFATAL_ERROR;
  }

  if (tasks().size() > 1) {  // Assume single consumer.
    LOG(ERROR) << "More than one consumer for the queue" << name();
    return CHECK_FATAL_ERROR;
  }

  return status;
}

ADD_MODULE(SenderBufferedQueue, "sender_buffered_queue",
           "terminates current task and enqueue packets for new task")
