/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 * Copyright (c) 2013 by Saso Kiselkov. All rights reserved.
 * Copyright 2013 Nexenta Systems, Inc.  All rights reserved.
 */

#ifndef _SYS_ARC_IMPL_H
#define	_SYS_ARC_IMPL_H

#include <sys/arc.h>
#include <sys/zio_crypt.h>
#include <sys/zthr.h>
#include <sys/aggsum.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note that buffers can be in one of 6 states:
 *	ARC_anon	- anonymous (discussed below)
 *	ARC_mru		- recently used, currently cached
 *	ARC_mru_ghost	- recently used, no longer in cache
 *	ARC_mfu		- frequently used, currently cached
 *	ARC_mfu_ghost	- frequently used, no longer in cache
 *	ARC_l2c_only	- exists in L2ARC but not other states
 * When there are no active references to the buffer, they are
 * are linked onto a list in one of these arc states.  These are
 * the only buffers that can be evicted or deleted.  Within each
 * state there are multiple lists, one for meta-data and one for
 * non-meta-data.  Meta-data (indirect blocks, blocks of dnodes,
 * etc.) is tracked separately so that it can be managed more
 * explicitly: favored over data, limited explicitly.
 *
 * Anonymous buffers are buffers that are not associated with
 * a DVA.  These are buffers that hold dirty block copies
 * before they are written to stable storage.  By definition,
 * they are "ref'd" and are considered part of arc_mru
 * that cannot be freed.  Generally, they will acquire a DVA
 * as they are written and migrate onto the arc_mru list.
 *
 * The ARC_l2c_only state is for buffers that are in the second
 * level ARC but no longer in any of the ARC_m* lists.  The second
 * level ARC itself may also contain buffers that are in any of
 * the ARC_m* states - meaning that a buffer can exist in two
 * places.  The reason for the ARC_l2c_only state is to keep the
 * buffer header in the hash table, so that reads that hit the
 * second level ARC benefit from these fast lookups.
 */

typedef struct arc_state {
	/*
	 * list of evictable buffers
	 */
	multilist_t *arcs_list[ARC_BUFC_NUMTYPES];
	/*
	 * total amount of evictable data in this state
	 */
	zfs_refcount_t arcs_esize[ARC_BUFC_NUMTYPES];
	/*
	 * total amount of data in this state; this includes: evictable,
	 * non-evictable, ARC_BUFC_DATA, and ARC_BUFC_METADATA.
	 */
	zfs_refcount_t arcs_size;
	/*
	 * supports the "dbufs" kstat
	 */
	arc_state_type_t arcs_state;
} arc_state_t;

typedef struct arc_callback arc_callback_t;

struct arc_callback {
	void			*acb_private;
	arc_read_done_func_t	*acb_done;
	arc_buf_t		*acb_buf;
	boolean_t		acb_encrypted;
	boolean_t		acb_compressed;
	boolean_t		acb_noauth;
	zbookmark_phys_t	acb_zb;
	zio_t			*acb_zio_dummy;
	zio_t			*acb_zio_head;
	arc_callback_t		*acb_next;
};

typedef struct arc_write_callback arc_write_callback_t;

struct arc_write_callback {
	void			*awcb_private;
	arc_write_done_func_t	*awcb_ready;
	arc_write_done_func_t	*awcb_children_ready;
	arc_write_done_func_t	*awcb_physdone;
	arc_write_done_func_t	*awcb_done;
	arc_buf_t		*awcb_buf;
};

/*
 * ARC buffers are separated into multiple structs as a memory saving measure:
 *   - Common fields struct, always defined, and embedded within it:
 *       - L2-only fields, always allocated but undefined when not in L2ARC
 *       - L1-only fields, only allocated when in L1ARC
 *
 *           Buffer in L1                     Buffer only in L2
 *    +------------------------+          +------------------------+
 *    | arc_buf_hdr_t          |          | arc_buf_hdr_t          |
 *    |                        |          |                        |
 *    |                        |          |                        |
 *    |                        |          |                        |
 *    +------------------------+          +------------------------+
 *    | l2arc_buf_hdr_t        |          | l2arc_buf_hdr_t        |
 *    | (undefined if L1-only) |          |                        |
 *    +------------------------+          +------------------------+
 *    | l1arc_buf_hdr_t        |
 *    |                        |
 *    |                        |
 *    |                        |
 *    |                        |
 *    +------------------------+
 *
 * Because it's possible for the L2ARC to become extremely large, we can wind
 * up eating a lot of memory in L2ARC buffer headers, so the size of a header
 * is minimized by only allocating the fields necessary for an L1-cached buffer
 * when a header is actually in the L1 cache. The sub-headers (l1arc_buf_hdr and
 * l2arc_buf_hdr) are embedded rather than allocated separately to save a couple
 * words in pointers. arc_hdr_realloc() is used to switch a header between
 * these two allocation states.
 */
