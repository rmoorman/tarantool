/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#define MH_SOURCE 1
#include "box/bsync.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "cfg.h"
#include "fio.h"
#include "coio.h"
#include "coio_buf.h"
#include "memory.h"
#include "scoped_guard.h"
#include "box/box.h"
#include "box/txn.h"
#include "box/port.h"
#include "box/schema.h"
#include "box/space.h"
#include "box/tuple.h"
#include "box/request.h"
#include "msgpuck/msgpuck.h"

#include "box/bsync_hash.h"
#include "box/iproto_constants.h"

#define BSYNC_MAX_HOSTS 14
#define BSYNC_TRACE {\
	say_debug("[%p] %s:%d current state: nconnected=%d, state=%d,"\
		" naccepted=%d, leader=%d", \
	fiber(), __PRETTY_FUNCTION__, __LINE__, bsync_state.num_connected, \
	bsync_state.state, bsync_state.num_accepted, (int)bsync_state.leader_id);}

static void* bsync_thread(void*);
static void bsync_process_fiber(va_list ap);
static void bsync_connected(uint8_t host_id);
static void bsync_out_fiber(va_list ap);

static struct coio_service bsync_coio;
static struct wal_writer* wal_local_writer = NULL;
static struct recovery_state *recovery_state;

static struct ev_loop* txn_loop;
static struct ev_loop* bsync_loop;
static struct ev_async txn_process_event;
static struct ev_async bsync_process_event;

struct bsync_host_info {/* for save in host queue */
	uint8_t code;
	struct bsync_operation *op;

	struct rlist list;
};

struct bsync_region {
	struct region pool;

	struct rlist list;
};

struct bsync_common {
	struct bsync_region *region;
	struct bsync_key *dup_key;
};

struct bsync_txn_info { /* txn information about operation */
	struct xrow_header *row;
	struct fiber *owner;
	struct bsync_operation *op;
	struct bsync_common *common;
	int result;
	bool repeat;
	bool proxy;

	struct rlist list;
	STAILQ_ENTRY(bsync_txn_info) fifo;
};
STAILQ_HEAD(bsync_fifo, bsync_txn_info);

enum bsync_operation_status {
	bsync_op_status_init = 0,
	bsync_op_status_accept = 1,
	bsync_op_status_wal = 2,
	bsync_op_status_submit = 3,
	bsync_op_status_yield = 4
};

struct bsync_operation {
	uint64_t gsn;
	uint64_t lsn;
	uint8_t server_id;
	uint8_t status;
	uint8_t accepted;
	uint8_t rejected;

	struct fiber *owner;
	struct bsync_common *common;
	struct bsync_txn_info *txn_data;

	struct rlist list;
};

enum bsync_host_flags {
	bsync_host_active_read = 0x01,
	bsync_host_active_write = 0x02,
	bsync_host_rollback = 0x04,
	bsync_host_disconnected = 0x08,
	bsync_host_reconnect_sleep = 0x10
};

struct bsync_host_data {
	uint8_t connected;
	uint8_t flags;
	uint64_t gsn;
	char source[REMOTE_SOURCE_MAXLEN];
	struct uri uri;
	struct fiber *fiber_out;
	struct fiber *fiber_in;
	uint64_t commit_gsn;
	uint64_t submit_gsn;

	struct rlist send_queue;
	struct rlist op_queue;
	struct mh_bsync_t *active_ops;
	/* election buffers */
	uint8_t election_code;
	uint8_t election_host;
};
static struct bsync_host_data bsync_index[BSYNC_MAX_HOSTS];

static struct bsync_state_ {
	uint8_t local_id;
	int8_t leader_id;
	uint64_t lsn;
	uint8_t num_hosts;
	uint8_t num_connected;
	uint8_t state;
	uint8_t num_accepted;
	bool rollback;
	uint64_t wal_commit_gsn;
	uint64_t wal_rollback_gsn;

	uint64_t recovery_hosts;
	ev_tstamp connect_timeout;
	ev_tstamp read_timeout;
	ev_tstamp write_timeout;
	ev_tstamp reconnect_timeout;
	ev_tstamp operation_timeout;
	ev_tstamp slow_host_timeout;
	ev_tstamp ping_timeout;
	ev_tstamp election_timeout;
	ev_tstamp submit_timeout;

	const char** iproxy_pos;
	const char* iproxy_end;

	struct rlist proxy_queue;
	struct rlist txn_queue;
	struct rlist execute_queue; // executing operations
	struct rlist commit_queue; // submitted operations

	struct rlist txn_fiber_cache;
	struct rlist bsync_fiber_cache;

	struct rlist election_ops;

	struct bsync_fifo txn_proxy_queue;
	struct bsync_fifo txn_proxy_input;

	struct bsync_fifo bsync_proxy_queue;
	struct bsync_fifo bsync_proxy_input;

	struct mempool region_pool;
	struct slab_cache slabc;
	struct rlist region_free;
	struct rlist region_gc;

	/* TODO : temporary hack for support system operations on proxy side */
	bool is_proceed_system;
	bool is_shutdown;
	struct cord cord;
	pthread_mutex_t mutex;
	pthread_mutex_t active_ops_mutex;
	pthread_cond_t cond;
} bsync_state;

#define BSYNC_LOCAL bsync_index[bsync_state.local_id]
#define BSYNC_LEADER bsync_index[bsync_state.leader_id]
#define BSYNC_REMOTE bsync_index[host_id]
#define BSYNC_LOCK(M) \
	tt_pthread_mutex_lock(&M); \
	auto guard = make_scoped_guard([&]() { \
		tt_pthread_mutex_unlock(&M); \
	})

static struct fiber*
bsync_fiber(struct rlist *lst, void (*f) (va_list))
{
	struct fiber *result = NULL;
	if (! rlist_empty(lst))
		result =  rlist_shift_entry(lst, struct fiber, state);
	else
		result = fiber_new("bsync_proc", f);
	return result;
}

static struct bsync_region *
bsync_new_region()
{BSYNC_TRACE
	BSYNC_LOCK(bsync_state.mutex);
	if (! rlist_empty(&bsync_state.region_free)) {
		return rlist_shift_entry(&bsync_state.region_free,
					 struct bsync_region, list);
	} else {
		struct bsync_region* region = (struct bsync_region *)
			mempool_alloc(&bsync_state.region_pool);
		memset(region, 0, sizeof(*region));
		region_create(&region->pool, &bsync_state.slabc);
		return region;
	}
}

static void
bsync_free_region(struct bsync_common *data)
{BSYNC_TRACE
	if (!data->region) return;
	tt_pthread_mutex_lock(&bsync_state.mutex);
	rlist_add_entry(&bsync_state.region_gc, data->region, list);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	data->region = NULL;
}

static void
bsync_dump_region()
{
	struct rlist region_gc;
	rlist_create(&region_gc);
	tt_pthread_mutex_lock(&bsync_state.mutex);
	rlist_swap(&region_gc, &bsync_state.region_gc);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	if (rlist_empty(&region_gc)) return;
	struct bsync_region *next =
		rlist_first_entry(&region_gc, struct bsync_region, list);
	struct bsync_region *cur = NULL;
	while (! rlist_empty(&region_gc)) {
		cur = next;
		next = rlist_next_entry(cur, list);
		region_reset(&cur->pool);
		rlist_move_entry(&bsync_state.region_free, cur, list);
	}
	rlist_del(&region_gc);
}

static uint8_t
bsync_max_host()
{BSYNC_TRACE
	uint8_t max_host_id = 0;
	for (uint8_t i = 1; i < bsync_state.num_hosts; ++i) {
		if (bsync_index[i].gsn >= bsync_index[max_host_id].gsn &&
			bsync_index[i].connected == 2)
		{
			max_host_id = i;
		}
	}
	say_debug("max_host_id is %d", (int)max_host_id);
	return max_host_id;
}

enum bsync_message_type {
	bsync_mtype_leader_proposal = 0,
	bsync_mtype_leader_promise = 1,
	bsync_mtype_leader_accept = 2,
	bsync_mtype_leader_submit = 3,
	bsync_mtype_leader_reject = 4,
	bsync_mtype_body = 5,
	bsync_mtype_submit = 6,
	bsync_mtype_reject = 7,
	bsync_mtype_proxy_request = 8,
	bsync_mtype_proxy_accept = 9,
	bsync_mtype_proxy_reject = 10,
	bsync_mtype_proxy_join = 11,
	bsync_mtype_commit = 12,
	bsync_mtype_rollback = 13,
	bsync_mtype_ping = 14,
	bsync_mtype_count = 15,
	bsync_mtype_hello = 16,
	bsync_mtype_none = 17
};

enum bsync_machine_state {
	bsync_state_started = 0,
	bsync_state_initial = 1,
	bsync_state_leader_accept = 2,
	bsync_state_recovery = 3,
	bsync_state_wait_recovery = 4,
	bsync_state_ready = 5
};

enum bsync_iproto_flags {
	bsync_iproto_commit_gsn = 0x01,
};

static const char* bsync_mtype_name[] = {
	"leader_proposal",
	"leader_promise",
	"leader_accept",
	"leader_submit",
	"leader_reject",
	"body",
	"submit",
	"reject",
	"proxy_request",
	"proxy_accept",
	"proxy_reject",
	"proxy_join",
	"commit",
	"rollback",
	"ping",
	"INVALID",
	"hello",
	"INVALID"
};

