/*
         Copyright 2016 Massachusetts Institute of Technology

         Licensed under the Apache License, Version 2.0 (the "License");
         you may not use this file except in compliance with the License.
         You may obtain a copy of the License at

                         http://www.apache.org/licenses/LICENSE-2.0

         Unless required by applicable law or agreed to in writing, software
         distributed under the License is distributed on an "AS IS" BASIS,
         WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
         See the License for the specific language governing permissions and
         limitations under the License.
*/

#include "io_thread.h"

#include "client_txn.h"
#include "global.h"
#include "helper.h"
#include "manager.h"
#include "math.h"
#include "mem_alloc.h"
#include "message.h"
#include "msg_queue.h"
#include "msg_thread.h"
#include "query.h"
#include "thread.h"
#include "tpcc_query.h"
#include "transport.h"
#include "txn.h"
#include "work_queue.h"
#include "ycsb.h"
#include "ycsb_query.h"

void InputThread::setup() {
  std::vector<Message *> *msgs;
  while (!simulation->is_setup_done()) {
#ifdef USE_RDMA
    msgs = tport_man.rdma_recv_msg(get_thd_id());
#else
    msgs = tport_man.recv_msg(get_thd_id());
#endif
    if (msgs == NULL) continue;
    while (!msgs->empty()) {
      Message *msg = msgs->front();
      if (msg->rtype == INIT_DONE) {
        // g_init_done[g_node_id] = 1;
        g_init_done[msg->return_node_id] = true;
        printf("Received INIT_DONE from node %ld\n", msg->return_node_id);
        fflush(stdout);
        simulation->process_setup_msg();
      } else if (msg->rtype == HEART_BEAT) {
        heartbeat_queue.enqueue(get_thd_id(), msg, false);
        msgs->erase(msgs->begin());
        continue;
      } else if (msg->rtype == STATS_COUNT) {
        stats_queue.enqueue(get_thd_id(), msg, false);
        msgs->erase(msgs->begin());
        continue;
      } else if (msg->rtype == RECOVERY) {
        recover_queue.enqueue(get_thd_id(), msg, false);
        msgs->erase(msgs->begin());
        continue;
      } else {
        assert(ISSERVER || ISREPLICA);
        // printf("Received Msg %d from node %ld\n",msg->rtype,msg->return_node_id);
#if CC_ALG == CALVIN
        if (msg->rtype == CALVIN_ACK || (msg->rtype == CL_QRY && ISCLIENTN(msg->get_return_id())) ||
            (msg->rtype == CL_QRY_O && ISCLIENTN(msg->get_return_id()))) {
          work_queue.sequencer_enqueue(get_thd_id(), msg);
          msgs->erase(msgs->begin());
          continue;
        }
        if (msg->rtype == RDONE || msg->rtype == CL_QRY || msg->rtype == CL_QRY_O) {
          assert(ISSERVERN(msg->get_return_id()));
          work_queue.sched_enqueue(get_thd_id(), msg);
          msgs->erase(msgs->begin());
          continue;
        }
#endif
        work_queue.enqueue(get_thd_id(), msg, false);
      }
      msgs->erase(msgs->begin());
    }
    delete msgs;
  }
  if (!ISCLIENT) {
    txn_man = (YCSBTxnManager *)mem_allocator.align_alloc(sizeof(YCSBTxnManager));
    new (txn_man) YCSBTxnManager();
    // txn_man = (TxnManager*) malloc(sizeof(TxnManager));
    uint64_t thd_id = get_thd_id();
    txn_man->init(thd_id, NULL);
  }
}

RC InputThread::run() {
  tsetup();
  printf("Running InputThread %ld\n", _thd_id);

  if (ISCLIENT) {
    client_recv_loop();
  } else {
    server_recv_loop();
  }

  return FINISH;
}