typedef struct l1arc_buf_hdr {
	kmutex_t		b_freeze_lock;
	zio_cksum_t		*b_freeze_cksum;

	arc_buf_t		*b_buf;
	uint32_t		b_bufcnt;
	/* for waiting on writes to complete */
	kcondvar_t		b_cv;
	uint8_t			b_byteswap;


	/* protected by arc state mutex */
	arc_state_t		*b_state;
	multilist_node_t	b_arc_node;

	/* updated atomically */
	clock_t			b_arc_access;
	uint32_t		b_mru_hits;
	uint32_t		b_mru_ghost_hits;
	uint32_t		b_mfu_hits;
	uint32_t		b_mfu_ghost_hits;
	uint32_t		b_l2_hits;

	/* self protecting */
	zfs_refcount_t		b_refcnt;

	arc_callback_t		*b_acb;
	abd_t			*b_pabd;
} l1arc_buf_hdr_t;

enum {
	L2ARC_DEV_HDR_EVICT_FIRST = (1 << 0)	/* mirror of l2ad_first */
};

/*
 * Pointer used in persistent L2ARC (for pointing to log blocks & ARC buffers).
 */
typedef struct l2arc_log_blkptr {
	uint64_t	lbp_daddr;	/* device address of log */
	/*
	 * lbp_prop is the same format as the blk_prop in blkptr_t:
	 *	* logical size (in sectors)
	 *	* physical (compressed) size (in sectors)
	 *	* compression algorithm (we always LZ4-compress l2arc logs)
	 *	* checksum algorithm (used for lbp_cksum)
	 *	* object type & level (unused for now)
	 */
	uint64_t	lbp_prop;
	zio_cksum_t	lbp_cksum;	/* fletcher4 of log */
} l2arc_log_blkptr_t;

/*
 * The persistent L2ARC device header.
 * Byte order of magic determines whether 64-bit bswap of fields is necessary.
 */
typedef struct l2arc_dev_hdr_phys {
	uint64_t	dh_magic;	/* L2ARC_DEV_HDR_MAGIC */
	zio_cksum_t	dh_self_cksum;	/* fletcher4 of fields below */

	/*
	 * Global L2ARC device state and metadata.
	 */
	uint64_t	dh_spa_guid;
	uint64_t	dh_alloc_space;		/* vdev space alloc status */
	uint64_t	dh_flags;		/* l2arc_dev_hdr_flags_t */

	/*
	 * Start of log block chain. [0] -> newest log, [1] -> one older (used
	 * for initiating prefetch).
	 */
	l2arc_log_blkptr_t	dh_start_lbps[2];

	const uint64_t	dh_pad[44];		/* pad to 512 bytes */
} l2arc_dev_hdr_phys_t;

/*
 * A single ARC buffer header entry in a l2arc_log_blk_phys_t.
 */
typedef struct l2arc_log_ent_phys {
	dva_t			le_dva;	/* dva of buffer */
	uint64_t		le_birth;	/* birth txg of buffer */
	zio_cksum_t		le_freeze_cksum;
	/*
	 * le_prop is the same format as the blk_prop in blkptr_t:
	 *	* logical size (in sectors)
	 *	* physical (compressed) size (in sectors)
	 *	* compression algorithm
	 *	* checksum algorithm (used for b_freeze_cksum)
	 *	* object type & level (used to restore arc_buf_contents_t)
	 */
	uint64_t		le_prop;
	uint64_t		le_daddr;	/* buf location on l2dev */
	const uint64_t		le_pad[7];	/* resv'd for future use */
} l2arc_log_ent_phys_t;

/*
 * These design limits give us the following metadata overhead (before
 * compression):
 *	avg_blk_sz	overhead
 *	1k		12.51 %
 *	2k		 6.26 %
 *	4k		 3.13 %
 *	8k		 1.56 %
 *	16k		 0.78 %
 *	32k		 0.39 %
 *	64k		 0.20 %
 *	128k		 0.10 %
 * Compression should be able to sequeeze these down by about a factor of 2x.
 */
