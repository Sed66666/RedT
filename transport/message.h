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

#ifndef _MESSAGE_H_
#define _MESSAGE_H_

#include "array.h"
#include "global.h"
#include "helper.h"
#include "logger.h"
#include "route_table.h"

class ycsb_request;
class LogRecord;
// class RouteAndStatus;
struct Item_no;
// class TxnManager;

class Message {
 public:
  virtual ~Message() {}
  static Message* create_message(char* buf);
  static Message* create_message(BaseQuery* query, RemReqType rtype);
  static Message* create_message(TxnManager* txn, RemReqType rtype);
  static Message* create_message(uint64_t txn_id, RemReqType rtype);
  static Message* create_message(uint64_t txn_id, uint64_t batch_id, RemReqType rtype);
  static Message* create_message(LogRecord* record, RemReqType rtype);
  static Message* create_message(route_table_node* route, status_node* node, bool need_flush,
                                 RemReqType rtype);
  static Message* create_message(uint64_t* access_count, uint64_t* latency, RemReqType rtype);
  static Message* create_message(uint64_t pid, uint64_t rid, NodeStatus node, RemReqType rtype);
  static Message* create_message(RemReqType rtype);
  static std::vector<Message*>* create_messages(char* buf);
  static void release_message(Message* msg);
  RemReqType rtype;
  uint64_t txn_id;
  uint64_t batch_id;
  uint64_t return_node_id;
  uint64_t return_center_id;
  uint64_t wq_time;
  uint64_t mq_time;
  uint64_t ntwk_time;
  // uint64_t txn_type;
  // uint64_t seq_id;
  // uint64_t trans_id;
  //  network part
  uint64_t send_time;
  uint64_t latency;

  // Collect other stats
  uint64_t current_abort_cnt;
  double lat_work_queue_time;
  double lat_msg_queue_time;
  double lat_cc_block_time;
  double lat_cc_time;
  double lat_process_time;
  double lat_network_time;
  double lat_other_time;

  uint64_t mget_size();
  uint64_t get_txn_id() { return txn_id; }
  uint64_t get_batch_id() { return batch_id; }
  uint64_t get_return_id() { return return_node_id; }
  void mcopy_from_buf(char* buf);
  void mcopy_to_buf(char* buf);
  void mcopy_from_txn(TxnManager* txn);
  void mcopy_to_txn(TxnManager* txn);
  RemReqType get_rtype() { return rtype; }

  virtual uint64_t get_size() = 0;
  virtual void copy_from_buf(char* buf) = 0;
  virtual void copy_to_buf(char* buf) = 0;
  virtual void copy_to_txn(TxnManager* txn) = 0;
  virtual void copy_from_txn(TxnManager* txn) = 0;
  virtual void init() = 0;
  virtual void release() = 0;
};

// Message types
class InitDoneMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}
};

class FinishMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}
  bool is_abort() { return rc == Abort; }

  uint64_t pid;
  RC rc;
  // uint64_t txn_id;
  // uint64_t batch_id;
  bool readonly;
#if CC_ALG == MAAT || USE_REPLICA
  uint64_t commit_timestamp;
#endif
};

class LogMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release();
  void copy_from_record(LogRecord* record);

  // Array<LogRecord*> log_records;
  LogRecord record;
};

class LogRspMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}
};

class LogFlushedMessage : public Message {
 public:
  void copy_from_buf(char* buf) {}
  void copy_to_buf(char* buf) {}
  void copy_from_txn(TxnManager* txn) {}
  void copy_to_txn(TxnManager* txn) {}
  uint64_t get_size() { return sizeof(LogFlushedMessage); }
  void init() {}
  void release() {}
};

class QueryResponseMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}

  RC rc;
  uint64_t pid;
};

class AckMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}

  RC rc;
#if CC_ALG == MAAT
  uint64_t lower;
  uint64_t upper;
#endif
  Array<uint64_t> failed_partition;
  // For Calvin PPS: part keys from secondary lookup for sequencer response
  Array<uint64_t> part_keys;
};

class PrepareMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}

  uint64_t pid;
  RC rc;
  uint64_t txn_id;
};

class ForwardMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}

  RC rc;
#if WORKLOAD == TPCC
  uint64_t o_id;
#endif
};

class DoneMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}
  uint64_t batch_id;
};

class ClientResponseMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}

  RC rc;
  uint64_t client_startts;
};

class ClientQueryMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_query(BaseQuery* query);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  RC rc;
  uint64_t pid;
  uint64_t ts;
#if CC_ALG == CALVIN
  uint64_t batch_id;
  uint64_t txn_id;
#endif
  uint64_t client_startts;
  uint64_t first_startts;
  Array<uint64_t> partitions;
};

class YCSBClientQueryMessage : public ClientQueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_query(BaseQuery* query);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  Array<ycsb_request*> requests;
};

