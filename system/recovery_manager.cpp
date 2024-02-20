
#include "recovery_manager.h"

#include "global.h"
#include "helper.h"
#include "manager.h"
#include "math.h"
#include "mem_alloc.h"
#include "message.h"
#include "msg_queue.h"
#include "msg_thread.h"
#include "netinet/ip_icmp.h"
#include "query.h"
#include "rdma.h"
#include "route_table.h"
#include "thread.h"
#include "transport.h"
#include "work_queue.h"

void HeartBeatThread::setup() {
  std::thread([&]() { tcp_listen(); }).detach();
}

RC HeartBeatThread::run() {
  tsetup();
  printf("Running HeartBeatThread %ld\n", _thd_id);
  heartbeat_loop_new();
  return FINISH;
}

RC HeartBeatThread::heartbeat_loop() {
  myrand rdm;
  rdm.init(get_thd_id());
  RC rc = RCOK;
  assert(rc == RCOK);
  uint64_t starttime, lastsendtime = 0;
  uint64_t now;
  Message* msg;
  while (!simulation->is_done()) {
    now = get_wall_clock();
    node_status.set_node_status(g_node_id, OnCall, get_thd_id());
    if (now - lastsendtime > HEARTBEAT_TIME) {
      if (is_center_primary(g_node_id)) {
        // leader send to other leaders
        // DEBUG_H("Node %ld is center %d primary\n", g_node_id, GET_CENTER_ID(g_node_id));
        send_tcp_heart_beat();
      } else {
        // slaves send to their own leaders
        uint64_t center_id = GET_CENTER_ID(g_node_id);
        uint64_t dest_id = get_center_primary(center_id);
        send_rdma_heart_beat(dest_id);
      }
      lastsendtime = get_wall_clock();
    }
    if (is_center_primary(g_node_id)) {
      // check_for_same_center();  // handle the failure in the same data center.

      msg = heartbeat_queue.dequeue(get_thd_id());
      if (msg) {
        HeartBeatMessage* hmsg = (HeartBeatMessage*)msg;
        // update_node_and_route_new(hmsg->heartbeatmsg, hmsg->return_node_id);

        PRINT_HEARTBEAT("\nheartbeat from node %d\n", hmsg->return_node_id);
        PRINT_HEARTBEAT("need_flush_route_ = %d\n", hmsg->need_flush_route_);
        hmsg->heartbeatmsg.printRouteTable();
        PRINT_HEARTBEAT("node %d local route table and status:\n", g_node_id);
        route_table.printRouteTable();
        for (int i = 0; i < NODE_CNT; i++) {
          PRINT_HEARTBEAT("node %d last_ts: %lu, status %d\n", i,
                          node_status.get_node_status(i)->last_ts,
                          node_status.get_node_status(i)->status);
        }

        if (hmsg->need_flush_route_) {
          update_node_and_route_new(hmsg->heartbeatmsg, hmsg->return_node_id);
        }

        // handle the failure in the different data center.
        if (g_node_id == 0) {
          // DEBUG_H("Node %ld is global primary\n", g_node_id);
          // check_for_other_center();
          delete msg;
        }
      }
    }
  }
  // printf("FINISH %ld:%ld\n",_node_id,_thd_id);
  // fflush(stdout);
  return FINISH;
}