#define	L2ARC_LOG_BLK_SIZE			(128 * 1024)	/* 128k */
#define	L2ARC_LOG_BLK_HEADER_LEN		(128)
#define	L2ARC_LOG_BLK_ENTRIES			/* 1023 entries */	\
	((L2ARC_LOG_BLK_SIZE - L2ARC_LOG_BLK_HEADER_LEN) /		\
	sizeof (l2arc_log_ent_phys_t))
/*
 * Maximum amount of data in an l2arc log block (used to terminate rebuilding
 * before we hit the write head and restore potentially corrupted blocks).
 */
#define	L2ARC_LOG_BLK_MAX_PAYLOAD_SIZE	\
	(SPA_MAXBLOCKSIZE * L2ARC_LOG_BLK_ENTRIES)
/*
 * For the persistence and rebuild algorithms to operate reliably we need
 * the L2ARC device to at least be able to hold 3 full log blocks (otherwise
 * excessive log block looping might confuse the log chain end detection).
 * Under normal circumstances this is not a problem, since this is somewhere
 * around only 400 MB.
 */
#define	L2ARC_PERSIST_MIN_SIZE	(3 * L2ARC_LOG_BLK_MAX_PAYLOAD_SIZE)

/*
 * A log block of up to 1023 ARC buffer log entries, chained into the
 * persistent L2ARC metadata linked list. Byte order of magic determines
 * whether 64-bit bswap of fields is necessary.
 */
typedef struct l2arc_log_blk_phys {
	/* Header - see L2ARC_LOG_BLK_HEADER_LEN above */
	uint64_t		lb_magic;	/* L2ARC_LOG_BLK_MAGIC */
	l2arc_log_blkptr_t	lb_back2_lbp;	/* back 2 steps in chain */
	uint64_t		lb_pad[9];	/* resv'd for future use */
	/* Payload */
	l2arc_log_ent_phys_t	lb_entries[L2ARC_LOG_BLK_ENTRIES];
} l2arc_log_blk_phys_t;

/*
 * These structures hold in-flight l2arc_log_blk_phys_t's as they're being
 * written to the L2ARC device. They may be compressed, hence the uint8_t[].
 */
typedef struct l2arc_log_blk_buf {
	uint8_t		lbb_log_blk[sizeof (l2arc_log_blk_phys_t)];
	list_node_t	lbb_node;
} l2arc_log_blk_buf_t;

/* Macros for the manipulation fields in the blk_prop format of blkptr_t */
#define	BLKPROP_GET_LSIZE(field)	\
	BF64_GET_SB((field), 0, SPA_LSIZEBITS, SPA_MINBLOCKSHIFT, 1)
#define	BLKPROP_SET_LSIZE(field, x)	\
	BF64_SET_SB((field), 0, SPA_LSIZEBITS, SPA_MINBLOCKSHIFT, 1, x)
#define	BLKPROP_GET_PSIZE(field)	\
	BF64_GET_SB((field), 16, SPA_PSIZEBITS, SPA_MINBLOCKSHIFT, 1)
#define	BLKPROP_SET_PSIZE(field, x)	\
	BF64_SET_SB((field), 16, SPA_PSIZEBITS, SPA_MINBLOCKSHIFT, 1, x)
#define	BLKPROP_GET_COMPRESS(field)	BF64_GET((field), 32, 7)
#define	BLKPROP_SET_COMPRESS(field, x)	BF64_SET((field), 32, 7, x)
#define	BLKPROP_GET_CHECKSUM(field)	BF64_GET((field), 40, 8)
#define	BLKPROP_SET_CHECKSUM(field, x)	BF64_SET((field), 40, 8, x)
#define	BLKPROP_GET_TYPE(field)		BF64_GET((field), 48, 8)
#define	BLKPROP_SET_TYPE(field, x)	BF64_SET((field), 48, 8, x)

/* Macros for manipulating a l2arc_log_blkptr_t->lbp_prop field */
#define	LBP_GET_LSIZE(lbp)		BLKPROP_GET_LSIZE((lbp)->lbp_prop)
#define	LBP_SET_LSIZE(lbp, x)		BLKPROP_SET_LSIZE((lbp)->lbp_prop, x)
#define	LBP_GET_PSIZE(lbp)		BLKPROP_GET_PSIZE((lbp)->lbp_prop)
#define	LBP_SET_PSIZE(lbp, x)		BLKPROP_SET_PSIZE((lbp)->lbp_prop, x)
#define	LBP_GET_COMPRESS(lbp)		BLKPROP_GET_COMPRESS((lbp)->lbp_prop)
#define	LBP_SET_COMPRESS(lbp, x)	BLKPROP_SET_COMPRESS((lbp)->lbp_prop, x)
#define	LBP_GET_CHECKSUM(lbp)		BLKPROP_GET_CHECKSUM((lbp)->lbp_prop)
#define	LBP_SET_CHECKSUM(lbp, x)	BLKPROP_SET_CHECKSUM((lbp)->lbp_prop, x)
#define	LBP_GET_TYPE(lbp)		BLKPROP_GET_TYPE((lbp)->lbp_prop)
#define	LBP_SET_TYPE(lbp, x)		BLKPROP_SET_TYPE((lbp)->lbp_prop, x)