#define SWITCH_TO_BSYNC {\
	tt_pthread_mutex_lock(&bsync_state.mutex); \
	bool was_empty = STAILQ_EMPTY(&bsync_state.bsync_proxy_queue); \
	STAILQ_INSERT_TAIL(&bsync_state.bsync_proxy_queue, info, fifo); \
	if (was_empty) ev_async_send(bsync_loop, &bsync_process_event); \
	tt_pthread_mutex_unlock(&bsync_state.mutex); }

#define SWITCH_TO_TXN {\
	say_debug("send to TXN operation %ld from %d", oper->gsn, __LINE__); \
	tt_pthread_mutex_lock(&bsync_state.mutex); \
	bool was_empty = STAILQ_EMPTY(&bsync_state.txn_proxy_queue); \
	STAILQ_INSERT_TAIL(&bsync_state.txn_proxy_queue, oper->txn_data, fifo); \
	if (was_empty) ev_async_send(txn_loop, &txn_process_event); \
	tt_pthread_mutex_unlock(&bsync_state.mutex); }

static bool
bsync_begin_op(struct bsync_key *key, uint32_t server_id)
{BSYNC_TRACE
	if (server_id == BSYNC_SERVER_ID) return true;
	/*
	 * TODO : special analyze for operations with spaces (remove/create)
	 */
	BSYNC_LOCK(bsync_state.active_ops_mutex);
	mh_int_t keys[BSYNC_MAX_HOSTS];
	for (uint8_t host_id = 0; host_id < bsync_state.num_hosts; ++host_id) {
		if (BSYNC_REMOTE.connected < 2 || host_id == bsync_state.local_id) {
			keys[host_id] = -1;
			continue;
		}
		keys[host_id] = mh_bsync_find(BSYNC_REMOTE.active_ops, *key, NULL);
		if (keys[host_id] == mh_end(BSYNC_REMOTE.active_ops))
			continue;
		struct mh_bsync_node_t *node =
			mh_bsync_node(BSYNC_REMOTE.active_ops, keys[host_id]);
		if (server_id != 0 && node->val.remote_id != server_id) {
			char buffer[128];
			snprintf(buffer, key->size, "%s", key->data);
			if (node->val.remote_id == 0) {
				say_error("conflict with operations from %s and %s"
					" in %s. space is %d, key is %s",
					bsync_index[server_id - 1].source,
					BSYNC_LOCAL.source,
					BSYNC_REMOTE.source,
					key->space_id,
					buffer);
			} else {
				say_error("conflict with operations from %s and %s"
					" in %s. space is %d, key is %s",
					bsync_index[server_id - 1].source,
					bsync_index[node->val.remote_id - 1].source,
					BSYNC_REMOTE.source,
					key->space_id,
					buffer);
			}
			assert(node->val.remote_id != 0 ||
				node->key.space_id < 512 ||
				node->key.data[0] != '4');
			return false;
		}
	}
	for (uint8_t host_id = 0; host_id < bsync_state.num_hosts; ++host_id) {
		if (keys[host_id] == -1) continue;
		if (keys[host_id] != mh_end(BSYNC_REMOTE.active_ops)) {
			struct mh_bsync_node_t *node =
				mh_bsync_node(BSYNC_REMOTE.active_ops, keys[host_id]);
			if (server_id == 0) {
				++node->val.local_ops;
			} else {
				node->val.remote_id = server_id;
				++node->val.remote_ops;
			}
			node->key.data = key->data;
		} else {
			struct mh_bsync_node_t node;
			node.key = *key;
			if (server_id == 0) {
				node.val.local_ops = 1;
				node.val.remote_ops = 0;
			} else {
				node.val.local_ops = 0;
				node.val.remote_ops = 1;
			}
			node.val.remote_id = server_id;
			mh_bsync_put(BSYNC_REMOTE.active_ops, &node, NULL, NULL);
		}
	}
	return true;
}

static void
bsync_end_op(uint8_t host_id, struct bsync_key *key, uint32_t server_id)
{BSYNC_TRACE
	if (server_id == BSYNC_SERVER_ID) return;
	BSYNC_LOCK(bsync_state.active_ops_mutex);
	mh_int_t k = mh_bsync_find(BSYNC_REMOTE.active_ops, *key, NULL);
	if (k == mh_end(BSYNC_REMOTE.active_ops)) return;
	struct mh_bsync_node_t *node =
		mh_bsync_node(BSYNC_REMOTE.active_ops, k);
	if (server_id != 0)
		--node->val.remote_ops;
	else
		--node->val.local_ops;
	if ((node->val.local_ops + node->val.remote_ops) == 0)
		mh_bsync_del(BSYNC_REMOTE.active_ops, k, NULL);
}

struct bsync_parse_data {
	uint32_t space_id;
	bool is_tuple;
	const char *data;
	const char *end;
};

static void
bsync_space_cb(void *d, uint8_t key, uint32_t v)
{
	if (key == IPROTO_SPACE_ID)
		((struct bsync_parse_data *)d)->space_id = v;
}

static void
bsync_tuple_cb(void *d, uint8_t key, const char *v, const char *vend)
{
	((struct bsync_parse_data *)d)->data = v;
	((struct bsync_parse_data *)d)->end = vend;
	((struct bsync_parse_data *)d)->is_tuple = (key == IPROTO_TUPLE);
}

#define BSYNC_MAX_KEY_PART_LEN 256
static void
bsync_parse_dup_key(struct bsync_common *data, struct key_def *key,
		    struct tuple *tuple)
{
	data->dup_key = (struct bsync_key *)
		region_alloc(&data->region->pool, sizeof(struct bsync_key));
	data->dup_key->size = 0;
	data->dup_key->data = (char *)region_alloc(&data->region->pool,
		BSYNC_MAX_KEY_PART_LEN * key->part_count);
	char *i_data = data->dup_key->data;
	for (uint32_t p = 0; p < key->part_count; ++p) {
		if (key->parts[p].type == NUM) {
			uint32_t v = tuple_field_u32(tuple, key->parts[p].fieldno);
			memcpy(i_data, &v, sizeof(uint32_t));
			data->dup_key->size += sizeof(uint32_t);
			i_data += sizeof(uint32_t);
			continue;
		}
		const char *key_part =
			tuple_field_cstr(tuple, key->parts[p].fieldno);
		size_t key_part_len = strlen(key_part);
		data->dup_key->size += key_part_len;
		memcpy(i_data, key_part, key_part_len + 1);
		i_data += data->dup_key->size;
	}
	data->dup_key->space_id = key->space_id;
}

int
bsync_write(struct recovery_state *r, struct txn_stmt *stmt) try
{BSYNC_TRACE
	if (bsync_state.rollback) {
		if (stmt->row->server_id != 0) {
			rlist_shift(&bsync_state.txn_queue);
			fiber_yield();
		}
		return 0;
	}
	if (wal_local_writer == NULL) return wal_write_lsn(r, stmt->row);
	bsync_dump_region();

	stmt->row->tm = ev_now(loop());
	stmt->row->sync = 0;
	struct bsync_txn_info *info = NULL;
	uint32_t server_id = stmt->row->server_id;
	if (server_id == 0) { /* local request */
		info = (struct bsync_txn_info *)
			region_alloc(&fiber()->gc, sizeof(struct bsync_txn_info));
		info->common = (struct bsync_common *)
			region_alloc(&fiber()->gc, sizeof(struct bsync_common));
		info->common->region = bsync_new_region();
		info->row = stmt->row;
		info->op = NULL;
		info->result = -1;
		info->repeat = false;
		info->proxy = false;
		if (stmt->new_tuple) {
			bsync_parse_dup_key(info->common,
				stmt->space->index[0]->key_def, stmt->new_tuple);
		} else {
			bsync_parse_dup_key(info->common,
				stmt->space->index[0]->key_def, stmt->old_tuple);
		}
		bool begin_result = bsync_begin_op(info->common->dup_key, server_id);
		(void)begin_result;
		assert(begin_result);
	} else { /* proxy request */
		info = rlist_shift_entry(&bsync_state.txn_queue,
					 struct bsync_txn_info, list);
		say_debug("send request %ld to bsync", info->op->gsn);
	}
	rlist_add_tail_entry(&bsync_state.execute_queue, info, list);
	info->owner = fiber();
	SWITCH_TO_BSYNC
	fiber_yield();BSYNC_TRACE
	if (!bsync_state.rollback) {
		rlist_del_entry(info, list);
		if (info->result >= 0) {
			info->repeat = false;
			return info->result;
		}
		tt_pthread_mutex_lock(&bsync_state.mutex);
		bsync_state.rollback = true;
		/* rollback in reverse order local operations */
		say_info("conflict detected, start to rollback");
		struct bsync_txn_info *s;
		rlist_foreach_entry_reverse(s, &bsync_state.execute_queue, list) {
			s->result = -1;
			if (s->repeat && server_id == 0) {
				// its a local request, we need to copy row for reapply it after rollback all operations
			}
			fiber_call(s->owner);
		}
		rlist_create(&bsync_state.execute_queue);
		STAILQ_INIT(&bsync_state.bsync_proxy_input);
		STAILQ_INIT(&bsync_state.bsync_proxy_queue);
		say_info("start to reapply commited ops");
		struct bsync_operation *oper;
		rlist_foreach_entry(oper, &bsync_state.commit_queue, list) {
			fiber_call(oper->txn_data->owner);
		}
		bsync_state.rollback = false;
		say_info("finish rollback execution");
		tt_pthread_mutex_unlock(&bsync_state.mutex);
	}
	info->owner = NULL;
	return info->result;
} catch (...) {
	say_crit("bsync_write found unhandled exception");
	throw;
}

