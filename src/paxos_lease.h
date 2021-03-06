/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#ifndef __PAXOS_LEASE_H__
#define __PAXOS_LEASE_H__

#define PAXOS_ACQUIRE_FORCE		0x00000001
#define PAXOS_ACQUIRE_QUIET_FAIL	0x00000002
#define PAXOS_ACQUIRE_SHARED		0x00000004
#define PAXOS_ACQUIRE_OWNER_NOWAIT	0x00000008
#define PAXOS_ACQUIRE_DEBUG_ALL		0x00000010

uint32_t leader_checksum(struct leader_record *lr);

uint32_t dblock_checksum(struct paxos_dblock *pd);

int paxos_lease_leader_read(struct task *task,
			    struct token *token,
			    struct leader_record *leader_ret,
			    const char *caller);

int paxos_lease_acquire(struct task *task,
			struct token *token,
			uint32_t flags,
		        struct leader_record *leader_ret,
			struct paxos_dblock *dblock_ret,
		        uint64_t acquire_lver,
		        int new_num_hosts);

int paxos_lease_release(struct task *task,
			struct token *token,
			struct sanlk_resource *resrename,
			struct leader_record *leader_last,
			struct leader_record *leader_ret);

int paxos_lease_init(struct task *task,
		     struct token *token,
		     int num_hosts, int max_hosts, int write_clear);

int paxos_lease_request_read(struct task *task, struct token *token,
                             struct request_record *rr);

int paxos_lease_request_write(struct task *task, struct token *token,
                              struct request_record *rr);

int paxos_read_resource(struct task *task,
			struct token *token,
			struct sanlk_resource *res);

int paxos_read_buf(struct task *task,
                   struct token *token,
                   char **buf_out);

int paxos_verify_leader(struct token *token,
                         struct sync_disk *disk,
                         struct leader_record *lr,
			 uint32_t checksum,
                         const char *caller);

int paxos_erase_dblock(struct task *task,
                       struct token *token,
                       uint64_t host_id);

int paxos_lease_leader_clobber(struct task *task,
                               struct token *token,
                               struct leader_record *leader,
                               const char *caller);
#endif
