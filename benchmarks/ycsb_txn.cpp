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

#include "global.h"
#include "config.h"
#include "helper.h"
#include "ycsb.h"
#include "ycsb_query.h"
#include "wl.h"
#include "thread.h"
#include "table.h"
#include "row.h"
#include "index_hash.h"
#include "index_btree.h"
#include "catalog.h"
#include "manager.h"
#include "row.h"
#include "row_lock.h"
#include "row_ts.h"
#include "row_mvcc.h"
#include "mem_alloc.h"
#include "query.h"
#include "msg_queue.h"
#include "message.h"
#include "qps/op.hh"
#include "src/sshed.hh"
#include "transport.h"
#include "qps/op.hh"
#include "stats.h"
#include <unordered_set>

void YCSBTxnManager::init(uint64_t thd_id, Workload * h_wl) {
	TxnManager::init(thd_id, h_wl);
	_wl = (YCSBWorkload *) h_wl;
  reset();
}

void YCSBTxnManager::reset() {
  state = YCSB_0;
  next_record_id = 0;
  remote_next_node_id = 0;
  for(int i = 0; i < g_node_cnt; i++) {
	remote_node[i].clear();
  }
  TxnManager::reset();
}

RC YCSBTxnManager::acquire_locks() {
  	uint64_t starttime = get_sys_clock();
	assert(CC_ALG == CALVIN);
	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	locking_done = false;
	RC rc = RCOK;
	incr_lr();
	assert(ycsb_query->requests.size() == g_req_per_query);
	assert(phase == CALVIN_RW_ANALYSIS);
		for (uint32_t rid = 0; rid < ycsb_query->requests.size(); rid ++) {
			ycsb_request * req = ycsb_query->requests[rid];
			uint64_t part_id = _wl->key_to_part( req->key );
		DEBUG("LK Acquire (%ld,%ld) %d,%ld -> %ld\n", get_txn_id(), get_batch_id(), req->acctype,
			req->key, GET_NODE_ID(part_id));
		if (GET_NODE_ID(part_id) != g_node_id) continue;
			INDEX * index = _wl->the_index;
			itemid_t * item;
			item = index_read(index, req->key, part_id);
			row_t * row = ((row_t *)item->location);
			RC rc2 = get_lock(row,req->acctype);
		if(rc2 != RCOK) {
		rc = rc2;
		}
		}
	if(decr_lr() == 0) {
		if (ATOM_CAS(lock_ready, false, true)) rc = RCOK;
	}
	txn_stats.wait_starttime = get_sys_clock();

	INC_STATS(get_thd_id(),calvin_sched_time,get_sys_clock() - starttime);
	locking_done = true;
	return rc;
}