static void
bsync_send_data(struct bsync_host_data *host, struct bsync_host_info *elem)
{BSYNC_TRACE
	/* TODO : check all send ops for memory leak case */
	if (host->connected < 2) return;
	rlist_add_tail_entry(&host->send_queue, elem, list);
	if ((host->flags & bsync_host_active_write) == 0) {
		fiber_call(host->fiber_out);
	}
}

static uint64_t
bsync_update_gsn(uint64_t gsn)
{BSYNC_TRACE
	assert(BSYNC_LOCAL.gsn <= gsn);
	BSYNC_LOCAL.gsn = gsn;
	return gsn;
}

static int
bsync_wal_write(struct xrow_header *row)
{BSYNC_TRACE
	if (bsync_state.is_shutdown) return -1;
	if (bsync_state.wal_commit_gsn) {
		row->commit_sn = bsync_state.wal_commit_gsn;
		bsync_state.wal_commit_gsn = 0;
	} else {
		assert(row->commit_sn == 0);
	}
	if (bsync_state.wal_rollback_gsn) {
		row->rollback_sn = bsync_state.wal_rollback_gsn;
		bsync_state.wal_rollback_gsn = 0;
	} else {
		assert(row->rollback_sn == 0);
	}
	return wal_write(wal_local_writer, row);
}

static void
bsync_rollback_slave(struct bsync_operation *oper)
{BSYNC_TRACE
	struct bsync_operation *op;
	rlist_foreach_entry(op, &bsync_state.commit_queue, list) {
		op->txn_data->repeat = true;
	}
	SWITCH_TO_TXN
	struct bsync_host_info *elem = (struct bsync_host_info *)
		region_alloc(&fiber()->gc, sizeof(struct bsync_host_info));
	elem->code = bsync_mtype_proxy_join;
	elem->op = NULL;
	bsync_send_data(&BSYNC_LEADER, elem);
}

static void
bsync_queue_slave(struct bsync_operation *oper)
{BSYNC_TRACE
	struct bsync_host_info *elem = (struct bsync_host_info *)
		region_alloc(&fiber()->gc, sizeof(struct bsync_host_info));
	oper->gsn = 0;
	oper->txn_data->row->server_id =
		oper->server_id = bsync_state.local_id + 1;
	elem->code = bsync_mtype_proxy_request;
	elem->op = oper;
	oper->txn_data->result = 0;
	say_debug("start to proceed request %ld", oper->lsn);
	rlist_add_tail_entry(&bsync_state.proxy_queue, oper, list);
	bsync_send_data(&BSYNC_LEADER, elem);
	/* wait accept or reject */
	fiber_yield();BSYNC_TRACE
	if (oper->txn_data->result < 0) {
		say_debug("request %ld rejected", oper->lsn);
		bsync_rollback_slave(oper);
		return;
	}
	say_debug("request %ld accepted to gsn %ld", oper->lsn, oper->gsn);
	oper->status = bsync_op_status_accept;
	oper->txn_data->row->server_id = BSYNC_SERVER_ID;
	oper->txn_data->row->lsn = bsync_update_gsn(oper->gsn);
	oper->status = bsync_op_status_wal;
	int wal_result = bsync_wal_write(oper->txn_data->row);
	elem->code = wal_result < 0 ? bsync_mtype_reject
				    : bsync_mtype_submit;
	rlist_add_tail_entry(&bsync_state.commit_queue, oper, list);
	bsync_send_data(&BSYNC_LEADER, elem);
	if (oper->status == bsync_op_status_wal) {
		oper->status = bsync_op_status_yield;
		oper->txn_data->result = wal_result;
		fiber_yield();BSYNC_TRACE
	}
	SWITCH_TO_TXN
	bsync_free_region(oper->common);
}

static void
bsync_wait_slow(struct bsync_operation *oper)
{BSYNC_TRACE
	ev_tstamp start = ev_now(loop());
	if (2 * oper->accepted > bsync_state.num_hosts) {
		bsync_state.wal_commit_gsn = oper->gsn;
	}
	while ((oper->accepted + oper->rejected) < bsync_state.num_hosts) {
		oper->status = bsync_op_status_yield;
		/* TODO : implement correct slow detection */
		fiber_yield_timeout(bsync_state.slow_host_timeout);BSYNC_TRACE
		if (ev_now(loop()) - start <= bsync_state.slow_host_timeout)
			continue;
	}
	for (uint8_t host_id = 0; host_id < bsync_state.num_hosts; ++host_id) {
		if (BSYNC_REMOTE.commit_gsn == BSYNC_REMOTE.submit_gsn ||
			bsync_state.local_id == host_id ||
			(BSYNC_REMOTE.flags & bsync_host_active_write) != 0)
		{
			continue;
		}
		if (BSYNC_REMOTE.fiber_out)
			fiber_call(BSYNC_REMOTE.fiber_out);
	}
	BSYNC_TRACE
}

static void
bsync_queue_leader(struct bsync_operation *oper, bool proxy)
{BSYNC_TRACE
	oper->status = bsync_op_status_init;
	oper->gsn = ++BSYNC_LOCAL.gsn;
	say_debug("start to proceed request %ld", oper->gsn);
	oper->server_id = oper->txn_data->row->server_id;
	oper->rejected = bsync_state.num_hosts - bsync_state.num_connected;
	oper->accepted = 0;
	for (uint8_t host_id = 0; host_id < bsync_state.num_hosts; ++host_id) {
		if (BSYNC_REMOTE.connected < 2 || host_id == bsync_state.local_id)
			continue;
		struct bsync_host_info *elem = (struct bsync_host_info *)
			region_alloc(&fiber()->gc, sizeof(struct bsync_host_info));
		elem->op = oper;
		if (oper->server_id == (host_id + 1)) {
			elem->code = bsync_mtype_proxy_accept;
			oper->txn_data->row->lsn = oper->lsn;
			oper->txn_data->row->server_id = oper->server_id;
		} else {
			elem->code = bsync_mtype_body;
			oper->txn_data->row->lsn = oper->gsn;
			oper->txn_data->row->server_id = BSYNC_SERVER_ID;
		}
		say_debug("********** send accept/body from fiber %ld",
			  (ptrdiff_t)elem->op->owner);
		bsync_send_data(&BSYNC_REMOTE, elem);
	}
	oper->txn_data->row->lsn = bsync_update_gsn(oper->gsn);
	oper->txn_data->row->server_id = BSYNC_SERVER_ID;
	oper->status = bsync_op_status_wal;
	oper->txn_data->result = bsync_wal_write(oper->txn_data->row);
	if (oper->txn_data->result < 0) ++oper->rejected;
	else ++oper->accepted;
	ev_tstamp start = ev_now(loop());
	oper->status = bsync_op_status_yield;
	while (2 * oper->accepted <= bsync_state.num_hosts) {
		if (ev_now(loop()) - start > bsync_state.operation_timeout ||
			2 * oper->rejected > bsync_state.num_hosts)
		{
			break;
		}
		fiber_yield_timeout(bsync_state.operation_timeout);BSYNC_TRACE
	}
	oper->txn_data->result =
		(2 * oper->accepted > bsync_state.num_hosts ? 0 : -1);
	if (!proxy) {
		SWITCH_TO_TXN
		bsync_wait_slow(oper);
		bsync_free_region(oper->common);
	}
}