class TPCCClientQueryMessage : public ClientQueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_query(BaseQuery* query);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  uint64_t txn_type;
  // common txn input for both payment & new-order
  uint64_t w_id;
  uint64_t d_id;
  uint64_t c_id;

  // payment
  uint64_t d_w_id;
  uint64_t c_w_id;
  uint64_t c_d_id;
  char c_last[LASTNAME_LEN];
  uint64_t h_amount;
  bool by_last_name;

  // new order
  Array<Item_no*> items;
  bool rbk;
  bool remote;
  uint64_t ol_cnt;
  uint64_t o_entry_d;
};

class PPSClientQueryMessage : public ClientQueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_query(BaseQuery* query);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  uint64_t txn_type;

  // getparts
  uint64_t part_key;
  // getproducts / getpartbyproduct
  uint64_t product_key;
  // getsuppliers / getpartbysupplier
  uint64_t supplier_key;

  // part keys from secondary lookup
  Array<uint64_t> part_keys;

  bool recon;
};
class DAClientQueryMessage : public ClientQueryMessage {
 public:
  void copy_from_buf(char* buf);           // ok
  void copy_to_buf(char* buf);             // ok
  void copy_from_query(BaseQuery* query);  // ok
  void copy_from_txn(TxnManager* txn);     // ok
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  DATxnType txn_type;
  uint64_t trans_id;
  uint64_t item_id;
  uint64_t seq_id;
  uint64_t write_version;
  uint64_t state;
  uint64_t next_state;
  uint64_t last_state;
};

class QueryMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release() {}

  uint64_t pid;
#if CC_ALG == WAIT_DIE || CC_ALG == TIMESTAMP || CC_ALG == MVCC || CC_ALG == WOUND_WAIT
  uint64_t ts;
#endif
#if CC_ALG == MVCC
  uint64_t thd_id;
#endif
#if CC_ALG == OCC || USE_REPLICA
  uint64_t start_ts;
#endif
#if MODE == QRY_ONLY_MODE
  uint64_t max_access;
#endif
};

class YCSBQueryMessage : public QueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  Array<ycsb_request*> requests;
};

class TPCCQueryMessage : public QueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  uint64_t txn_type;
  uint64_t state;

  // common txn input for both payment & new-order
  uint64_t w_id;
  uint64_t d_id;
  uint64_t c_id;

  // payment
  uint64_t d_w_id;
  uint64_t c_w_id;
  uint64_t c_d_id;
  char c_last[LASTNAME_LEN];
  uint64_t h_amount;
  bool by_last_name;

  // new order
  Array<Item_no*> items;
  bool rbk;
  bool remote;
  uint64_t ol_cnt;
  uint64_t o_entry_d;
};

class PPSQueryMessage : public QueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  uint64_t txn_type;
  uint64_t state;

  // getparts
  uint64_t part_key;
  // getproducts / getpartbyproduct
  uint64_t product_key;
  // getsuppliers / getpartbysupplier
  uint64_t supplier_key;

  // part keys from secondary lookup
  Array<uint64_t> part_keys;
};

class DAQueryMessage : public QueryMessage {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init();
  void release();

  DATxnType txn_type;
  uint64_t trans_id;
  uint64_t item_id;
  uint64_t seq_id;
  uint64_t write_version;
  uint64_t state;
  uint64_t next_state;
  uint64_t last_state;
};

class HeartBeatMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release();
  void copy_from_node_status(route_table_node* route_table, status_node* node_status);

  // LogRecord record;
  RouteAndStatus heartbeatmsg;
  bool need_flush_route_;
  // RouteTable* _route;
  // NodeStatus* _status;
};

class StatsCountMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release();
  void copy_from_access_count(uint64_t* access_count);
  void copy_from_latency(uint64_t* latency);

  uint64_t access_count_[PART_CNT];
  uint64_t latency_[CENTER_CNT];

  void printAccessCount() {
    PRINT_HEARTBEAT("node %d access count:\n", return_node_id);
    for (int i = 0; i < PART_CNT; i++) {
      PRINT_HEARTBEAT("partition %d : %d\n", i, access_count_[i]);
    }
  }
  void printLatency() {
    PRINT_HEARTBEAT("node %d latency:\n", return_node_id);
    for (int i = 0; i < CENTER_CNT; i++) {
      PRINT_HEARTBEAT("center %d : %dms\n", i, latency_[i]);
    }
  }
};

class ReplicaRecoverMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release();
  void copy_from_replica(uint64_t pid, uint64_t rid);

  uint64_t partition_id;
  uint64_t replica_id;
};

// Will not be sent to another data nodes
class WaitTxnMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release();

  TxnManager* txn;
};

class CheckMessage : public Message {
 public:
  void copy_from_buf(char* buf);
  void copy_to_buf(char* buf);
  void copy_from_txn(TxnManager* txn);
  void copy_to_txn(TxnManager* txn);
  uint64_t get_size();
  void init() {}
  void release();

  uint64_t status;
};
#endif
