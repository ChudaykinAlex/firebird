/*
 *	PROGRAM:	JRD Lock Manager
 *	MODULE:		lock_proto.h
 *	DESCRIPTION:	Prototype header file for lock.cpp
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 */

#ifndef LOCK_LOCK_PROTO_H
#define LOCK_LOCK_PROTO_H

#if defined(USE_SHMEM_EXT) && (! defined(SRQ_ABS_PTR))
#define SRQ_ABS_PTR(x) ABS_PTR(x)
#define SRQ_REL_PTR(x) REL_PTR(x)
#endif

#include <stdio.h>
#include <sys/types.h>

#include "../common/classes/semaphore.h"
#include "../common/classes/rwlock.h"
#include "../common/classes/GenericMap.h"
#include "../common/classes/init.h"
#include "../common/classes/RefCounted.h"
#include "../common/classes/array.h"
#include "../common/StatusArg.h"
#include "../common/ThreadStart.h"
#include "../common/isc_s_proto.h"
#include "../common/classes/auto.h"
#ifdef USE_SHMEM_EXT
#include "../common/classes/objects_array.h"
#endif
#include "../common/file_params.h"
#include "../jrd/que.h"

typedef FB_UINT64 LOCK_OWNER_T; // Data type for the Owner ID
typedef SINT64 LOCK_DATA_T;

// Maximum lock series for gathering statistics and querying data

inline constexpr int LCK_MAX_SERIES	= 7;

// Lock query data aggregates

inline constexpr int LCK_MIN	= 1;
inline constexpr int LCK_MAX	= 2;
inline constexpr int LCK_CNT	= 3;
inline constexpr int LCK_SUM	= 4;
inline constexpr int LCK_AVG	= 5;
inline constexpr int LCK_ANY	= 6;

// Lock states
// in LCK_convert the type of level is USHORT instead of UCHAR
inline constexpr UCHAR LCK_none	= 0;
inline constexpr UCHAR LCK_null	= 1;
inline constexpr UCHAR LCK_SR	= 2;	// Shared Read
inline constexpr UCHAR LCK_PR	= 3;	// Protected Read
inline constexpr UCHAR LCK_SW	= 4;	// Shared Write
inline constexpr UCHAR LCK_PW	= 5;	// Protected Write
inline constexpr UCHAR LCK_EX	= 6;	// Exclusive
inline constexpr UCHAR LCK_max	= 7;

enum locklevel_t {LCK_read = LCK_PR, LCK_write = LCK_EX};

inline constexpr SSHORT LCK_WAIT	= 1;
inline constexpr SSHORT LCK_NO_WAIT	= 0;

// Lock block types

inline constexpr UCHAR type_null	= 0;
inline constexpr UCHAR type_lhb		= 1;
inline constexpr UCHAR type_lrq		= 2;
inline constexpr UCHAR type_lbl		= 3;
inline constexpr UCHAR type_his		= 4;
inline constexpr UCHAR type_shb		= 5;
inline constexpr UCHAR type_own		= 6;
inline constexpr UCHAR type_lpr		= 7;

// Version number of the lock table.
// Must be increased every time the shmem layout is changed.
inline constexpr USHORT BASE_LHB_VERSION = 19;
inline constexpr USHORT PLATFORM_LHB_VERSION = 128;	// 64-bit target

#if SIZEOF_VOID_P == 8
inline constexpr USHORT LHB_VERSION = PLATFORM_LHB_VERSION | BASE_LHB_VERSION;
#else
inline constexpr USHORT LHB_VERSION = BASE_LHB_VERSION;
#endif


// Lock header block -- one per lock file, lives up front

struct lhb : public Firebird::MemoryHeader
{
	USHORT lhb_type;				// memory tag - always type_lhb
	SRQ_PTR lhb_secondary;			// Secondary lock header block
	SRQ_PTR lhb_active_owner;		// Active owner, if any
	srq lhb_owners;					// Que of active owners
	srq lhb_processes;				// Que of active processes
	srq lhb_free_processes;			// Free process blocks
	srq lhb_free_owners;			// Free owner blocks
	srq lhb_free_locks;				// Free lock blocks
	srq lhb_free_requests;			// Free lock requests
	ULONG lhb_length;				// Size of lock table
	ULONG lhb_used;					// Bytes of lock table in use
	USHORT lhb_hash_slots;			// Number of hash slots allocated