static void
bsync_proxy_processor()
{BSYNC_TRACE
	struct bsync_operation *oper = (struct bsync_operation *)
		region_alloc0(&fiber()->gc, sizeof(struct bsync_operation));
	struct bsync_host_info *send = (struct bsync_host_info *)
		region_alloc0(&fiber()->gc, sizeof(bsync_host_info));
	send->op = oper;
	oper->txn_data = (struct bsync_txn_info *)
		region_alloc(&fiber()->gc, sizeof(struct bsync_txn_info));
	oper->txn_data->common = oper->common = (struct bsync_common *)
		region_alloc0(&fiber()->gc, sizeof(struct bsync_common));
	oper->txn_data->row = (struct xrow_header *)
		region_alloc0(&fiber()->gc, sizeof(struct xrow_header));
	oper->txn_data->op = oper;
	oper->txn_data->result = 0;
	oper->txn_data->owner = NULL;
	oper->txn_data->repeat = false;
	oper->txn_data->proxy = true;
	xrow_header_decode(oper->txn_data->row, bsync_state.iproxy_pos,
			   bsync_state.iproxy_end);
	struct iovec xrow_body[XROW_BODY_IOVMAX];
	memcpy(xrow_body, oper->txn_data->row->body,
		sizeof(oper->txn_data->row->body));
	for (int i = 0; i < oper->txn_data->row->bodycnt; ++i) {
		oper->txn_data->row->body[i].iov_base =
			region_alloc(&fiber()->gc, xrow_body[i].iov_len);
		memcpy(oper->txn_data->row->body[i].iov_base,
			xrow_body[i].iov_base, xrow_body[i].iov_len);
	}
	bsync_state.iproxy_pos = NULL;
	bsync_state.iproxy_end = NULL;
	oper->server_id = oper->txn_data->row->server_id;
	oper->lsn = oper->txn_data->row->lsn;
	oper->owner = fiber();
	bool slave_proxy = (bsync_state.leader_id != bsync_state.local_id);
	if (slave_proxy) {
		oper->status = bsync_op_status_wal;
		oper->gsn = bsync_update_gsn(oper->txn_data->row->lsn);
		say_debug("start to apply request %ld to WAL", oper->gsn);
		oper->txn_data->result = bsync_wal_write(oper->txn_data->row);
		say_debug("submit request %ld", oper->gsn);
		send->code = (oper->txn_data->result < 0 ? bsync_mtype_reject :
							   bsync_mtype_submit);
		bsync_send_data(&BSYNC_LEADER, send);
		if (oper->txn_data->result >= 0) {
			rlist_add_tail_entry(&bsync_state.commit_queue, oper, list);
		}
	}
	if (oper->txn_data->result >= 0) {
		say_debug("start to apply request %ld to TXN", oper->gsn);
		SWITCH_TO_TXN
		fiber_yield();BSYNC_TRACE
	}
	if (slave_proxy) {
		if (oper->status != bsync_op_status_submit) {
			oper->status = bsync_op_status_yield;
			fiber_yield();BSYNC_TRACE
		}
		SWITCH_TO_TXN
	} else if (oper->txn_data->result < 0) {
		uint8_t host_id = oper->server_id - 1;
		if (BSYNC_REMOTE.flags & bsync_host_rollback)
			return;
		BSYNC_REMOTE.flags |= bsync_host_rollback;
		/* drop all active operations from host */
		tt_pthread_mutex_lock(&bsync_state.mutex);
		struct bsync_txn_info *info;
		STAILQ_FOREACH(info, &bsync_state.txn_proxy_input, fifo) {
			if (info->row->server_id == oper->server_id)
				info->result = -1;
		}
		STAILQ_FOREACH(info, &bsync_state.txn_proxy_queue, fifo) {
			if (info->row->server_id == oper->server_id)
				info->result = -1;
		}
		tt_pthread_cond_signal(&bsync_state.cond);
		tt_pthread_mutex_unlock(&bsync_state.mutex);
		oper->gsn = -1;
		send->code = bsync_mtype_proxy_reject;
		bsync_send_data(&BSYNC_REMOTE, send);
	} else {
		bsync_queue_leader(oper, true);
		uint8_t host_id = oper->server_id - 1;
		send->code = (oper->txn_data->result < 0 ? bsync_mtype_reject :
							   bsync_mtype_commit);
		bsync_send_data(&BSYNC_REMOTE, send);
		bsync_wait_slow(oper);
		SWITCH_TO_TXN
	}
	fiber_yield();
	bsync_free_region(oper->common);
}

static void
bsync_proceed_rollback(struct bsync_txn_info *info)
{BSYNC_TRACE
	info->result = -1;
	SWITCH_TO_BSYNC
	tt_pthread_mutex_lock(&bsync_state.mutex);
	tt_pthread_cond_wait(&bsync_state.cond, &bsync_state.mutex);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
}

/*
 * Command handlers block
 */

static void
bsync_body(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	uint64_t gsn = mp_decode_uint(ipos);
	(void)gsn;
	assert(host_id == bsync_state.leader_id);
	bsync_state.iproxy_pos = ipos;
	bsync_state.iproxy_end = iend;
	fiber_call(
		bsync_fiber(&bsync_state.bsync_fiber_cache, bsync_process_fiber)
	);
}

static void
bsync_do_submit(uint8_t host_id, struct bsync_host_info *info)
{BSYNC_TRACE
	++info->op->accepted;
	bsync_end_op(host_id, info->op->common->dup_key, info->op->server_id);
	BSYNC_REMOTE.submit_gsn = info->op->gsn;
	if (info->op->status == bsync_op_status_yield)
		fiber_call(info->op->owner);
}

static void
bsync_submit(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	*ipos = iend;
	assert(!rlist_empty(&BSYNC_REMOTE.op_queue));
	bsync_do_submit(host_id, rlist_shift_entry(&BSYNC_REMOTE.op_queue,
						struct bsync_host_info, list));
	BSYNC_TRACE
}

static void
bsync_do_reject(uint8_t host_id, struct bsync_host_info *info)
{BSYNC_TRACE
	++info->op->rejected;
	bsync_end_op(host_id, info->op->common->dup_key, info->op->server_id);
	if (info->op->status == bsync_op_status_yield)
		fiber_call(info->op->owner);
}

static void
bsync_reject(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	*ipos = iend;
	assert(!rlist_empty(&BSYNC_REMOTE.op_queue));
	bsync_do_reject(host_id, rlist_shift_entry(&BSYNC_REMOTE.op_queue,
						struct bsync_host_info, list));
}

static void
bsync_proxy_request(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id;
	if ((BSYNC_REMOTE.flags & bsync_host_rollback) != 0 ||
		bsync_state.state != bsync_state_ready)
	{
		/*
		 * skip all proxy requests from node in conflict state or
		 * if we lost consensus
		 */
		*ipos = iend;
		return;
	}
	bsync_state.iproxy_pos = ipos;
	bsync_state.iproxy_end = iend;
	fiber_call(
		bsync_fiber(&bsync_state.bsync_fiber_cache, bsync_process_fiber)
	);
}

static void
bsync_proxy_accept(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	uint64_t gsn = mp_decode_uint(ipos);
	struct bsync_operation *op =
		rlist_shift_entry(&bsync_state.proxy_queue, bsync_operation, list);
	op->gsn = gsn;
	assert(*ipos == iend);
	fiber_call(op->owner);
}

static void
bsync_proxy_reject(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	assert(!rlist_empty(&bsync_state.proxy_queue));
	struct bsync_operation *op =
		rlist_shift_entry(&bsync_state.proxy_queue, bsync_operation, list);
	assert(*ipos == iend);
	op->txn_data->result = -1;
	fiber_call(op->owner);
}

static void
bsync_proxy_join(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	*ipos = iend;
	BSYNC_REMOTE.flags &= !bsync_host_rollback;
}

static void
bsync_commit_op(uint64_t commit_gsn)
{BSYNC_TRACE
	struct bsync_operation *op = NULL;
	do {
		assert(!rlist_empty(&bsync_state.commit_queue));
		op = rlist_shift_entry(&bsync_state.commit_queue,
					bsync_operation, list);
		if (op->status == bsync_op_status_wal) {
			op->status = bsync_op_status_submit;
		} else {
			assert(op->status == bsync_op_status_yield);
			fiber_call(op->owner);
		}
	} while (op->gsn < commit_gsn);
	bsync_state.wal_commit_gsn = commit_gsn;
}

static void
bsync_commit(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	assert(*ipos == iend);
}

static void
bsync_rollback(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	assert(bsync_state.local_id != bsync_state.leader_id);
	uint64_t gsn = mp_decode_uint(ipos);
	(void)gsn;
	/* TODO:
	 * 1. rollback from proxy queue
	 * 2. rollback from commit queue
	 * 3. push commit point to WAL
	 * 4. push rollback point to WAL
	 */
}

static void
bsync_ping(uint8_t host_id, const char **ipos, const char *iend)
{
	(void)host_id; (void)ipos; (void)iend;
	uint64_t gsn = mp_decode_uint(ipos);
	if (gsn == BSYNC_REMOTE.gsn) return;
	uint8_t max_host = bsync_max_host();
	BSYNC_REMOTE.gsn = gsn;
	if (bsync_state.state == bsync_state_ready)
		return;
	if (bsync_max_host() != max_host)
		bsync_connected(host_id);
}

/*
 * Fibers block
 */

static void
bsync_txn_proceed_request(struct bsync_txn_info *info)
{BSYNC_TRACE
	struct request *req = (struct request *)
		region_alloc(&fiber()->gc, sizeof(struct request));
	request_create(req, info->row->type);
	request_decode(req, (const char*)info->row->body[0].iov_base,
			info->row->body[0].iov_len);
	req->header = info->row;
	info->op->common->region = bsync_new_region();
	struct bsync_parse_data data;
	request_header_decode(info->row, bsync_space_cb, bsync_tuple_cb, &data);
	struct space *space = space_cache_find(data.space_id);
	assert(space && space->index_count > 0 &&
		space->index[0]->key_def->iid == 0);
	bsync_state.is_proceed_system = space_is_system(space);
	struct tuple *tuple = NULL;
	if (data.is_tuple) {
		tuple = tuple_new(space->format, data.data, data.end);
		space_validate_tuple(space, tuple);
	} else {
		const char *key = req->key;
		uint32_t part_count = mp_decode_array(&key);
		tuple = space->index[0]->findByKey(key, part_count);
	}
	TupleGuard guard(tuple);
	bsync_parse_dup_key(info->common, space->index[0]->key_def, tuple);
	if (bsync_begin_op(info->common->dup_key, info->op->server_id)) {
		rlist_add_tail_entry(&bsync_state.txn_queue, info, list);
		try {
			box_process(req, &null_port);
		} catch (Exception *e) {
			if (info->op->server_id == BSYNC_SERVER_ID) {
				if (!bsync_state.rollback) {
					rlist_del_entry(info, list);
				}
				goto error;
			}
			throw;
		}
	} else {BSYNC_TRACE
		bsync_proceed_rollback(info);
	}
	return;
error:
	bsync_state.is_proceed_system = false;
	info->owner = fiber();
	if (!info->repeat) {
		SWITCH_TO_BSYNC
	}
}

static void
bsync_txn_process_fiber(va_list /* ap */)
{
restart:BSYNC_TRACE
	struct bsync_txn_info *info = STAILQ_FIRST(&bsync_state.txn_proxy_input);
	STAILQ_REMOVE_HEAD(&bsync_state.txn_proxy_input, fifo);
	if (info->result < 0) {
		SWITCH_TO_BSYNC
	} else {
		while (true) {
			bsync_txn_proceed_request(info);
			if (info->repeat) {
				info->repeat = false;
				fiber_yield();
			} else {
				break;
			}
		}
		if (info->proxy) {
			SWITCH_TO_BSYNC
		}
	}
	fiber_gc();
	rlist_add_tail_entry(&bsync_state.txn_fiber_cache, fiber(), state);
	fiber_yield();BSYNC_TRACE
	goto restart;
}