RC HeartBeatThread::heartbeat_loop_new() {
  myrand rdm;
  rdm.init(get_thd_id());
  RC rc = RCOK;
  assert(rc == RCOK);
  uint64_t starttime, lastsendtime = 0;
  uint64_t now;
  Message* msg;
  uint64_t last_collect_time = get_wall_clock();
  while (!simulation->is_done()) {
    now = get_wall_clock();
    node_status.set_node_status(g_node_id, OnCall, get_thd_id());

    // send heartbeat
    if (now - lastsendtime > HEARTBEAT_TIME) {
      if (is_center_primary(g_node_id)) {
        // center leaders send to other leaders
        send_tcp_heart_beat(false);
      } else {
        // in a center, followers send to their own leader
        uint64_t center_id = GET_CENTER_ID(g_node_id);
        uint64_t dest_id = get_center_primary(center_id);
        send_rdma_heart_beat(dest_id);
      }
      lastsendtime = get_wall_clock();
    }

    // send statics
    if (now - last_collect_time > COLLECT_TIME) {
      send_stats();
      last_collect_time = get_wall_clock();
      // node 0 receives statics message and generates plan
      if (g_node_id == 0) {
        sleep(1);
        PRINT_HEARTBEAT("\n***node 0 collect statics...\n");
        int access_collector[CENTER_CNT][PART_CNT];
        int latency_collector[CENTER_CNT][CENTER_CNT];
        int score[PART_CNT][CENTER_CNT];
        memset(access_collector, 0, sizeof(access_collector));
        memset(latency_collector, 0, sizeof(latency_collector));
        memset(score, 0, sizeof(latency_collector));

        // summary statics
        while (msg = stats_queue.dequeue(get_thd_id())) {
          auto stats_msg = dynamic_cast<StatsCountMessage*>(msg);
          PRINT_HEARTBEAT("\n");
          stats_msg->printAccessCount();
          stats_msg->printLatency();
          for (int i = 0; i < PART_CNT; i++) {
            access_collector[stats_msg->return_center_id][i] += stats_msg->access_count_[i];
            PRINT_HEARTBEAT("access_collector[%d][%d]:%d\n", stats_msg->return_center_id, i,
                            access_collector[stats_msg->return_center_id][i]);
          }
          for (int i = 0; i < CENTER_CNT; i++) {
            latency_collector[stats_msg->return_center_id][i] = stats_msg->latency_[i];
            PRINT_HEARTBEAT("latency_collector[%d][%d]:%d\n", stats_msg->return_center_id, i,
                            latency_collector[stats_msg->return_center_id][i]);
          }
        }

        // calculate score and build set
        struct PartitionInformation {
          int partition_id;
          int target_location;
          int score;
          auto operator<(const PartitionInformation& other) const -> bool {
            return score < other.score;
          }
        };
        set<PartitionInformation> next_partition;
        unordered_map<int, int> remain_partitions;

        int temp = 0;
        for (int partition_idx = 0; partition_idx < PART_CNT; partition_idx++) {
          for (int location = 0; location < CENTER_CNT; location++) {
            for (int access_location = 0; access_location < CENTER_CNT; access_location++) {
              temp += access_collector[access_location][partition_idx] *
                      latency_collector[access_location][location];
            }
            score[partition_idx][location] = temp;
            next_partition.emplace(PartitionInformation{partition_idx, location, temp});
            temp = 0;
          }
          remain_partitions.emplace(partition_idx, REPLICA_COUNT);
        }

        // print score
        PRINT_HEARTBEAT("\nscore:\n");
        for (int i = 0; i < PART_CNT; i++) {
          for (int j = 0; j < CENTER_CNT; j++) {
            PRINT_HEARTBEAT("p%d.d%d.%d ", i, j, score[i][j]);
          }
          PRINT_HEARTBEAT("\n");
        }

        // generate plan
        int plan[PART_CNT][REPLICA_COUNT];
        fill(&plan[0][0], &plan[0][0] + PART_CNT * REPLICA_COUNT, -1);
        for (auto iterator = next_partition.begin(); iterator != next_partition.end(); iterator++) {
          // check if the partition has no more replica
          if (remain_partitions.find(iterator->partition_id)->second == 0) {
            continue;
          }
          for (int replica_idx = 0; replica_idx < REPLICA_COUNT; replica_idx++) {
            if (plan[iterator->partition_id][replica_idx] == -1) {
              plan[iterator->partition_id][replica_idx] = iterator->target_location;
              score[iterator->partition_id][iterator->target_location] = -1;
              auto map_iterator = remain_partitions.find(iterator->partition_id);
              map_iterator->second -= 1;
              break;
            }
          }
        }

        PRINT_HEARTBEAT("\nplan:\n");
        for (int i = 0; i < PART_CNT; i++) {
          for (int j = 0; j < REPLICA_COUNT; j++) {
            PRINT_HEARTBEAT("p%d.r%d.%d ", i, j, plan[i][j]);
          }
          PRINT_HEARTBEAT("\n");
        }

        // update local route table
        PRINT_HEARTBEAT("\nlocal route table and status:\n");
        route_table.printRouteTable();
        node_status.printStatusTable();
        PRINT_HEARTBEAT("update local route table\n");
        for (int partition_idx = 0; partition_idx < PART_CNT; partition_idx++) {
          for (int replica_idx = 0; replica_idx < REPLICA_COUNT; replica_idx++) {
            route_table.set_route_node_new(replica_idx, partition_idx,
                                           plan[partition_idx][replica_idx]);
          }
        }
        PRINT_HEARTBEAT("node 0 local route table and status:\n");
        route_table.printRouteTable();
        node_status.printStatusTable();
        // send heartbeat containing route table
        send_tcp_heart_beat(true);
      }
      PRINT_HEARTBEAT("***collect finish\n\n");
    }

    if (is_center_primary(g_node_id)) {
      msg = heartbeat_queue.dequeue(get_thd_id());
      if (msg) {
        auto heartbeat_message = dynamic_cast<HeartBeatMessage*>(msg);

        PRINT_HEARTBEAT("\nheartbeat from node %d\n", heartbeat_message->return_node_id);
        PRINT_HEARTBEAT("need_flush_route_ = %d\n", heartbeat_message->need_flush_route_);
        heartbeat_message->heartbeatmsg.printRouteTable();
        PRINT_HEARTBEAT("node %d local route table and status:\n", g_node_id);
        route_table.printRouteTable();
        node_status.printStatusTable();

        if (heartbeat_message->need_flush_route_) {
          update_node_and_route_new(heartbeat_message->heartbeatmsg,
                                    heartbeat_message->return_node_id);
        }
      }
    }
  }
  return FINISH;
}