	SRQ_PTR lhb_history;
	ULONG lhb_scan_interval;		// Deadlock scan interval (secs)
	ULONG lhb_acquire_spins;
	FB_UINT64 lhb_acquires;
	FB_UINT64 lhb_acquire_blocks;
	FB_UINT64 lhb_acquire_retries;
	FB_UINT64 lhb_retry_success;
	FB_UINT64 lhb_enqs;
	FB_UINT64 lhb_converts;
	FB_UINT64 lhb_downgrades;
	FB_UINT64 lhb_deqs;
	FB_UINT64 lhb_read_data;
	FB_UINT64 lhb_write_data;
	FB_UINT64 lhb_query_data;
	FB_UINT64 lhb_operations[LCK_MAX_SERIES];
	FB_UINT64 lhb_waits;
	FB_UINT64 lhb_denies;
	FB_UINT64 lhb_timeouts;
	FB_UINT64 lhb_blocks;
	FB_UINT64 lhb_wakeups;
	FB_UINT64 lhb_scans;
	FB_UINT64 lhb_deadlocks;
	srq lhb_data[LCK_MAX_SERIES];
	srq lhb_hash[1];			// Hash table
};

// Secondary header block -- exists only in V3.3 and later lock managers.
// It is pointed to by the word in the lhb that used to contain a pattern.

struct shb
{
	UCHAR shb_type;					// memory tag - always type_shb
	SRQ_PTR shb_history;
	SRQ_PTR shb_remove_node;		// Node removing itself
	SRQ_PTR shb_insert_que;			// Queue inserting into
	SRQ_PTR shb_insert_prior;		// Prior of inserting queue
};

// Lock block

struct lbl
{
	UCHAR lbl_type;					// mem tag: type_lbl=in use, type_null=free
	UCHAR lbl_state;				// High state granted
	UCHAR lbl_size;					// Key bytes allocated
	UCHAR lbl_length;				// Key bytes used
	srq lbl_requests;				// Requests granted
	srq lbl_lhb_hash;				// Collision que for hash table
	srq lbl_lhb_data;				// Lock data que by series
	LOCK_DATA_T lbl_data;			// User data
	UCHAR lbl_series;				// Lock series
	UCHAR lbl_flags;				// Unused. Misc flags
	USHORT lbl_pending_lrq_count;	// count of lbl_requests with LRQ_pending
	USHORT lbl_counts[LCK_max];		// Counts of granted locks
	UCHAR lbl_key[1];				// Key value
};

/* Lock requests */

struct lrq
{
	UCHAR lrq_type;					// mem tag: type_lrq=in use, type_null=free
	UCHAR lrq_requested;			// Level requested
	UCHAR lrq_state;				// State of lock request
	USHORT lrq_flags;				// Misc crud
	SRQ_PTR lrq_owner;				// Owner making request
	SRQ_PTR lrq_lock;				// Lock requested
	LOCK_DATA_T lrq_data;			// Lock data requested
	srq lrq_own_requests;			// Locks granted for owner
	srq lrq_lbl_requests;			// Que of requests (active, pending)
	srq lrq_own_blocks;				// Owner block que
	srq lrq_own_pending;			// Owner pending que
	lock_ast_t lrq_ast_routine;		// Block ast routine
	void* lrq_ast_argument;			// Ast argument
};

// lrq_flags
inline constexpr USHORT LRQ_blocking		= 1;	// Request is blocking
inline constexpr USHORT LRQ_pending			= 2;	// Request is pending
inline constexpr USHORT LRQ_rejected		= 4;	// Request is rejected
inline constexpr USHORT LRQ_deadlock		= 8;	// Request has been seen by the deadlock-walk
inline constexpr USHORT LRQ_repost			= 16;	// Request block used for repost
inline constexpr USHORT LRQ_scanned			= 32;	// Request already scanned for deadlock
inline constexpr USHORT LRQ_blocking_seen	= 64;	// Blocking notification received by owner
inline constexpr USHORT LRQ_just_granted	= 128;	// Request is just granted and blocked owners still have not sent blocking AST
inline constexpr USHORT LRQ_wait_timeout	= 256;	// Request is being waited on with a timeout

// Process block