void YCSBTxnManager::get_num_msgs_statistics() {
	//node location of replicas
	unordered_set<uint64_t> w_pry_loc;
	unordered_set<uint64_t> r_pry_loc;
	unordered_set<uint64_t> w_sec_loc;
	unordered_set<uint64_t> r_sec_loc;
	//note: each node is counted only once, with priority in descending order.
	uint64_t w_pry_num[g_center_cnt];	//num of nodes with primary replicas to write, in each center
	uint64_t r_pry_num[g_center_cnt];	//num of nodes with primary replicas to read, in each center
	uint64_t w_sec_num[g_center_cnt];	//num of nodes with secondary replicas to write, in each center
	uint64_t r_sec_num[g_center_cnt];	//num of nodes with secondary replicas to read, in each center
	for(uint64_t i=0;i<g_center_cnt;i++){
		w_pry_num[i] = 0; r_pry_num[i] = 0; w_sec_num[i] = 0; r_sec_num[i] = 0;
	}
	// //TAPIR may read from the nearest replica
	// unordered_set<uint64_t> read_to_reduce; 
	// uint64_t reduce_cnt = 0;
	
	//get location
	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	for(int i = 0; i < ycsb_query->requests.size(); i++) {
		ycsb_request * req = ycsb_query->requests[i];
		uint64_t part_id = _wl->key_to_part(req->key);
		if(req->acctype == WR){
			w_pry_loc.insert(GET_NODE_ID(part_id));
			w_sec_loc.insert(GET_FOLLOWER1_NODE(part_id));
			w_sec_loc.insert(GET_FOLLOWER2_NODE(part_id));
		}else if(req->acctype == RD){
			r_pry_loc.insert(GET_NODE_ID(part_id));
			r_sec_loc.insert(GET_FOLLOWER1_NODE(part_id));
			r_sec_loc.insert(GET_FOLLOWER2_NODE(part_id));			
			// if(GET_CENTER_ID(GET_NODE_ID(part_id)) != g_center_id && (GET_CENTER_ID(GET_FOLLOWER1_NODE(part_id)) == g_center_id || GET_CENTER_ID(GET_FOLLOWER2_NODE(part_id)) == g_center_id)){
			// 	read_to_reduce.insert(GET_NODE_ID(part_id));
			// }
		}else assert(false);
	}
	
	//get num of nodes per center
	for(int j=0;j<g_node_cnt;j++){
		if(w_pry_loc.count(j) > 0){
			w_pry_num[GET_CENTER_ID(j)]++;
		}else if(r_pry_loc.count(j) > 0){
			r_pry_num[GET_CENTER_ID(j)]++;
			// if(read_to_reduce.count(j)>0) reduce_cnt++;				
		}else if(w_sec_loc.count(j) > 0){
			w_sec_num[GET_CENTER_ID(j)]++;
		}else if(r_sec_loc.count(j) > 0){
			r_sec_num[GET_CENTER_ID(j)]++;
		}
	}
	
	//get number of messages
	assert(num_msgs_rw==0);
	assert(num_msgs_prep==0);
	assert(num_msgs_commit==0);
	int m = 2;
	int M = 2;

#if USE_TAPIR
	//for TAPIR
	for(uint64_t i=0;i<g_center_cnt;i++){
		if(i == g_center_id){
		}else{
			num_msgs_rw += w_pry_num[i] + r_pry_num[i] + w_sec_num[i] + r_sec_num[i];
			num_msgs_prep += w_pry_num[i] + w_sec_num[i];
			num_msgs_commit += w_pry_num[i] + r_pry_num[i] + w_sec_num[i] + r_sec_num[i];
		}
	}
#endif

	num_msgs_rw *= 2;
	num_msgs_prep *= 2;
	num_msgs_commit *= 2;
}


RC YCSBTxnManager::send_remote_subtxn() {
	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	RC rc = RCOK;

	bool is_primary[g_node_cnt]; //is center_master has primary replica or not
	// int remote_replica_node[g_node_cnt];
	for(int i = 0; i < ycsb_query->requests.size(); i++) {
		// next_record_id++;
		ycsb_request * req = ycsb_query->requests[i];
		uint64_t part_id = _wl->key_to_part(req->key);
		vector<uint64_t> node_id;
#if USE_REPLICA	
		// node_id.push_back(GET_NODE_ID(part_id));
		// node_id.push_back(GET_FOLLOWER1_NODE(part_id));
		// node_id.push_back(GET_FOLLOWER2_NODE(part_id));		
#else
		node_id.push_back(GET_NODE_ID(part_id));		
#endif
// #if USE_TAPIR && TAPIR_REPLICA
// 		for(int j = 0; j < node_id.size(); j++) {
// 			// remote_replica_node[j] = 1;
// 			ycsb_query->partitions_touched.add_unique(GET_PART_ID(0,node_id[j]));
// 		}
// 		if(req->acctype == WR) ycsb_query->partitions_modified.add_unique(_wl->key_to_part(ycsb_query->requests[i]->key));
// 	}
// 	rsp_cnt = query->partitions_touched.size() - 1;
// 	#if TAPIR_DEBUG
// 		printf("send %d rqry %d messages\n",get_txn_id(), rsp_cnt);
// 	#endif
// 	for(int i = 0; i < query->partitions_touched.size(); i++) {
// 		if(query->partitions_touched[i] != g_node_id) {
// 	#if TAPIR_DEBUG
// 				printf("send %d rqry message to node:%d \n",get_txn_id(), query->partitions_touched[i]);
// 	#endif
// 			msg_queue.enqueue(get_thd_id(),Message::create_message(this,RQRY),query->partitions_touched[i]);
// 		}
		
// 	}
// 		// for(int i = 0; i < g_node_cnt; i++) {
// 		// if(i != g_node_id && remote_node[i].size() > 0) {//send message to all masters
// 		// 	remote_next_node_id = i;
// 		// 	// printf("%d \n",remote_node[i].size());
// 		// 	msg_queue.enqueue(get_thd_id(),Message::create_message(this,RQRY),i);
// 		// 	// printf("send subtxn to %d\n", i);
// 		// }
// #else
		uint64_t n_id = GET_NODE_ID(part_id);
		remote_node[n_id].push_back(i);
		// ycsb_query->centers_touched.add_unique(center_id);
		ycsb_query->partitions_touched.add_unique(GET_PART_ID(0,n_id));
		// ycsb_query->centers_touched.add_unique(center_id);
#if USE_TAPIR && TAPIR_REPLICA 
		n_id = GET_FOLLOWER1_NODE(part_id);
		remote_node[n_id].push_back(i);
		n_id = GET_FOLLOWER2_NODE(part_id);
		remote_node[n_id].push_back(i);
#endif
		//center_master is set as the first toughed primary, if not exist, use the first toughed backup.
		// auto ret = center_master.insert(pair<uint64_t, uint64_t>(center_id, node_id[j]));
		if(req->acctype == WR) ycsb_query->partitions_modified.add_unique(_wl->key_to_part(ycsb_query->requests[i]->key));
	}
#if USE_TAPIR && TAPIR_REPLICA
	// rsp_cnt = query->partitions_touched.size() - 1;
	rsp_cnt = 0;
	for(int i = 0; i < query->partitions_touched.size(); i++) {
		uint64_t part_id = query->partitions_touched[i];
		ir_log_rsp_cnt[i] = 3;
		uint64_t l_node = GET_NODE_ID(part_id);
		uint64_t f1 = GET_FOLLOWER1_NODE(part_id);
		uint64_t f2 = GET_FOLLOWER2_NODE(part_id);
		bool exist = false;
		if(l_node == g_node_id || f1 == g_node_id || f2 == g_node_id) {
			ir_log_rsp_cnt[i]--;
		}
	}
#else
	rsp_cnt = query->partitions_touched.size() - 1;
#endif
	for(int i = 0; i < g_node_cnt; i++) {
		if(i != g_node_id && remote_node[i].size() > 0) {//send message to all masters
#if USE_TAPIR && TAPIR_REPLICA
			rsp_cnt++;
#endif
			remote_next_node_id = i;
			// printf("%d \n",remote_node[i].size());
#if TAPIR_DEBUG
			printf("send %d rqry message to node:%d \n",get_txn_id(), i);
#endif
			msg_queue.enqueue(get_thd_id(),Message::create_message(this,RQRY),i);
			// printf("txn %ld send subtxn to %d\n", get_txn_id(), i);
		}
	}
// #endif
	// printf("txn %d send subtxn success \n", get_txn_id());
	get_num_msgs_statistics();
	return rc;
}