/* Macros for manipulating a l2arc_log_ent_phys_t->le_prop field */
#define	LE_GET_LSIZE(le)		BLKPROP_GET_LSIZE((le)->le_prop)
#define	LE_SET_LSIZE(le, x)		BLKPROP_SET_LSIZE((le)->le_prop, x)
#define	LE_GET_PSIZE(le)		BLKPROP_GET_PSIZE((le)->le_prop)
#define	LE_SET_PSIZE(le, x)		BLKPROP_SET_PSIZE((le)->le_prop, x)
#define	LE_GET_COMPRESS(le)		BLKPROP_GET_COMPRESS((le)->le_prop)
#define	LE_SET_COMPRESS(le, x)		BLKPROP_SET_COMPRESS((le)->le_prop, x)
#define	LE_GET_CHECKSUM(le)		BLKPROP_GET_CHECKSUM((le)->le_prop)
#define	LE_SET_CHECKSUM(le, x)		BLKPROP_SET_CHECKSUM((le)->le_prop, x)
#define	LE_GET_TYPE(le)			BLKPROP_GET_TYPE((le)->le_prop)
#define	LE_SET_TYPE(le, x)		BLKPROP_SET_TYPE((le)->le_prop, x)

#define	PTR_SWAP(x, y)		\
	do {			\
		void *tmp = (x);\
		x = y;		\
		y = tmp;	\
		_NOTE(CONSTCOND)\
	} while (0)

#define	L2ARC_DEV_HDR_MAGIC	0x5a46534341434845LLU	/* ASCII: "ZFSCACHE" */
#define	L2ARC_LOG_BLK_MAGIC	0x4c4f47424c4b4844LLU	/* ASCII: "LOGBLKHD" */

/*
 * L2ARC Internals
 */
typedef struct l2arc_dev {
	vdev_t			*l2ad_vdev;	/* vdev */
	spa_t			*l2ad_spa;	/* spa */
	uint64_t		l2ad_hand;	/* next write location */
	uint64_t		l2ad_start;	/* first addr on device */
	uint64_t		l2ad_end;	/* last addr on device */
	boolean_t		l2ad_first;	/* first sweep through */
	boolean_t		l2ad_writing;	/* currently writing */
	kmutex_t		l2ad_mtx;	/* lock for buffer list */
	list_t			l2ad_buflist;	/* buffer list */
	list_node_t		l2ad_node;	/* device list node */
	zfs_refcount_t		l2ad_alloc;	/* allocated bytes */
	/*
	 * Persistence-related stuff
	 */
	l2arc_dev_hdr_phys_t	*l2ad_dev_hdr;	/* persistent device header */
	uint64_t		l2ad_dev_hdr_asize; /* aligned hdr size */
	l2arc_log_blk_phys_t	l2ad_log_blk;	/* currently open log block */
	int			l2ad_log_ent_idx; /* index into cur log blk */
	/* number of bytes in current log block's payload */
	uint64_t		l2ad_log_blk_payload_asize;
	/* flag indicating whether a rebuild is scheduled or is going on */
	boolean_t		l2ad_rebuild;
	boolean_t		l2ad_rebuild_cancel;
} l2arc_dev_t;

/*
 * Encrypted blocks will need to be stored encrypted on the L2ARC
 * disk as they appear in the main pool. In order for this to work we
 * need to pass around the encryption parameters so they can be used
 * to write data to the L2ARC. This struct is only defined in the
 * arc_buf_hdr_t if the L1 header is defined and has the ARC_FLAG_ENCRYPTED
 * flag set.
 */