struct prc
{
	UCHAR prc_type;					// memory tag - always type_lpr
	int prc_process_id;				// Process ID
	srq prc_lhb_processes;			// Process que
	srq prc_owners;					// Owners
	Firebird::event_t prc_blocking;	// Blocking event block
	USHORT prc_flags;				// Unused. Misc flags
};

// Owner block

struct own
{
	UCHAR own_type;					// memory tag - always type_own
	UCHAR own_owner_type;			// type of owner
	SSHORT own_count;				// init count for the owner
	LOCK_OWNER_T own_owner_id;		// Owner ID
	srq own_lhb_owners;				// Owner que (global)
	srq own_prc_owners;				// Owner que (process wide)
	srq own_requests;				// Lock requests granted
	srq own_blocks;					// Lock requests blocking
	srq own_pending;				// Lock requests pending
	SRQ_PTR own_process;			// Process we belong to
	ThreadId own_thread_id;			// Last thread attached to the owner
	FB_UINT64 own_acquire_time;		// lhb_acquires when owner last tried acquire()
	USHORT own_waits;				// Number of requests we are waiting on
	USHORT own_ast_count;			// Number of ASTs being delivered
	Firebird::event_t own_wakeup;	// Wakeup event block
	USHORT own_flags;				// Misc stuff
};

// Flags in own_flags
inline constexpr USHORT OWN_scanned		= 1;	// Owner has been deadlock scanned
inline constexpr USHORT OWN_wakeup		= 2;	// Owner has been awoken
inline constexpr USHORT OWN_signaled	= 4;	// Signal is thought to be delivered

// Lock manager history block

struct his
{
	UCHAR his_type;					// memory tag - always type_his
	UCHAR his_operation;			// operation that occurred
	SRQ_PTR his_next;				// SRQ_PTR to next item in history list
	SRQ_PTR his_process;			// owner to record for this operation
	SRQ_PTR his_lock;				// lock to record for operation
	SRQ_PTR his_request;			// request to record for operation
};

// his_operation definitions
// should be UCHAR according to his_operation but is USHORT in lock.cpp:post_operation
inline constexpr UCHAR his_enq			= 1;
inline constexpr UCHAR his_deq			= 2;
inline constexpr UCHAR his_convert		= 3;
inline constexpr UCHAR his_signal		= 4;
inline constexpr UCHAR his_post_ast		= 5;
inline constexpr UCHAR his_wait			= 6;
inline constexpr UCHAR his_del_process	= 7;
inline constexpr UCHAR his_del_lock		= 8;
inline constexpr UCHAR his_del_request	= 9;
inline constexpr UCHAR his_deny			= 10;
inline constexpr UCHAR his_grant		= 11;
inline constexpr UCHAR his_leave_ast	= 12;
inline constexpr UCHAR his_scan			= 13;
inline constexpr UCHAR his_dead			= 14;
inline constexpr UCHAR his_enter		= 15;
inline constexpr UCHAR his_bug			= 16;
inline constexpr UCHAR his_active		= 17;
inline constexpr UCHAR his_cleanup		= 18;
inline constexpr UCHAR his_del_owner	= 19;
inline constexpr UCHAR his_MAX			= his_del_owner;

namespace Firebird {
	class AtomicCounter;
	class Mutex;
	class RWLock;
	class Config;
}

namespace Jrd {

class thread_db;

class LockManager final : public Firebird::GlobalStorage, public Firebird::IpcObject
{
	class LockTableGuard
	{
	public:
		explicit LockTableGuard(LockManager* lm, const char* f, SRQ_PTR owner = 0)
			: m_lm(lm), m_owner(owner)
		{
			if (!m_lm->m_localMutex.tryEnter(f))
			{
				m_lm->m_localMutex.enter(f);
				m_lm->m_blockage = true;
			}

			if (m_owner)
				m_lm->acquire_shmem(m_owner);
		}

		~LockTableGuard()
		{
			try
			{
				if (m_owner)
					m_lm->release_shmem(m_owner);

				m_lm->m_localMutex.leave();
			}
			catch (const Firebird::Exception&)
			{
				DtorException::devHalt();
			}
		}