RC YCSBTxnManager::run_txn(yield_func_t &yield, uint64_t cor_id) {

	RC rc = RCOK;
	assert(CC_ALG != CALVIN);
	// printf("centerID: %d\n", g_center_id);
	if(IS_LOCAL(txn->txn_id) && state == YCSB_0 && next_record_id == 0) {
		DEBUG("Running txn %ld\n",txn->txn_id);
#if DEBUG_PRINTF
		// printf("[txn start]txn：%d，ts：%lu\n",txn->txn_id,get_timestamp());
#endif
		//query->print();
		query->partitions_touched.add_unique(GET_PART_ID(0,g_node_id));
#if PARAL_SUBTXN
		rc = send_remote_subtxn();
		start_rw_time = get_sys_clock();
#endif
	}
	
	uint64_t starttime = get_sys_clock();

#if BATCH_INDEX_AND_READ
	//batch read all index for remote access
	ycsb_batch_read(yield,R_INDEX,cor_id);
	//batch read all row for remote access
	ycsb_batch_read(yield,R_ROW,cor_id);
#endif
	while(rc == RCOK && !is_done()) {
#if CC_ALG == WOUND_WAIT
		if (txn_state == WOUNDED) {
			rc = Abort;
			break;
		}  
#endif
		rc = run_txn_state(yield, cor_id);
	}
#if CC_ALG == WOUND_WAIT
	if (txn_state == WOUNDED) 
		rc = Abort;
#endif
#if BATCH_INDEX_AND_READ
	reqId_index.erase(reqId_index.begin(),reqId_index.end());
	reqId_row.erase(reqId_row.begin(),reqId_row.end());
#endif

    if(rc == Abort) total_num_atomic_retry++;
	uint64_t curr_time = get_sys_clock();
	txn_stats.process_time += curr_time - starttime;
	txn_stats.process_time_short += curr_time - starttime;
	txn_stats.wait_starttime = get_sys_clock();

	// if(IS_LOCAL(get_txn_id())) {
	// 	INC_STATS(get_thd_id(), trans_read_write_count, 1);
	// 	INC_STATS(get_thd_id(), trans_read_write_time, get_sys_clock() - start_rw_time);
	// 	start_logging_time = get_sys_clock();
	// }
	if(rsp_cnt > 0 && IS_LOCAL(txn->txn_id)) {
		return WAIT;
	} else {
		// if(IS_LOCAL(get_txn_id())) {
		// 	INC_STATS(get_thd_id(), trans_read_write_count, 1);
		// 	INC_STATS(get_thd_id(), trans_read_write_time, get_sys_clock() - start_rw_time);
		// 	start_logging_time = get_sys_clock();
		// }
	}
	if(IS_LOCAL(get_txn_id())) {  
		if(is_done() && rc == RCOK) {
			// printf("a txn is done\n");
#if CC_ALG == WOUND_WAIT
      		txn_state = STARTCOMMIT;
#endif
			rc = start_commit(yield, cor_id);
		}
		else if(rc == Abort)
		rc = start_abort(yield, cor_id);
	} else if(rc == Abort){
		rc = abort(yield, cor_id);
	}
// #endif
  return rc;
}