static void
bsync_txn_process(ev_loop *loop, ev_async *watcher, int event)
{BSYNC_TRACE
	(void)loop; (void)watcher; (void)event;
	if (bsync_state.is_proceed_system)
		return;
	tt_pthread_mutex_lock(&bsync_state.mutex);
	STAILQ_CONCAT(&bsync_state.txn_proxy_input,
			&bsync_state.txn_proxy_queue);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	bsync_state.is_proceed_system = false;
	while (!STAILQ_EMPTY(&bsync_state.txn_proxy_input)) {
		struct bsync_txn_info *info =
			STAILQ_FIRST(&bsync_state.txn_proxy_input);
		if (info->owner) {BSYNC_TRACE
			STAILQ_REMOVE_HEAD(&bsync_state.txn_proxy_input, fifo);
			say_debug("[%p] send request %ld to txn",
				  fiber(), info->op->gsn);
			fiber_call(info->owner);
		} else {BSYNC_TRACE
			fiber_call(bsync_fiber(&bsync_state.txn_fiber_cache,
				bsync_txn_process_fiber));
		}
	}
	if (bsync_state.is_proceed_system) {
		bsync_state.is_proceed_system = false;
		bsync_txn_process(loop, watcher, event);
	}
}

static void
bsync_process_fiber(va_list /* ap */)
{
restart:BSYNC_TRACE
	if (bsync_state.iproxy_end) {
		assert(bsync_state.state == bsync_state_ready);
		bsync_proxy_processor();
	} else {
		struct bsync_txn_info *info =
			STAILQ_FIRST(&bsync_state.bsync_proxy_input);
		STAILQ_REMOVE_HEAD(&bsync_state.bsync_proxy_input, fifo);
		if (info->op) {BSYNC_TRACE
			if (bsync_state.state != bsync_state_ready)
				return;
			fiber_call(info->op->owner);
		} else {BSYNC_TRACE
			struct bsync_operation *oper = (struct bsync_operation *)
				region_alloc(&fiber()->gc, sizeof(struct bsync_operation));
			oper->common = (struct bsync_common *)
				region_alloc(&fiber()->gc, sizeof(struct bsync_common));
			oper->txn_data = info;
			oper->txn_data->op = oper;
			oper->common->dup_key = oper->txn_data->common->dup_key;
			oper->common->region = oper->txn_data->common->region;
			oper->lsn = oper->txn_data->row->lsn = ++bsync_state.lsn;
			oper->owner = fiber();
			if (bsync_state.state != bsync_state_ready) {
				rlist_add_tail_entry(&bsync_state.election_ops,
						     oper, list);
				fiber_yield_timeout(bsync_state.election_timeout);
				if (bsync_state.state != bsync_state_ready) {
					BSYNC_TRACE
					info->result = -1;
					SWITCH_TO_TXN
					goto exit;
				}
			}
			if (bsync_state.leader_id == bsync_state.local_id) {
				oper->server_id = 0;
				bsync_queue_leader(oper, false);
			} else {
				oper->server_id = (bsync_state.local_id + 1);
				bsync_queue_slave(oper);
			}
		}
	}
exit:	fiber_gc();
	rlist_add_tail_entry(&bsync_state.bsync_fiber_cache, fiber(), state);
	fiber_yield();
	goto restart;
}

static void
bsync_shutdown_fiber(va_list /* ap */)
{
	if (bsync_state.wal_commit_gsn) {
		struct xrow_header *row = (struct xrow_header *)
			region_alloc(&fiber()->gc, sizeof(struct xrow_header));
		memset(row, 0, sizeof(struct xrow_header));
		bsync_wal_write(row);
		fiber_gc();
	}
	recovery_delete(recovery_state);
	/* TODO : cancel all active bsync process fibers */
	evio_service_stop(&bsync_coio.evio_service);
	ev_break(bsync_loop, 1);
}

static void
bsync_shutdown()
{
	fiber_call(fiber_new("bsync_shutdown", bsync_shutdown_fiber));
}

static void
bsync_process(struct ev_loop *loop, struct ev_async *watcher, int event)
{
	(void)loop; (void)watcher; (void)event;
	if (bsync_state.is_shutdown) {
		bsync_shutdown();
		return;
	}
	tt_pthread_mutex_lock(&bsync_state.mutex);
	STAILQ_CONCAT(&bsync_state.bsync_proxy_input,
			&bsync_state.bsync_proxy_queue);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	while (!STAILQ_EMPTY(&bsync_state.bsync_proxy_input)) {BSYNC_TRACE
		fiber_call(bsync_fiber(&bsync_state.bsync_fiber_cache,
					bsync_process_fiber));
	}
}

/*
 * Leader election block
 */

static void
bsync_connected(uint8_t host_id)
{BSYNC_TRACE
	if (bsync_state.leader_id == bsync_state.local_id) {BSYNC_TRACE
		BSYNC_REMOTE.election_code = bsync_mtype_leader_submit;
		if (BSYNC_REMOTE.fiber_out != fiber()) {
			fiber_call(bsync_index[host_id].fiber_out);
		}
		return;
	}
	if (bsync_state.num_connected == bsync_state.num_hosts &&
		bsync_state.state == bsync_state_started)
	{
		say_info("all hosts connected");
		bsync_state.state = bsync_state_initial;
	}
	if (2 * bsync_state.num_connected <= bsync_state.num_hosts ||
		bsync_state.state != bsync_state_initial)
	{
		return;
	}
	say_info("consensus connected");
	bsync_state.state = bsync_state_initial;
	uint8_t max_host_id = bsync_max_host();
	say_info("next leader should be %s", bsync_index[max_host_id].source);
	if (max_host_id != bsync_state.local_id) {
		bsync_index[max_host_id].election_code =
			bsync_mtype_leader_proposal;
		bsync_index[max_host_id].election_host = max_host_id;
		if (bsync_index[max_host_id].fiber_out != fiber())
			fiber_call(bsync_index[max_host_id].fiber_out);
		return;
	}
	bsync_state.num_accepted = 1;
	bsync_state.state = bsync_state_leader_accept;
	for (uint8_t i = 0; i < bsync_state.num_hosts; ++i) {
		if (i == max_host_id || bsync_index[i].connected < 2) continue;
		bsync_index[i].election_code = bsync_mtype_leader_promise;
		bsync_index[i].election_host = max_host_id;
		if (bsync_index[i].fiber_out != fiber())
			fiber_call(bsync_index[i].fiber_out);
	}
}

static void
bsync_election(va_list /* ap */)
{BSYNC_TRACE
	fiber_yield_timeout(bsync_state.election_timeout);BSYNC_TRACE
	if (bsync_state.state != bsync_state_started)
		return;
	bsync_state.state = bsync_state_initial;
	if (2 * bsync_state.num_connected <= bsync_state.num_hosts)
		return;
	bsync_connected(BSYNC_MAX_HOSTS);
}

static void
bsync_disconnected(uint8_t host_id)
{BSYNC_TRACE
	BSYNC_REMOTE.flags = bsync_host_disconnected;
	--bsync_state.num_connected;
	{
		BSYNC_LOCK(bsync_state.active_ops_mutex);
		mh_bsync_clear(BSYNC_REMOTE.active_ops);
	}
	/* TODO : clean up wait queue using fiber_call() */
	say_warn("disconnecting host %s", BSYNC_REMOTE.source);
	struct bsync_host_info *elem;
	rlist_foreach_entry(elem, &BSYNC_REMOTE.op_queue, list) {
		bsync_do_reject(host_id, elem);
	}
	rlist_foreach_entry(elem, &BSYNC_REMOTE.send_queue, list) {
		switch (elem->code) {
		case bsync_mtype_body:
		case bsync_mtype_proxy_accept:
			bsync_do_reject(host_id, elem);
			break;
		case bsync_mtype_rollback:
			/* TODO */
			break;
		}
	}
	rlist_create(&BSYNC_REMOTE.send_queue);
	rlist_create(&BSYNC_REMOTE.op_queue);
	if (host_id == bsync_state.leader_id) {
		if (!rlist_empty(&bsync_state.proxy_queue)) {
			struct bsync_operation *oper =
				rlist_shift_entry(&bsync_state.proxy_queue,
						struct bsync_operation, list);
			oper->txn_data->result = -1;
			fiber_call(oper->owner);
			rlist_create(&bsync_state.proxy_queue);
		}
	}
	if (2 * bsync_state.num_connected <= bsync_state.num_hosts ||
		host_id == bsync_state.leader_id)
	{
		bsync_state.leader_id = -1;
		bsync_state.state = bsync_state_initial;
		rlist_create(&bsync_state.election_ops);
		bsync_connected(host_id);
	}
}

static void
bsync_leader_proposal(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	say_info("receive leader proposal from %s", BSYNC_REMOTE.source);
	BSYNC_REMOTE.commit_gsn = BSYNC_REMOTE.submit_gsn =
		BSYNC_REMOTE.gsn = mp_decode_uint(ipos);
	if (bsync_state.state == bsync_state_started)
		bsync_state.state = bsync_state_initial;
	bsync_connected(host_id);
}