typedef struct arc_buf_hdr_crypt {
	abd_t			*b_rabd;	/* raw encrypted data */
	dmu_object_type_t	b_ot;		/* object type */
	uint32_t		b_ebufcnt;	/* count of encrypted buffers */

	/* dsobj for looking up encryption key for l2arc encryption */
	uint64_t		b_dsobj;

	/* encryption parameters */
	uint8_t			b_salt[ZIO_DATA_SALT_LEN];
	uint8_t			b_iv[ZIO_DATA_IV_LEN];

	/*
	 * Technically this could be removed since we will always be able to
	 * get the mac from the bp when we need it. However, it is inconvenient
	 * for callers of arc code to have to pass a bp in all the time. This
	 * also allows us to assert that L2ARC data is properly encrypted to
	 * match the data in the main storage pool.
	 */
	uint8_t			b_mac[ZIO_DATA_MAC_LEN];
} arc_buf_hdr_crypt_t;

typedef struct l2arc_buf_hdr {
	/* protected by arc_buf_hdr mutex */
	l2arc_dev_t		*b_dev;		/* L2ARC device */
	uint64_t		b_daddr;	/* disk address, offset byte */
	uint32_t		b_hits;

	list_node_t		b_l2node;
} l2arc_buf_hdr_t;

typedef struct l2arc_write_callback {
	l2arc_dev_t	*l2wcb_dev;		/* device info */
	arc_buf_hdr_t	*l2wcb_head;		/* head of write buflist */
	list_t		l2wcb_log_blk_buflist;	/* in-flight log blocks */
} l2arc_write_callback_t;

struct arc_buf_hdr {
	/* protected by hash lock */
	dva_t			b_dva;
	uint64_t		b_birth;

	arc_buf_contents_t	b_type;
	arc_buf_hdr_t		*b_hash_next;
	arc_flags_t		b_flags;

	/*
	 * This field stores the size of the data buffer after
	 * compression, and is set in the arc's zio completion handlers.
	 * It is in units of SPA_MINBLOCKSIZE (e.g. 1 == 512 bytes).
	 *
	 * While the block pointers can store up to 32MB in their psize
	 * field, we can only store up to 32MB minus 512B. This is due
	 * to the bp using a bias of 1, whereas we use a bias of 0 (i.e.
	 * a field of zeros represents 512B in the bp). We can't use a
	 * bias of 1 since we need to reserve a psize of zero, here, to
	 * represent holes and embedded blocks.
	 *
	 * This isn't a problem in practice, since the maximum size of a
	 * buffer is limited to 16MB, so we never need to store 32MB in
	 * this field. Even in the upstream illumos code base, the
	 * maximum size of a buffer is limited to 16MB.
	 */
	uint16_t		b_psize;

	/*
	 * This field stores the size of the data buffer before
	 * compression, and cannot change once set. It is in units
	 * of SPA_MINBLOCKSIZE (e.g. 2 == 1024 bytes)
	 */
	uint16_t		b_lsize;	/* immutable */
	uint64_t		b_spa;		/* immutable */

	/* L2ARC fields. Undefined when not in L2ARC. */
	l2arc_buf_hdr_t		b_l2hdr;
	/* L1ARC fields. Undefined when in l2arc_only state */
	l1arc_buf_hdr_t		b_l1hdr;
	/*
	 * Encryption parameters. Defined only when ARC_FLAG_ENCRYPTED
	 * is set and the L1 header exists.
	 */
	arc_buf_hdr_crypt_t b_crypt_hdr;
};