RC YCSBTxnManager::run_txn_post_wait() {
	uint64_t starttime = get_sys_clock();
	get_row_post_wait(row);
	uint64_t curr_time = get_sys_clock();
	txn_stats.process_time += curr_time - starttime;
	txn_stats.process_time_short += curr_time - starttime;
	next_ycsb_state();
	INC_STATS(get_thd_id(),trans_benchmark_compute_time,get_sys_clock() - curr_time);
	return RCOK;
}

bool YCSBTxnManager::is_done() { 
// #if PARAL_SUBTXN
// 	return (next_record_id >= ((YCSBQuery*)query)->requests.size() && rsp_cnt == 0);
// #else
	return next_record_id >= ((YCSBQuery*)query)->requests.size();
// #endif
}

void YCSBTxnManager::next_ycsb_state() {
  switch(state) {
	case YCSB_0:
	  state = YCSB_1;
	  break;
	case YCSB_1:
	  next_record_id++;
	  if(!IS_LOCAL(txn->txn_id) || !is_done()) {
		state = YCSB_0;
	  } else {
		state = YCSB_FIN;
	  }
	  break;
	case YCSB_FIN:
	  break;
	default:
	  assert(false);
  }
}

bool YCSBTxnManager::is_local_request(uint64_t idx) {
  return GET_NODE_ID(_wl->key_to_part(((YCSBQuery*)query)->requests[idx]->key)) == g_node_id;
}

#if BATCH_INDEX_AND_READ
void YCSBTxnManager::ycsb_batch_read(yield_func_t &yield,BatchReadType rtype, uint64_t cor_id){
  	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	vector<vector<uint64_t>> remote_index(g_node_cnt);

	for(int i=0;i<ycsb_query->requests.size();i++){
		ycsb_request * req = ycsb_query->requests[i];
		uint64_t part_id = _wl->key_to_part( req->key );
		uint64_t loc = GET_NODE_ID(part_id);
		if(loc != g_node_id){  //remote
			remote_index[loc].push_back(i);
		}
	}
	for(int i=0;i<g_node_cnt;i++){
		if(remote_index[i].size()>0){
			batch_read(yield, rtype, i, remote_index, cor_id);
		}
	}
	for(int i=0;i<g_node_cnt;i++){
		if(remote_index[i].size()>0){
			get_batch_read(yield, rtype,i, remote_index, cor_id);
		}
	}
 }
#endif

RC YCSBTxnManager::send_remote_request() {
	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	uint64_t dest_node_id = GET_NODE_ID(ycsb_query->requests[next_record_id]->key);
	ycsb_query->partitions_touched.add_unique(GET_PART_ID(0,dest_node_id));
    // DEBUG("ycsb send remote request %ld, %ld\n",txn->txn_id,txn->batch_id);
    msg_queue.enqueue(get_thd_id(),Message::create_message(this,RQRY),dest_node_id);

	return WAIT_REM;
}