static void
bsync_leader_promise(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	BSYNC_REMOTE.commit_gsn = BSYNC_REMOTE.submit_gsn =
		BSYNC_REMOTE.gsn = mp_decode_uint(ipos);
	uint8_t max_host_id = bsync_max_host();
	if (host_id != max_host_id || bsync_state.state > bsync_state_initial) {
		say_warn("reject leader promise from %s", BSYNC_REMOTE.source);
		BSYNC_REMOTE.election_code = bsync_mtype_leader_reject;
		BSYNC_REMOTE.election_host = max_host_id;
	} else {
		say_info("accept leader promise from %s", BSYNC_REMOTE.source);
		BSYNC_REMOTE.election_code = bsync_mtype_leader_accept;
		bsync_state.state = bsync_state_leader_accept;
	}
	fiber_call(BSYNC_REMOTE.fiber_out);
}

static void
bsync_election_ops()
{BSYNC_TRACE
	if (rlist_empty(&bsync_state.election_ops))
		return;
	while (!rlist_empty(&bsync_state.election_ops) &&
		bsync_state.state == bsync_state_ready)
	{
		struct bsync_operation *oper =
			rlist_shift_entry(&bsync_state.election_ops,
					  struct bsync_operation, list);
		fiber_call(oper->owner);
	}
}

static void
bsync_leader_accept(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	if (bsync_state.state != bsync_state_leader_accept) return;
	if (2 * ++bsync_state.num_accepted <= bsync_state.num_hosts) return;
	say_info("new leader are %s", BSYNC_LOCAL.source);
	bsync_state.leader_id = bsync_state.local_id;
	bsync_state.wal_commit_gsn = BSYNC_LOCAL.gsn;
	bsync_state.recovery_hosts = 0;
	for (uint8_t i = 0; i < bsync_state.num_hosts; ++i) {
		if (i == bsync_state.local_id || bsync_index[i].connected < 2)
			continue;
		if (bsync_index[i].gsn < BSYNC_LOCAL.gsn)
			bsync_state.recovery_hosts |= (1 << i);
		bsync_index[i].election_code = bsync_mtype_leader_submit;
		fiber_call(bsync_index[i].fiber_out);
	}
	assert(bsync_state.recovery_hosts == 0);
	if (bsync_state.recovery_hosts) {
		bsync_state.state = bsync_state_recovery;
	} else {
		bsync_state.state = bsync_state_ready;
		bsync_election_ops();
	}
}

static void
bsync_leader_submit(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	bsync_state.leader_id = host_id;
	bsync_state.state = bsync_state_ready;
	say_info("new leader are %s", BSYNC_REMOTE.source);
	bsync_election_ops();
}

static void
bsync_leader_reject(uint8_t host_id, const char **ipos, const char *iend)
{BSYNC_TRACE
	(void)host_id; (void)ipos; (void)iend;
	uint8_t max_id = mp_decode_uint(ipos);
	bsync_index[max_id].commit_gsn = bsync_index[max_id].submit_gsn =
		bsync_index[max_id].gsn = mp_decode_uint(ipos);
	bsync_state.state = bsync_state_initial;
	bsync_connected(host_id);
}

/*
 * Network block
 */

typedef void (*bsync_handler_t)(uint8_t host_id,
				const char** ipos, const char* iend);
static bsync_handler_t bsync_handlers[] = {
	bsync_leader_proposal,
	bsync_leader_promise,
	bsync_leader_accept,
	bsync_leader_submit,
	bsync_leader_reject,
	bsync_body,
	bsync_submit,
	bsync_reject,
	bsync_proxy_request,
	bsync_proxy_accept,
	bsync_proxy_reject,
	bsync_proxy_join,
	bsync_commit,
	bsync_rollback,
	bsync_ping
};

static void
bsync_decode_extended_header(uint8_t /* host_id */, const char **pos)
{
	uint32_t len = mp_decode_uint(pos);
	const char *end = *pos + len;
	if (!len) return;
	uint32_t flags = mp_decode_uint(pos);
	assert(flags);
	if (flags & bsync_iproto_commit_gsn)
		bsync_commit_op(mp_decode_uint(pos));
	/* ignore all unknown data from extended header */
	*pos = end;
}

static uint32_t
bsync_read_package(struct ev_io *coio, struct ibuf *in, uint8_t host_id)
{
	if (host_id < BSYNC_MAX_HOSTS)
		BSYNC_REMOTE.flags |= bsync_host_active_read;
	/* Read fixed header */
	if (ibuf_size(in) < 1)
		coio_breadn(coio, in, 1);
	/* Read length */
	if (mp_typeof(*in->pos) != MP_UINT) {
		tnt_raise(ClientError, ER_INVALID_MSGPACK,
			  "packet length");
	}
	ssize_t to_read = mp_check_uint(in->pos, in->end);
	if (to_read > 0)
		coio_breadn(coio, in, to_read);
	uint32_t len = mp_decode_uint((const char **) &in->pos);
	/* Read header and body */
	to_read = len - ibuf_size(in);
	if (to_read > 0)
		coio_breadn(coio, in, to_read);
	if (host_id < BSYNC_MAX_HOSTS)
		BSYNC_REMOTE.flags &= !bsync_host_active_read;
	const char *pos = (const char *)in->pos;
	bsync_decode_extended_header(host_id, (const char **) &in->pos);
	return len - (in->pos - pos);
}

static void
bsync_incoming(struct ev_io *coio, struct iobuf *iobuf, uint8_t host_id)
{BSYNC_TRACE
	struct ibuf *in = &iobuf->in;
	auto coio_guard = make_scoped_guard([&]() {
		BSYNC_REMOTE.fiber_in = NULL;
	});
	while (!bsync_state.is_shutdown) {
		/* cleanup buffer */
		iobuf_reset(iobuf);
		fiber_gc();
		uint32_t len = bsync_read_package(coio, in, host_id);
		/* proceed message */
		const char* iend = (const char *)in->pos + len;
		const char **ipos = (const char **)&in->pos;
		if (BSYNC_REMOTE.connected == 2) {
			uint32_t type = mp_decode_uint(ipos);
			assert(type < bsync_mtype_count);
			say_debug("receive message from %s, type %s, length %d",
				BSYNC_REMOTE.source, bsync_mtype_name[type], len);
			assert(type < sizeof(bsync_handlers));
			(*bsync_handlers[type])(host_id, ipos, iend);
		} else {
			*ipos = iend;
			say_warn("receive message from disconnected host %s",
				 BSYNC_REMOTE.source);
		}
	}
}

static void
bsync_accept_handler(va_list ap)
{BSYNC_TRACE
	struct ev_io coio = va_arg(ap, struct ev_io);
	struct sockaddr *addr = va_arg(ap, struct sockaddr *);
	socklen_t addrlen = va_arg(ap, socklen_t);
	struct iobuf *iobuf = va_arg(ap, struct iobuf *);
	(void)addr; (void)addrlen;
	auto coio_guard = make_scoped_guard([&]() {
		iobuf_delete(iobuf);
	});
	struct ibuf *in = &iobuf->in;
	bsync_read_package(&coio, in, BSYNC_MAX_HOSTS);
	const char **ipos = (const char **)&in->pos;
	uint32_t type = mp_decode_uint(ipos);
	(void)type;
	assert(type == bsync_mtype_hello);
	uint8_t host_id = mp_decode_uint(ipos);
	assert(host_id < bsync_state.num_hosts);
	BSYNC_REMOTE.commit_gsn = BSYNC_REMOTE.submit_gsn =
		BSYNC_REMOTE.gsn = mp_decode_uint(ipos);
	BSYNC_REMOTE.fiber_in = fiber();
	say_info("receive incoming connection from %s, gsn=%ld, status=%d",
		 BSYNC_REMOTE.source, BSYNC_REMOTE.gsn, (int)BSYNC_REMOTE.connected);
	if (++BSYNC_REMOTE.connected == 1) {BSYNC_TRACE
		if (BSYNC_REMOTE.flags & bsync_host_reconnect_sleep) {
			fiber_call(BSYNC_REMOTE.fiber_out);
		} else if (BSYNC_REMOTE.flags & bsync_host_disconnected) {
			fiber_start(
				fiber_new(BSYNC_REMOTE.source, bsync_out_fiber),
				&host_id);
		}
	}
	if (BSYNC_REMOTE.connected == 2) {
		++bsync_state.num_connected;
		say_info("host %s connected", BSYNC_REMOTE.source);
		bsync_connected(host_id);
	}
	try {
		bsync_incoming(&coio, iobuf, host_id);
	} catch(...) {
		if (--BSYNC_REMOTE.connected == 1) {BSYNC_TRACE
			bsync_disconnected(host_id);
			BSYNC_REMOTE.connected = 0;
			BSYNC_REMOTE.fiber_out = NULL;
		}
		throw;
	}
}

static int
bsync_extended_header_size(uint8_t host_id)
{
	if (BSYNC_REMOTE.submit_gsn > BSYNC_REMOTE.commit_gsn)
		return mp_sizeof_uint(bsync_iproto_commit_gsn) +
			mp_sizeof_uint(BSYNC_REMOTE.submit_gsn);
	else
		return 0;
}

static char *
bsync_extended_header_encode(uint8_t host_id, char *pos)
{
	pos = mp_encode_uint(pos, bsync_extended_header_size(host_id));
	if (BSYNC_REMOTE.submit_gsn > BSYNC_REMOTE.commit_gsn) {
		pos = mp_encode_uint(pos, bsync_iproto_commit_gsn);
		pos = mp_encode_uint(pos, BSYNC_REMOTE.submit_gsn);
		BSYNC_REMOTE.commit_gsn = BSYNC_REMOTE.submit_gsn;
	}
	return pos;
}