		void setOwner(SLONG owner)
		{
			fb_assert(owner && m_owner && m_lm->m_sharedMemory &&
				m_owner == m_lm->m_sharedMemory->getHeader()->lhb_active_owner);
			m_owner = m_lm->m_sharedMemory->getHeader()->lhb_active_owner = owner;
		}

	private:
		// Forbid copying
		LockTableGuard(const LockTableGuard&);
		LockTableGuard& operator=(const LockTableGuard&);

		LockManager* m_lm;
		SRQ_PTR m_owner;
	};

	class LockTableCheckout
	{
	public:
		LockTableCheckout(LockManager* lm, const char* f)
			: m_lm(lm), m_owner(m_lm->m_sharedMemory->getHeader()->lhb_active_owner)
#ifdef DEV_BUILD
			  , from(f)
#define FB_LOCKED_FROM from
#else
#define FB_LOCKED_FROM NULL
#endif
		{
			m_lm->release_shmem(m_owner);
			m_lm->m_localMutex.leave();
		}

		~LockTableCheckout()
		{
			try
			{
				if (!m_lm->m_localMutex.tryEnter(FB_LOCKED_FROM))
				{
					m_lm->m_localMutex.enter(FB_LOCKED_FROM);
					m_lm->m_blockage = true;
				}

				m_lm->acquire_shmem(m_owner);
			}
			catch (const Firebird::Exception&)
			{
				DtorException::devHalt();
			}
		}

	private:
		// Forbid copying
		LockTableCheckout(const LockTableCheckout&);
		LockTableCheckout& operator=(const LockTableCheckout&);

		LockManager* m_lm;
		const SRQ_PTR m_owner;
#ifdef DEV_BUILD
		const char* from;
#endif
	};
#undef FB_LOCKED_FROM

	const int PID;

public:
	explicit LockManager(const Firebird::string&, const Firebird::Config* conf);
	~LockManager();

	bool initializeOwner(Firebird::CheckStatusWrapper*, LOCK_OWNER_T, UCHAR, SRQ_PTR*);
	void shutdownOwner(thread_db*, SRQ_PTR*);

	SRQ_PTR enqueue(thread_db*, Firebird::CheckStatusWrapper*, SRQ_PTR, const USHORT,
		const UCHAR*, const USHORT, UCHAR, lock_ast_t, void*, LOCK_DATA_T, SSHORT, SRQ_PTR);
	bool convert(thread_db*, Firebird::CheckStatusWrapper*, SRQ_PTR, UCHAR, SSHORT, lock_ast_t, void*);
	UCHAR downgrade(thread_db*, Firebird::CheckStatusWrapper*, const SRQ_PTR);
	bool dequeue(const SRQ_PTR);

	void repost(thread_db*, lock_ast_t, void*, SRQ_PTR);
	bool cancelWait(SRQ_PTR);

	LOCK_DATA_T queryData(const USHORT, const USHORT);
	LOCK_DATA_T readData(SRQ_PTR);
	LOCK_DATA_T readData2(USHORT, const UCHAR*, USHORT, SRQ_PTR);
	LOCK_DATA_T writeData(SRQ_PTR, LOCK_DATA_T);

	void exceptionHandler(const Firebird::Exception& ex, ThreadFinishSync<LockManager*>::ThreadRoutine* routine);

private:
	void acquire_shmem(SRQ_PTR);
	UCHAR* alloc(USHORT, Firebird::CheckStatusWrapper*);
	lbl* alloc_lock(USHORT, Firebird::CheckStatusWrapper*);
	void blocking_action(thread_db*, SRQ_PTR);
	void blocking_action_thread();
	void bug(Firebird::CheckStatusWrapper*, const TEXT*);
	void bug_assert(const TEXT*, ULONG);
	SRQ_PTR create_owner(Firebird::CheckStatusWrapper*, LOCK_OWNER_T, UCHAR);
	bool create_process(Firebird::CheckStatusWrapper*);
	void deadlock_clear();
	lrq* deadlock_scan(own*, lrq*);
	lrq* deadlock_walk(lrq*, bool*);
	void debug_delay(ULONG);
	lbl* find_lock(USHORT, const UCHAR*, USHORT, USHORT*);
	lrq* get_request(SRQ_PTR);
	void grant(lrq*, lbl*);
	bool grant_or_que(thread_db*, lrq*, lbl*, SSHORT);
	bool init_owner_block(Firebird::CheckStatusWrapper*, own*, UCHAR, LOCK_OWNER_T);
	void insert_data_que(lbl*);
	void insert_tail(SRQ, SRQ);
	bool internal_convert(thread_db* database, Firebird::CheckStatusWrapper*, SRQ_PTR, UCHAR, SSHORT,
		lock_ast_t, void*);
	void internal_dequeue(SRQ_PTR);
	static USHORT lock_state(const lbl*);
	void post_blockage(thread_db*, lrq*, lbl*);
	void post_history(USHORT, SRQ_PTR, SRQ_PTR, SRQ_PTR, bool);
	void post_pending(lbl*);
	void post_wakeup(own*);
	bool probe_processes();
	void purge_owner(SRQ_PTR, own*);
	void purge_process(prc*);
	void remap_local_owners();
	void remove_que(SRQ);
	void release_shmem(SRQ_PTR);
	void release_request(lrq*);
	bool signal_owner(thread_db*, own*);