void YCSBTxnManager::copy_remote_requests(YCSBQueryMessage * msg) {
	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	//msg->requests.init(ycsb_query->requests.size());
#if PARAL_SUBTXN == true
#if USE_TAPIR && TAPIR_REPLICA
	for(int i = 0; i < ycsb_query->requests.size();i++) {
		YCSBQuery::copy_request_to_msg(ycsb_query,msg,i);
	}
#else
	uint64_t remote_node_id = remote_next_node_id;
	uint64_t record_id = remote_node[remote_node_id][0];
	uint64_t index = 0;
	while(index < remote_node[remote_node_id].size()) {
		YCSBQuery::copy_request_to_msg(ycsb_query,msg,record_id);
		index++;
		record_id = remote_node[remote_node_id][index];
	}
#endif
#else
	uint64_t dest_node_id = GET_NODE_ID(ycsb_query->requests[next_record_id]->key);
	#if ONE_NODE_RECIEVE == 1 && defined(NO_REMOTE) && LESS_DIS_NUM == 10
	while (next_record_id < ycsb_query->requests.size() && GET_NODE_ID(ycsb_query->requests[next_record_id]->key) == dest_node_id) {
	#else
	while (next_record_id < ycsb_query->requests.size() && !is_local_request(next_record_id) &&
			GET_NODE_ID(ycsb_query->requests[next_record_id]->key) == dest_node_id) {
	#endif
		if(ycsb_query->requests[next_record_id]->acctype == WR && IS_LOCAL(get_txn_id())) 	
			ycsb_query->partitions_modified.add_unique(_wl->key_to_part(ycsb_query->requests[next_record_id]->key));
		YCSBQuery::copy_request_to_msg(ycsb_query,msg,next_record_id++);
	}
#endif
}



RC YCSBTxnManager::run_txn_state(yield_func_t &yield, uint64_t cor_id) {
	YCSBQuery* ycsb_query = (YCSBQuery*) query;
	ycsb_request * req = ycsb_query->requests[next_record_id];
	uint64_t part_id = _wl->key_to_part( req->key );
#if USE_TAPIR && TAPIR_REPLICA
	bool loc = GET_NODE_ID(part_id) == g_node_id || GET_FOLLOWER1_NODE(part_id) == g_node_id || GET_FOLLOWER2_NODE(part_id) == g_node_id;
#else
	bool loc = GET_NODE_ID(part_id) == g_node_id;
#endif
	
	
	RC rc = RCOK;
	switch (state) {
	case YCSB_0 :
		if(loc) {
			if(ycsb_query->requests[next_record_id]->acctype == WR && IS_LOCAL(get_txn_id())) 	
				ycsb_query->partitions_modified.add_unique(part_id);
			rc = run_ycsb_0(yield,req,row,cor_id);
		} else {
#if PARAL_SUBTXN == true
			rc = RCOK;
#else
			rc = send_remote_request();
#endif
		}
	  break;
	case YCSB_1 :
		//read local row,for message queue by TCP/IP,write set has actually been written in this point,
		if(loc) {
			rc = run_ycsb_1(req->acctype,row);  
		} else {
			rc = RCOK;
		}
		break;
	case YCSB_FIN :
		state = YCSB_FIN;
		break;
	default:
		assert(false);
  }

  if (rc == RCOK) next_ycsb_state();

  return rc;
}

RC YCSBTxnManager::run_ycsb_0(yield_func_t &yield,ycsb_request * req,row_t *& row_local,uint64_t cor_id) {
  uint64_t starttime = get_sys_clock();
  RC rc = RCOK;
  int part_id = _wl->key_to_part( req->key );
  access_t type = req->acctype;
  itemid_t * m_item;
  INC_STATS(get_thd_id(),trans_benchmark_compute_time,get_sys_clock() - starttime);
  m_item = index_read(_wl->the_index, req->key, part_id);
  starttime = get_sys_clock();
  row_t * row = ((row_t *)m_item->location);
  INC_STATS(get_thd_id(),trans_benchmark_compute_time,get_sys_clock() - starttime);
  rc = get_row(yield,row, type,row_local,cor_id);
  return rc;
}

RC YCSBTxnManager::run_ycsb_1(access_t acctype, row_t * row_local) {
  uint64_t starttime = get_sys_clock();
  if (acctype == RD || acctype == SCAN) {
	int fid = 0;
	char * data = row_local->get_data();
	uint64_t fval __attribute__ ((unused));
	fval = *(uint64_t *)(&data[fid * 100]); //read fata and store to fval
#if ISOLATION_LEVEL == READ_COMMITTED || ISOLATION_LEVEL == READ_UNCOMMITTED
	// Release lock after read
	release_last_row_lock();
#endif

  } 
  else {
	assert(acctype == WR);
		int fid = 0;
	  char * data = row_local->get_data();
	  *(uint64_t *)(&data[fid * 100]) = 0; //write data, set data[0]=0
#if YCSB_ABORT_MODE
	if (data[0] == 'a') return RCOK;
#endif

#if ISOLATION_LEVEL == READ_UNCOMMITTED
	// Release lock after write
	release_last_row_lock();
#endif
  }
  INC_STATS(get_thd_id(),trans_benchmark_compute_time,get_sys_clock() - starttime);
  return RCOK;
}

RC YCSBTxnManager::run_calvin_txn(yield_func_t &yield,uint64_t cor_id) {
  RC rc = RCOK;
  uint64_t starttime = get_sys_clock();
  YCSBQuery* ycsb_query = (YCSBQuery*) query;
  DEBUG("(%ld,%ld) Run calvin txn\n",txn->txn_id,txn->batch_id);
  while(!calvin_exec_phase_done() && rc == RCOK) {
	DEBUG("(%ld,%ld) phase %d\n",txn->txn_id,txn->batch_id,this->phase);
	switch(this->phase) {
	  case CALVIN_RW_ANALYSIS:

		// Phase 1: Read/write set analysis
		calvin_expected_rsp_cnt = ycsb_query->get_participants(_wl);
#if YCSB_ABORT_MODE
		if(query->participant_nodes[g_node_id] == 1) {
		  calvin_expected_rsp_cnt--;
		}
#else
		calvin_expected_rsp_cnt = 0;
#endif
		DEBUG("(%ld,%ld) expects %d responses;\n", txn->txn_id, txn->batch_id,
			  calvin_expected_rsp_cnt);

		this->phase = CALVIN_LOC_RD;
		break;
	  case CALVIN_LOC_RD:
		// Phase 2: Perform local reads
		DEBUG("(%ld,%ld) local reads\n",txn->txn_id,txn->batch_id);
		rc = run_ycsb(yield,cor_id);
		//release_read_locks(query);

		this->phase = CALVIN_SERVE_RD;
		break;
	  case CALVIN_SERVE_RD:
		// Phase 3: Serve remote reads
		// If there is any abort logic, relevant reads need to be sent to all active nodes...
		if(query->participant_nodes[g_node_id] == 1) {
		  rc = send_remote_reads();
		}
		if(query->active_nodes[g_node_id] == 1) {
		  this->phase = CALVIN_COLLECT_RD;
		  if(calvin_collect_phase_done()) {
			rc = RCOK;
		  } else {
			DEBUG("(%ld,%ld) wait in collect phase; %d / %d rfwds received\n", txn->txn_id,
				  txn->batch_id, rsp_cnt, calvin_expected_rsp_cnt);
			rc = WAIT;
		  }
		} else { // Done
		  rc = RCOK;
		  this->phase = CALVIN_DONE;
		}

		break;
	  case CALVIN_COLLECT_RD:
		// Phase 4: Collect remote reads
		this->phase = CALVIN_EXEC_WR;
		break;
	  case CALVIN_EXEC_WR:
		// Phase 5: Execute transaction / perform local writes
		DEBUG("(%ld,%ld) execute writes\n",txn->txn_id,txn->batch_id);
		rc = run_ycsb(yield,cor_id);
		this->phase = CALVIN_DONE;
		break;
	  default:
		assert(false);
	}

  }
  uint64_t curr_time = get_sys_clock();
  txn_stats.process_time += curr_time - starttime;
  txn_stats.process_time_short += curr_time - starttime;
  txn_stats.wait_starttime = get_sys_clock();
  return rc;
}

RC YCSBTxnManager::run_ycsb(yield_func_t &yield,uint64_t cor_id) {
  RC rc = RCOK;
  assert(CC_ALG == CALVIN);
  YCSBQuery* ycsb_query = (YCSBQuery*) query;

  for (uint64_t i = 0; i < ycsb_query->requests.size(); i++) {
	  ycsb_request * req = ycsb_query->requests[i];
	if (this->phase == CALVIN_LOC_RD && req->acctype == WR) continue;
	if (this->phase == CALVIN_EXEC_WR && req->acctype == RD) continue;

		uint64_t part_id = _wl->key_to_part( req->key );
	bool loc = GET_NODE_ID(part_id) == g_node_id;

	if (!loc) continue;

	rc = run_ycsb_0(yield,req,row,cor_id);
	assert(rc == RCOK);

	rc = run_ycsb_1(req->acctype,row);
	assert(rc == RCOK);
  }
  return rc;

}