static uint64_t
bsync_mp_real_size(uint64_t size)
{
	return mp_sizeof_uint(size) + size;
}

static int
encode_request(uint8_t host_id, struct bsync_host_info *elem, struct iovec *iov)
{
	int iovcnt = 1;
	ssize_t bsize = bsync_mp_real_size(bsync_extended_header_size(host_id)) +
			mp_sizeof_uint(elem->code);
	if (elem->code != bsync_mtype_proxy_request &&
	    elem->code != bsync_mtype_proxy_reject &&
	    elem->code != bsync_mtype_proxy_join &&
	    elem->code != bsync_mtype_commit)
	{
		bsize += mp_sizeof_uint(elem->op->gsn);
	}
	ssize_t fsize = bsize;
	if (elem->code == bsync_mtype_body ||
		elem->code == bsync_mtype_proxy_request)
	{
		iovcnt += xrow_header_encode(elem->op->txn_data->row, iov + 1);
		for (int i = 1; i < iovcnt; ++i) {
			bsize += iov[i].iov_len;
		}
	}
	iov[0].iov_len = mp_sizeof_uint(bsize) + fsize;
	iov[0].iov_base = region_alloc(&fiber()->gc, iov[0].iov_len);
	char* pos = (char*)iov[0].iov_base;
	pos = mp_encode_uint(pos, bsize);
	pos = bsync_extended_header_encode(host_id, pos);
	pos = mp_encode_uint(pos, elem->code);
	if (elem->code != bsync_mtype_proxy_request &&
	    elem->code != bsync_mtype_proxy_reject &&
	    elem->code != bsync_mtype_proxy_join &&
	    elem->code != bsync_mtype_commit)
	{
		pos = mp_encode_uint(pos, elem->op->gsn);
	}
	return iovcnt;
}

static void
bsync_writev(struct ev_io *coio, struct iovec *iov, int iovcnt, uint8_t host_id)
{
	BSYNC_REMOTE.flags |= bsync_host_active_write;
	coio_writev_timeout(coio, iov, iovcnt, -1, bsync_state.write_timeout);
	BSYNC_REMOTE.flags &= !bsync_host_active_write;
	if (errno == ETIMEDOUT) {
		tnt_raise(SocketError, coio->fd, "timeout");
	}
}

#define BUFV_IOVMAX 10*XROW_IOVMAX
static void
bsync_send_queue(struct ev_io *coio, uint8_t host_id)
{BSYNC_TRACE
	assert(bsync_state.state == bsync_state_ready);
	while(!rlist_empty(&BSYNC_REMOTE.send_queue)) {
		struct iovec iov[BUFV_IOVMAX];
		int iovcnt = 0;
		do {
			struct bsync_host_info *elem =
				rlist_shift_entry(&BSYNC_REMOTE.send_queue,
					struct bsync_host_info, list);
			if (elem->code == bsync_mtype_body ||
				elem->code == bsync_mtype_proxy_accept)
			{
				rlist_add_tail_entry(&BSYNC_REMOTE.op_queue,
						     elem, list);
			}
			assert(elem->code <= bsync_mtype_hello);
			say_debug("send to %s message with type %s, gsn %ld",
				BSYNC_REMOTE.source, bsync_mtype_name[elem->code],
				elem->op ? elem->op->gsn : -1);
			iovcnt += encode_request(host_id, elem, iov + iovcnt);
		} while (!rlist_empty(&BSYNC_REMOTE.send_queue) &&
			 (iovcnt + XROW_IOVMAX) < BUFV_IOVMAX);
		bsync_writev(coio, iov, iovcnt, host_id);
	}
}

static void
bsync_send(struct ev_io *coio, uint8_t host_id)
{BSYNC_TRACE
	if (rlist_empty(&BSYNC_REMOTE.send_queue)) {
		if (BSYNC_REMOTE.election_code == bsync_mtype_none) {
			BSYNC_REMOTE.election_code = bsync_mtype_ping;
			BSYNC_REMOTE.election_host = bsync_state.local_id;
		}
		struct iovec iov[1];
		struct bsync_host_data *host =
			&bsync_index[BSYNC_REMOTE.election_host];
		ssize_t size = mp_sizeof_uint(BSYNC_REMOTE.election_code) +
			bsync_mp_real_size(bsync_extended_header_size(host_id));
		switch (BSYNC_REMOTE.election_code) {
		case bsync_mtype_hello:
		case bsync_mtype_leader_reject:
			size += mp_sizeof_uint(BSYNC_REMOTE.election_host);
			/* no break */
		case bsync_mtype_ping:
		case bsync_mtype_leader_promise:
		case bsync_mtype_leader_proposal:
			size += mp_sizeof_uint(host->gsn);
			/* no break */
		default:
			break;
		}
		iov[0].iov_len = mp_sizeof_uint(size) + size;
		iov[0].iov_base = region_alloc(&fiber()->gc, iov[0].iov_len);
		char *pos = (char *)iov[0].iov_base;
		pos = mp_encode_uint(pos, size);
		pos = bsync_extended_header_encode(host_id, pos);
		pos = mp_encode_uint(pos, BSYNC_REMOTE.election_code);
		switch (BSYNC_REMOTE.election_code) {
		case bsync_mtype_hello:
		case bsync_mtype_leader_reject:
			pos = mp_encode_uint(pos, BSYNC_REMOTE.election_host);
			/* no break */
		case bsync_mtype_ping:
		case bsync_mtype_leader_promise:
		case bsync_mtype_leader_proposal:
			pos = mp_encode_uint(pos, host->gsn);
			/* no break */
		default:
			break;
		}
		assert(BSYNC_REMOTE.election_code <= bsync_mtype_hello);
		say_debug("send to %s message with type %s",
			BSYNC_REMOTE.source,
			bsync_mtype_name[BSYNC_REMOTE.election_code]);
		BSYNC_REMOTE.election_code = bsync_mtype_none;
		bsync_writev(coio, iov, 1, host_id);
	} else {
		bsync_send_queue(coio, host_id);
	}
}

static void
bsync_outgoing(struct ev_io *coio, uint8_t host_id)
{BSYNC_TRACE
	if (BSYNC_REMOTE.fiber_out == fiber()) {BSYNC_TRACE
		BSYNC_REMOTE.election_code = bsync_mtype_hello;
		BSYNC_REMOTE.election_host = bsync_state.local_id;
		bsync_send(coio, host_id);
		if (++BSYNC_REMOTE.connected == 2) {
			++bsync_state.num_connected;
			say_info("host %s connected", BSYNC_REMOTE.source);
			bsync_connected(host_id);
		}
	}
	while(!bsync_state.is_shutdown && BSYNC_REMOTE.fiber_out == fiber()) {
		if (bsync_extended_header_size(host_id) > 0 &&
			rlist_empty(&BSYNC_REMOTE.send_queue) &&
			!bsync_state.is_shutdown)
		{
			fiber_yield_timeout(bsync_state.submit_timeout);
		}
		bsync_send(coio, host_id);
		fiber_gc();
		fiber_yield_timeout(bsync_state.ping_timeout);
	}
}

static void
bsync_out_fiber(va_list ap)
{BSYNC_TRACE
	uint8_t host_id = *va_arg(ap, uint8_t*);
	say_info("send outgoing connection to %s", BSYNC_REMOTE.source);
	BSYNC_REMOTE.fiber_out = fiber();
	struct ev_io coio;
	coio_init(&coio);
	auto coio_guard = make_scoped_guard([&] {
		evio_close(loop(), &coio);
	});
	char host[URI_MAXHOST] = { '\0' };
	if (BSYNC_REMOTE.uri.host) {
		snprintf(host, sizeof(host), "%.*s",
			(int) BSYNC_REMOTE.uri.host_len, BSYNC_REMOTE.uri.host);
	}
	char service[URI_MAXSERVICE];
	snprintf(service, sizeof(service), "%.*s",
		(int) BSYNC_REMOTE.uri.service_len, BSYNC_REMOTE.uri.service);
	while (!bsync_state.is_shutdown) try {BSYNC_TRACE
		int r = coio_connect_timeout(&coio, &BSYNC_REMOTE.uri, 0, 0,
					     bsync_state.connect_timeout);
		if (r == -1) {
			say_warn("connection timeout to %s, wait %f",
				 BSYNC_REMOTE.source, bsync_state.reconnect_timeout);
			BSYNC_REMOTE.flags |= bsync_host_reconnect_sleep;
			fiber_yield_timeout(bsync_state.reconnect_timeout);
			BSYNC_REMOTE.flags &= !bsync_host_reconnect_sleep;
			continue;
		}
		break;
	} catch (...) {BSYNC_TRACE
		BSYNC_REMOTE.flags |= bsync_host_reconnect_sleep;
		fiber_yield_timeout(bsync_state.reconnect_timeout);
		BSYNC_REMOTE.flags &= !bsync_host_reconnect_sleep;
	}
	BSYNC_TRACE
	try {
		bsync_outgoing(&coio, host_id);
	} catch (...) {
	}
	BSYNC_TRACE
	if (BSYNC_REMOTE.fiber_out != fiber()) return;
	BSYNC_REMOTE.fiber_out = NULL;
	BSYNC_REMOTE.flags &= !bsync_host_active_write;
	if (--BSYNC_REMOTE.connected == 1) {
		bsync_disconnected(host_id);
	}
}

/*
 * System block:
 * 1. initialize local variables;
 * 2. read cfg;
 * 3. start/stop cord and event loop
 */