RC HeartBeatThread::send_rdma_heart_beat(uint64_t dest_id) {
  PRINT_HEARTBEAT("call send_rdma_heart_beat\n");
  write_remote_heartbeat(dest_id);
  RouteAndStatus result = read_remote_status(dest_id);
  update_node_and_route(result, dest_id);
  return RCOK;
}

RC HeartBeatThread::send_tcp_heart_beat(bool need_flush) {
  for (int i = 0; i < CENTER_CNT; i++) {
    uint64_t dest_id = get_center_primary(i);
    if (dest_id == g_node_id) continue;  // no need to send heartbeat to itself
    if (dest_id == -1) continue;
    auto message =
        Message::create_message(route_table.table, node_status.table, need_flush, HEART_BEAT);
    msg_queue.enqueue(get_thd_id(), message, dest_id);
    DEBUG_H("Node %ld send TCP heartbeat to %ld\n", g_node_id, dest_id);
  }
  return RCOK;
}

RC HeartBeatThread::send_stats() {
  auto ips = tport_man.get_ip();
  for (int i = 0; i < CENTER_CNT; i++) {
    if (i != g_node_id) {
      auto time = tcp_ping(ips[i]);
      PRINT_HEARTBEAT("%d -> %d: %d ms\n", g_node_id, i, time);
      if (time == 0) {
        latency[i] = 1;
      } else {
        latency[i] = time;
      }
    }
  }
  auto message = Message::create_message(access_count, latency, STATS_COUNT);
  if (g_node_id != 0) {
    msg_queue.enqueue(get_thd_id(), message, 0);
  } else {
    stats_queue.enqueue(get_thd_id(), message, false);
  }
  memset(access_count, 0, sizeof(access_count));
  PRINT_HEARTBEAT("\n***Node %ld send stats to 0\n\n", g_node_id);
  return RCOK;
}