typedef struct arc_stats {
	kstat_named_t arcstat_hits;
	kstat_named_t arcstat_misses;
	kstat_named_t arcstat_demand_data_hits;
	kstat_named_t arcstat_demand_data_misses;
	kstat_named_t arcstat_demand_metadata_hits;
	kstat_named_t arcstat_demand_metadata_misses;
	kstat_named_t arcstat_prefetch_data_hits;
	kstat_named_t arcstat_prefetch_data_misses;
	kstat_named_t arcstat_prefetch_metadata_hits;
	kstat_named_t arcstat_prefetch_metadata_misses;
	kstat_named_t arcstat_mru_hits;
	kstat_named_t arcstat_mru_ghost_hits;
	kstat_named_t arcstat_mfu_hits;
	kstat_named_t arcstat_mfu_ghost_hits;
	kstat_named_t arcstat_deleted;
	/*
	 * Number of buffers that could not be evicted because the hash lock
	 * was held by another thread.  The lock may not necessarily be held
	 * by something using the same buffer, since hash locks are shared
	 * by multiple buffers.
	 */
	kstat_named_t arcstat_mutex_miss;
	/*
	 * Number of buffers skipped when updating the access state due to the
	 * header having already been released after acquiring the hash lock.
	 */
	kstat_named_t arcstat_access_skip;
	/*
	 * Number of buffers skipped because they have I/O in progress, are
	 * indirect prefetch buffers that have not lived long enough, or are
	 * not from the spa we're trying to evict from.
	 */
	kstat_named_t arcstat_evict_skip;
	/*
	 * Number of times arc_evict_state() was unable to evict enough
	 * buffers to reach its target amount.
	 */
	kstat_named_t arcstat_evict_not_enough;
	kstat_named_t arcstat_evict_l2_cached;
	kstat_named_t arcstat_evict_l2_eligible;
	kstat_named_t arcstat_evict_l2_ineligible;
	kstat_named_t arcstat_evict_l2_skip;
	kstat_named_t arcstat_hash_elements;
	kstat_named_t arcstat_hash_elements_max;
	kstat_named_t arcstat_hash_collisions;
	kstat_named_t arcstat_hash_chains;
	kstat_named_t arcstat_hash_chain_max;
	kstat_named_t arcstat_p;
	kstat_named_t arcstat_c;
	kstat_named_t arcstat_c_min;
	kstat_named_t arcstat_c_max;
	/* Not updated directly; only synced in arc_kstat_update. */
	kstat_named_t arcstat_size;
	/*
	 * Number of compressed bytes stored in the arc_buf_hdr_t's b_pabd.
	 * Note that the compressed bytes may match the uncompressed bytes
	 * if the block is either not compressed or compressed arc is disabled.
	 */
	kstat_named_t arcstat_compressed_size;
	/*
	 * Uncompressed size of the data stored in b_pabd. If compressed
	 * arc is disabled then this value will be identical to the stat
	 * above.
	 */
	kstat_named_t arcstat_uncompressed_size;
	/*
	 * Number of bytes stored in all the arc_buf_t's. This is classified
	 * as "overhead" since this data is typically short-lived and will
	 * be evicted from the arc when it becomes unreferenced unless the
	 * zfs_keep_uncompressed_metadata or zfs_keep_uncompressed_level
	 * values have been set (see comment in dbuf.c for more information).
	 */
	kstat_named_t arcstat_overhead_size;
	/*
	 * Number of bytes consumed by internal ARC structures necessary
	 * for tracking purposes; these structures are not actually
	 * backed by ARC buffers. This includes arc_buf_hdr_t structures
	 * (allocated via arc_buf_hdr_t_full and arc_buf_hdr_t_l2only
	 * caches), and arc_buf_t structures (allocated via arc_buf_t
	 * cache).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_hdr_size;
	/*
	 * Number of bytes consumed by ARC buffers of type equal to
	 * ARC_BUFC_DATA. This is generally consumed by buffers backing
	 * on disk user data (e.g. plain file contents).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_data_size;
	/*
	 * Number of bytes consumed by ARC buffers of type equal to
	 * ARC_BUFC_METADATA. This is generally consumed by buffers
	 * backing on disk data that is used for internal ZFS
	 * structures (e.g. ZAP, dnode, indirect blocks, etc).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_metadata_size;
	/*
	 * Number of bytes consumed by dmu_buf_impl_t objects.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_dbuf_size;
	/*
	 * Number of bytes consumed by dnode_t objects.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_dnode_size;
	/*
	 * Number of bytes consumed by bonus buffers.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_bonus_size;
	/*
	 * Total number of bytes consumed by ARC buffers residing in the
	 * arc_anon state. This includes *all* buffers in the arc_anon
	 * state; e.g. data, metadata, evictable, and unevictable buffers
	 * are all included in this value.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_anon_size;
	/*
	 * Number of bytes consumed by ARC buffers that meet the
	 * following criteria: backing buffers of type ARC_BUFC_DATA,
	 * residing in the arc_anon state, and are eligible for eviction
	 * (e.g. have no outstanding holds on the buffer).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_anon_evictable_data;
	/*
	 * Number of bytes consumed by ARC buffers that meet the
	 * following criteria: backing buffers of type ARC_BUFC_METADATA,
	 * residing in the arc_anon state, and are eligible for eviction
	 * (e.g. have no outstanding holds on the buffer).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_anon_evictable_metadata;
	/*
	 * Total number of bytes consumed by ARC buffers residing in the
	 * arc_mru state. This includes *all* buffers in the arc_mru
	 * state; e.g. data, metadata, evictable, and unevictable buffers
	 * are all included in this value.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mru_size;
	/*
	 * Number of bytes consumed by ARC buffers that meet the
	 * following criteria: backing buffers of type ARC_BUFC_DATA,
	 * residing in the arc_mru state, and are eligible for eviction
	 * (e.g. have no outstanding holds on the buffer).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mru_evictable_data;
	/*
	 * Number of bytes consumed by ARC buffers that meet the
	 * following criteria: backing buffers of type ARC_BUFC_METADATA,
	 * residing in the arc_mru state, and are eligible for eviction
	 * (e.g. have no outstanding holds on the buffer).
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mru_evictable_metadata;
	/*
	 * Total number of bytes that *would have been* consumed by ARC
	 * buffers in the arc_mru_ghost state. The key thing to note
	 * here, is the fact that this size doesn't actually indicate
	 * RAM consumption. The ghost lists only consist of headers and
	 * don't actually have ARC buffers linked off of these headers.
	 * Thus, *if* the headers had associated ARC buffers, these
	 * buffers *would have* consumed this number of bytes.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mru_ghost_size;
	/*
	 * Number of bytes that *would have been* consumed by ARC
	 * buffers that are eligible for eviction, of type
	 * ARC_BUFC_DATA, and linked off the arc_mru_ghost state.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mru_ghost_evictable_data;
	/*
	 * Number of bytes that *would have been* consumed by ARC
	 * buffers that are eligible for eviction, of type
	 * ARC_BUFC_METADATA, and linked off the arc_mru_ghost state.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mru_ghost_evictable_metadata;
	/*
	 * Total number of bytes consumed by ARC buffers residing in the
	 * arc_mfu state. This includes *all* buffers in the arc_mfu
	 * state; e.g. data, metadata, evictable, and unevictable buffers
	 * are all included in this value.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mfu_size;
	/*
	 * Number of bytes consumed by ARC buffers that are eligible for
	 * eviction, of type ARC_BUFC_DATA, and reside in the arc_mfu
	 * state.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mfu_evictable_data;
	/*
	 * Number of bytes consumed by ARC buffers that are eligible for
	 * eviction, of type ARC_BUFC_METADATA, and reside in the
	 * arc_mfu state.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mfu_evictable_metadata;
	/*
	 * Total number of bytes that *would have been* consumed by ARC
	 * buffers in the arc_mfu_ghost state. See the comment above
	 * arcstat_mru_ghost_size for more details.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mfu_ghost_size;
	/*
	 * Number of bytes that *would have been* consumed by ARC
	 * buffers that are eligible for eviction, of type
	 * ARC_BUFC_DATA, and linked off the arc_mfu_ghost state.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mfu_ghost_evictable_data;
	/*
	 * Number of bytes that *would have been* consumed by ARC
	 * buffers that are eligible for eviction, of type
	 * ARC_BUFC_METADATA, and linked off the arc_mru_ghost state.
	 * Not updated directly; only synced in arc_kstat_update.
	 */
	kstat_named_t arcstat_mfu_ghost_evictable_metadata;
	kstat_named_t arcstat_l2_hits;
	kstat_named_t arcstat_l2_misses;
	kstat_named_t arcstat_l2_feeds;
	kstat_named_t arcstat_l2_rw_clash;
	kstat_named_t arcstat_l2_read_bytes;
	kstat_named_t arcstat_l2_write_bytes;
	kstat_named_t arcstat_l2_writes_sent;
	kstat_named_t arcstat_l2_writes_done;
	kstat_named_t arcstat_l2_writes_error;
	kstat_named_t arcstat_l2_writes_lock_retry;
	kstat_named_t arcstat_l2_evict_lock_retry;
	kstat_named_t arcstat_l2_evict_reading;
	kstat_named_t arcstat_l2_evict_l1cached;
	kstat_named_t arcstat_l2_free_on_write;
	kstat_named_t arcstat_l2_abort_lowmem;
	kstat_named_t arcstat_l2_cksum_bad;
	kstat_named_t arcstat_l2_io_error;
	kstat_named_t arcstat_l2_lsize;
	kstat_named_t arcstat_l2_psize;
	/* Not updated directly; only synced in arc_kstat_update. */
	kstat_named_t arcstat_l2_hdr_size;
        kstat_named_t arcstat_l2_log_blk_writes;
        kstat_named_t arcstat_l2_log_blk_avg_size;
        kstat_named_t arcstat_l2_data_to_meta_ratio;
        kstat_named_t arcstat_l2_rebuild_successes;
        kstat_named_t arcstat_l2_rebuild_abort_unsupported;
        kstat_named_t arcstat_l2_rebuild_abort_io_errors;
        kstat_named_t arcstat_l2_rebuild_abort_cksum_errors;
        kstat_named_t arcstat_l2_rebuild_abort_loop_errors;
        kstat_named_t arcstat_l2_rebuild_abort_lowmem;
        kstat_named_t arcstat_l2_rebuild_size;
        kstat_named_t arcstat_l2_rebuild_bufs;
        kstat_named_t arcstat_l2_rebuild_bufs_precached;
        kstat_named_t arcstat_l2_rebuild_psize;
        kstat_named_t arcstat_l2_rebuild_log_blks;
	kstat_named_t arcstat_memory_throttle_count;
	kstat_named_t arcstat_memory_direct_count;
	kstat_named_t arcstat_memory_indirect_count;
	kstat_named_t arcstat_memory_all_bytes;
	kstat_named_t arcstat_memory_free_bytes;
	kstat_named_t arcstat_memory_available_bytes;
	kstat_named_t arcstat_no_grow;
	kstat_named_t arcstat_tempreserve;
	kstat_named_t arcstat_loaned_bytes;
	kstat_named_t arcstat_prune;
	/* Not updated directly; only synced in arc_kstat_update. */
	kstat_named_t arcstat_meta_used;
	kstat_named_t arcstat_meta_limit;
	kstat_named_t arcstat_dnode_limit;
	kstat_named_t arcstat_meta_max;
	kstat_named_t arcstat_meta_min;
	kstat_named_t arcstat_async_upgrade_sync;
	kstat_named_t arcstat_demand_hit_predictive_prefetch;
	kstat_named_t arcstat_demand_hit_prescient_prefetch;
	kstat_named_t arcstat_need_free;
	kstat_named_t arcstat_sys_free;
	kstat_named_t arcstat_raw_size;
} arc_stats_t;