RC InputThread::client_recv_loop() {
  int rsp_cnts[g_servers_per_client];
  memset(rsp_cnts, 0, g_servers_per_client * sizeof(int));

  run_starttime = get_sys_clock();
  uint64_t return_node_offset;
  uint64_t inf;

  std::vector<Message *> *msgs;

  while (!simulation->is_done()) {
    heartbeat();
    uint64_t starttime = get_sys_clock();
#ifdef USE_RDMA
    msgs = tport_man.rdma_recv_msg(get_thd_id());
#else
    msgs = tport_man.recv_msg(get_thd_id());
#endif
    INC_STATS(_thd_id, mtx[28], get_sys_clock() - starttime);
    starttime = get_sys_clock();
    // while((m_query = work_queue.get_next_query(get_thd_id())) != NULL) {
    // Message * msg = work_queue.dequeue();
    if (msgs == NULL) continue;
    while (!msgs->empty()) {
      Message *msg = msgs->front();
      assert(msg->rtype == CL_RSP);
#if ONE_NODE_RECIEVE == 1
      return_node_offset = msg->return_node_id;
#else
      return_node_offset = msg->return_node_id - g_server_start_node;
#endif
      assert(return_node_offset < g_servers_per_client);
      rsp_cnts[return_node_offset]++;
      INC_STATS(get_thd_id(), txn_cnt, 1);
      uint64_t timespan = get_sys_clock() - ((ClientResponseMessage *)msg)->client_startts;
      INC_STATS(get_thd_id(), txn_run_time, timespan);
      if (warmup_done) {
        INC_STATS_ARR(get_thd_id(), client_client_latency, timespan);
      }
      // INC_STATS_ARR(get_thd_id(),all_lat,timespan);
      inf = client_man.dec_inflight(return_node_offset);
      DEBUG("Recv %ld from %ld, %ld -- %f\n", ((ClientResponseMessage *)msg)->txn_id,
            msg->return_node_id, inf, float(timespan) / BILLION);
      assert(inf >= 0);
      // delete message here
      msgs->erase(msgs->begin());
    }
    delete msgs;
    INC_STATS(_thd_id, mtx[29], get_sys_clock() - starttime);
  }

  printf("FINISH %ld:%ld\n", _node_id, _thd_id);
  fflush(stdout);
  return FINISH;
}

RC InputThread::server_recv_loop() {
  myrand rdm;
  rdm.init(get_thd_id());
  RC rc = RCOK;
  assert(rc == RCOK);
  uint64_t starttime;

  std::vector<Message *> *msgs;
  // #if RDMA_ONE_SIDE == true && SERVER_GENERATE_QUERIES == true
  // 	while (!simulation->is_setup_done()) {
  // #else
  while (!simulation->is_done()) {
    // #endif
    heartbeat();
    starttime = get_sys_clock();

#ifdef USE_RDMA
    msgs = tport_man.rdma_recv_msg(get_thd_id());
#else
    msgs = tport_man.recv_msg(get_thd_id());
#endif
    INC_STATS(_thd_id, mtx[28], get_sys_clock() - starttime);
    starttime = get_sys_clock();

    if (msgs == NULL) continue;
    while (!msgs->empty()) {
      Message *msg = msgs->front();
      if (msg->rtype == INIT_DONE) {
        msgs->erase(msgs->begin());
        continue;
      } else if (msg->rtype == HEART_BEAT) {
        int center_id = static_cast<int>(msg->return_center_id);
        heartbeat_queue.enqueue(get_thd_id(), msg, false);
        msgs->erase(msgs->begin());
        continue;
      } else if (msg->rtype == STATS_COUNT) {
        stats_queue.enqueue(get_thd_id(), msg, false);
        msgs->erase(msgs->begin());
        continue;
      } else if (msg->rtype == RECOVERY) {
        recover_queue.enqueue(get_thd_id(), msg, false);
        msgs->erase(msgs->begin());
        continue;
      }
#if CC_ALG == CALVIN
      if (msg->rtype == CALVIN_ACK || (msg->rtype == CL_QRY && ISCLIENTN(msg->get_return_id())) ||
          (msg->rtype == CL_QRY_O && ISCLIENTN(msg->get_return_id()))) {
        work_queue.sequencer_enqueue(get_thd_id(), msg);
        msgs->erase(msgs->begin());
        continue;
      }
      if (msg->rtype == RDONE || msg->rtype == CL_QRY || msg->rtype == CL_QRY_O) {
        assert(ISSERVERN(msg->get_return_id()));
        work_queue.sched_enqueue(get_thd_id(), msg);
        msgs->erase(msgs->begin());
        continue;
      }
#endif
      DEBUG_T("Receive Msg %d, (%ld,%ld) from %ld\n", msg->rtype, msg->txn_id, msg->batch_id,
              msg->return_node_id);
      work_queue.enqueue(get_thd_id(), msg, false);
      msgs->erase(msgs->begin());
    }
    delete msgs;
    INC_STATS(_thd_id, mtx[29], get_sys_clock() - starttime);
  }
  printf("FINISH %ld:%ld\n", _node_id, _thd_id);
  fflush(stdout);
  return FINISH;
}

void OutputThread::setup() {
  DEBUG_M("OutputThread::setup MessageThread alloc\n");
  messager = (MessageThread *)mem_allocator.alloc(sizeof(MessageThread));
  messager->init(_thd_id);
  while (!simulation->is_setup_done()) {
    messager->run();
  }
}

RC OutputThread::run() {
  tsetup();
  printf("Running OutputThread %ld\n", _thd_id);
  // #if RDMA_ONE_SIDE == true && SERVER_GENERATE_QUERIES == true
  // 	while (!simulation->is_setup_done()) {
  // #else
  while (!simulation->is_done()) {
    // #endif
    heartbeat();
    messager->run();
  }

  // extra_wait_time should be as small as possible
  printf("FINISH %ld:%ld, extra wait time: %lu(ns)\n", _node_id, _thd_id, extra_wait_time);
  fflush(stdout);
  return FINISH;
}