RouteAndStatus HeartBeatThread::read_remote_status(uint64_t target_server) {
  uint64_t operate_size = SIZE_OF_ROUTE + SIZE_OF_STATUS;
  // sizeof(route_table_node) + sizeof(status_node);
  uint64_t thd_id = get_thd_id();
  uint64_t remote_offset = rdma_buffer_size - rdma_log_size - rdma_routetable_size;
  char* local_buf = Rdma::get_status_client_memory(thd_id);

  uint64_t starttime;
  uint64_t endtime;
  starttime = get_sys_clock();
  auto res_s = rc_qp[target_server][thd_id]->send_normal(
      {.op = IBV_WR_RDMA_READ, .flags = IBV_SEND_SIGNALED, .len = operate_size, .wr_id = 0},
      {.local_addr = reinterpret_cast<rdmaio::RMem::raw_ptr_t>(local_buf),
       .remote_addr = remote_offset,
       .imm_data = 0});
  RDMA_ASSERT(res_s == rdmaio::IOCode::Ok);
  auto res_p = rc_qp[target_server][thd_id]->wait_one_comp(RDMA_CALLS_TIMEOUT);
  // RDMA_ASSERT(res_p == rdmaio::IOCode::Ok);
  if (res_p != rdmaio::IOCode::Ok) {
    // todo: handle error.
    node_status.set_node_status(target_server, NS::Failure, get_thd_id());
    DEBUG_T("Thd %ld send RDMA one-sided failed--read RDMA heartbeat %ld.\n", get_thd_id(),
            target_server);
    DEBUG_H("Center primary node %ld failed, because read RDMA heartbeat failed, result %d\n",
            target_server, res_p.code);
  }
  DEBUG_H("HEARTBEAT read primary node status\n");
  endtime = get_sys_clock();

  status_node* temp_status = (status_node*)mem_allocator.alloc(SIZE_OF_STATUS);
  route_table_node* temp_route = (route_table_node*)mem_allocator.alloc(SIZE_OF_ROUTE);
  memcpy(temp_route, local_buf, SIZE_OF_ROUTE);
  memcpy(temp_status, local_buf + SIZE_OF_ROUTE, SIZE_OF_STATUS);

  RouteAndStatus result;
  result._status = temp_status;
  result._route = temp_route;
  return result;
}

bool HeartBeatThread::write_remote_heartbeat(uint64_t target_server) {
  uint64_t thd_id = get_thd_id();

  char* local_buf = Rdma::get_status_client_memory(thd_id);

  uint64_t time = get_wall_clock();
  uint64_t operate_size = sizeof(uint64_t);
  memset(local_buf, 0, operate_size);
  memcpy(local_buf, &time, operate_size);
  uint64_t remote_offset = rdma_buffer_size - rdma_log_size - rdma_routetable_size;
  remote_offset += SIZE_OF_ROUTE + sizeof(status_node) * g_node_id + sizeof(NS);

  auto res_s = rc_qp[target_server][thd_id]->send_normal(
      {.op = IBV_WR_RDMA_WRITE, .flags = IBV_SEND_SIGNALED, .len = operate_size, .wr_id = 0},
      {.local_addr = reinterpret_cast<rdmaio::RMem::raw_ptr_t>(local_buf),
       .remote_addr = remote_offset,
       .imm_data = 0});
  RDMA_ASSERT(res_s == rdmaio::IOCode::Ok);
  DEBUG_H("Node %ld issue rdma heartbeat to %ld\n", g_node_id, target_server);
  auto res_p = rc_qp[target_server][thd_id]->wait_one_comp(RDMA_CALLS_TIMEOUT);
  if (res_p != rdmaio::IOCode::Ok) {
    // todo: handle error.
    node_status.set_node_status(target_server, NS::Failure, get_thd_id());
    DEBUG_T("Thd %ld send RDMA one-sided failed--write RDMA heartbeat %ld.\n", get_thd_id(),
            target_server);
    DEBUG_H("Center primary node %ld failed, because write RDMA heartbeat failed, result %d\n",
            target_server, res_p);
    return false;
  }
  DEBUG_H("Node %ld issue rdma heartbeat to %ld success\n", g_node_id, target_server);
  return true;
}

RC HeartBeatThread::check_for_same_center() {
  uint64_t node_cnt_per_center = g_node_cnt / g_center_cnt;
  uint64_t center_id = GET_CENTER_ID(g_node_id);
  for (int i = 0; i < node_cnt_per_center; i++) {
    uint64_t dest_id = center_id + i * g_center_cnt;
    status_node* st = node_status.get_node_status(dest_id);
    bool is_alive = st->status == OnCall;
    if (dest_id == g_node_id) {
      assert(is_alive);
      continue;
    }

    uint64_t time = get_sys_clock();
    if (time > st->last_ts && time - st->last_ts > SAME_CENTER_FAILED_TIME) {
      DEBUG_H(
          "Node %ld find in same data center node %ld status Failure due to timeout %lu : %lu\n",
          g_node_id, dest_id, time, st->last_ts);
      // update the status of failed node.
      node_status.set_node_status(dest_id, NS::Failure, get_thd_id());
      is_alive = false;
    }
    if (!is_alive) {
      // DEBUG_H("Node %ld find in same data center node %ld status Failure %d\n", g_node_id,
      // dest_id, st->status); recover the replica on failed node.
      generate_recovery_msg(dest_id);
    }
  }
}