typedef enum free_memory_reason_t {
	FMR_UNKNOWN,
	FMR_NEEDFREE,
	FMR_LOTSFREE,
	FMR_SWAPFS_MINFREE,
	FMR_PAGES_PP_MAXIMUM,
	FMR_HEAP_ARENA,
	FMR_ZIO_ARENA,
} free_memory_reason_t;

#define	ARCSTAT(stat)	(arc_stats.stat.value.ui64)

#define	ARCSTAT_INCR(stat, val) \
	atomic_add_64(&arc_stats.stat.value.ui64, (val))

#define	ARCSTAT_BUMP(stat)	ARCSTAT_INCR(stat, 1)
#define	ARCSTAT_BUMPDOWN(stat)	ARCSTAT_INCR(stat, -1)

#define	arc_no_grow	ARCSTAT(arcstat_no_grow) /* do not grow cache size */
#define	arc_p		ARCSTAT(arcstat_p)	/* target size of MRU */
#define	arc_c		ARCSTAT(arcstat_c)	/* target size of cache */
#define	arc_c_min	ARCSTAT(arcstat_c_min)	/* min target cache size */
#define	arc_c_max	ARCSTAT(arcstat_c_max)	/* max target cache size */
#define	arc_sys_free	ARCSTAT(arcstat_sys_free) /* target system free bytes */
#define	arc_need_free	ARCSTAT(arcstat_need_free) /* bytes to be freed */