static void
bsync_cfg_push_host(uint8_t host_id, const char *ibegin,
		    const char* iend, const char* localhost)
{
	assert(iend > ibegin);
	memcpy(BSYNC_REMOTE.source, ibegin, iend - ibegin);
	BSYNC_REMOTE.source[iend - ibegin] = 0;
	BSYNC_REMOTE.fiber_in = NULL;
	BSYNC_REMOTE.fiber_out = NULL;
	BSYNC_REMOTE.election_code = bsync_mtype_none;
	BSYNC_REMOTE.election_host = 0;
	BSYNC_REMOTE.commit_gsn = 0;
	uri_parse(&BSYNC_REMOTE.uri, BSYNC_REMOTE.source);
	if (strcmp(BSYNC_REMOTE.source, localhost) == 0) {
		bsync_state.local_id = host_id;
	}
	BSYNC_REMOTE.flags = 0;
	rlist_create(&BSYNC_REMOTE.op_queue);
	rlist_create(&BSYNC_REMOTE.send_queue);
	BSYNC_REMOTE.active_ops = mh_bsync_new();
	if (BSYNC_REMOTE.active_ops == NULL)
		panic("out of memory");
}

static void
bsync_cfg_read()
{
	bsync_state.leader_id = -1;
	bsync_state.num_connected = 1;
	bsync_state.state = bsync_state_started;
	bsync_state.local_id = BSYNC_MAX_HOSTS;

	bsync_state.read_timeout = cfg_getd("bsync_read_timeout");
	bsync_state.write_timeout = cfg_getd("bsync_write_timeout");
	bsync_state.connect_timeout = cfg_getd("bsync_connect_timeout");
	bsync_state.reconnect_timeout = cfg_getd("bsync_reconnect_timeout");
	bsync_state.operation_timeout = cfg_getd("bsync_operation_timeout");
	bsync_state.ping_timeout = cfg_getd("bsync_ping_timeout");
	bsync_state.election_timeout = cfg_getd("bsync_election_timeout");
	bsync_state.slow_host_timeout = cfg_getd("bsync_slow_host_timeout");
	bsync_state.submit_timeout = cfg_getd("bsync_submit_timeout");

	const char* hosts = cfg_gets("bsync_replica");
	const char* localhost = cfg_gets("bsync_local");
	if (hosts == NULL) {
		tnt_raise(ClientError, ER_CFG,
			  "bsync.replica: expected host:port[;host_port]*");
	}
	const char* i_begin = hosts;
	uint8_t host_id = 0;
	for (const char* i_cur = i_begin; *i_cur; ++i_cur) {
		if (*i_cur != ';') continue;
		bsync_cfg_push_host(host_id++, i_begin, i_cur, localhost);
		i_begin = i_cur + 1;
	}
	bsync_cfg_push_host(host_id++, i_begin, hosts + strlen(hosts), localhost);
	if (bsync_state.local_id == BSYNC_MAX_HOSTS) {
		tnt_raise(ClientError, ER_CFG,
			  "bsync.local not found in bsync.replica");
	}
	bsync_state.num_hosts = host_id;
	BSYNC_LOCAL.connected = 2;
	BSYNC_LOCAL.gsn = 1;
}

static void
bsync_init_state(const struct vclock* vclock)
{
	if (vclock->lsn[BSYNC_SERVER_ID] == -1)
		BSYNC_LOCAL.gsn = 0;
	else
		BSYNC_LOCAL.gsn = vclock->lsn[BSYNC_SERVER_ID];
}

ev_loop *
bsync_init(wal_writer* initial, ev_loop *loop, struct vclock *vclock)
{BSYNC_TRACE
	assert (initial != NULL);
	if (cfg_geti("bsync_enable") <= 0) {
		say_info("bsync_enable=%d\n", cfg_geti("bsync_enable"));
		return loop;
	}
	wal_local_writer = initial;
	bsync_state.iproxy_end = NULL;
	bsync_state.iproxy_pos = NULL;
	bsync_state.rollback = false;
	bsync_state.wal_commit_gsn = 0;
	bsync_state.wal_rollback_gsn = 0;
	bsync_state.is_shutdown = false;
	bsync_state.is_proceed_system = false;

	/* I. Initialize the state. */
	pthread_mutexattr_t errorcheck;

	(void) tt_pthread_mutexattr_init(&errorcheck);
#ifndef NDEBUG
	(void) tt_pthread_mutexattr_settype(&errorcheck, PTHREAD_MUTEX_ERRORCHECK);
#endif
	tt_pthread_mutexattr_settype(&errorcheck, PTHREAD_MUTEX_RECURSIVE);
	/* Initialize queue lock mutex. */
	(void) tt_pthread_mutex_init(&bsync_state.mutex, &errorcheck);
	(void) tt_pthread_mutex_init(&bsync_state.active_ops_mutex, &errorcheck);
	(void) tt_pthread_mutexattr_destroy(&errorcheck);

	(void) tt_pthread_cond_init(&bsync_state.cond, NULL);

	STAILQ_INIT(&bsync_state.txn_proxy_input);
	STAILQ_INIT(&bsync_state.txn_proxy_queue);
	STAILQ_INIT(&bsync_state.bsync_proxy_input);
	STAILQ_INIT(&bsync_state.bsync_proxy_queue);
	rlist_create(&bsync_state.txn_fiber_cache);
	rlist_create(&bsync_state.bsync_fiber_cache);
	rlist_create(&bsync_state.proxy_queue);
	rlist_create(&bsync_state.commit_queue);
	rlist_create(&bsync_state.txn_queue);
	rlist_create(&bsync_state.execute_queue);
	rlist_create(&bsync_state.election_ops);
	rlist_create(&bsync_state.region_free);
	rlist_create(&bsync_state.region_gc);

	slab_cache_create(&bsync_state.slabc, &runtime);
	mempool_create(&bsync_state.region_pool, &bsync_state.slabc,
			sizeof(struct bsync_region));

	ev_async_init(&txn_process_event, bsync_txn_process);
	ev_async_init(&bsync_process_event, bsync_process);

	txn_loop = loop();

	ev_async_start(loop(), &txn_process_event);

	/* II. Start the thread. */
	bsync_cfg_read();
	bsync_init_state(vclock);
	tt_pthread_mutex_lock(&bsync_state.mutex);
	if (cord_start(&bsync_state.cord, "bsync", bsync_thread, NULL)) {
		wal_local_writer = NULL;
		return loop;
	}
	tt_pthread_cond_wait(&bsync_state.cond, &bsync_state.mutex);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	return bsync_loop;
}

static void*
bsync_thread(void*)
{BSYNC_TRACE
	tt_pthread_mutex_lock(&bsync_state.mutex);
	iobuf_init();
	coio_service_init(&bsync_coio, "bsync", bsync_accept_handler, NULL);
	evio_service_start(&bsync_coio.evio_service, BSYNC_LOCAL.source);
	for (uint8_t host_id = 0; host_id < bsync_state.num_hosts; ++host_id) {
		if (host_id == bsync_state.local_id) continue;
		BSYNC_REMOTE.connected = 0;
		fiber_start(
			fiber_new(BSYNC_REMOTE.source, bsync_out_fiber),
			&host_id);
	}
	if (bsync_state.num_hosts > 1) {
		fiber_call(fiber_new("bsync initial election", bsync_election));
	} else {
		bsync_state.leader_id = bsync_state.local_id;
		bsync_state.state = bsync_state_ready;
	}
	bsync_loop = loop();
	ev_async_start(bsync_loop, &bsync_process_event);
	tt_pthread_cond_signal(&bsync_state.cond);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	try {
		ev_run(loop(), 0);
	} catch (...) {
		say_crit("bsync_thread found unhandled exception");
		throw;
	}
	ev_async_stop(bsync_loop, &bsync_process_event);
	say_info("bsync stopped");
	return NULL;
}

void
bsync_writer_stop(struct recovery_state *r)
{BSYNC_TRACE
	if (wal_local_writer == NULL) {
		recovery_delete(r);
		return;
	}
	recovery_state = r;
	bsync_state.is_shutdown = true;
	tt_pthread_mutex_lock(&bsync_state.mutex);
	ev_async_send(bsync_loop, &bsync_process_event);
	tt_pthread_mutex_unlock(&bsync_state.mutex);
	if (cord_join(&bsync_state.cord)) {
		panic_syserror("BSYNC writer: thread join failed");
	}
	ev_async_stop(txn_loop, &txn_process_event);
	wal_local_writer = NULL;
	rlist_del(&bsync_state.txn_fiber_cache);
	rlist_del(&bsync_state.bsync_fiber_cache);
	rlist_del(&bsync_state.proxy_queue);
	rlist_del(&bsync_state.commit_queue);
	rlist_del(&bsync_state.txn_queue);
	rlist_del(&bsync_state.execute_queue);
	rlist_del(&bsync_state.election_ops);
	rlist_del(&bsync_state.region_free);
	rlist_del(&bsync_state.region_gc);
	for (uint8_t host_id = 0; host_id < bsync_state.num_hosts; ++host_id) {
		rlist_del(&BSYNC_REMOTE.send_queue);
		rlist_del(&BSYNC_REMOTE.op_queue);
		mh_bsync_delete(BSYNC_REMOTE.active_ops);
	}
	tt_pthread_mutex_destroy(&bsync_state.mutex);
	tt_pthread_mutex_destroy(&bsync_state.active_ops_mutex);
	slab_cache_destroy(&bsync_state.slabc);
}