RC HeartBeatThread::check_for_other_center() {
  uint64_t node_cnt_per_center = g_node_cnt / g_center_cnt;
  for (int j = 0; j < g_center_cnt; j++) {
    uint64_t dest_id = get_center_primary(j);
    status_node* st = node_status.get_node_status(dest_id);
    bool is_alive = st->status == NS::OnCall;
    if (dest_id == g_node_id) {
      assert(is_alive);
      continue;
    }
    // Check whether other data center is alive.
    if (dest_id != -1) {
      uint64_t time = get_wall_clock();
      // get_wall_clock();
      if (time > st->last_ts && time - st->last_ts > INTER_CENTER_FAILED_TIME) {
        is_alive = false;
      }
      DEBUG_H("Node primary check node %lu time %lu %lu\n", dest_id, time, st->last_ts);
    } else {
      is_alive = false;
      DEBUG_H("Node primary check no node alive in data center %d\n", j);
    }
    // if dead
    if (!is_alive) {
      // update the status of failed node.
      for (int i = 0; i < node_cnt_per_center; i++) {
        uint64_t dest_id2 = j + i * g_center_cnt;
        if (dest_id2 == g_node_id) continue;
        uint64_t time = get_wall_clock();
        status_node* st = node_status.get_node_status(dest_id2);
        DEBUG_H("Node %ld check node %ld in a failed data center\n", g_node_id, dest_id2);
        if (time > st->last_ts && time - st->last_ts > INTER_CENTER_FAILED_TIME) {
          DEBUG_H("Node %ld find in other data center node %ld status Failure\n", g_node_id,
                  dest_id);
          // update the status of failed node.
          node_status.set_node_status(dest_id2, NS::Failure, get_thd_id());
        }
        if (st->status == NS::Failure) generate_recovery_msg(dest_id2);
      }
    }
  }
}

bool HeartBeatThread::is_global_primary(uint64_t nid) {
  return g_node_id == 0;

  uint64_t node_cnt_per_center = g_node_cnt / g_center_cnt;
  for (int i = 0; i < g_node_cnt; i++) {
    uint64_t node_id = i;
    status_node* st = node_status.get_node_status(node_id);
    // Skip the failed data node.
    if (st->status == NS::Failure) continue;
    // We assume the first active node is global primary
    // Now we check whether this node alive.
    if (node_id == nid) {
      DEBUG_H("Node %ld now is the primary\n", g_node_id);
      return true;
    }
    uint64_t time = get_wall_clock();
    if (time > st->last_ts && time - st->last_ts > INTER_CENTER_FAILED_TIME) {
      DEBUG_H(
          "Node %ld find global primary node %ld status Failure, now %lu last heartbeat time %lu\n",
          g_node_id, node_id, time, st->last_ts);
      // The origin global primary RM failed.
      node_status.set_node_status(node_id, NS::Failure, get_thd_id());
    } else {
      return false;
    }
  }
  return false;
}

RC HeartBeatThread::update_node_and_route(RouteAndStatus result, uint64_t origin_dest) {
  // node status
  PRINT_HEARTBEAT("call update_node_and_route\n");
  DEBUG_H("Node %ld recieve heart beat from %ld in other data center\n", g_node_id, origin_dest);
  for (int i = 0; i < g_node_cnt; i++) {
    status_node* st = node_status.get_node_status(i);
    status_node tmp_st = result._status[i];
    DEBUG_H("Node %ld old ts %lu received ts %lu\n", i, st->last_ts, tmp_st.last_ts);
    if (tmp_st.last_ts > st->last_ts) {
      st->last_ts = tmp_st.last_ts;
      st->status = tmp_st.status;

      if ((i == g_node_id && st->status == Failure) ||
          (i == origin_dest && st->status == Failure)) {
        assert(false);
      }
      DEBUG_H("Node %ld update ts %lu and status %s\n", i, st->last_ts,
              st->status == OnCall ? "OnCall" : "Failure");
    }
  }
  // route
  for (int i = 0; i < g_part_cnt; i++) {
    // primary
    route_node_ts p_rt = route_table.get_primary(i);
    route_node_ts tmp_p_rt = result._route[i].primary;
    if (tmp_p_rt.last_ts > p_rt.last_ts) {
      route_table.set_primary(i, tmp_p_rt.node_id, tmp_p_rt.last_ts);
      DEBUG_H("Route %ld update primary ts %lu and node %d\n", i, tmp_p_rt.last_ts,
              tmp_p_rt.node_id);
    }
    // secondary 1
    route_node_ts s1_rt = route_table.get_secondary_1(i);
    route_node_ts tmp_s1_rt = result._route[i].secondary_1;
    if (tmp_s1_rt.last_ts > s1_rt.last_ts) {
      route_table.set_secondary_1(i, tmp_s1_rt.node_id, tmp_s1_rt.last_ts);
      DEBUG_H("Route %ld update second1 ts %lu and node %d\n", i, tmp_s1_rt.last_ts,
              tmp_s1_rt.node_id);
    }
    // secondary 2
    route_node_ts s2_rt = route_table.get_secondary_2(i);
    route_node_ts tmp_s2_rt = result._route[i].secondary_2;
    if (tmp_s2_rt.last_ts > s2_rt.last_ts) {
      route_table.set_secondary_2(i, tmp_s2_rt.node_id, tmp_s2_rt.last_ts);
      DEBUG_H("Route %ld update second2 ts %lu and node %d\n", i, tmp_s2_rt.last_ts,
              tmp_s2_rt.node_id);
    }
  }
}

