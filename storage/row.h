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

#ifndef _ROW_H_
#define _ROW_H_

#include <cassert>
#include "global.h"
#include "routine.h"

#define ROW_DEFAULT_SIZE 1000


#define DECL_SET_VALUE(type) void set_value(int col_id, type value);

#define SET_VALUE(type) \
	void row_t::set_value(int col_id, type value) { set_value(col_id, &value); }

#define DECL_GET_VALUE(type) void get_value(int col_id, type &value);

/*
#define GET_VALUE(type)\
	void row_t::get_value(int col_id, type & value) {\
	value = *(type *)data; \
	}
	*/
#define GET_VALUE(type)\
	void row_t::get_value(int col_id, type & value) {\
		int pos = get_schema()->get_field_index(col_id);\
	DEBUG("get_value pos %d -- %lx\n",pos,(uint64_t)this); \
		value = *(type *)&data[pos];\
	}

class table_t;
class Catalog;
class TxnManager;
class Row_lock;
class Row_mvcc;
class Row_ts;
class Row_occ;
class Row_maat;
class Row_specex;
class Row_null;
class Row_rdma_2pl;

//struct RdmaMVHis;

struct RdmaMVHis {
    uint64_t mutex;//lock
    uint64_t rts;
    uint64_t start_ts;
    uint64_t end_ts;
    uint64_t txn_id;
    //RTS、start_ts、end_ts、txn-id：
	char data[ROW_DEFAULT_SIZE];
};

class row_t {
public:
	static int get_row_size(int tuple_size);
	RC init(table_t * host_table, uint64_t part_id, uint64_t row_id = 0);
	RC switch_schema(table_t * host_table);
	// not every row has a manager
	void init_manager(row_t * row);
  	RC remote_copy_row(row_t* remote_row, TxnManager * txn, Access *access);

	table_t * get_table();
	Catalog * get_schema();
	const char * get_table_name();
	uint64_t get_field_cnt();
	uint64_t get_tuple_size();
	uint64_t get_row_id() { return _row_id; };

	void copy(row_t * src);

	void 		set_primary_key(uint64_t key) { _primary_key = key; };
	uint64_t 	get_primary_key() {return _primary_key; };
	uint64_t 	get_part_id() { return _part_id; };

	void set_value(int id, void * ptr);
	void set_value(int id, void * ptr, int size);
	void set_value(const char * col_name, void * ptr);
	char * get_value(int id);
	char * get_value(char * col_name);

	DECL_SET_VALUE(uint64_t);
	DECL_SET_VALUE(int64_t);
	DECL_SET_VALUE(double);
	DECL_SET_VALUE(UInt32);
	DECL_SET_VALUE(SInt32);

	DECL_GET_VALUE(uint64_t);
	DECL_GET_VALUE(int64_t);
	DECL_GET_VALUE(double);
	DECL_GET_VALUE(UInt32);
	DECL_GET_VALUE(SInt32);


	void set_data(char * data);
	char * get_data();

	void free_row();

	// for concurrency control. can be lock, timestamp etc.
	RC get_lock(access_t type, TxnManager * txn);
	RC get_ts(uint64_t &orig_wts, uint64_t &orig_rts);
	RC get_row(access_t type, TxnManager * txn, row_t *& row, uint64_t &orig_wts, uint64_t &orig_rts);
	RC get_row(access_t type, TxnManager *txn, Access *access);
	RC get_row(yield_func_t &yield,access_t type, TxnManager *txn, Access *access,uint64_t cor_id);
	RC get_row_post_wait(access_t type, TxnManager * txn, row_t *& row);
	uint64_t return_row(RC rc, access_t type, TxnManager *txn, row_t *row);
	void return_row(RC rc, access_t type, TxnManager * txn, row_t * row, uint64_t _min_commit_ts);

	#if CC_ALG == RDMA_NO_WAIT 
		volatile uint64_t _tid_word; 
		volatile uint64_t wts; //commit timestamp of the latest transaction that writes this item
		volatile uint64_t _txn_id; //For debug.
		// volatile uint64_t lock_type;
		// volatile uint64_t lock_owner[LOCK_LENGTH]; //解锁
		Row_rdma_2pl * manager;
	#elif CC_ALG == RDMA_NO_WAIT3
		volatile uint64_t _tid_word; 
		volatile uint64_t wts; //commit timestamp of the latest transaction that writes this item
		volatile uint64_t lock_type;
		volatile uint64_t lock_owner[LOCK_LENGTH]; //解锁
		Row_rdma_2pl * manager;
	#elif CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE || CC_ALG == CALVIN || CC_ALG == WOUND_WAIT
		Row_lock * manager;
	#elif CC_ALG == TIMESTAMP
	 	Row_ts * manager;
	#elif CC_ALG == MVCC
		Row_mvcc * manager;
	#elif CC_ALG == OCC 
		Row_occ * manager;
	#elif CC_ALG == MAAT
		Row_maat * manager;
	#elif CC_ALG == HSTORE_SPEC
		Row_specex * manager;
	#elif CC_ALG == AVOID
		Row_avoid * manager;
	#elif CC_ALG == CNULL
		Row_null * manager;
	#endif
	int tuple_size;
	table_t * table;
	char table_name[15];
    int table_idx;
private:
	// primary key should be calculated from the data stored in the row.
	uint64_t 		_primary_key;
	uint64_t		_part_id;
	bool part_info;
	uint64_t _row_id;
public:
#if RDMA_ONE_SIDE == true// == CHANGE_MSG_QUEUE || USE_RDMA == CHANGE_TCP_ONLY
	//#if CC_ALG != RDMA_MVCC
    char data[1];
	// char data[HIS_CHAIN_NUM * sizeof(get_row_size)]
    //#endif
#else
	char * data;
#endif
};

#endif