	void validate_history(const SRQ_PTR history_header);
	void validate_lhb(const lhb*);
	void validate_lock(const SRQ_PTR, USHORT, const SRQ_PTR);
	void validate_owner(const SRQ_PTR, USHORT);
	void validate_request(const SRQ_PTR, USHORT, USHORT);
	void validate_shb(const SRQ_PTR);

	void wait_for_request(thread_db*, lrq*, SSHORT);
	bool init_shared_file(Firebird::CheckStatusWrapper*);
	void get_shared_file_name(Firebird::PathName&, ULONG extend = 0) const;

	static void blocking_action_thread(LockManager* lockMgr)
	{
		lockMgr->blocking_action_thread();
	}

	bool initialize(Firebird::SharedMemoryBase* sm, bool init) override;
	void mutexBug(int osErrorCode, const char* text) override;
	bool checkHeader(const Firebird::MemoryHeader* header, bool raiseError = true) override;

	USHORT getType() const override { return Firebird::SharedMemoryBase::SRAM_LOCK_MANAGER; }
	USHORT getVersion() const override { return LHB_VERSION; }
	const char* getName() const override { return "LockManager"; }

	bool m_bugcheck;
	prc* m_process;
	SRQ_PTR m_processOffset;

	Firebird::Mutex m_localMutex;
	Firebird::RWLock m_remapSync;
	Firebird::AtomicCounter m_waitingOwners;

	ThreadFinishSync<LockManager*> m_cleanupSync;
	Firebird::Semaphore m_startupSemaphore;

public:
	Firebird::AutoPtr<Firebird::SharedMemory<lhb> > m_sharedMemory;

private:
	bool m_blockage;

	const Firebird::string& m_dbId;
	const Firebird::Config* const m_config;

	// configurations parameters - cached values
	const ULONG m_acquireSpins;
	const ULONG m_memorySize;
	const bool m_useBlockingThread;

#ifdef USE_SHMEM_EXT
	struct SecondaryFile : public Jrd::MemoryHeader
	{
	};

	class Extent : public SharedMemory<SecondaryFile>
	{
	public:
		Extent() { }
		explicit Extent(Firebird::MemoryPool&) { }

		Extent(const SharedMemoryBase& p)
		{
			assign(p);
		}

		Extent(Firebird::MemoryPool&, const SharedMemoryBase& p)
		{
			assign(p);
		}

		~Extent()
		{
			sh_mem_header = NULL;	// avoid unmapping in dtor
		}

		Extent& operator=(const SharedMemoryBase& p)
		{
			assign(p);
			return *this;
		}

		void assign(const SharedMemoryBase& p);

		bool initialize(bool init);
		void mutexBug(int osErrorCode, const char* text);
	};

	Firebird::ObjectsArray<Extent> m_extents;

	ULONG getTotalMapped() const
	{
		return (ULONG) m_extents.getCount() * getExtentSize();
	}

	ULONG getExtentSize() const
	{
		return m_memorySize;
	}

	ULONG getStartOffset(ULONG n) const
	{
		return n * getExtentSize();
	}

	SRQ_PTR REL_PTR(const void* item);
	void* ABS_PTR(SRQ_PTR item);
	bool createExtent(ULONG memorySize = 0);
#endif
};

} // namespace

#endif // LOCK_LOCK_PROTO_H