auto HeartBeatThread::update_node_and_route_new(RouteAndStatus result, uint64_t origin_dest) -> RC {
  PRINT_HEARTBEAT("call update_node_and_route_new\n");
  // node status
  DEBUG_H("Node %ld recieve heart beat from %ld in other data center\n", g_node_id, origin_dest);
  for (int i = 0; i < g_node_cnt; i++) {
    status_node* st = node_status.get_node_status(i);
    status_node tmp_st = result._status[i];
    DEBUG_H("Node %ld old ts %lu received ts %lu\n", i, st->last_ts, tmp_st.last_ts);
    if (tmp_st.last_ts > st->last_ts) {
      st->last_ts = tmp_st.last_ts;
      st->status = tmp_st.status;

      if ((i == g_node_id && st->status == Failure) ||
          (i == origin_dest && st->status == Failure)) {
        assert(false);
      }
      DEBUG_H("Node %ld update ts %lu and status %s\n", i, st->last_ts,
              st->status == OnCall ? "OnCall" : "Failure");
    }
  }
  // route
  for (int i = 0; i < PART_CNT; i++) {
    route_node_ts p_rt;
    route_node_ts tmp_p_rt;
    for (int j = 0; j < REPLICA_COUNT; j++) {
      p_rt = route_table.get_route_node_new(j, i);
      tmp_p_rt = result._route[i].new_secondary[j];
      if (tmp_p_rt.last_ts > p_rt.last_ts) {
        route_table.set_route_node_new(j, i, tmp_p_rt.node_id, tmp_p_rt.last_ts);
        DEBUG_H("Route %ld update primary ts %lu and node %d\n", i, tmp_p_rt.last_ts,
                tmp_p_rt.node_id);
      }
    }
  }
}

vector<Replica> HeartBeatThread::get_node_replica(uint64_t dest_id) {
  vector<Replica> replica_list;
  for (int i = 0; i < g_part_cnt; i++) {
    route_node_ts p_rt = route_table.get_primary(i);
    if (p_rt.node_id == dest_id) {
      replica_list.push_back(Replica(i, 0));
      DEBUG_H("Failed node %ld has primary replica %ld\n", dest_id, i);
    }
    // secondary 1
    route_node_ts s1_rt = route_table.get_secondary_1(i);
    if (s1_rt.node_id == dest_id) {
      replica_list.push_back(Replica(i, 1));
      DEBUG_H("Failed node %ld has second1 replica %ld\n", dest_id, i);
    }
    // secondary 2
    route_node_ts s2_rt = route_table.get_secondary_2(i);
    if (s2_rt.node_id == dest_id) {
      replica_list.push_back(Replica(i, 2));
      DEBUG_H("Failed node %ld has second2 replica %ld\n", dest_id, i);
    }
  }
  return replica_list;
}

auto HeartBeatThread::get_node_replica_new(uint64_t dest_id) -> vector<Replica> {
  vector<Replica> replica_list;
  for (int i = 0; i < g_part_cnt; i++) {
    route_node_ts p_rt;
    for (int j = 0; j < REPLICA_COUNT; j++) {
      p_rt = route_table.get_route_node_new(j, i);
      if (p_rt.node_id == dest_id) {
        replica_list.push_back(Replica(i, j));
        DEBUG_H("Failed node %ld has primary replica %ld\n", dest_id, i);
      }
    }
  }
  return replica_list;
}