extern int arc_zio_arena_free_shift;
extern taskq_t *arc_prune_taskq;
extern arc_stats_t arc_stats;
extern hrtime_t arc_growtime;
extern boolean_t arc_warm;
extern int arc_grow_retry;
extern int arc_shrink_shift;
extern zthr_t		*arc_adjust_zthr;
extern kmutex_t		arc_adjust_lock;
extern kcondvar_t	arc_adjust_waiters_cv;
extern boolean_t	arc_adjust_needed;
extern kmutex_t arc_prune_mtx;
extern list_t arc_prune_list;
extern aggsum_t arc_size;
extern arc_state_t	*arc_mfu;
extern arc_state_t	*arc_mru;
extern uint_t zfs_arc_pc_percent;
extern int arc_lotsfree_percent;

extern void arc_reduce_target_size(int64_t to_free);
extern boolean_t arc_reclaim_needed(void);
extern void arc_kmem_reap_soon(void);

extern void arc_lowmem_init(void);
extern void arc_lowmem_fini(void);
extern void arc_prune_async(int64_t);
extern int arc_memory_throttle(spa_t *spa, uint64_t reserve, uint64_t txg);
extern uint64_t arc_free_memory(void);
extern int64_t arc_available_memory(void);
extern void arc_tuning_update(void);

extern int param_set_arc_long(const char *buf, zfs_kernel_param_t *kp);
extern int param_set_arc_int(const char *buf, zfs_kernel_param_t *kp);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_ARC_IMPL_H */