uint64_t HeartBeatThread::caculate_suitable_node(Replica rep, uint64_t failed_id) {
  /* ------Check the same data center of failed node------- */
  uint64_t center_id = GET_CENTER_ID(failed_id);
  assert(g_node_cnt % CENTER_CNT == 0);
  uint64_t node_cnt_per_dc = g_node_cnt / CENTER_CNT;
  uint64_t suitable_node = -1;
  /* --Now we simply choose another node in the same data center.-- */
  for (int i = 0; i < node_cnt_per_dc; i++) {
    uint64_t node_id = i * CENTER_CNT + center_id;
    status_node* st = node_status.get_node_status(node_id);
    if (st->status == NS::Failure) continue;
    DEBUG_H("Generate recover msg, node %ld state %ld\n", node_id, st->status);
    suitable_node = node_id;
    break;
  }
  if (suitable_node != -1) return suitable_node;

  /* ------If all the node in the same data center fails ------- */
  for (int i = 0; i < g_node_cnt; i++) {
    uint64_t node_id = i;
    status_node* st = node_status.get_node_status(node_id);
    if (st->status == NS::Failure) continue;
    suitable_node = node_id;
    break;
  }
  if (suitable_node != -1)
    return suitable_node;
  else
    assert(false);
}

RC HeartBeatThread::generate_recovery_msg(uint64_t failed_id) {
  vector<Replica> replica_list = get_node_replica(failed_id);
  for (int i = 0; i < replica_list.size(); i++) {
    Replica rep = replica_list[i];
    uint64_t new_dest_id = caculate_suitable_node(rep, failed_id);
    if (rep.replica_id == 0) {
      route_table.table[rep.partition_id].primary.node_id = -1;
      route_table.table[rep.partition_id].primary.last_ts = get_wall_clock();
    }
    if (rep.replica_id == 1) {
      route_table.table[rep.partition_id].secondary_1.node_id = -1;
      route_table.table[rep.partition_id].secondary_1.last_ts = get_wall_clock();
    }
    if (rep.replica_id == 2) {
      route_table.table[rep.partition_id].secondary_2.node_id = -1;
      route_table.table[rep.partition_id].secondary_2.last_ts = get_wall_clock();
    }

    DEBUG_H(
        "Node %ld send recover msg to %ld, failed id %ld, failed part %ld, failed replica %ld\n",
        g_node_id, new_dest_id, failed_id, rep.replica_id, rep.replica_id);
    if (new_dest_id == g_node_id) {
      recover_queue.enqueue(
          get_thd_id(),
          Message::create_message(rep.partition_id, rep.replica_id, node_status, RECOVERY),
          new_dest_id);
    } else {
      msg_queue.enqueue(
          get_thd_id(),
          Message::create_message(rep.partition_id, rep.replica_id, node_status, RECOVERY),
          new_dest_id);
    }
  }
}

int HeartBeatThread::tcp_ping(const char* ip) {
  int sock = 0;
  struct sockaddr_in serv_addr;

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    std::cout << "Create socket error" << std::endl;
    return -1;
  }

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(9876);

  if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
    std::cout << "Invalid ip address" << std::endl;
    return -1;
  }

  auto start = std::chrono::high_resolution_clock::now();
  if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    std::cout << "Connect failed" << std::endl;
    return -1;
  }
  auto stop = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
  auto time = duration.count();
  // std::cout << "Connection time: " << time << " ms" << std::endl;

  close(sock);
  return time;
}

int HeartBeatThread::tcp_listen() {
  int server_fd, new_socket;
  struct sockaddr_in address;
  int opt = 1;
  int addrlen = sizeof(address);

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;  // local address
  address.sin_port = htons(9876);        // port

  if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }
  if (listen(server_fd, 3) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  while (true) {
    std::cout << "Wait for connection..." << std::endl;
    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
      perror("accept");
      continue;
    }

    std::cout << "Connection established, close..." << std::endl;

    close(new_socket);
    std::cout << "Continue listening..." << std::endl;
  }

  close(new_socket);
  close(server_fd);

  return 0;
}