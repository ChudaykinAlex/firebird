/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		vio.cpp
 *	DESCRIPTION:	Virtual IO
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
 *
 * 2001.07.06 Sean Leyne - Code Cleanup, removed "#ifdef READONLY_DATABASE"
 *                         conditionals, as the engine now fully supports
 *                         readonly databases.
 * 2002.08.21 Dmitry Yemanov: fixed bug with a buffer overrun,
 *                            which at least caused invalid dependencies
 *                            to be stored (DB$xxx, for example)
 * 2002.10.21 Nickolay Samofatov: Added support for explicit pessimistic locks
 * 2002.10.29 Nickolay Samofatov: Added support for savepoints
 * 2002.10.29 Sean Leyne - Removed obsolete "Netware" port
 * 2002.12.22 Alex Peshkoff: Bugcheck(291) fix for update_in_place
 *							 of record, modified by pre_trigger
 * 2003.03.01 Nickolay Samofatov: Fixed database corruption when backing out
 *                           the savepoint after large number of DML operations
 *                           (so transaction-level savepoint is dropped) and
 *							 record was updated _not_ under the savepoint and
 *							 deleted under savepoint. Bug affected all kinds
 *							 of savepoints (explicit, statement, PSQL, ...)
 * 2003.03.02 Nickolay Samofatov: Use B+ tree to store undo log
 *
 */

#include "firebird.h"
#include <unordered_map>
#include <stdio.h>
#include <string.h>
#include "../jrd/jrd.h"
#include "../jrd/val.h"
#include "../jrd/req.h"
#include "../jrd/tra.h"
#include "../jrd/ids.h"
#include "../jrd/lck.h"
#include "../jrd/lls.h"
#include "../jrd/scl.h"
#include "../jrd/sqz.h"
#include "../jrd/flags.h"
#include "../jrd/ods.h"
#include "../jrd/os/pio.h"
#include "../jrd/btr.h"
#include "../jrd/exe.h"
#include "../jrd/scl.h"
#include "../common/classes/alloc.h"
#include "../common/ThreadStart.h"
#include "../jrd/vio_debug.h"
#include "../jrd/blb_proto.h"
#include "../jrd/btr_proto.h"
#include "../jrd/cch_proto.h"
#include "../jrd/dfw_proto.h"
#include "../jrd/dpm_proto.h"
#include "../jrd/err_proto.h"
#include "../jrd/evl_proto.h"
#include "../yvalve/gds_proto.h"
#include "../jrd/idx_proto.h"
#include "../common/isc_s_proto.h"
#include "../common/isc_proto.h"
#include "../jrd/jrd_proto.h"
#include "../jrd/ini_proto.h"
#include "../jrd/lck_proto.h"
#include "../jrd/met_proto.h"
#include "../jrd/mov_proto.h"
#include "../jrd/pag_proto.h"
#include "../jrd/scl_proto.h"
#include "../jrd/tpc_proto.h"
#include "../jrd/tra_proto.h"
#include "../jrd/vio_proto.h"
#include "../jrd/dyn_ut_proto.h"
#include "../jrd/Function.h"
#include "../common/StatusArg.h"
#include "../jrd/GarbageCollector.h"
#include "../jrd/ProfilerManager.h"
#include "../jrd/trace/TraceManager.h"
#include "../jrd/trace/TraceJrdHelpers.h"
#include "../common/Task.h"
#include "../jrd/WorkerAttachment.h"

using namespace Jrd;
using namespace Firebird;

static void check_class(thread_db*, jrd_tra*, record_param*, record_param*, USHORT);
static bool check_nullify_source(thread_db*, record_param*, record_param*, int, int = -1);
static void check_owner(thread_db*, jrd_tra*, record_param*, record_param*, USHORT);
static void check_repl_state(thread_db*, jrd_tra*, record_param*, record_param*, USHORT);
static int check_precommitted(const jrd_tra*, const record_param*);
static void check_rel_field_class(thread_db*, record_param*, jrd_tra*);
static void delete_record(thread_db*, record_param*, ULONG, MemoryPool*);
static UCHAR* delete_tail(thread_db*, record_param*, ULONG, UCHAR* = nullptr, const UCHAR* = nullptr);
static void expunge(thread_db*, record_param*, const jrd_tra*, ULONG);
static bool dfw_should_know(thread_db*, record_param* org_rpb, record_param* new_rpb,
	USHORT irrelevant_field, bool void_update_is_relevant = false);
static void garbage_collect(thread_db*, record_param*, ULONG, RecordStack&);


#ifdef VIO_DEBUG
#include <stdio.h>
#include <stdarg.h>

int vio_debug_flag = DEBUG_TRACE_ALL_INFO;

void VIO_trace(int level, const char* format, ...)
{
	if (vio_debug_flag <= level)
		return;

	Firebird::string buffer;
	va_list params;
	va_start(params, format);
	buffer.vprintf(format, params);
	va_end(params);

	buffer.rtrim("\n");

	gds__trace(buffer.c_str());
}

#endif

enum UndoDataRet
{
	udExists,		// record data was restored from undo-log
	udForceBack,	// force read first back version
	udForceTwice,	// force read second back version
	udNone			// record was not changed under current savepoint, use it as is
};

static void gbak_put_search_system_schema_flag(thread_db* tdbb, record_param* rpb, jrd_tra* transaction);

static UndoDataRet get_undo_data(thread_db* tdbb, jrd_tra* transaction,
	record_param* rpb, MemoryPool* pool);

static void invalidate_cursor_records(jrd_tra*, record_param*);

// flags to pass into list_staying
inline constexpr int LS_ACTIVE_RPB	= 0x01;
inline constexpr int LS_NO_RESTART	= 0x02;

static void list_staying(thread_db*, record_param*, RecordStack&, int flags = 0);
static void list_staying_fast(thread_db*, record_param*, RecordStack&, record_param* = NULL, int flags = 0);
static void notify_garbage_collector(thread_db* tdbb, record_param* rpb,
	TraNumber tranid = MAX_TRA_NUMBER);

enum class PrepareResult
{
	SUCCESS,
	CONFLICT,
	DELETED,
	SKIP_LOCKED,
	LOCK_ERROR
};

static PrepareResult prepare_update(thread_db*, jrd_tra*, TraNumber commit_tid_read, record_param*,
	record_param*, record_param*, PageStack&, bool);

static void protect_system_table_insert(thread_db* tdbb, const Request* req, const jrd_rel* relation,
	bool force_flag = false);
static void protect_system_table_delupd(thread_db* tdbb, const jrd_rel* relation, const char* operation,
	bool force_flag = false);
static void purge(thread_db*, record_param*);
static void replace_record(thread_db*, record_param*, PageStack*, const jrd_tra*);
static void refresh_fk_fields(thread_db*, Record*, record_param*, record_param*);
static SSHORT set_metadata_id(thread_db*, Record*, USHORT, drq_type_t, const char*);
static void set_nbackup_id(thread_db*, Record*, USHORT, drq_type_t, const char*);
static void set_owner_name(thread_db*, Record*, USHORT);
static bool set_security_class(thread_db*, Record*, USHORT);
static void set_system_flag(thread_db*, Record*, USHORT);
static void verb_post(thread_db*, jrd_tra*, record_param*, Record*);

namespace Jrd
{

class SweepTask : public Task
{
	struct RelInfo; // forward decl

public:
	SweepTask(thread_db* tdbb, MemoryPool* pool, TraceSweepEvent* traceSweep) : Task(),
		m_pool(pool),
		m_dbb(NULL),
		m_items(*m_pool),
		m_stop(false),
		m_nextRelID(0),
		m_lastRelID(0),
		m_relInfo(*m_pool)
	{
		m_dbb = tdbb->getDatabase();
		Attachment* att = tdbb->getAttachment();

		int workers = 1;
		if (att->att_parallel_workers > 0)
			workers = att->att_parallel_workers;

		for (int i = 0; i < workers; i++)
			m_items.add(FB_NEW_POOL(*m_pool) Item(this));

		m_items[0]->m_ownAttach = false;
		m_items[0]->m_attStable = att->getStable();
		m_items[0]->m_tra = tdbb->getTransaction();

		m_relInfo.grow(m_items.getCount());

		m_lastRelID = att->att_relations->count();
	};

	virtual ~SweepTask()
	{
		for (Item** p = m_items.begin(); p < m_items.end(); p++)
			delete *p;
	};

	class Item : public Task::WorkItem
	{
	public:
		Item(SweepTask* task) : Task::WorkItem(task),
			m_inuse(false),
			m_ownAttach(true),
			m_tra(NULL),
			m_relInfo(NULL),
			m_firstPP(0),
			m_lastPP(0)
		{}

		virtual ~Item()
		{
			if (!m_ownAttach || !m_attStable)
				return;

			Attachment* att = NULL;
			{
				AttSyncLockGuard guard(*m_attStable->getSync(), FB_FUNCTION);
				att = m_attStable->getHandle();
				if (!att)
					return;
				fb_assert(att->att_use_count > 0);
			}

			FbLocalStatus status;
			if (m_tra)
			{
				BackgroundContextHolder tdbb(att->att_database, att, &status, FB_FUNCTION);
				TRA_commit(tdbb, m_tra, false);
			}
			WorkerAttachment::releaseAttachment(&status, m_attStable);
		}

		SweepTask* getSweepTask() const
		{
			return reinterpret_cast<SweepTask*> (m_task);
		}

		bool init(thread_db* tdbb)
		{
			FbStatusVector* status = tdbb->tdbb_status_vector;

			Attachment* att = NULL;

			if (m_ownAttach && !m_attStable.hasData())
				m_attStable = WorkerAttachment::getAttachment(status, getSweepTask()->m_dbb);

			if (m_attStable)
				att = m_attStable->getHandle();

			if (!att)
			{
				Arg::Gds(isc_bad_db_handle).copyTo(status);
				return false;
			}

			tdbb->setDatabase(att->att_database);
			tdbb->setAttachment(att);

			if (m_ownAttach && !m_tra)
			{
				const UCHAR sweep_tpb[] =
				{
					isc_tpb_version1, isc_tpb_read,
					isc_tpb_read_committed, isc_tpb_rec_version
				};

				try
				{
					WorkerContextHolder holder(tdbb, FB_FUNCTION);
					m_tra = TRA_start(tdbb, sizeof(sweep_tpb), sweep_tpb);
					DPM_scan_pages(tdbb);
				}
				catch(const Exception& ex)
				{
					ex.stuffException(tdbb->tdbb_status_vector);
					return false;
				}
			}

			tdbb->setTransaction(m_tra);
			tdbb->tdbb_flags |= TDBB_sweeper;

			return true;
		}

		bool m_inuse;
		bool m_ownAttach;
		RefPtr<StableAttachmentPart> m_attStable;
		jrd_tra* m_tra;

		// part of work: relation, first and last PP's to work on
		RelInfo* m_relInfo;
		ULONG m_firstPP;
		ULONG m_lastPP;
	};

	bool handler(WorkItem& _item);

	bool getWorkItem(WorkItem** pItem);
	bool getResult(IStatus* status)
	{
		if (status)
		{
			status->init();
			status->setErrors(m_status.getErrors());
		}

		return m_status.isSuccess();
	}

	int getMaxWorkers()
	{
		return m_items.getCount();
	}

private:
	// item is handled, get next portion of work and update RelInfo
	// also, detect if relation is handled completely
	// return true if there is some more work to do
	bool updateRelInfo(Item* item)
	{
		RelInfo* relInfo = item->m_relInfo;

		if (relInfo->countPP == 0 || relInfo->nextPP >= relInfo->countPP)
		{
			relInfo->workers--;
			return false;
		}

		item->m_firstPP = relInfo->nextPP;
		item->m_lastPP = item->m_firstPP;
		if (item->m_lastPP >= relInfo->countPP)
			item->m_lastPP = relInfo->countPP - 1;
		relInfo->nextPP = item->m_lastPP + 1;

		return true;
	}

	void setError(IStatus* status, bool stopTask)
	{
		const bool copyStatus = (m_status.isSuccess() && status && status->getState() == IStatus::STATE_ERRORS);
		if (!copyStatus && (!stopTask || m_stop))
			return;

		MutexLockGuard guard(m_mutex, FB_FUNCTION);
		if (m_status.isSuccess() && copyStatus)
			m_status.save(status);
		if (stopTask)
			m_stop = true;
	}

	MemoryPool* m_pool;
	Database* m_dbb;
	Mutex m_mutex;
	HalfStaticArray<Item*, 8> m_items;
	StatusHolder m_status;
	volatile bool m_stop;

	struct RelInfo
	{
		RelInfo()
		{
			memset(this, 0, sizeof(*this));
		}

		USHORT rel_id;
		ULONG  countPP;	// number of pointer pages in relation
		ULONG  nextPP;	// number of PP to assign to next worker
		ULONG  workers;	// number of workers for this relation
	};

	USHORT m_nextRelID;		// next relation to work on
	USHORT m_lastRelID;		// last relation to work on
	HalfStaticArray<RelInfo, 8> m_relInfo;	// relations worked on
};


bool SweepTask::handler(WorkItem& _item)
{
	Item* item = reinterpret_cast<Item*>(&_item);

	ThreadContextHolder tdbb(NULL);

	if (!item->init(tdbb))
	{
		setError(tdbb->tdbb_status_vector, true);
		return false;
	}

	WorkerContextHolder wrkHolder(tdbb, FB_FUNCTION);

	record_param rpb;
	jrd_rel* relation = NULL;

	try
	{
		RelInfo* relInfo = item->m_relInfo;

		Database* dbb = tdbb->getDatabase();
		Attachment* att = tdbb->getAttachment();

		/*relation = (*att->att_relations)[relInfo->rel_id];
		if (relation)*/
			relation = MET_lookup_relation_id(tdbb, relInfo->rel_id, false);

		if (relation &&
			!(relation->rel_flags & (REL_deleted | REL_deleting)) &&
			!relation->isTemporary() &&
			relation->getPages(tdbb)->rel_pages)
		{
			jrd_rel::GCShared gcGuard(tdbb, relation);
			if (!gcGuard.gcEnabled())
			{
				string str;
				str.printf("Acquire garbage collection lock failed (%s)", relation->rel_name.toQuotedString().c_str());
				status_exception::raise(Arg::Gds(isc_random) << Arg::Str(str));
			}

			jrd_tra* tran = tdbb->getTransaction();

			if (relInfo->countPP == 0)
				relInfo->countPP = relation->getPages(tdbb)->rel_pages->count();

			rpb.rpb_relation = relation;
			rpb.rpb_org_scans = relation->rel_scan_count++;
			rpb.rpb_record = NULL;
			rpb.rpb_stream_flags = RPB_s_no_data | RPB_s_sweeper;
			rpb.getWindow(tdbb).win_flags = WIN_large_scan;

			rpb.rpb_number.compose(dbb->dbb_max_records, dbb->dbb_dp_per_pp, 0, 0, item->m_firstPP);
			rpb.rpb_number.decrement();

			RecordNumber lastRecNo;
			lastRecNo.compose(dbb->dbb_max_records, dbb->dbb_dp_per_pp, 0, 0, item->m_lastPP + 1);
			lastRecNo.decrement();

			while (VIO_next_record(tdbb, &rpb, tran, NULL, DPM_next_pointer_page))
			{
				CCH_RELEASE(tdbb, &rpb.getWindow(tdbb));

				if (relation->rel_flags & REL_deleting)
					break;

				if (rpb.rpb_number >= lastRecNo)
					break;

				if (m_stop)
					break;

				JRD_reschedule(tdbb);

				tran->tra_oldest_active = dbb->dbb_oldest_snapshot;
			}

			delete rpb.rpb_record;
			--relation->rel_scan_count;
		}

		return !m_stop;
	}
	catch(const Exception& ex)
	{
		ex.stuffException(tdbb->tdbb_status_vector);

		delete rpb.rpb_record;
		if (relation)
		{
			if (relation->rel_scan_count) {
				--relation->rel_scan_count;
			}
		}
	}

	setError(tdbb->tdbb_status_vector, true);
	return false;
}

bool SweepTask::getWorkItem(WorkItem** pItem)
{
	MutexLockGuard guard(m_mutex, FB_FUNCTION);

	Item* item = reinterpret_cast<Item*> (*pItem);

	if (item == NULL)
	{
		for (Item** p = m_items.begin(); p < m_items.end(); p++)
			if (!(*p)->m_inuse)
			{
				(*p)->m_inuse = true;
				*pItem = item = *p;
				break;
			}
	}
	else if (updateRelInfo(item))
		return true;

	if (!item)
		return false;

	// assign part of task to item
	if (m_nextRelID >= m_lastRelID)
	{
		// find not handled relation and help to handle it
		RelInfo* relInfo = m_relInfo.begin();
		for (; relInfo < m_relInfo.end(); relInfo++)
			if (relInfo->workers > 0)
			{
				item->m_relInfo = relInfo;
				relInfo->workers++;
				if (updateRelInfo(item))
					return true;
			}

		item->m_inuse = false;
		return false;
	}

	// start to handle next relation
	USHORT relID = m_nextRelID++;
	RelInfo* relInfo = m_relInfo.begin();
	for (; relInfo < m_relInfo.end(); relInfo++)
		if (relInfo->workers == 0)
		{
			relInfo->workers++;
			relInfo->rel_id = relID;
			relInfo->countPP = 0;
			item->m_relInfo = relInfo;
			item->m_firstPP = item->m_lastPP = 0;
			relInfo->nextPP = item->m_lastPP + 1;

			return true;
		}


	item->m_inuse = false;
	return false;
}

}; // namespace Jrd


namespace
{
	inline UCHAR* unpack(record_param* rpb, ULONG outLength, UCHAR* output)
	{
		if (rpb->rpb_flags & rpb_not_packed)
		{
			const auto length = MIN(rpb->rpb_length, outLength);

			memcpy(output, rpb->rpb_address, length);
			output += length;

			if (rpb->rpb_length > length)
			{
				// Short records may be zero-padded up to the fragmented header size.
				// Take it into account while checking for a possible buffer overrun.

				auto tail = rpb->rpb_address + length;
				const auto end = rpb->rpb_address + rpb->rpb_length;

				while (tail < end)
				{
					if (*tail++)
						BUGCHECK(179);	// msg 179 decompression overran buffer
				}
			}

			return output;
		}

		return Compressor::unpack(rpb->rpb_length, rpb->rpb_address, outLength, output);
	}
};


static bool assert_gc_enabled(const jrd_tra* transaction, const jrd_rel* relation)
{
/**************************************
 *
 *	a s s e r t _ g c _ e n a b l e d
 *
 **************************************
 *
 * Functional description
 *	Ensure that calls of purge\expunge\VIO_backout are safe and don't break
 *  results of online validation run.
 *
 * Notes
 *  System and temporary relations are not validated online.
 *  Non-zero rel_sweep_count is possible only under GCShared control when
 *  garbage collection is enabled.
 *
 *  VIO_backout is more complex as it could run without GCShared control.
 *  Therefore we additionally check if we own relation lock in "write" mode -
 *  in this case online validation is not run against given relation.
 *
 **************************************/
	if (relation->rel_sweep_count || relation->isSystem() || relation->isTemporary())
		return true;

	if (relation->rel_flags & REL_gc_disabled)
		return false;

	vec<Lock*>* vector = transaction->tra_relation_locks;
	if (!vector || relation->rel_id >= vector->count())
		return false;

	Lock* lock = (*vector)[relation->rel_id];
	if (!lock)
		return false;

	return (lock->lck_physical == LCK_SW) || (lock->lck_physical == LCK_EX);
}


// Pick up relation ids
#include "../jrd/ini.h"


// General protection against gbak impersonators, to be used for VIO_modify and VIO_store.
inline void check_gbak_cheating_insupd(thread_db* tdbb, const jrd_rel* relation, const char* op)
{
	const Attachment* const attachment = tdbb->getAttachment();
	const Request* const request = tdbb->getRequest();

	if (relation->isSystem() && attachment->isGbak() && !(attachment->att_flags & ATT_creator) &&
		!request->hasInternalStatement())
	{
		status_exception::raise(Arg::Gds(isc_protect_sys_tab) <<
			Arg::Str(op) << relation->rel_name.toQuotedString());
	}
}

// Used in VIO_erase.
inline void check_gbak_cheating_delete(thread_db* tdbb, const jrd_rel* relation)
{
	const Attachment* const attachment = tdbb->getAttachment();

	if (relation->isSystem() && attachment->isGbak())
	{
		if (attachment->att_flags & ATT_creator)
		{
			// TDBB_dont_post_dfw signals that we are in DFW.
			if (tdbb->tdbb_flags & TDBB_dont_post_dfw)
				return;

			// There are 2 tables whose contents gbak might delete:
			// - RDB$INDEX_SEGMENTS if it detects inconsistencies while restoring
			// - RDB$FILES if switch -k is set
			switch(relation->rel_id)
			{
			case rel_segments:
			case rel_files:
				return;

			// fix_plugins_schemas may also delete these objects:
			case rel_relations:
			case rel_rfr:
			case rel_fields:
			case rel_vrel:
			case rel_refc:
			case rel_rcon:
			case rel_ccon:
			case rel_triggers:
			case rel_indices:
			case rel_gens:
			case rel_classes:
			case rel_priv:
				return;
			}
		}

		protect_system_table_delupd(tdbb, relation, "DELETE", true);
	}
}

inline int wait(thread_db* tdbb, jrd_tra* transaction, const record_param* rpb, bool probe)
{
	if (!probe && transaction->getLockWait())
		tdbb->bumpRelStats(RuntimeStatistics::RECORD_WAITS, rpb->rpb_relation->rel_id);

	return TRA_wait(tdbb, transaction, rpb->rpb_transaction_nr,
		probe ? jrd_tra::tra_probe : jrd_tra::tra_wait);
}

inline bool checkGCActive(thread_db* tdbb, record_param* rpb, int& state)
{
	Lock temp_lock(tdbb, sizeof(SINT64), LCK_record_gc);
	temp_lock.setKey(((SINT64) rpb->rpb_page << 16) | rpb->rpb_line);

	ThreadStatusGuard temp_status(tdbb);

	if (!LCK_lock(tdbb, &temp_lock, LCK_SR, LCK_NO_WAIT))
	{
		rpb->rpb_transaction_nr = LCK_read_data(tdbb, &temp_lock);
		state = tra_active;
		return true;
	}

	LCK_release(tdbb, &temp_lock);
	rpb->rpb_flags &= ~rpb_gc_active;
	state = tra_dead;
	return false;
}

inline void waitGCActive(thread_db* tdbb, const record_param* rpb)
{
	Lock temp_lock(tdbb, sizeof(SINT64), LCK_record_gc);
	temp_lock.setKey(((SINT64) rpb->rpb_page << 16) | rpb->rpb_line);

	SSHORT wait = LCK_WAIT;

	jrd_tra* transaction = tdbb->getTransaction();
	if (transaction->tra_number == rpb->rpb_transaction_nr)
	{
		// There is no sense to wait for self
		wait = LCK_NO_WAIT;
	}

	if (!LCK_lock(tdbb, &temp_lock, LCK_SR, wait))
		ERR_punt();

	LCK_release(tdbb, &temp_lock);
}

inline Lock* lockGCActive(thread_db* tdbb, const jrd_tra* transaction, const record_param* rpb)
{
	AutoPtr<Lock> lock(FB_NEW_RPT(*tdbb->getDefaultPool(), 0)
		Lock(tdbb, sizeof(SINT64), LCK_record_gc));
	lock->setKey(((SINT64) rpb->rpb_page << 16) | rpb->rpb_line);
	lock->lck_data = transaction->tra_number;

	ThreadStatusGuard temp_status(tdbb);

	if (!LCK_lock(tdbb, lock, LCK_EX, LCK_NO_WAIT))
		return NULL;

	return lock.release();
}

static const UCHAR gc_tpb[] =
{
	isc_tpb_version1, isc_tpb_read,
	isc_tpb_read_committed, isc_tpb_rec_version,
	isc_tpb_ignore_limbo
};


inline void clearRecordStack(RecordStack& stack)
{
/**************************************
 *
 *	c l e a r R e c o r d S t a c k
 *
 **************************************
 *
 * Functional description
 *	Clears stack, deleting each entry, popped from it.
 *
 **************************************/
	while (stack.hasData())
	{
		Record* r = stack.pop();
		// records from undo log must not be deleted
		if (!r->isTempActive())
			delete r;
	}
}

inline bool needDfw(thread_db* tdbb, const jrd_tra* transaction)
{
/**************************************
 *
 *	n e e d D f w
 *
 **************************************
 *
 * Functional description
 *	Checks, should DFW be called or not
 *	when system relations are modified.
 *
 **************************************/
	return !((transaction->tra_flags & TRA_system) || (tdbb->tdbb_flags & TDBB_dont_post_dfw));
}

void VIO_backout(thread_db* tdbb, record_param* rpb, const jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ b a c k o u t
 *
 **************************************
 *
 * Functional description
 *	Backout the current version of a record.  This may called
 *	either because of transaction death or because the record
 *	violated a unique index.  In either case, get rid of the
 *	current version and back an old version.
 *
 *	This routine is called with an inactive record_param, and has to
 *	take great pains to avoid conflicting with another process
 *	which is also trying to backout the same record.  On exit
 *	there is no active record_param, and the record may or may not have
 *	been backed out, depending on whether we encountered conflict.
 *	But this record is doomed, and if we don't get it somebody
 *	will.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	fb_assert(assert_gc_enabled(transaction, rpb->rpb_relation));

	jrd_rel* const relation = rpb->rpb_relation;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES,
		"VIO_backout (rel_id %u, record_param %" SQUADFORMAT", transaction %" SQUADFORMAT")\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0);
#endif

	// If there is data in the record, fetch it now.  If the old version
	// is a differences record, we will need it sooner.  In any case, we
	// will need it eventually to clean up blobs and indices. If the record
	// has changed in between, stop now before things get worse.

	record_param temp = *rpb;
	if (!DPM_get(tdbb, &temp, LCK_read))
		return;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		temp.rpb_page, temp.rpb_line, temp.rpb_transaction_nr,
		temp.rpb_flags, temp.rpb_b_page, temp.rpb_b_line,
		temp.rpb_f_page, temp.rpb_f_line);

	if (temp.rpb_b_page != rpb->rpb_b_page || temp.rpb_b_line != rpb->rpb_b_line ||
		temp.rpb_transaction_nr != rpb->rpb_transaction_nr)
	{
		VIO_trace(DEBUG_WRITES_INFO,
			"    wrong record!)\n");
	}
#endif

	if (temp.rpb_b_page != rpb->rpb_b_page || temp.rpb_b_line != rpb->rpb_b_line ||
		temp.rpb_transaction_nr != rpb->rpb_transaction_nr)
	{
		CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
		return;
	}

	AutoLock gcLockGuard(tdbb, lockGCActive(tdbb, transaction, &temp));

	if (!gcLockGuard)
	{
		CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
		return;
	}

	RecordStack going, staying;
	Record* data = NULL;
	Record* old_data = NULL;

	AutoTempRecord gc_rec1;
	AutoTempRecord gc_rec2;

	bool samePage;
	bool deleted;

	if ((temp.rpb_flags & rpb_deleted) && (!(temp.rpb_flags & rpb_delta)))
		CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
	else
	{
		temp.rpb_record = gc_rec1 = VIO_gc_record(tdbb, relation);
		VIO_data(tdbb, &temp, relation->rel_pool);
		data = temp.rpb_prior;
		old_data = temp.rpb_record;
		rpb->rpb_prior = temp.rpb_prior;
		going.push(temp.rpb_record);
	}

	// Set up an extra record parameter block.  This will be used to preserve
	// the main record information while we chase fragments.

	record_param temp2 = temp = *rpb;

	// If there is an old version of the record, fetch it's data now.

	RuntimeStatistics::Accumulator backversions(tdbb, relation,
												RuntimeStatistics::RECORD_BACKVERSION_READS);

	if (rpb->rpb_b_page)
	{
		temp.rpb_record = gc_rec2 = VIO_gc_record(tdbb, relation);

		while (true)
		{
			if (!DPM_get(tdbb, &temp, LCK_read))
				return;

			if (temp.rpb_b_page != rpb->rpb_b_page || temp.rpb_b_line != rpb->rpb_b_line ||
				temp.rpb_transaction_nr != rpb->rpb_transaction_nr)
			{
				CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
				return;
			}

			if (temp.rpb_flags & rpb_delta)
				temp.rpb_prior = data;

			if (!DPM_fetch_back(tdbb, &temp, LCK_read, -1))
			{
				fb_utils::init_status(tdbb->tdbb_status_vector);
				continue;
			}

			++backversions;

			if (temp.rpb_flags & rpb_deleted)
				CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
			else
				VIO_data(tdbb, &temp, relation->rel_pool);

			temp.rpb_page = rpb->rpb_b_page;
			temp.rpb_line = rpb->rpb_b_line;

			break;
		}
	}

	// Re-fetch the record.

	if (!DPM_get(tdbb, rpb, LCK_write))
		return;

#ifdef VIO_DEBUG
	if (temp2.rpb_b_page != rpb->rpb_b_page || temp.rpb_b_line != rpb->rpb_b_line ||
		temp.rpb_transaction_nr != rpb->rpb_transaction_nr)
	{
		VIO_trace(DEBUG_WRITES_INFO,
			"    record changed!)\n");
	}
#endif

	// If the record is in any way suspicious, release the record and give up.

	if (rpb->rpb_b_page != temp2.rpb_b_page || rpb->rpb_b_line != temp2.rpb_b_line ||
		rpb->rpb_transaction_nr != temp2.rpb_transaction_nr)
	{
		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		return;
	}

	// even if the record isn't suspicious, it may have changed a little

	temp2 = *rpb;
	rpb->rpb_undo = old_data;

	if (rpb->rpb_flags & rpb_delta)
		rpb->rpb_prior = data;

	// Handle the case of no old version simply.

	if (!rpb->rpb_b_page)
	{
		if (!(rpb->rpb_flags & rpb_deleted))
		{
			DPM_backout_mark(tdbb, rpb, transaction);

			RecordStack empty_staying;
			IDX_garbage_collect(tdbb, rpb, going, empty_staying);
			BLB_garbage_collect(tdbb, going, empty_staying, rpb->rpb_page, relation);
			going.pop();

			if (!DPM_get(tdbb, rpb, LCK_write))
			{
				fb_assert(false);
				return;
			}

			if (rpb->rpb_b_page != temp2.rpb_b_page || rpb->rpb_b_line != temp2.rpb_b_line ||
				rpb->rpb_transaction_nr != temp2.rpb_transaction_nr)
			{
				fb_assert(false);
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				return;
			}

			fb_assert(rpb->rpb_flags & rpb_gc_active);
			rpb->rpb_flags &= ~rpb_gc_active;

			temp2 = *rpb;
			rpb->rpb_undo = old_data;

			if (rpb->rpb_flags & rpb_delta)
				rpb->rpb_prior = data;
		}

		gcLockGuard.release();
		delete_record(tdbb, rpb, 0, NULL);

		tdbb->bumpRelStats(RuntimeStatistics::RECORD_BACKOUTS, relation->rel_id);
		return;
	}

	// If both record versions are on the same page, things are a little simpler

	samePage = (rpb->rpb_page == temp.rpb_page && !rpb->rpb_prior);
	deleted = (temp2.rpb_flags & rpb_deleted);

	if (!deleted)
	{
		DPM_backout_mark(tdbb, rpb, transaction);

		rpb->rpb_prior = NULL;
		list_staying_fast(tdbb, rpb, staying, &temp);
		IDX_garbage_collect(tdbb, rpb, going, staying);
		BLB_garbage_collect(tdbb, going, staying, rpb->rpb_page, relation);

		if (going.hasData())
			going.pop();

		clearRecordStack(staying);

		if (!DPM_get(tdbb, rpb, LCK_write))
		{
			fb_assert(false);
			return;
		}

		if (rpb->rpb_b_page != temp2.rpb_b_page || rpb->rpb_b_line != temp2.rpb_b_line ||
			rpb->rpb_transaction_nr != temp2.rpb_transaction_nr)
		{
			fb_assert(false);
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			return;
		}

		fb_assert(rpb->rpb_flags & rpb_gc_active);
		rpb->rpb_flags &= ~rpb_gc_active;

		temp2 = *rpb;
		rpb->rpb_undo = old_data;

		if (rpb->rpb_flags & rpb_delta)
			rpb->rpb_prior = data;
	}

	gcLockGuard.release();

	if (samePage)
	{
		DPM_backout(tdbb, rpb);

		if (!deleted)
			delete_tail(tdbb, &temp2, rpb->rpb_page);
	}
	else
	{
		// Bring the old version forward.  If the outgoing version was deleted,
		// there is no garbage collection to be done.

		rpb->rpb_address = temp.rpb_address;
		rpb->rpb_length = temp.rpb_length;
		rpb->rpb_flags = temp.rpb_flags & rpb_deleted;
		if (temp.rpb_prior)
			rpb->rpb_flags |= rpb_delta;
		rpb->rpb_b_page = temp.rpb_b_page;
		rpb->rpb_b_line = temp.rpb_b_line;
		rpb->rpb_transaction_nr = temp.rpb_transaction_nr;
		rpb->rpb_format_number = temp.rpb_format_number;

		if (deleted)
			replace_record(tdbb, rpb, 0, transaction);
		else
		{
			// There is cleanup to be done.  Bring the old version forward first
			DPM_update(tdbb, rpb, 0, transaction);
			delete_tail(tdbb, &temp2, rpb->rpb_page);
		}

		// Next, delete the old copy of the now current version.

		if (!DPM_fetch(tdbb, &temp, LCK_write))
			BUGCHECK(291);		// msg 291 cannot find record back version

		delete_record(tdbb, &temp, rpb->rpb_page, NULL);
	}

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_BACKOUTS, relation->rel_id);
}


bool VIO_chase_record_version(thread_db* tdbb, record_param* rpb,
							  jrd_tra* transaction, MemoryPool* pool,
							  bool writelock, bool noundo)
{
/**************************************
 *
 *	V I O _ c h a s e _ r e c o r d _ v e r s i o n
 *
 **************************************
 *
 * Functional description
 *	This is the key routine in all of JRD.  Given a record, determine
 *	what the version, if any, is appropriate for this transaction.  This
 *	is primarily done by playing with transaction numbers.  If, in the
 *	process, a record is found that requires garbage collection, by all
 *	means garbage collect it.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* const dbb = tdbb->getDatabase();
	Jrd::Attachment* const attachment = transaction->tra_attachment;
	jrd_rel* const relation = rpb->rpb_relation;

	const bool gcPolicyCooperative = dbb->dbb_flags & DBB_gc_cooperative;
	const bool gcPolicyBackground = dbb->dbb_flags & DBB_gc_background;
	const TraNumber oldest_snapshot = relation->isTemporary() ?
		attachment->att_oldest_snapshot : transaction->tra_oldest_active;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE_ALL,
		"VIO_chase_record_version (rel_id %u, record_param %" QUADFORMAT"d, transaction %"
		SQUADFORMAT", pool %p)\n",
		relation->rel_id,
		rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0,
		(void*) pool);

	VIO_trace(DEBUG_TRACE_ALL_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	CommitNumber current_snapshot_number;
	bool int_gc_done = (attachment->att_flags & ATT_no_cleanup);

	int state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr, &current_snapshot_number);

	// Reset (if appropriate) the garbage collect active flag to reattempt the backout

	if (rpb->rpb_flags & rpb_gc_active)
		checkGCActive(tdbb, rpb, state);

	// Take care about modifications performed by our own transaction

	rpb->rpb_runtime_flags &= ~RPB_CLEAR_FLAGS;
	int forceBack = 0;

	if (rpb->rpb_stream_flags & RPB_s_unstable)
		noundo = true;

	if (state == tra_us && !noundo && !(transaction->tra_flags & TRA_system))
	{
		switch (get_undo_data(tdbb, transaction, rpb, pool))
		{
			case udExists:
				return true;
			case udForceBack:
				forceBack = 1;
				break;
			case udForceTwice:
				forceBack = 2;
				break;
			case udNone:
				break;
		}
	}

	if (state == tra_committed)
		state = check_precommitted(transaction, rpb);

	// Handle the fast path first.  If the record is committed, isn't deleted,
	// and doesn't have an old version that is a candidate for garbage collection,
	// return without further ado

	if ((state == tra_committed || state == tra_us) && !forceBack &&
		!(rpb->rpb_flags & (rpb_deleted | rpb_damaged)) &&
		(rpb->rpb_b_page == 0 ||
		  (rpb->rpb_transaction_nr >= oldest_snapshot && !(tdbb->tdbb_flags & TDBB_sweeper))))
	{
		if (gcPolicyBackground && rpb->rpb_b_page)
			notify_garbage_collector(tdbb, rpb);

		return true;
	}

	// OK, something about the record is fishy.  Loop thru versions until a
	// satisfactory version is found or we run into a brick wall.  Do any
	// garbage collection that seems appropriate.

	RuntimeStatistics::Accumulator backversions(tdbb, relation,
												RuntimeStatistics::RECORD_BACKVERSION_READS);

	const bool skipLocked = rpb->rpb_stream_flags & RPB_s_skipLocked;

	if (skipLocked && (state == tra_active || state == tra_limbo))
	{
		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		return false;
	}

	// First, save the record indentifying information to be restored on exit

	while (true)
	{
#ifdef VIO_DEBUG
		VIO_trace(DEBUG_READS_INFO,
			"   chase record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
			", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
			rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
			rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
			rpb->rpb_f_page, rpb->rpb_f_line);
#endif

		if (rpb->rpb_flags & rpb_damaged)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			return false;
		}

		// Worry about intermediate GC if necessary
		if (!int_gc_done &&
			(
			 ((tdbb->tdbb_flags & TDBB_sweeper) && state == tra_committed &&
				rpb->rpb_b_page != 0 && rpb->rpb_transaction_nr >= oldest_snapshot)))
		{
			jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

			int_gc_done = true;
			if (gcGuard.gcEnabled())
			{
				VIO_intermediate_gc(tdbb, rpb, transaction);

				// Go back to be primary record version and chase versions all over again.
				if (!DPM_get(tdbb, rpb, LCK_read))
					return false;

				state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr);
				continue;
			}
		}

		if (state == tra_committed)
			state = check_precommitted(transaction, rpb);

		// If the transaction is a read committed and chooses the no version
		// option, wait for reads also!

		if ((transaction->tra_flags & TRA_read_committed) &&
			!(transaction->tra_flags & TRA_read_consistency) &&
			(!(transaction->tra_flags & TRA_rec_version) || writelock))
		{
			if (state == tra_limbo)
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				state = wait(tdbb, transaction, rpb, false);

				if (!DPM_get(tdbb, rpb, LCK_read))
					return false;

				state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr);

				// will come back with active if lock mode is no wait

				if (state == tra_active)
				{
					// error if we cannot ignore limbo, else fall through
					// to next version

					if (!(transaction->tra_flags & TRA_ignore_limbo))
					{
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
						ERR_post(Arg::Gds(isc_deadlock) << Arg::Gds(isc_trainlim));
					}

					state = tra_limbo;
				}
			}
			else if (state == tra_active && !(rpb->rpb_flags & rpb_gc_active))
			{
				// A read committed, no record version transaction has to wait
				// if the record has been modified by an active transaction. But
				// it shouldn't wait if this is a transient fragmented backout
				// of a dead record version.

				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				state = wait(tdbb, transaction, rpb, false);

				if (state == tra_committed)
					state = check_precommitted(transaction, rpb);

				if (state == tra_active)
				{
					tdbb->bumpRelStats(RuntimeStatistics::RECORD_CONFLICTS, relation->rel_id);

					// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
					ERR_post(Arg::Gds(isc_deadlock) <<
							 Arg::Gds(isc_read_conflict) <<
							 Arg::Gds(isc_concurrent_transaction) << Arg::Int64(rpb->rpb_transaction_nr));
				}

				// refetch the record and try again.  The active transaction
				// could have updated the record a second time.
				// go back to outer loop

				if (!DPM_get(tdbb, rpb, LCK_read))
					return false;

				state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr, &current_snapshot_number);
				continue;
			}
		}

		fb_assert(!forceBack || state == tra_us);
		if (state == tra_us && forceBack)
		{
			state = tra_active;
			forceBack--;
		}

		switch (state)
		{
			// If it's dead, back it out, if possible.  Otherwise continue to chase backward

		case tra_dead:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT") is dead (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, transaction->tra_number);
#endif
			if (gcPolicyBackground && !(rpb->rpb_flags & rpb_chained) &&
				(attachment->att_flags & ATT_notify_gc))
			{
				notify_garbage_collector(tdbb, rpb);
			}

		case tra_precommitted:
			{	// scope
			jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

			if ((attachment->att_flags & ATT_NO_CLEANUP) || !gcGuard.gcEnabled() ||
				(rpb->rpb_flags & (rpb_chained | rpb_gc_active)))
			{
				if (rpb->rpb_b_page == 0)
				{
					CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
					return false;
				}

				record_param temp = *rpb;
				if ((!(rpb->rpb_flags & rpb_deleted)) || (rpb->rpb_flags & rpb_delta))
				{
					VIO_data(tdbb, rpb, pool);
					rpb->rpb_page = temp.rpb_page;
					rpb->rpb_line = temp.rpb_line;

					if (!(DPM_fetch(tdbb, rpb, LCK_read)))
					{
						if (!DPM_get(tdbb, rpb, LCK_read))
							return false;
						break;
					}

					if (rpb->rpb_b_page != temp.rpb_b_page || rpb->rpb_b_line != temp.rpb_b_line ||
						rpb->rpb_f_page != temp.rpb_f_page || rpb->rpb_f_line != temp.rpb_f_line ||
						(rpb->rpb_flags != temp.rpb_flags &&
						 !(state == tra_dead && rpb->rpb_flags == (temp.rpb_flags | rpb_gc_active))))
					{
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

						if (!DPM_get(tdbb, rpb, LCK_read))
							return false;
						break;
					}

					if (temp.rpb_transaction_nr != rpb->rpb_transaction_nr)
						break;

					if (rpb->rpb_b_page == 0)
					{
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
						return false;
					}

					if (rpb->rpb_flags & rpb_delta)
						rpb->rpb_prior = rpb->rpb_record;
				}
				// Fetch a back version.  If a latch timeout occurs, refetch the
				// primary version and start again.  If the primary version is
				// gone, then return 'record not found'.
				if (!DPM_fetch_back(tdbb, rpb, LCK_read, -1))
				{
					if (!DPM_get(tdbb, rpb, LCK_read))
						return false;
				}

				++backversions;
				break;
			}

			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			VIO_backout(tdbb, rpb, transaction);

			if (!DPM_get(tdbb, rpb, LCK_read))
				return false;

			}	// scope
			break;

			// If it's active, prepare to fetch the old version.

		case tra_limbo:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT") is in limbo (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, transaction->tra_number);
#endif

			if (!(transaction->tra_flags & TRA_ignore_limbo))
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

				// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
				ERR_post(Arg::Gds(isc_rec_in_limbo) << Arg::Int64(rpb->rpb_transaction_nr));
			}

		case tra_active:
#ifdef VIO_DEBUG
			if (state == tra_active)
			{
				VIO_trace(DEBUG_READS_INFO,
					"    record's transaction (%" SQUADFORMAT") is active (my TID - %" SQUADFORMAT")\n",
					rpb->rpb_transaction_nr, transaction->tra_number);
			}
#endif
			// we can't use this one so if there aren't any more just stop now.

			if (rpb->rpb_b_page == 0)
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				return false;
			}

			// hvlad: if I'm garbage collector I don't need to read backversion
			// of active record. Just do notify self about it
			if (tdbb->tdbb_flags & TDBB_sweeper)
			{
				if (gcPolicyBackground)
					notify_garbage_collector(tdbb, rpb);

				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				return false;
			}

			if (!(rpb->rpb_flags & rpb_delta))
			{
				rpb->rpb_prior = NULL;

				// Fetch a back version.  If a latch timeout occurs, refetch the
				// primary version and start again.  If the primary version is
				// gone, then return 'record not found'.
				if (!DPM_fetch_back(tdbb, rpb, LCK_read, -1))
				{
					if (!DPM_get(tdbb, rpb, LCK_read))
						return false;
				}

				++backversions;
				break;
			}
			else
			{
				// oh groan, we've got to get data.  This means losing our lock and that
				// means possibly having the world change underneath us.  Specifically, the
				// primary record may change (because somebody modified or backed it out) and
				// the first record back may disappear because the primary record was backed
				// out, and now the first backup back in the primary record's place.

				record_param temp = *rpb;
				VIO_data(tdbb, rpb, pool);
				if (temp.rpb_flags & rpb_chained)
				{
					rpb->rpb_page = temp.rpb_b_page;
					rpb->rpb_line = temp.rpb_b_line;
					if (!DPM_fetch(tdbb, rpb, LCK_read))
					{
						// Things have changed, start all over again.
						if (!DPM_get(tdbb, rpb, LCK_read))
							return false;	// entire record disappeared
						break;	// start from the primary version again
					}
				}
				else
				{
					rpb->rpb_page = temp.rpb_page;
					rpb->rpb_line = temp.rpb_line;
					if (!DPM_fetch(tdbb, rpb, LCK_read))
					{
						// Things have changed, start all over again.
						if (!DPM_get(tdbb, rpb, LCK_read))
							return false;	// entire record disappeared
						break;	// start from the primary version again
					}

					if (rpb->rpb_transaction_nr != temp.rpb_transaction_nr)
					{
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
						if (!DPM_get(tdbb, rpb, LCK_read))
							return false;
						break;
					}

					if (rpb->rpb_b_page == 0)
					{
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
						return false;
					}

					if (!(rpb->rpb_flags & rpb_delta))
						rpb->rpb_prior = NULL;

					// Fetch a back version.  If a latch timeout occurs, refetch the
					// primary version and start again.  If the primary version is
					// gone, then return 'record not found'.
					if (!DPM_fetch_back(tdbb, rpb, LCK_read, -1))
					{
						if (!DPM_get(tdbb, rpb, LCK_read))
							return false;
					}

					++backversions;
				}
			}
			break;

		case tra_us:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT") is us (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, transaction->tra_number);
#endif

			if (!noundo && !(rpb->rpb_flags & rpb_chained) && !(transaction->tra_flags & TRA_system))
			{
				fb_assert(forceBack == 0);
				forceBack = 0;
				switch (get_undo_data(tdbb, transaction, rpb, pool))
				{
					case udExists:
						return true;
					case udForceBack:
						forceBack = 1;
						break;
					case udForceTwice:
						forceBack = 2;
						break;
					case udNone:
						break;
				}

				if (forceBack)
					break;
			}

			if (rpb->rpb_flags & rpb_deleted)
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				return false;
			}
			return true;

			// If it's committed, worry a bit about garbage collection.

		case tra_committed:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT") is committed (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, transaction->tra_number);
#endif
			if (rpb->rpb_flags & rpb_deleted)
			{
				if (rpb->rpb_transaction_nr < oldest_snapshot &&
					!(attachment->att_flags & ATT_no_cleanup))
				{
					if (!gcPolicyCooperative && (attachment->att_flags & ATT_notify_gc) &&
						!rpb->rpb_relation->isTemporary())
					{
						notify_garbage_collector(tdbb, rpb);
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
					}
					else
					{
						CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

						jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

						if (!gcGuard.gcEnabled())
							return false;

						expunge(tdbb, rpb, transaction, 0);
					}

					return false;
				}

				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				return false;
			}

			// Check if no garbage collection can (should) be done.
			// It might be important not to garbage collect if the primary
			// record version is not yet committed because garbage collection
			// might interfere with the updater (prepare_update, update_in_place...).
			// That might be the reason for the rpb_chained check.

			const bool cannotGC =
				rpb->rpb_transaction_nr >= oldest_snapshot || rpb->rpb_b_page == 0 ||
				(rpb->rpb_flags & rpb_chained) || (attachment->att_flags & ATT_no_cleanup);

			if (cannotGC)
			{
				if (gcPolicyBackground &&
					(attachment->att_flags & (ATT_notify_gc | ATT_garbage_collector)) &&
					rpb->rpb_b_page != 0 && !(rpb->rpb_flags & rpb_chained) )
				{
					// VIO_chase_record_version
					notify_garbage_collector(tdbb, rpb);
				}

				return true;
			}

			// Garbage collect.

			if (!gcPolicyCooperative && (attachment->att_flags & ATT_notify_gc) &&
				!rpb->rpb_relation->isTemporary())
			{
				notify_garbage_collector(tdbb, rpb);
				return true;
			}

			{ // scope
				jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

				if (!gcGuard.gcEnabled())
					return true;

				purge(tdbb, rpb);
			}

			// Go back to be primary record version and chase versions all over again.
			if (!DPM_get(tdbb, rpb, LCK_read))
				return false;
		} // switch (state)

		state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr, &current_snapshot_number);

		// Reset (if appropriate) the garbage collect active flag to reattempt the backout

		if (!(rpb->rpb_flags & rpb_chained) && (rpb->rpb_flags & rpb_gc_active))
			checkGCActive(tdbb, rpb, state);
	}
}


void VIO_copy_record(thread_db* tdbb, jrd_rel* relation, Record* orgRecord, Record* newRecord)
{
/**************************************
 *
 *	V I O _ c o p y _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Copy the given record to a new destination,
 *	taking care about possible format differences.
 **************************************/
	// dimitr:	Clear the req_null flag that may stay active after the last
	//			boolean evaluation. Here we use only EVL_field() calls that
	//			do not touch this flag and data copying is done only for
	//			non-NULL fields, so req_null should never be seen inside blb::move().
	//			See CORE-6090 for details.

	const auto request = tdbb->getRequest();
	request->req_flags &= ~req_null;

	const auto orgFormat = orgRecord->getFormat();
	const auto newFormat = newRecord->getFormat();
	fb_assert(orgFormat && newFormat);

	// Copy the original record to the new record. If the format hasn't changed,
	// this is a simple move. If the format has changed, each field must be
	// fetched and moved separately, remembering to set the missing flag.

	if (newFormat->fmt_version == orgFormat->fmt_version)
	{
		newRecord->copyDataFrom(orgRecord, true);
		return;
	}

	dsc orgDesc, newDesc;

	for (USHORT i = 0; i < newFormat->fmt_count; i++)
	{
		newRecord->clearNull(i);

		if (EVL_field(relation, newRecord, i, &newDesc))
		{
			if (EVL_field(relation, orgRecord, i, &orgDesc))
			{
				// If the source is not a blob or it's a temporary blob,
				// then we'll need to materialize the resulting blob.
				// Thus blb::move() is called with rpb and field ID.
				// See also CORE-5600.

				const bool materialize =
					(DTYPE_IS_BLOB_OR_QUAD(newDesc.dsc_dtype) &&
						!(DTYPE_IS_BLOB_OR_QUAD(orgDesc.dsc_dtype) &&
							((bid*) orgDesc.dsc_address)->bid_internal.bid_relation_id));

				if (materialize)
					blb::move(tdbb, &orgDesc, &newDesc, relation, newRecord, i);
				else
					MOV_move(tdbb, &orgDesc, &newDesc);
			}
			else
			{
				newRecord->setNull(i);

				if (!newDesc.isUnknown())
					memset(newDesc.dsc_address, 0, newDesc.dsc_length);
			}
		}
	}
}


void VIO_data(thread_db* tdbb, record_param* rpb, MemoryPool* pool)
{
/**************************************
 *
 *	V I O _ d a t a
 *
 **************************************
 *
 * Functional description
 *	Given an active record parameter block, fetch the full record.
 *
 *	This routine is called with an active record_param and exits with
 *	an INactive record_param.  Yes, Virginia, getting the data for a
 *	record means losing control of the record.  This turns out
 *	to matter a lot.
 **************************************/
	SET_TDBB(tdbb);

	jrd_rel* const relation = rpb->rpb_relation;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_READS,
		"VIO_data (rel_id %u, record_param %" QUADFORMAT"d, pool %p)\n",
		relation->rel_id, rpb->rpb_number.getValue(), (void*)pool);


	VIO_trace(DEBUG_READS_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line,
		rpb->rpb_transaction_nr, rpb->rpb_flags,
		rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	// If we're not already set up for this format version number, find
	// the format block and set up the record block.  This is a performance
	// optimization.

	Record* const record = VIO_record(tdbb, rpb, NULL, pool);
	const Format* const format = record->getFormat();

	record->setTransactionNumber(rpb->rpb_transaction_nr);

	// If the record is a delta version, start with data from prior record.
	UCHAR* tail;
	const UCHAR* tail_end;

	Difference difference;

	// Primary record version not uses prior version
	Record* prior = (rpb->rpb_flags & rpb_chained) ? rpb->rpb_prior : nullptr;

	if (prior)
	{
		tail = difference.getData();
		tail_end = tail + difference.getCapacity();

		if (prior != record)
			record->copyDataFrom(prior);
	}
	else
	{
		tail = record->getData();
		tail_end = tail + record->getLength();
	}

	// Set up prior record point for next version

	rpb->rpb_prior = (rpb->rpb_b_page && (rpb->rpb_flags & rpb_delta)) ? record : NULL;

	// Snarf data from record

	tail = unpack(rpb, tail_end - tail, tail);

	RuntimeStatistics::Accumulator fragments(tdbb, relation, RuntimeStatistics::RECORD_FRAGMENT_READS);

	if (rpb->rpb_flags & rpb_incomplete)
	{
		const ULONG back_page  = rpb->rpb_b_page;
		const USHORT back_line = rpb->rpb_b_line;
		const USHORT save_flags = rpb->rpb_flags;
		const ULONG save_f_page = rpb->rpb_f_page;
		const USHORT save_f_line = rpb->rpb_f_line;

		while (rpb->rpb_flags & rpb_incomplete)
		{
			DPM_fetch_fragment(tdbb, rpb, LCK_read);
			tail = unpack(rpb, tail_end - tail, tail);
			++fragments;
		}

		rpb->rpb_b_page = back_page;
		rpb->rpb_b_line = back_line;
		rpb->rpb_flags = save_flags;
		rpb->rpb_f_page = save_f_page;
		rpb->rpb_f_line = save_f_line;
	}

	CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

	// If this is a delta version, apply changes
	ULONG length;
	if (prior)
	{
		const auto diffLength = tail - difference.getData();
		length = difference.apply(diffLength, record->getLength(), record->getData());
	}
	else
	{
		length = tail - record->getData();
	}

	if (format->fmt_length != length)
	{
#ifdef VIO_DEBUG
		VIO_trace(DEBUG_WRITES,
			"VIO_data (record_param %" QUADFORMAT"d, length %d expected %d)\n",
			rpb->rpb_number.getValue(), length, format->fmt_length);

		VIO_trace(DEBUG_WRITES_INFO,
			"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
			"d, flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
			rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr, rpb->rpb_flags,
			rpb->rpb_b_page, rpb->rpb_b_line, rpb->rpb_f_page, rpb->rpb_f_line);
#endif
		BUGCHECK(183);			// msg 183 wrong record length
	}

	rpb->rpb_address = record->getData();
	rpb->rpb_length = format->fmt_length;
}


static bool check_prepare_result(PrepareResult prepare_result, jrd_tra* transaction,
	Request* request, record_param* rpb)
{
/**************************************
 *
 *	c h e c k _ p r e p a r e _ r e s u l t
 *
 **************************************
 *
 * Functional description
 *	Called by VIO_modify and VIO_erase. Raise update conflict error if not in
 *  read consistency transaction or lock error happens or if request is already
 *  in update conflict mode. In latter case set TRA_ex_restart flag to correctly
 *  handle request restart.
 *	If record should be skipped, return false also.
 *
 **************************************/
	if (prepare_result == PrepareResult::SUCCESS)
		return true;

	if ((rpb->rpb_stream_flags & RPB_s_skipLocked) && prepare_result == PrepareResult::SKIP_LOCKED)
		return false;

	fb_assert(prepare_result != PrepareResult::SKIP_LOCKED);

	Request* top_request = request->req_snapshot.m_owner;

	const bool restart_ready = top_request &&
		(top_request->req_flags & req_restart_ready);

	// Second update conflict when request is already in update conflict mode
	// means we have some (indirect) UPDATE\DELETE in WHERE clause of primary
	// cursor. In this case all we can do is restart whole request immediately.
	const bool secondary = top_request &&
		(top_request->req_flags & req_update_conflict) &&
		(prepare_result != PrepareResult::LOCK_ERROR);

	if (!(transaction->tra_flags & TRA_read_consistency) || prepare_result == PrepareResult::LOCK_ERROR ||
		secondary || !restart_ready)
	{
		if (secondary)
			transaction->tra_flags |= TRA_ex_restart;

		ERR_post(Arg::Gds(isc_deadlock) <<
			Arg::Gds(isc_update_conflict) <<
			Arg::Gds(isc_concurrent_transaction) << Arg::Int64(rpb->rpb_transaction_nr));
	}

	if (top_request)
	{
		top_request->req_flags |= req_update_conflict;
		top_request->req_conflict_txn = rpb->rpb_transaction_nr;
	}
	return false;
}


bool VIO_erase(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ e r a s e
 *
 **************************************
 *
 * Functional description
 *	Erase an existing record.
 *
 *	This routine is entered with an inactive
 *	record_param and leaves having created an erased
 *	stub.
 *
 **************************************/
	QualifiedName object_name;

	SET_TDBB(tdbb);
	Request* request = tdbb->getRequest();
	jrd_rel* relation = rpb->rpb_relation;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES,
		"VIO_erase (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT")\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction->tra_number);

	VIO_trace(DEBUG_WRITES_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);

#endif

	// If the stream was sorted, the various fields in the rpb are
	// probably junk.  Just to make sure that everything is cool, refetch the record.

	if (rpb->rpb_runtime_flags & (RPB_refetch | RPB_undo_read))
	{
		VIO_refetch_record(tdbb, rpb, transaction, false, true);
		rpb->rpb_runtime_flags &= ~RPB_refetch;
		fb_assert(!(rpb->rpb_runtime_flags & RPB_undo_read));
	}

	// Special case system transaction

	if (transaction->tra_flags & TRA_system)
	{
		// hvlad: what if record was created\modified by user tx also,
		// i.e. if there is backversion ???
		VIO_backout(tdbb, rpb, transaction);
		return true;
	}

	transaction->tra_flags |= TRA_write;

	check_gbak_cheating_delete(tdbb, relation);

	// If we're about to erase a system relation, check to make sure
	// everything is completely kosher.

	DSC desc, desc2, schemaDesc;

	if (needDfw(tdbb, transaction))
	{
		jrd_rel* r2;
		const jrd_prc* procedure;
		USHORT id;
		DeferredWork* work;

		switch ((RIDS) relation->rel_id)
		{
		case rel_database:
		case rel_log:
		case rel_global_auth_mapping:
			protect_system_table_delupd(tdbb, relation, "DELETE", true);
			break;

		case rel_schemas:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			break;

		case rel_types:
		 	if (!tdbb->getAttachment()->locksmith(tdbb, CREATE_USER_TYPES))
		 		protect_system_table_delupd(tdbb, relation, "DELETE", true);
		 	if (EVL_field(0, rpb->rpb_record, f_typ_sys_flag, &desc) && MOV_get_long(tdbb, &desc, 0))
		 		protect_system_table_delupd(tdbb, relation, "DELETE", true);
			break;

		case rel_db_creators:
			if (!tdbb->getAttachment()->locksmith(tdbb, GRANT_REVOKE_ANY_DDL_RIGHT))
				protect_system_table_delupd(tdbb, relation, "DELETE");
			break;

		case rel_pages:
		case rel_formats:
		case rel_trans:
		case rel_refc:
		case rel_ccon:
		case rel_msgs:
		case rel_roles:
		case rel_sec_users:
		case rel_sec_user_attributes:
		case rel_auth_mapping:
		case rel_dpds:
		case rel_dims:
		case rel_filters:
		case rel_vrel:
		case rel_args:
		case rel_packages:
		case rel_charsets:
		case rel_pubs:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			break;

		case rel_relations:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			if (EVL_field(0, rpb->rpb_record, f_rel_id, &desc2))
			{
				id = MOV_get_long(tdbb, &desc2, 0);
				if (id < (int) rel_MAX)
				{
					IBERROR(187);	// msg 187 cannot delete system relations
				}
				EVL_field(0, rpb->rpb_record, f_rel_name, &desc);
				EVL_field(0, rpb->rpb_record, f_rel_schema, &schemaDesc);
				DFW_post_work(transaction, dfw_delete_relation, &desc, &schemaDesc, id);
				jrd_rel* rel_drop = MET_lookup_relation_id(tdbb, id, false);
				if (rel_drop)
					MET_scan_relation(tdbb, rel_drop);
			}
			break;

		case rel_procedures:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_prc_id, &desc2);
			id = MOV_get_long(tdbb, &desc2, 0);

			if (EVL_field(0, rpb->rpb_record, f_prc_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			EVL_field(0, rpb->rpb_record, f_prc_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_prc_name, &desc);

			DFW_post_work(transaction, dfw_delete_procedure, &desc, &schemaDesc, id, object_name.package);
			MET_lookup_procedure_id(tdbb, id, false, true, 0);
			break;

		case rel_collations:
			protect_system_table_delupd(tdbb, relation, "DELETE");

			EVL_field(0, rpb->rpb_record, f_coll_schema, &schemaDesc);

			EVL_field(0, rpb->rpb_record, f_coll_cs_id, &desc2);
			id = MOV_get_long(tdbb, &desc2, 0);

			EVL_field(0, rpb->rpb_record, f_coll_id, &desc2);
			id = INTL_CS_COLL_TO_TTYPE(id, MOV_get_long(tdbb, &desc2, 0));

			EVL_field(0, rpb->rpb_record, f_coll_name, &desc);
			DFW_post_work(transaction, dfw_delete_collation, &desc, &schemaDesc, id);
			break;

		case rel_exceptions:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_xcp_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_xcp_name, &desc);
			DFW_post_work(transaction, dfw_delete_exception, &desc, &schemaDesc, 0);
			break;

		case rel_gens:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_gen_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_gen_name, &desc);
			DFW_post_work(transaction, dfw_delete_generator, &desc, &schemaDesc, 0);
			break;

		case rel_funs:
			protect_system_table_delupd(tdbb, relation, "DELETE");

			EVL_field(0, rpb->rpb_record, f_fun_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_fun_name, &desc);

			if (EVL_field(0, rpb->rpb_record, f_fun_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			EVL_field(0, rpb->rpb_record, f_fun_id, &desc2);
			id = MOV_get_long(tdbb, &desc2, 0);

			DFW_post_work(transaction, dfw_delete_function, &desc, &schemaDesc, id, object_name.package);
			Function::lookup(tdbb, id, false, true, 0);
			break;

		case rel_indices:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_idx_relation, &desc);
			EVL_field(0, rpb->rpb_record, f_idx_id, &desc2);
			if ( (id = MOV_get_long(tdbb, &desc2, 0)) )
			{
				QualifiedName relation_name;

				EVL_field(0, rpb->rpb_record, f_idx_schema, &schemaDesc);
				MOV_get_metaname(tdbb, &schemaDesc, relation_name.schema);

				MOV_get_metaname(tdbb, &desc, relation_name.object);
				r2 = MET_lookup_relation(tdbb, relation_name);
				fb_assert(r2);

				DSC idx_name;
				EVL_field(0, rpb->rpb_record, f_idx_name, &idx_name);

				// hvlad: lets add index name to the DFW item even if we add it again later within
				// additional argument. This is needed to make DFW work items different for different
				// indexes dropped at the same transaction and to not merge them at DFW_merge_work.
				work = DFW_post_work(transaction, dfw_delete_index, &idx_name, &schemaDesc, r2->rel_id);

				// add index id and name (the latter is required to delete dependencies correctly)
				DFW_post_work_arg(transaction, work, &idx_name, &schemaDesc, id, dfw_arg_index_name);

				// get partner relation for FK index
				if (EVL_field(0, rpb->rpb_record, f_idx_foreign, &desc2))
				{
					DSC desc3;
					EVL_field(0, rpb->rpb_record, f_idx_name, &desc3);

					QualifiedName index_name;
					MOV_get_metaname(tdbb, &schemaDesc, index_name.schema);
					MOV_get_metaname(tdbb, &desc3, index_name.object);

					jrd_rel* partner;
					index_desc idx;

					if ((BTR_lookup(tdbb, r2, id - 1, &idx, r2->getBasePages())) &&
						MET_lookup_partner(tdbb, r2, &idx, index_name) &&
						(partner = MET_lookup_relation_id(tdbb, idx.idx_primary_relation, false)) )
					{
						DFW_post_work_arg(transaction, work, nullptr, nullptr, partner->rel_id,
										  dfw_arg_partner_rel_id);
					}
					else
					{
						// can't find partner relation - impossible ?
						// add empty argument to let DFW know dropping
						// index was bound with FK
						DFW_post_work_arg(transaction, work, nullptr, nullptr, 0, dfw_arg_partner_rel_id);
					}
				}
			}
			break;

		case rel_rfr:
			protect_system_table_delupd(tdbb, relation, "DELETE");

			EVL_field(0, rpb->rpb_record, f_rfr_schema, &schemaDesc);
			MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);

			EVL_field(0, rpb->rpb_record, f_rfr_rname, &desc);
			DFW_post_work(transaction, dfw_update_format, &desc, &schemaDesc, 0);

			EVL_field(0, rpb->rpb_record, f_rfr_fname, &desc2);
			MOV_get_metaname(tdbb, &desc, object_name.object);

			if ( (r2 = MET_lookup_relation(tdbb, object_name)) )
				DFW_post_work(transaction, dfw_delete_rfr, &desc2, &schemaDesc, r2->rel_id);

			EVL_field(0, rpb->rpb_record, f_rfr_field_source_schema, &schemaDesc);
			MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);

			EVL_field(0, rpb->rpb_record, f_rfr_sname, &desc2);
			MOV_get_metaname(tdbb, &desc2, object_name.object);

			if (fb_utils::implicit_domain(object_name.object.c_str()))
				DFW_post_work(transaction, dfw_delete_global, &desc2, &schemaDesc, 0);

			break;

		case rel_prc_prms:
			protect_system_table_delupd(tdbb, relation, "DELETE");

			EVL_field(0, rpb->rpb_record, f_prm_schema, &schemaDesc);
			MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);

			EVL_field(0, rpb->rpb_record, f_prm_procedure, &desc);
			MOV_get_metaname(tdbb, &desc, object_name.object);

			if (EVL_field(0, rpb->rpb_record, f_prm_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			EVL_field(0, rpb->rpb_record, f_prm_name, &desc2);

			if ((procedure = MET_lookup_procedure(tdbb, object_name, true)))
			{
				work = DFW_post_work(transaction, dfw_delete_prm, &desc2, &schemaDesc, procedure->getId(),
					object_name.package);

				// procedure name to track parameter dependencies
				DFW_post_work_arg(transaction, work, &desc, &schemaDesc, procedure->getId(), dfw_arg_proc_name);
			}

			if (!EVL_field(0, rpb->rpb_record, f_prm_fname, &desc2))
			{
				EVL_field(0, rpb->rpb_record, f_prm_field_source_schema, &schemaDesc);
				EVL_field(0, rpb->rpb_record, f_prm_sname, &desc2);
				DFW_post_work(transaction, dfw_delete_global, &desc2, &schemaDesc, 0);
			}

			break;

		case rel_fields:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_fld_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_fld_name, &desc);
			DFW_post_work(transaction, dfw_delete_field, &desc, &schemaDesc, 0);
			MET_change_fields(tdbb, transaction, &schemaDesc, &desc);
			break;

		case rel_files:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			{
				const bool nameDefined = EVL_field(0, rpb->rpb_record, f_file_name, &desc);

				const auto shadowNumber = EVL_field(0, rpb->rpb_record, f_file_shad_num, &desc2) ?
					MOV_get_long(tdbb, &desc2, 0) : 0;

				const auto fileFlags = EVL_field(0, rpb->rpb_record, f_file_flags, &desc2) ?
					MOV_get_long(tdbb, &desc2, 0) : 0;

				if (shadowNumber)
				{
					if (!(fileFlags & FILE_inactive))
					{
						const auto work = (fileFlags & FILE_nodelete) ?
							dfw_delete_shadow_nodelete : dfw_delete_shadow;

						DFW_post_work(transaction, work, &desc, nullptr, shadowNumber);
					}
				}
				else if (fileFlags & FILE_difference)
				{
					if (fileFlags & FILE_backing_up)
						DFW_post_work(transaction, dfw_end_backup, &desc, nullptr, 0);

					if (nameDefined)
						DFW_post_work(transaction, dfw_delete_difference, &desc, nullptr, 0);
				}
			}
			break;

		case rel_classes:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_cls_class, &desc);
			DFW_post_work(transaction, dfw_compute_security, &desc, nullptr, 0);
			break;

		case rel_triggers:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_trg_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_trg_rname, &desc2);
			DFW_post_work(transaction, dfw_update_format, &desc2, &schemaDesc, 0);
			EVL_field(0, rpb->rpb_record, f_trg_name, &desc);
			work = DFW_post_work(transaction, dfw_delete_trigger, &desc, &schemaDesc, 0);

			if (!(desc2.dsc_flags & DSC_null))
				DFW_post_work_arg(transaction, work, &desc2, &schemaDesc, 0, dfw_arg_rel_name);

			if (EVL_field(0, rpb->rpb_record, f_trg_type, &desc2))
			{
				DFW_post_work_arg(transaction, work, &desc2, &schemaDesc,
					(USHORT) MOV_get_int64(tdbb, &desc2, 0), dfw_arg_trg_type);
			}

			break;

		case rel_priv:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			EVL_field(0, rpb->rpb_record, f_file_name, &desc);
			if (!tdbb->getRequest()->hasInternalStatement())
			{
				EVL_field(0, rpb->rpb_record, f_prv_grantor, &desc);
				MetaName grantor;
				MOV_get_metaname(tdbb, &desc, grantor);

				const auto attachment = tdbb->getAttachment();
				const MetaString& currentUser = attachment->getUserName();

				if (grantor != currentUser)
				{
					ERR_post(Arg::Gds(isc_no_priv) << Arg::Str("REVOKE") <<
													  Arg::Str("TABLE") <<
													  Arg::Str("RDB$USER_PRIVILEGES"));
				}
			}
			EVL_field(0, rpb->rpb_record, f_prv_rname, &desc);
			EVL_field(0, rpb->rpb_record, f_prv_o_type, &desc2);
			id = MOV_get_long(tdbb, &desc2, 0);

			if (EVL_field(0, rpb->rpb_record, f_prv_rel_schema, &schemaDesc))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, id);
			else
				DFW_post_work(transaction, dfw_grant, &desc, nullptr, id);
			break;

		case rel_rcon:
			protect_system_table_delupd(tdbb, relation, "DELETE");

			// ensure relation partners is known
			EVL_field(0, rpb->rpb_record, f_rcon_rname, &desc);

			{
				QualifiedName relation_name;

				EVL_field(0, rpb->rpb_record, f_rcon_schema, &schemaDesc);
				MOV_get_metaname(tdbb, &schemaDesc, relation_name.schema);

				MOV_get_metaname(tdbb, &desc, relation_name.object);
				r2 = MET_lookup_relation(tdbb, relation_name);
				fb_assert(r2);

				if (r2)
					MET_scan_partners(tdbb, r2);
			}

			break;

		case rel_backup_history:
			if (!tdbb->getAttachment()->locksmith(tdbb, USE_NBACKUP_UTILITY))
				protect_system_table_delupd(tdbb, relation, "DELETE", true);
			break;

		case rel_pub_tables:
			protect_system_table_delupd(tdbb, relation, "DELETE");
			DFW_post_work(transaction, dfw_change_repl_state, {}, {}, 1);
			break;

		default:    // Shut up compiler warnings
			break;
		}
	}

	// We're about to erase the record. Post a refetch request
	// to all the active cursors positioned at this record.

	invalidate_cursor_records(transaction, rpb);

	// If the page can be updated simply, we can skip the remaining crud

	Database* dbb = tdbb->getDatabase();
	const bool backVersion = (rpb->rpb_b_page != 0);

	record_param temp;
	temp.rpb_transaction_nr = transaction->tra_number;
	temp.rpb_address = NULL;
	temp.rpb_length = 0;
	temp.rpb_flags = rpb_deleted;
	temp.rpb_format_number = rpb->rpb_format_number;
	temp.getWindow(tdbb).win_flags = WIN_secondary;

	if (rpb->rpb_transaction_nr == transaction->tra_number)
	{
		VIO_update_in_place(tdbb, transaction, rpb, &temp);

		if (transaction->tra_save_point && transaction->tra_save_point->isChanging())
			verb_post(tdbb, transaction, rpb, rpb->rpb_undo);

		// We have INSERT + DELETE or UPDATE + DELETE in the same transaction.
		// UPDATE has already notified GC, while INSERT has not. Check for
		// backversion allows to avoid second notification in case of UPDATE.

		if ((dbb->dbb_flags & DBB_gc_background) && !rpb->rpb_relation->isTemporary() && !backVersion)
			notify_garbage_collector(tdbb, rpb, transaction->tra_number);

		tdbb->bumpRelStats(RuntimeStatistics::RECORD_DELETES, relation->rel_id);
		return true;
	}

	const TraNumber tid_fetch = rpb->rpb_transaction_nr;
	if (DPM_chain(tdbb, rpb, &temp))
	{
		rpb->rpb_b_page = temp.rpb_b_page;
		rpb->rpb_b_line = temp.rpb_b_line;
		rpb->rpb_flags |= rpb_deleted;
#ifdef VIO_DEBUG
		VIO_trace(DEBUG_WRITES_INFO,
			"   VIO_erase: successfully chained\n");
#endif
	}
	else
	{
		// Update stub didn't find one page -- do a long, hard update
		PageStack stack;
		const auto prepare_result = prepare_update(tdbb, transaction, tid_fetch, rpb, &temp, 0, stack, false);
		if (!check_prepare_result(prepare_result, transaction, request, rpb))
			return false;

		// Old record was restored and re-fetched for write.  Now replace it.

		rpb->rpb_transaction_nr = transaction->tra_number;
		rpb->rpb_b_page = temp.rpb_page;
		rpb->rpb_b_line = temp.rpb_line;
		rpb->rpb_address = NULL;
		rpb->rpb_length = 0;
		rpb->rpb_flags |= rpb_deleted;
		rpb->rpb_flags &= ~rpb_delta;

		replace_record(tdbb, rpb, &stack, transaction);
	}

	// Check to see if recursive revoke needs to be propagated

	if ((RIDS) relation->rel_id == rel_priv)
	{
		if (EVL_field(0, rpb->rpb_record, f_prv_rel_schema, &desc))
			MOV_get_metaname(tdbb, &desc, object_name.schema);

		EVL_field(0, rpb->rpb_record, f_prv_rname, &desc);
		MOV_get_metaname(tdbb, &desc, object_name.object);

		EVL_field(0, rpb->rpb_record, f_prv_grant, &desc2);

		if (MOV_get_long(tdbb, &desc2, 0) == WITH_GRANT_OPTION)		// ADMIN option should not cause cascade
		{
			QualifiedName revokee;

			if (EVL_field(0, rpb->rpb_record, f_prv_user_schema, &desc2))
				MOV_get_metaname(tdbb, &desc2, revokee.schema);

			EVL_field(0, rpb->rpb_record, f_prv_user, &desc2);
			MOV_get_metaname(tdbb, &desc2, revokee.object);

			EVL_field(0, rpb->rpb_record, f_prv_priv, &desc2);
			const string privilege = MOV_make_string2(tdbb, &desc2, ttype_ascii);

			MET_revoke(tdbb, transaction, object_name, revokee, privilege);
		}
	}

	if (transaction->tra_save_point && transaction->tra_save_point->isChanging())
		verb_post(tdbb, transaction, rpb, 0);

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_DELETES, relation->rel_id);

	// for an autocommit transaction, mark a commit as necessary

	if (transaction->tra_flags & TRA_autocommit)
		transaction->tra_flags |= TRA_perform_autocommit;

	// VIO_erase

	if (backVersion && !(tdbb->getAttachment()->att_flags & ATT_no_cleanup) &&
		(dbb->dbb_flags & DBB_gc_cooperative))
	{
		jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);
		if (gcGuard.gcEnabled())
		{
			temp = *rpb;
			if (DPM_get(tdbb, &temp, LCK_read))
				VIO_intermediate_gc(tdbb, &temp, transaction);
		}
	}
	else if ((dbb->dbb_flags & DBB_gc_background) && !rpb->rpb_relation->isTemporary())
	{
		notify_garbage_collector(tdbb, rpb, transaction->tra_number);
	}
	return true;
}


void VIO_fini(thread_db* tdbb)
{
/**************************************
 *
 *	V I O _ f i n i
 *
 **************************************
 *
 * Functional description
 *	Shutdown the garbage collector thread.
 *
 **************************************/
	Database* dbb = tdbb->getDatabase();

	if (dbb->dbb_flags & DBB_garbage_collector)
	{
		dbb->dbb_flags &= ~DBB_garbage_collector;
		dbb->dbb_gc_sem.release(); // Wake up running thread
		dbb->dbb_gc_fini.waitForCompletion();
	}
}

static void delete_version_chain(thread_db* tdbb, record_param* rpb, bool delete_head)
{
/**************************************
 *
 *	d e l e t e _ v e r s i o n _ c h a i n
 *
 **************************************
 *
 * Functional description
 *	Delete a chain of back record versions.  This is called from
 *	VIO_intermediate_gc.  One enters this routine with an
 *	inactive record_param for a version chain that has
 *	1) just been created and never attached to primary version or
 *     (delete_head = true)
 *	2) just had been replaced with another version chain
 *     (delete_head = false)
 *	Therefore we can do a fetch on the back pointers we've got
 *	because we have the last existing copy of them.
 *
 **************************************/
#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE,
		"delete_version_chain (record_param %" SQUADFORMAT", delete_head %s)\n",
		rpb->rpb_number.getValue(), delete_head ? "true" : "false");

	VIO_trace(DEBUG_TRACE_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	// It's possible to get rpb_page == 0 from VIO_intermediate_gc via
	// staying_chain_rpb. This case happens there when the staying record
	// stack has 1 item at the moment this rpb is created. So return to
	// avoid an error on DPM_fetch below.
	if (!rpb->rpb_page)
		return;

	ULONG prior_page = 0;

	// Note that the page number of the oldest version in the chain should
	// be stored in rpb->rpb_page before exiting this function because
	// VIO_intermediate_gc will use it as a prior page number.
	while (rpb->rpb_b_page != 0 || delete_head)
	{
		if (!delete_head)
		{
			prior_page = rpb->rpb_page;
			rpb->rpb_page = rpb->rpb_b_page;
			rpb->rpb_line = rpb->rpb_b_line;
		}
		else
			delete_head = false;

		if (!DPM_fetch(tdbb, rpb, LCK_write))
			BUGCHECK(291);		// msg 291 cannot find record back version

		record_param temp_rpb = *rpb;
		DPM_delete(tdbb, &temp_rpb, prior_page);
		delete_tail(tdbb, &temp_rpb, temp_rpb.rpb_page);
	}
}


void VIO_intermediate_gc(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ i n t e r m e d i a t e _ g c
 *
 **************************************
 *
 * Functional description
 *  Garbage-collect committed versions that are not needed for anyone.
 *  This function shall be called with active RPB, and exits with inactive RPB
 *
 **************************************/
	Database *dbb = tdbb->getDatabase();
	Attachment* att = tdbb->getAttachment();

	// If current record is not a primary version, release it and fetch primary version
	if (rpb->rpb_flags & rpb_chained)
	{
		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		if (!DPM_get(tdbb, rpb, LCK_read))
			return;
	}

	// If head version is being backed out - do not interfere with this process
	if (rpb->rpb_flags & rpb_gc_active)
	{
		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		return;
	}

	// Read data for all versions of a record
	RecordStack staying, going;
	list_staying(tdbb, rpb, staying, LS_ACTIVE_RPB | LS_NO_RESTART);

	// We can only garbage collect here if there is more than one version of a record
	if (!staying.hasMore(1))
	{
		clearRecordStack(staying);
		return;
	}

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE,
		"VIO_intermediate_gc (record_param %" SQUADFORMAT", transaction %"
		SQUADFORMAT")\n",
		rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0);

	VIO_trace(DEBUG_TRACE_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	TipCache* tipCache = dbb->dbb_tip_cache;
	tipCache->updateActiveSnapshots(tdbb, &att->att_active_snapshots);

	// Determine what records need to stay and which need to go.
	// For that we iterate all records in newest->oldest order (natural order is oldest->newest)
	// and move unnecessary records from staying into going stack.
	CommitNumber current_snapshot_number = 0, prev_snapshot_number;
	RecordStack::reverse_iterator rev_i(staying);
	while (rev_i.hasData())
	{
		prev_snapshot_number = current_snapshot_number;

		Record *record = rev_i.object();
		CommitNumber cn = tipCache->cacheState(record->getTransactionNumber());
		if (cn >= CN_PREHISTORIC && cn <= CN_MAX_NUMBER)
			current_snapshot_number = att->att_active_snapshots.getSnapshotForVersion(cn);
		else
			current_snapshot_number = 0;

		if (current_snapshot_number && current_snapshot_number == prev_snapshot_number)
		{
			going.push(record);
			rev_i.remove();
		}
		else
			++rev_i;
	}

	// If there is no garbage to collect - leave now
	if (!going.hasData())
	{
		clearRecordStack(staying);
		return;
	}

	// Delta-compress and store new versions chain for staying records (iterate oldest->newest)
	record_param staying_chain_rpb;
	staying_chain_rpb.rpb_relation = rpb->rpb_relation;
	staying_chain_rpb.getWindow(tdbb).win_flags = WIN_secondary;

	RelationPages* relPages = rpb->rpb_relation->getPages(tdbb);
	PageStack precedence_stack;

	RecordStack::const_iterator const_i(staying);
	fb_assert(staying.hasData());
	Record* current_record = const_i.object(), *org_record;
	++const_i;
	bool prior_delta = false;

	Difference difference;

	while (const_i.hasData())
	{
		org_record = current_record;
		current_record = const_i.object();

		const ULONG diffLength =
			difference.make(current_record->getLength(), current_record->getData(),
							org_record->getLength(), org_record->getData());

		staying_chain_rpb.rpb_flags = rpb_chained | (prior_delta ? rpb_delta : 0);

		if (diffLength && diffLength < org_record->getLength())
		{
			staying_chain_rpb.rpb_address = difference.getData();
			staying_chain_rpb.rpb_length = diffLength;
			prior_delta = true;
		}
		else
		{
			staying_chain_rpb.rpb_address = org_record->getData();
			staying_chain_rpb.rpb_length = org_record->getLength();
			prior_delta = false;
		}

		staying_chain_rpb.rpb_transaction_nr = org_record->getTransactionNumber();
		staying_chain_rpb.rpb_format_number = org_record->getFormat()->fmt_version;

		if (staying_chain_rpb.rpb_page)
		{
			staying_chain_rpb.rpb_b_page = staying_chain_rpb.rpb_page;
			staying_chain_rpb.rpb_b_line = staying_chain_rpb.rpb_line;
			staying_chain_rpb.rpb_page = 0;
			staying_chain_rpb.rpb_line = 0;
		}

		staying_chain_rpb.rpb_number = rpb->rpb_number;
		DPM_store(tdbb, &staying_chain_rpb, precedence_stack, DPM_secondary);
		precedence_stack.push(PageNumber(relPages->rel_pg_space_id, staying_chain_rpb.rpb_page));
		++const_i;
	}

	// If primary version has been deleted then chain first staying version too
	if (rpb->rpb_flags & rpb_deleted)
	{
		staying_chain_rpb.rpb_address = current_record->getData();
		staying_chain_rpb.rpb_length = current_record->getLength();
		staying_chain_rpb.rpb_flags = rpb_chained | (prior_delta ? rpb_delta : 0);
		prior_delta = false;
		staying_chain_rpb.rpb_transaction_nr = current_record->getTransactionNumber();
		staying_chain_rpb.rpb_format_number = current_record->getFormat()->fmt_version;

		if (staying_chain_rpb.rpb_page)
		{
			staying_chain_rpb.rpb_b_page = staying_chain_rpb.rpb_page;
			staying_chain_rpb.rpb_b_line = staying_chain_rpb.rpb_line;
			staying_chain_rpb.rpb_page = 0;
			staying_chain_rpb.rpb_line = 0;
		}

		staying_chain_rpb.rpb_number = rpb->rpb_number;
		DPM_store(tdbb, &staying_chain_rpb, precedence_stack, DPM_secondary);
		precedence_stack.push(PageNumber(relPages->rel_pg_space_id, staying_chain_rpb.rpb_page));
	}

	// Read head version with write lock and check if it is still the same version
	record_param temp_rpb = *rpb;

	// If record no longer exists - return
	if (!DPM_get(tdbb, &temp_rpb, LCK_write))
	{
		delete_version_chain(tdbb, &staying_chain_rpb, true);
		clearRecordStack(staying);
		clearRecordStack(going);
		return;
	}

	// If record changed in any way - return
	if (temp_rpb.rpb_transaction_nr != rpb->rpb_transaction_nr || temp_rpb.rpb_b_line != rpb->rpb_b_line ||
		temp_rpb.rpb_b_page != rpb->rpb_b_page)
	{
		CCH_RELEASE(tdbb, &temp_rpb.getWindow(tdbb));
		delete_version_chain(tdbb, &staying_chain_rpb, true);
		clearRecordStack(staying);
		clearRecordStack(going);
		return;
	}

	// Ensure that new versions are written to disk before we update back-pointer
	// of primary version to point to them
	while (precedence_stack.hasData())
		CCH_precedence(tdbb, &temp_rpb.getWindow(tdbb), precedence_stack.pop());

	// Update back-pointer to point to new versions chain (if any)
	temp_rpb.rpb_b_page = staying_chain_rpb.rpb_page;
	temp_rpb.rpb_b_line = staying_chain_rpb.rpb_line;
	CCH_MARK(tdbb, &temp_rpb.getWindow(tdbb));

	if (prior_delta)
		temp_rpb.rpb_flags |= rpb_delta;
	else
		temp_rpb.rpb_flags &= ~rpb_delta;

	DPM_rewrite_header(tdbb, &temp_rpb);
	CCH_RELEASE(tdbb, &temp_rpb.getWindow(tdbb));

	// Delete old versions chain
	delete_version_chain(tdbb, rpb, false);

	// Garbage-collect blobs and indices
	BLB_garbage_collect(tdbb, going, staying, rpb->rpb_page, rpb->rpb_relation);
	IDX_garbage_collect(tdbb, rpb, going, staying);

	// Free memory for record versions we fetched during list_staying
	clearRecordStack(staying);
	clearRecordStack(going);

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_IMGC, rpb->rpb_relation->rel_id);
}

bool VIO_garbage_collect(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ g a r b a g e _ c o l l e c t
 *
 **************************************
 *
 * Functional description
 *	Do any garbage collection appropriate to the current
 *	record.  This is called during index creation to avoid
 *	unnecessary work as well as false duplicate records.
 *
 *	If the record complete goes away, return false.
 *
 **************************************/
	SET_TDBB(tdbb);
	Jrd::Attachment* attachment = transaction->tra_attachment;

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_TRACE,
		"VIO_garbage_collect (rel_id %u, record_param %" QUADFORMAT"d, transaction %"
		SQUADFORMAT")\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0);

	VIO_trace(DEBUG_TRACE_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

	if ((attachment->att_flags & ATT_no_cleanup) || !gcGuard.gcEnabled())
		return true;

	const TraNumber oldest_snapshot = rpb->rpb_relation->isTemporary() ?
		attachment->att_oldest_snapshot : transaction->tra_oldest_active;

	while (true)
	{
		if (rpb->rpb_flags & rpb_damaged)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			return false;
		}

		int state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr);

		// Reset (if appropriate) the garbage collect active flag to reattempt the backout

		if (rpb->rpb_flags & rpb_gc_active)
		{
			if (checkGCActive(tdbb, rpb, state))
				return true;
		}

		fb_assert(!(rpb->rpb_flags & rpb_gc_active));

		if (state == tra_committed)
			state = check_precommitted(transaction, rpb);

		if (state == tra_dead)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			VIO_backout(tdbb, rpb, transaction);
		}
		else
		{
			if (rpb->rpb_flags & rpb_deleted)
			{
				if (rpb->rpb_transaction_nr >= oldest_snapshot)
					return true;

				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				expunge(tdbb, rpb, transaction, 0);
				return false;
			}

			if (rpb->rpb_transaction_nr >= oldest_snapshot || rpb->rpb_b_page == 0)
				return true;

			purge(tdbb, rpb);
		}

		if (!DPM_get(tdbb, rpb, LCK_read))
			return false;
	}
}


Record* VIO_gc_record(thread_db* tdbb, jrd_rel* relation)
{
/**************************************
 *
 *	V I O _ g c _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Allocate from a relation's vector of garbage
 *	collect record blocks. Their scope is strictly
 *	limited to temporary usage and should never be
 *	copied to permanent record parameter blocks.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	const Format* const format = MET_current(tdbb, relation);

	// Set the active flag on an inactive garbage collect record block and return it

	for (Record** iter = relation->rel_gc_records.begin();
		 iter != relation->rel_gc_records.end();
		 ++iter)
	{
		Record* const record = *iter;
		fb_assert(record);

		if (!record->isTempActive())
		{
			// initialize record for reuse
			record->reset(format);
			record->setTempActive();
			return record;
		}
	}

	// Allocate a garbage collect record block if all are active

	Record* const record = FB_NEW_POOL(*relation->rel_pool)
		Record(*relation->rel_pool, format, true);
	relation->rel_gc_records.add(record);
	return record;
}


bool VIO_get(thread_db* tdbb, record_param* rpb, jrd_tra* transaction, MemoryPool* pool)
{
/**************************************
 *
 *	V I O _ g e t
 *
 **************************************
 *
 * Functional description
 *	Get a specific record from a relation.
 *
 **************************************/
	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_READS,
		"VIO_get (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT", pool %p)\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0,
		(void*) pool);
#endif

	// Fetch data page from a modify/erase input stream with a write
	// lock. This saves an upward conversion to a write lock when
	// refetching the page in the context of the output stream.

	const USHORT lock_type = (rpb->rpb_stream_flags & RPB_s_update) ? LCK_write : LCK_read;

	if (!DPM_get(tdbb, rpb, lock_type) ||
		!VIO_chase_record_version(tdbb, rpb, transaction, pool, false, false))
	{
		return false;
	}

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_READS_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	if (rpb->rpb_runtime_flags & RPB_undo_data)
		fb_assert(rpb->getWindow(tdbb).win_bdb == NULL);
	else
		fb_assert(rpb->getWindow(tdbb).win_bdb != NULL);

	if (pool && !(rpb->rpb_runtime_flags & RPB_undo_data))
	{
		if (rpb->rpb_stream_flags & RPB_s_no_data)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			rpb->rpb_address = NULL;
			rpb->rpb_length = 0;
		}
		else
			VIO_data(tdbb, rpb, pool);
	}

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_IDX_READS, rpb->rpb_relation->rel_id);
	return true;
}


bool VIO_get_current(thread_db* tdbb,
					record_param* rpb,
					jrd_tra* transaction,
					MemoryPool* pool,
					bool foreign_key,
					bool& rec_tx_active)
{
/**************************************
 *
 *	V I O _ g e t _ c u r r e n t
 *
 **************************************
 *
 * Functional description
 *	Get the current (most recent) version of a record.  This is
 *	called by IDX to determine whether a unique index has been
 *	duplicated.  If the target record's transaction is active,
 *	wait for it.  If the record is deleted or disappeared, return
 *	false.  If the record is committed, return true.
 *	If foreign_key is true, we are checking for a foreign key,
 *	looking to see if a primary key/unique key exists.  For a
 *	no wait transaction, if state of transaction inserting primary key
 *	record is tra_active, we should not see the uncommitted record
 *
 **************************************/
	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_TRACE,
		"VIO_get_current (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT", pool %p)\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0,
		(void*) pool);
#endif

	rec_tx_active = false;

	bool counted = false;

	while (true)
	{
		// If the record doesn't exist, no problem.

		if (!DPM_get(tdbb, rpb, LCK_read))
			return false;

#ifdef VIO_DEBUG
		VIO_trace(DEBUG_TRACE_INFO,
			"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
			", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
			rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
			rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
			rpb->rpb_f_page, rpb->rpb_f_line);
#endif

		// Get data if there is data.

		if (rpb->rpb_flags & rpb_damaged)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			return false;
		}

		if (rpb->rpb_flags & rpb_deleted)
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		else
			VIO_data(tdbb, rpb, pool);

		if (!counted)
		{
			tdbb->bumpRelStats(RuntimeStatistics::RECORD_IDX_READS, rpb->rpb_relation->rel_id);
			counted = true;
		}

		// If we deleted the record, everything's fine, otherwise
		// the record must be considered real.

		if (rpb->rpb_transaction_nr == transaction->tra_number)
			break;

		// check the state of transaction  - tra_us is taken care of above
		// For now check the state in the tip_cache or tip bitmap. If
		// record is committed (most cases), this will be faster.

		int state = (transaction->tra_flags & TRA_read_committed) ?
			TPC_cache_state(tdbb, rpb->rpb_transaction_nr) :
			TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr);

		// Reset (if appropriate) the garbage collect active flag to reattempt the backout

		if (rpb->rpb_flags & rpb_gc_active)
		{
			if (checkGCActive(tdbb, rpb, state))
			{
				waitGCActive(tdbb, rpb);
				continue;
			}
		}

		fb_assert(!(rpb->rpb_flags & rpb_gc_active));

		if (state == tra_committed)
			state = check_precommitted(transaction, rpb);

		switch (state)
		{
		case tra_committed:
			return !(rpb->rpb_flags & rpb_deleted);
		case tra_dead:
			// Run backout otherwise false key violation could be reported, see CORE-5110
			//
			// if (transaction->tra_attachment->att_flags & ATT_no_cleanup)
			//	return !foreign_key;

			{
				jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

				if (!gcGuard.gcEnabled())
					return !foreign_key;

				VIO_backout(tdbb, rpb, transaction);
			}
			continue;
		}

		// The record belongs to somebody else.  Wait for him to commit, rollback, or die.

		const TraNumber tid_fetch = rpb->rpb_transaction_nr;

		// Wait as long as it takes for an active transaction which has modified
		// the record.

		state = wait(tdbb, transaction, rpb, false);

		if (state == tra_committed)
			state = check_precommitted(transaction, rpb);

		switch (state)
		{
		case tra_committed:
			// If the record doesn't exist anymore, no problem. This
			// can happen in two cases.  The transaction that inserted
			// the record deleted it or the transaction rolled back and
			// removed the records it modified and marked itself
			// committed

			if (!DPM_get(tdbb, rpb, LCK_read))
				return false;

			// if the transaction actually rolled back and what
			// we are reading is another record (newly inserted),
			// loop back and try again.

			if (tid_fetch != rpb->rpb_transaction_nr)
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				continue;
			}

			// Get latest data if there is data.

			if (rpb->rpb_flags & rpb_deleted)
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				return false;
			}

			VIO_data(tdbb, rpb, pool);
			return true;

		case tra_limbo:
			if (!(transaction->tra_flags & TRA_ignore_limbo))
			{
				// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
				ERR_post(Arg::Gds(isc_rec_in_limbo) << Arg::Int64(rpb->rpb_transaction_nr));
			}
			[[fallthrough]];

		case tra_active:
			// clear lock error from status vector
			fb_utils::init_status(tdbb->tdbb_status_vector);
			rec_tx_active = true;

			// 1. if record just inserted
			//	  then FK can't reference it but PK must check it's new value
			// 2. if record just deleted
			//    then FK can't reference it but PK must check it's old value
			// 3. if record just modified
			//	  then FK can reference it if key field values are not changed

			if (!rpb->rpb_b_page)
				return !foreign_key;

			if (rpb->rpb_flags & rpb_deleted)
				return !foreign_key;

			if (foreign_key)
			{
				if (!(rpb->rpb_flags & rpb_uk_modified))
				{
					rec_tx_active = false;
					return true;
				}
				return false;
			}

			return true;

		case tra_dead:
			// if (transaction->tra_attachment->att_flags & ATT_no_cleanup)
			//	return !foreign_key;

			{
				jrd_rel::GCShared gcGuard(tdbb, rpb->rpb_relation);

				if (!gcGuard.gcEnabled())
					return !foreign_key;

				VIO_backout(tdbb, rpb, transaction);
			}
			break;

		default:
			fb_assert(false);
		}
	}

	return !(rpb->rpb_flags & rpb_deleted);
}


void VIO_init(thread_db* tdbb)
{
/**************************************
 *
 *	V I O _ i n i t
 *
 **************************************
 *
 * Functional description
 *	Activate the garbage collector thread.
 *
 **************************************/
	Database* dbb = tdbb->getDatabase();
	Jrd::Attachment* attachment = tdbb->getAttachment();

	if (dbb->readOnly() || !(dbb->dbb_flags & DBB_gc_background))
		return;

	// If there's no presence of a garbage collector running then start one up.

	if (!(dbb->dbb_flags & DBB_garbage_collector))
	{
		const ULONG old = dbb->dbb_flags.exchangeBitOr(DBB_gc_starting);
		if (!(old & DBB_gc_starting))
		{
			if (old & DBB_garbage_collector)
				dbb->dbb_flags &= ~DBB_gc_starting;
			else
			{
				try
				{
					dbb->dbb_gc_fini.run(dbb);
				}
				catch (const Exception&)
				{
					dbb->dbb_flags &= ~DBB_gc_starting;
					ERR_bugcheck_msg("cannot start garbage collector thread");
				}

				dbb->dbb_gc_init.enter();
			}
		}
	}

	// Database backups and sweeps perform their own garbage collection
	// unless passing a no garbage collect switch which means don't
	// notify the garbage collector to garbage collect. Every other
	// attachment notifies the garbage collector to do their dirty work.

	if ((dbb->dbb_flags & DBB_garbage_collector) &&
		!(attachment->att_flags & ATT_no_cleanup) &&
		!attachment->isGbak())
	{
		attachment->att_flags |= ATT_notify_gc;
	}
}

bool VIO_modify(thread_db* tdbb, record_param* org_rpb, record_param* new_rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ m o d i f y
 *
 **************************************
 *
 * Functional description
 *	Modify an existing record.
 *
 **************************************/
	SET_TDBB(tdbb);

	QualifiedName object_name;
	const auto attachment = tdbb->getAttachment();
	jrd_rel* relation = org_rpb->rpb_relation;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES,
		"VIO_modify (rel_id %u, org_rpb %" QUADFORMAT"d, new_rpb %" QUADFORMAT"d, "
		"transaction %" SQUADFORMAT")\n",
		relation->rel_id, org_rpb->rpb_number.getValue(), new_rpb->rpb_number.getValue(),
		transaction ? transaction->tra_number : 0);

	VIO_trace(DEBUG_WRITES_INFO,
		"   old record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		org_rpb->rpb_page, org_rpb->rpb_line, org_rpb->rpb_transaction_nr,
		org_rpb->rpb_flags, org_rpb->rpb_b_page, org_rpb->rpb_b_line,
		org_rpb->rpb_f_page, org_rpb->rpb_f_line);
#endif

	transaction->tra_flags |= TRA_write;
	new_rpb->rpb_transaction_nr = transaction->tra_number;
	new_rpb->rpb_flags = 0;
	new_rpb->getWindow(tdbb).win_flags = WIN_secondary;

	// If the stream was sorted, the various fields in the rpb are
	// probably junk.  Just to make sure that everything is cool,
	// refetch and release the record.

	if (org_rpb->rpb_runtime_flags & (RPB_refetch | RPB_undo_read))
	{
		const bool undo_read = (org_rpb->rpb_runtime_flags & RPB_undo_read);
		AutoTempRecord old_record;
		if (undo_read)
		{
			old_record = VIO_gc_record(tdbb, relation);
			old_record->copyFrom(org_rpb->rpb_record);
		}

		VIO_refetch_record(tdbb, org_rpb, transaction, false, true);
		org_rpb->rpb_runtime_flags &= ~RPB_refetch;
		fb_assert(!(org_rpb->rpb_runtime_flags & RPB_undo_read));

		if (undo_read)
			refresh_fk_fields(tdbb, old_record, org_rpb, new_rpb);
	}

	// If we're the system transaction, modify stuff in place.  This saves
	// endless grief on cleanup

	if (transaction->tra_flags & TRA_system)
	{
		VIO_update_in_place(tdbb, transaction, org_rpb, new_rpb);
		tdbb->bumpRelStats(RuntimeStatistics::RECORD_UPDATES, relation->rel_id);
		return true;
	}

	check_gbak_cheating_insupd(tdbb, relation, "UPDATE");

	if (attachment->isGbak() &&
		!(attachment->att_flags & ATT_gbak_restore_has_schema) &&
		!(attachment->att_database->dbb_flags & DBB_creating))
	{
		gbak_put_search_system_schema_flag(tdbb, new_rpb, transaction);
	}

	// If we're about to modify a system relation, check to make sure
	// everything is completely kosher.

	DSC desc1, desc2, schemaDesc;

	if (needDfw(tdbb, transaction))
	{
		constexpr SLONG nullLinger = 0;

		switch ((RIDS) relation->rel_id)
		{
		case rel_segments:
		case rel_vrel:
		case rel_args:
		case rel_filters:
		case rel_trans:
		case rel_dims:
		case rel_prc_prms:
		case rel_auth_mapping:
		case rel_roles:
		case rel_ccon:
		case rel_pub_tables:
		case rel_priv:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			break;

		case rel_types:
		 	if (!tdbb->getAttachment()->locksmith(tdbb, CREATE_USER_TYPES))
		 		protect_system_table_delupd(tdbb, relation, "UPDATE", true);
			if (EVL_field(0, org_rpb->rpb_record, f_typ_sys_flag, &desc1) && MOV_get_long(tdbb, &desc1, 0))
		 		protect_system_table_delupd(tdbb, relation, "UPDATE", true);
			break;

		case rel_db_creators:
			if (!tdbb->getAttachment()->locksmith(tdbb, GRANT_REVOKE_ANY_DDL_RIGHT))
				protect_system_table_delupd(tdbb, relation, "UPDATE");
			break;

		case rel_pages:
		case rel_formats:
		case rel_msgs:
		case rel_log:
		case rel_dpds:
		case rel_rcon:
		case rel_refc:
		case rel_backup_history:
		case rel_global_auth_mapping:
			protect_system_table_delupd(tdbb, relation, "UPDATE", true);
			break;

		case rel_database:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_class(tdbb, transaction, org_rpb, new_rpb, f_dat_class);
			if (!EVL_field(0, org_rpb->rpb_record, f_dat_linger, &desc1))
				desc1.makeLong(0, const_cast<SLONG*>(&nullLinger));
			if (!EVL_field(0, new_rpb->rpb_record, f_dat_linger, &desc2))
				desc2.makeLong(0, const_cast<SLONG*>(&nullLinger));
			if (MOV_compare(tdbb, &desc1, &desc2))
				DFW_post_work(transaction, dfw_set_linger, &desc2, nullptr, 0);
			break;

		case rel_schemas:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_class(tdbb, transaction, org_rpb, new_rpb, f_sch_class);
			break;

		case rel_relations:
			EVL_field(0, org_rpb->rpb_record, f_rel_schema, &schemaDesc);
			EVL_field(0, org_rpb->rpb_record, f_rel_name, &desc1);
			if (!check_nullify_source(tdbb, org_rpb, new_rpb, f_rel_source))
				protect_system_table_delupd(tdbb, relation, "UPDATE");
			else
			{
				MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
				MOV_get_metaname(tdbb, &desc1, object_name.object);
				SCL_check_relation(tdbb, object_name, SCL_alter);
			}
			check_class(tdbb, transaction, org_rpb, new_rpb, f_rel_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_rel_owner);
			DFW_post_work(transaction, dfw_update_format, &desc1, &schemaDesc, 0);
			break;

		case rel_packages:
			EVL_field(0, org_rpb->rpb_record, f_pkg_schema, &schemaDesc);
			if (!check_nullify_source(tdbb, org_rpb, new_rpb, f_pkg_header_source, f_pkg_body_source))
				protect_system_table_delupd(tdbb, relation, "UPDATE");
			else
			{
				if (EVL_field(0, org_rpb->rpb_record, f_pkg_name, &desc1))
				{
					MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
					MOV_get_metaname(tdbb, &desc1, object_name.object);
					SCL_check_package(tdbb, object_name, SCL_alter);
				}
			}
			check_class(tdbb, transaction, org_rpb, new_rpb, f_pkg_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_pkg_owner);
			break;

		case rel_procedures:
			EVL_field(0, org_rpb->rpb_record, f_prc_schema, &schemaDesc);
			EVL_field(0, org_rpb->rpb_record, f_prc_name, &desc1);

			if (EVL_field(0, org_rpb->rpb_record, f_prc_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			if (!check_nullify_source(tdbb, org_rpb, new_rpb, f_prc_source))
				protect_system_table_delupd(tdbb, relation, "UPDATE");
			else
			{
				if (object_name.package.hasData())
					SCL_check_package(tdbb, object_name.getSchemaAndPackage(), SCL_alter);
				else
				{
					MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
					MOV_get_metaname(tdbb, &desc1, object_name.object);
					SCL_check_procedure(tdbb, object_name, SCL_alter);
				}
			}

			check_class(tdbb, transaction, org_rpb, new_rpb, f_prc_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_prc_owner);

			if (dfw_should_know(tdbb, org_rpb, new_rpb, f_prc_desc, true))
			{
				EVL_field(0, org_rpb->rpb_record, f_prc_id, &desc2);
				const USHORT id = MOV_get_long(tdbb, &desc2, 0);
				DFW_post_work(transaction, dfw_modify_procedure, &desc1, &schemaDesc, id, object_name.package);
			}
			break;

		case rel_funs:
			EVL_field(0, org_rpb->rpb_record, f_fun_schema, &schemaDesc);
			EVL_field(0, org_rpb->rpb_record, f_fun_name, &desc1);

			if (EVL_field(0, org_rpb->rpb_record, f_fun_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			if (!check_nullify_source(tdbb, org_rpb, new_rpb, f_fun_source))
				protect_system_table_delupd(tdbb, relation, "UPDATE");
			else
			{
				if (object_name.package.hasData())
				{
					const auto package = object_name.getSchemaAndPackage();
					SCL_check_package(tdbb, package, SCL_alter);
					SCL_check_package(tdbb, package, SCL_alter);
				}
				else
				{
					MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
					MOV_get_metaname(tdbb, &desc1, object_name.object);
					SCL_check_function(tdbb, object_name, SCL_alter);
				}
			}

			check_class(tdbb, transaction, org_rpb, new_rpb, f_fun_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_fun_owner);

			if (dfw_should_know(tdbb, org_rpb, new_rpb, f_fun_desc, true))
			{
				EVL_field(0, org_rpb->rpb_record, f_fun_id, &desc2);
				const USHORT id = MOV_get_long(tdbb, &desc2, 0);
				DFW_post_work(transaction, dfw_modify_function, &desc1, &schemaDesc, id, object_name.package);
			}
			break;

		case rel_gens:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_class(tdbb, transaction, org_rpb, new_rpb, f_gen_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_gen_owner);
			break;

		case rel_rfr:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			{
				check_rel_field_class(tdbb, org_rpb, transaction);
				check_rel_field_class(tdbb, new_rpb, transaction);
				check_class(tdbb, transaction, org_rpb, new_rpb, f_rfr_class);

				bool rc1 = EVL_field(NULL, org_rpb->rpb_record, f_rfr_null_flag, &desc1);

				if ((!rc1 || MOV_get_long(tdbb, &desc1, 0) == 0))
				{
					dsc desc3, desc4;
					bool rc2 = EVL_field(NULL, new_rpb->rpb_record, f_rfr_null_flag, &desc2);
					bool rc3 = EVL_field(NULL, org_rpb->rpb_record, f_rfr_sname, &desc3);
					bool rc4 = EVL_field(NULL, new_rpb->rpb_record, f_rfr_sname, &desc4);

					if ((rc2 && MOV_get_long(tdbb, &desc2, 0) != 0) ||
						(rc3 && rc4 && MOV_compare(tdbb, &desc3, &desc4)))
					{
						EVL_field(0, new_rpb->rpb_record, f_rfr_schema, &schemaDesc);
						EVL_field(0, new_rpb->rpb_record, f_rfr_rname, &desc1);
						EVL_field(0, new_rpb->rpb_record, f_rfr_id, &desc2);

						DeferredWork* work = DFW_post_work(transaction, dfw_check_not_null, &desc1, &schemaDesc, 0);
						SortedArray<int>& ids = DFW_get_ids(work);

						int id = MOV_get_long(tdbb, &desc2, 0);
						FB_SIZE_T pos;
						if (!ids.find(id, pos))
							ids.insert(pos, id);
					}
				}
			}
			break;

		case rel_fields:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			EVL_field(0, org_rpb->rpb_record, f_fld_name, &desc1);

			if (dfw_should_know(tdbb, org_rpb, new_rpb, f_fld_desc, true))
			{
				EVL_field(0, org_rpb->rpb_record, f_fld_schema, &schemaDesc);

				MET_change_fields(tdbb, transaction, &schemaDesc, &desc1);
				EVL_field(0, new_rpb->rpb_record, f_fld_name, &desc2);
				DeferredWork* dw = MET_change_fields(tdbb, transaction, &schemaDesc, &desc2);
				dsc desc3, desc4;
				bool rc1, rc2;

				if (dw)
				{
					// Did we convert computed field into physical, stored field?
					// If we did, then force the deletion of the dependencies.
					// Warning: getting the result of MET_change_fields is the last relation
					// that was affected, but for computed fields, it's an implicit domain
					// and hence it can be used only by a single field and therefore one relation.
					rc1 = EVL_field(0, org_rpb->rpb_record, f_fld_computed, &desc3);
					rc2 = EVL_field(0, new_rpb->rpb_record, f_fld_computed, &desc4);
					if (rc1 != rc2 || rc1 && MOV_compare(tdbb, &desc3, &desc4)) {
						DFW_post_work_arg(transaction, dw, &desc1, &schemaDesc, 0, dfw_arg_force_computed);
					}
				}

				dw = DFW_post_work(transaction, dfw_modify_field, &desc1, &schemaDesc, 0);
				DFW_post_work_arg(transaction, dw, &desc2, &schemaDesc, 0, dfw_arg_new_name);

				rc1 = EVL_field(NULL, org_rpb->rpb_record, f_fld_null_flag, &desc3);
				rc2 = EVL_field(NULL, new_rpb->rpb_record, f_fld_null_flag, &desc4);

				if ((!rc1 || MOV_get_long(tdbb, &desc3, 0) == 0) && rc2 && MOV_get_long(tdbb, &desc4, 0) != 0)
					DFW_post_work_arg(transaction, dw, &desc2, &schemaDesc, 0, dfw_arg_field_not_null);
			}
			check_class(tdbb, transaction, org_rpb, new_rpb, f_fld_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_fld_owner);
			break;

		case rel_classes:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			EVL_field(0, org_rpb->rpb_record, f_cls_class, &desc1);
			DFW_post_work(transaction, dfw_compute_security, &desc1, nullptr, 0);
			EVL_field(0, new_rpb->rpb_record, f_cls_class, &desc1);
#ifdef DEV_BUILD
			MOV_get_metaname(tdbb, &desc1, object_name.object);
			fb_assert(strncmp(object_name.object.c_str(), "SQL$", 4) == 0);
#endif
			DFW_post_work(transaction, dfw_compute_security, &desc1, nullptr, 0);
			break;

		case rel_indices:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			EVL_field(0, new_rpb->rpb_record, f_idx_relation, &desc1);

			if (dfw_should_know(tdbb, org_rpb, new_rpb, f_idx_desc, true))
			{
				EVL_field(0, new_rpb->rpb_record, f_idx_schema, &schemaDesc);
				EVL_field(0, new_rpb->rpb_record, f_idx_name, &desc1);

				if (EVL_field(0, new_rpb->rpb_record, f_idx_exp_blr, &desc2))
				{
					DFW_post_work(transaction, dfw_create_expression_index,
								  &desc1, &schemaDesc, tdbb->getDatabase()->dbb_max_idx);
				}
				else
				{
					DFW_post_work(transaction, dfw_create_index, &desc1, &schemaDesc,
								  tdbb->getDatabase()->dbb_max_idx);
				}
			}
			break;

		case rel_triggers:
			EVL_field(0, new_rpb->rpb_record, f_trg_schema, &schemaDesc);
			EVL_field(0, new_rpb->rpb_record, f_trg_rname, &desc1);
			if (!check_nullify_source(tdbb, org_rpb, new_rpb, f_trg_source))
				protect_system_table_delupd(tdbb, relation, "UPDATE");
			else
			{
				MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
				MOV_get_metaname(tdbb, &desc1, object_name.object);
				SCL_check_relation(tdbb, object_name, SCL_control | SCL_alter);
			}

			if (dfw_should_know(tdbb, org_rpb, new_rpb, f_trg_desc, true))
			{
				EVL_field(0, new_rpb->rpb_record, f_trg_rname, &desc1);
				DFW_post_work(transaction, dfw_update_format, &desc1, &schemaDesc, 0);
				EVL_field(0, org_rpb->rpb_record, f_trg_rname, &desc1);
				DFW_post_work(transaction, dfw_update_format, &desc1, &schemaDesc, 0);
				EVL_field(0, org_rpb->rpb_record, f_trg_name, &desc1);
				DeferredWork* dw = DFW_post_work(transaction, dfw_modify_trigger, &desc1, &schemaDesc, 0);

				if (EVL_field(0, new_rpb->rpb_record, f_trg_rname, &desc2))
					DFW_post_work_arg(transaction, dw, &desc2, &schemaDesc, 0, dfw_arg_rel_name);

				if (EVL_field(0, new_rpb->rpb_record, f_trg_type, &desc2))
				{
					DFW_post_work_arg(transaction, dw, &desc2, &schemaDesc,
						(USHORT) MOV_get_int64(tdbb, &desc2, 0), dfw_arg_trg_type);
				}
			}
			break;

		case rel_files:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			{
				EVL_field(0, new_rpb->rpb_record, f_file_name, &desc1);

				const auto orgFileFlags = EVL_field(0, org_rpb->rpb_record, f_file_flags, &desc2) ?
					MOV_get_long(tdbb, &desc2, 0) : 0;
				const auto newFileFlags = EVL_field(0, new_rpb->rpb_record, f_file_flags, &desc2) ?
					MOV_get_long(tdbb, &desc2, 0) : 0;

				if ((newFileFlags & FILE_difference) && orgFileFlags != newFileFlags)
				{
					DFW_post_work(transaction,
								  (newFileFlags & FILE_backing_up) ? dfw_begin_backup : dfw_end_backup,
								  &desc1, nullptr, 0);
				}
			}
			// Nullify the unsupported fields
			new_rpb->rpb_record->setNull(f_file_seq);
			new_rpb->rpb_record->setNull(f_file_start);
			new_rpb->rpb_record->setNull(f_file_length);
			break;

		case rel_charsets:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_class(tdbb, transaction, org_rpb, new_rpb, f_cs_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_cs_owner);
			break;

		case rel_collations:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_class(tdbb, transaction, org_rpb, new_rpb, f_coll_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_coll_owner);
			break;

		case rel_exceptions:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_class(tdbb, transaction, org_rpb, new_rpb, f_xcp_class);
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_xcp_owner);
			break;

		case rel_pubs:
			protect_system_table_delupd(tdbb, relation, "UPDATE");
			check_owner(tdbb, transaction, org_rpb, new_rpb, f_pub_owner);
			check_repl_state(tdbb, transaction, org_rpb, new_rpb, f_pub_active_flag);
			break;

		default:
			break;
		}
	}

	// We're about to modify the record. Post a refetch request
	// to all the active cursors positioned at this record.

	invalidate_cursor_records(transaction, new_rpb);

	// hvlad: prepare_update() take EX lock on data page. Subsequent call of
	// IDX_modify_flag_uk_modified() will read database - if relation's partners
	// list has not been scanned yet. It could lead to single thread deadlock
	// if the same page should be fetched for read.
	// Explicit scan of relation's partners allows to avoid possible deadlock.

	MET_scan_partners(tdbb, org_rpb->rpb_relation);

	/* We're almost ready to go.  To modify the record, we must first
	make a copy of the old record someplace else.  Then we must re-fetch
	the record (for write) and verify that it is legal for us to
	modify it -- that it was written by a transaction that was committed
	when we started.  If not, the transaction that wrote the record
	is either active, dead, or in limbo.  If the transaction is active,
	wait for it to finish.  If it commits, we can't procede and must
	return an update conflict.  If the transaction is dead, back out the
	old version of the record and try again.  If in limbo, punt.
	*/

	if (org_rpb->rpb_transaction_nr == transaction->tra_number &&
		org_rpb->rpb_format_number == new_rpb->rpb_format_number)
	{
		IDX_modify_flag_uk_modified(tdbb, org_rpb, new_rpb, transaction);
		VIO_update_in_place(tdbb, transaction, org_rpb, new_rpb);

		if (!(transaction->tra_flags & TRA_system) &&
			transaction->tra_save_point && transaction->tra_save_point->isChanging())
		{
			verb_post(tdbb, transaction, org_rpb, org_rpb->rpb_undo);
		}

		tdbb->bumpRelStats(RuntimeStatistics::RECORD_UPDATES, relation->rel_id);
		return true;
	}

	const bool backVersion = (org_rpb->rpb_b_page != 0);
	record_param temp;
	PageStack stack;
	const auto prepare_result = prepare_update(tdbb, transaction, org_rpb->rpb_transaction_nr, org_rpb,
										&temp, new_rpb, stack, false);
	if (!check_prepare_result(prepare_result, transaction, tdbb->getRequest(), org_rpb))
		return false;

	IDX_modify_flag_uk_modified(tdbb, org_rpb, new_rpb, transaction);

	// Old record was restored and re-fetched for write.  Now replace it.

	org_rpb->rpb_transaction_nr = new_rpb->rpb_transaction_nr;
	org_rpb->rpb_format_number = new_rpb->rpb_format_number;
	org_rpb->rpb_b_page = temp.rpb_page;
	org_rpb->rpb_b_line = temp.rpb_line;
	org_rpb->rpb_address = new_rpb->rpb_address;
	org_rpb->rpb_length = new_rpb->rpb_length;
	org_rpb->rpb_flags &= ~(rpb_delta | rpb_uk_modified);
	org_rpb->rpb_flags |= new_rpb->rpb_flags & (rpb_delta | rpb_uk_modified);

	stack.merge(new_rpb->rpb_record->getPrecedence());

	replace_record(tdbb, org_rpb, &stack, transaction);

	if (!(transaction->tra_flags & TRA_system) &&
		transaction->tra_save_point && transaction->tra_save_point->isChanging())
	{
		verb_post(tdbb, transaction, org_rpb, 0);
	}

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_UPDATES, relation->rel_id);

	// for an autocommit transaction, mark a commit as necessary

	if (transaction->tra_flags & TRA_autocommit)
		transaction->tra_flags |= TRA_perform_autocommit;

	// VIO_modify

	Database* dbb = tdbb->getDatabase();

	if (backVersion && !(tdbb->getAttachment()->att_flags & ATT_no_cleanup) &&
		(dbb->dbb_flags & DBB_gc_cooperative))
	{
		jrd_rel::GCShared gcGuard(tdbb, org_rpb->rpb_relation);
		if (gcGuard.gcEnabled())
		{
			temp.rpb_number = org_rpb->rpb_number;
			if (DPM_get(tdbb, &temp, LCK_read))
				VIO_intermediate_gc(tdbb, &temp, transaction);
		}
	}
	else if ((dbb->dbb_flags & DBB_gc_background) && !org_rpb->rpb_relation->isTemporary())
	{
		notify_garbage_collector(tdbb, org_rpb, transaction->tra_number);
	}
	return true;
}


bool VIO_next_record(thread_db* tdbb,
					 record_param* rpb,
					 jrd_tra* transaction,
					 MemoryPool* pool,
					 FindNextRecordScope scope,
					 const RecordNumber* upper)
{
/**************************************
 *
 *	V I O _ n e x t _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Get the next record in a record stream.
 *
 **************************************/
	SET_TDBB(tdbb);

	// Fetch data page from a modify/erase input stream with a write
	// lock. This saves an upward conversion to a write lock when
	// refetching the page in the context of the output stream.

	const USHORT lock_type = (rpb->rpb_stream_flags & RPB_s_update) ? LCK_write : LCK_read;

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_TRACE,
		"VIO_next_record (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT", pool %p)\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0,
		(void*) pool);

	VIO_trace(DEBUG_TRACE_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	do
	{
		if (!DPM_next(tdbb, rpb, lock_type, scope))
			return false;

		if (upper && rpb->rpb_number > *upper)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			return false;
		}
	} while (!VIO_chase_record_version(tdbb, rpb, transaction, pool, false, false));

	if (rpb->rpb_runtime_flags & RPB_undo_data)
		fb_assert(rpb->getWindow(tdbb).win_bdb == NULL);
	else
		fb_assert(rpb->getWindow(tdbb).win_bdb != NULL);

	if (pool && !(rpb->rpb_runtime_flags & RPB_undo_data))
	{
		if (rpb->rpb_stream_flags & RPB_s_no_data)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			rpb->rpb_address = NULL;
			rpb->rpb_length = 0;
		}
		else
			VIO_data(tdbb, rpb, pool);
	}

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_READS_INFO,
		"VIO_next_record got record  %" SLONGFORMAT":%d, rpb_trans %"
		SQUADFORMAT", flags %d, back %" SLONGFORMAT":%d, fragment %"
		SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_SEQ_READS, rpb->rpb_relation->rel_id);
	return true;
}


Record* VIO_record(thread_db* tdbb, record_param* rpb, const Format* format, MemoryPool* pool)
{
/**************************************
 *
 *	V I O _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Allocate a record block big enough for a given format.
 *
 **************************************/
	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_TRACE,
		"VIO_record (rel_id %u, record_param %" QUADFORMAT"d, format %d, pool %p)\n",
		relation->rel_id, rpb->rpb_number.getValue(), format ? format->fmt_version : 0,
		(void*) pool);
#endif

	// If format wasn't given, look one up

	if (!format)
		format = MET_format(tdbb, rpb->rpb_relation, rpb->rpb_format_number);

	Record* record = rpb->rpb_record;

	if (!record)
	{
		if (!pool)
			pool = rpb->rpb_relation->rel_pool;

		record = rpb->rpb_record = FB_NEW_POOL(*pool) Record(*pool, format);
	}

	record->reset(format);

	return record;
}


bool VIO_refetch_record(thread_db* tdbb, record_param* rpb, jrd_tra* transaction,
						bool writelock, bool noundo)
{
/**************************************
 *
 *	V I O _ r e f e t c h _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Refetch & release the record, if we unsure,
 *  whether information about it is still valid.
 *
 **************************************/
#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_READS,
		"VIO_refetch_record (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT")\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0);
#endif

	const TraNumber tid_fetch = rpb->rpb_transaction_nr;

	if (!DPM_get(tdbb, rpb, LCK_read) ||
		!VIO_chase_record_version(tdbb, rpb, transaction, tdbb->getDefaultPool(), writelock, noundo))
	{
		if (writelock)
			return false;

		ERR_post(Arg::Gds(isc_no_cur_rec));
	}

	if (!(rpb->rpb_runtime_flags & RPB_undo_data))
	{
		if (rpb->rpb_stream_flags & RPB_s_no_data)
		{
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			rpb->rpb_address = NULL;
			rpb->rpb_length = 0;
		}
		else
			VIO_data(tdbb, rpb, tdbb->getDefaultPool());
	}

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_RPT_READS, rpb->rpb_relation->rel_id);

	// If record is present, and the transaction is read committed,
	// make sure the record has not been updated.  Also, punt after
	// VIO_data() call which will release the page.

	if (!writelock &&
		(transaction->tra_flags & TRA_read_committed) &&
		(tid_fetch != rpb->rpb_transaction_nr) &&
		// added to check that it was not current transaction,
		// who modified the record. Alex P, 18-Jun-03
		(rpb->rpb_transaction_nr != transaction->tra_number) &&
		// dimitr: reads using the undo log are also OK
		!(rpb->rpb_runtime_flags & RPB_undo_read))
	{
		tdbb->bumpRelStats(RuntimeStatistics::RECORD_CONFLICTS, rpb->rpb_relation->rel_id);

		// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
		ERR_post(Arg::Gds(isc_deadlock) <<
				 Arg::Gds(isc_update_conflict) <<
				 Arg::Gds(isc_concurrent_transaction) << Arg::Int64(rpb->rpb_transaction_nr));
	}

	return true;
}


void VIO_store(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ s t o r e
 *
 **************************************
 *
 * Functional description
 *	Store a new record.
 *
 **************************************/
	SET_TDBB(tdbb);
	const auto attachment = tdbb->getAttachment();
	const auto request = tdbb->getRequest();
	const auto relation = rpb->rpb_relation;

	DeferredWork* work = NULL;
	USHORT object_id;
	QualifiedName object_name;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES,
		"VIO_store (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT
		")\n", relation->rel_id, rpb->rpb_number.getValue(),
		transaction ? transaction->tra_number : 0);
#endif

	transaction->tra_flags |= TRA_write;
	DSC desc, desc2, schemaDesc;

	check_gbak_cheating_insupd(tdbb, relation, "INSERT");

	if (attachment->isGbak() &&
		!(attachment->att_flags & ATT_gbak_restore_has_schema) &&
		!(attachment->att_database->dbb_flags & DBB_creating))
	{
		struct ObjTypeFieldId
		{
			ObjectType objType;
			USHORT fieldId;
		};

		struct SchemaFieldDependency
		{
			USHORT fieldId;
			std::optional<ObjTypeFieldId> dependencyId;
		};

		static const std::unordered_map<USHORT, std::vector<SchemaFieldDependency>> schemaFields = {
			{rel_database, {
				{f_dat_charset_schema, ObjTypeFieldId{obj_charset, f_dat_charset}}
			}},
			{rel_fields, {
				{f_fld_schema}
			}},
			{rel_segments, {
				{f_seg_schema}
			}},
			{rel_indices, {
				{f_idx_schema},
				{f_idx_foreign_schema, ObjTypeFieldId{obj_index, f_idx_foreign}}
			}},
			{rel_rfr, {
				{f_rfr_schema},
				{f_rfr_field_source_schema, ObjTypeFieldId{obj_field, f_rfr_sname}}
			}},
			{rel_relations, {
				{f_rel_schema}
			}},
			{rel_vrel, {
				{f_vrl_schema},
				{f_vrl_rname_schema, ObjTypeFieldId{obj_relation, f_vrl_rname}}
			}},
			{rel_triggers, {
				{f_trg_schema}
			}},
			/* RDB$DEPENDENCIES is not used in GBAK
			{rel_dpds, {
				{f_dpd_schema},
				{f_dpd_o_schema}
			}},
			*/
			{rel_funs, {
				{f_fun_schema}
			}},
			{rel_args, {
				{f_arg_schema},
				{f_arg_rel_schema, ObjTypeFieldId{obj_relation, f_arg_rname}},
				{f_arg_field_source_schema, ObjTypeFieldId{obj_field, f_arg_sname}}
			}},
			{rel_msgs, {
				{f_msg_schema}
			}},
			{rel_gens, {
				{f_gen_schema}
			}},
			{rel_dims, {
				{f_dims_schema}
			}},
			{rel_rcon, {
				{f_rcon_schema}
			}},
			{rel_refc, {
				{f_refc_schema},
				{f_refc_uq_schema, ObjTypeFieldId{obj_any, f_refc_uq}}
			}},
			{rel_ccon, {
				{f_ccon_schema}
			}},
			{rel_procedures, {
				{f_prc_schema}
			}},
			{rel_prc_prms, {
				{f_prm_schema},
				{f_prm_rel_schema, ObjTypeFieldId{obj_relation, f_prm_rname}},
				{f_prm_field_source_schema, ObjTypeFieldId{obj_field, f_prm_sname}}
			}},
			{rel_charsets, {
				{f_cs_schema},
				{f_cs_def_coll_schema, ObjTypeFieldId{obj_collation, f_cs_def_collate}}
			}},
			{rel_collations, {
				{f_coll_schema}
			}},
			{rel_exceptions, {
				{f_xcp_schema}
			}},
			{rel_packages, {
				{f_pkg_schema}
			}},
			{rel_pub_tables, {
				{f_pubtab_tab_schema, ObjTypeFieldId{obj_relation, f_pubtab_tab_name}}
			}},
		};

		ObjectsArray<MetaString> schemaSearchPath({SYSTEM_SCHEMA, PUBLIC_SCHEMA});

		if (const auto relSchemaFields = schemaFields.find(relation->rel_id); relSchemaFields != schemaFields.end())
		{
			for (const auto [fieldId, dependency] : relSchemaFields->second)
			{
				if (rpb->rpb_record->isNull(fieldId) &&
					(!dependency.has_value() || !rpb->rpb_record->isNull(dependency->fieldId)) &&
					!EVL_field(0, rpb->rpb_record, fieldId, &schemaDesc))
				{
					auto schemaName = PUBLIC_SCHEMA;
					QualifiedName depName;

					if (dependency.has_value() &&
						dependency->objType != obj_any &&
						EVL_field(0, rpb->rpb_record, dependency->fieldId, &desc))
					{
						MOV_get_metaname(tdbb, &desc, depName.object);

						if (MET_qualify_existing_name(tdbb, depName, {dependency->objType}, &schemaSearchPath))
							schemaName = depName.schema.c_str();
					}

					desc.makeText(static_cast<USHORT>(strlen(schemaName)), CS_METADATA,
						(UCHAR*) schemaName);

					MOV_move(tdbb, &desc, &schemaDesc);
					rpb->rpb_record->clearNull(fieldId);
				}
			}
		}

		if (relation->rel_id == rel_priv)
		{
			static constexpr int privSchemaFields[][2] = {
				{f_prv_user_schema, f_prv_u_type},
				{f_prv_rel_schema, f_prv_o_type}
			};

			for (const auto& [schemaFieldId, objTypeFieldId] : privSchemaFields)
			{
				if (rpb->rpb_record->isNull(schemaFieldId) &&
					!rpb->rpb_record->isNull(objTypeFieldId) &&
					EVL_field(0, rpb->rpb_record, objTypeFieldId, &desc2))
				{
					const auto objType = MOV_get_long(tdbb, &desc2, 0);

					switch (objType)
					{
						case obj_relations:
						case obj_views:
						case obj_procedures:
						case obj_functions:
						case obj_packages:
						case obj_generators:
						case obj_domains:
						case obj_exceptions:
						case obj_charsets:
						case obj_collations:

						case obj_relation:
						case obj_view:
						case obj_trigger:
						case obj_procedure:
						case obj_exception:
						case obj_field:
						case obj_index:
						case obj_charset:
						case obj_generator:
						case obj_udf:
						case obj_collation:
						case obj_package_header:
						case obj_package_body:
							EVL_field(0, rpb->rpb_record, schemaFieldId, &desc2);

							desc.makeText(static_cast<USHORT>(strlen(PUBLIC_SCHEMA)), CS_METADATA,
								(UCHAR*) PUBLIC_SCHEMA);

							MOV_move(tdbb, &desc, &desc2);
							rpb->rpb_record->clearNull(schemaFieldId);

							break;
					}
				}
			}

			desc2.setNull();
		}

		gbak_put_search_system_schema_flag(tdbb, rpb, transaction);
	}

	desc.setNull();

	if (needDfw(tdbb, transaction))
	{
		switch ((RIDS) relation->rel_id)
		{
		case rel_pages:
		case rel_formats:
		case rel_trans:
		case rel_rcon:
		case rel_refc:
		case rel_ccon:
		case rel_sec_users:
		case rel_sec_user_attributes:
		case rel_msgs:
		case rel_prc_prms:
		case rel_args:
		case rel_auth_mapping:
		case rel_dpds:
		case rel_dims:
		case rel_segments:
			protect_system_table_insert(tdbb, request, relation);
			break;

		case rel_roles:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_rol_name, &desc);
			if (set_security_class(tdbb, rpb->rpb_record, f_rol_class))
				DFW_post_work(transaction, dfw_grant, &desc, nullptr, obj_sql_role);
			break;

		case rel_db_creators:
			if (!tdbb->getAttachment()->locksmith(tdbb, GRANT_REVOKE_ANY_DDL_RIGHT))
				protect_system_table_insert(tdbb, request, relation);
			break;

		case rel_types:
			if (!(tdbb->getDatabase()->dbb_flags & DBB_creating))
			{
				if (!tdbb->getAttachment()->locksmith(tdbb, CREATE_USER_TYPES))
					protect_system_table_insert(tdbb, request, relation, true);
				else if (EVL_field(0, rpb->rpb_record, f_typ_sys_flag, &desc) && MOV_get_long(tdbb, &desc, 0))
		 			protect_system_table_insert(tdbb, request, relation, true);
			}
			break;

		case rel_log:
		case rel_global_auth_mapping:
			protect_system_table_insert(tdbb, request, relation, true);
			break;

		case rel_database:
			protect_system_table_insert(tdbb, request, relation);
			if (set_security_class(tdbb, rpb->rpb_record, f_dat_class))
				DFW_post_work(transaction, dfw_grant, {}, {}, obj_database);
			break;

		case rel_schemas:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_sch_schema, &desc);
			if (set_security_class(tdbb, rpb->rpb_record, f_sch_class))
				DFW_post_work(transaction, dfw_grant, &desc, nullptr, obj_schema);
			break;

		case rel_relations:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_rel_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_rel_name, &desc);
			DFW_post_work(transaction, dfw_create_relation, &desc, &schemaDesc, 0);
			DFW_post_work(transaction, dfw_update_format, &desc, &schemaDesc, 0);
			set_system_flag(tdbb, rpb->rpb_record, f_rel_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_rel_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_rel_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_relation);
			break;

		case rel_packages:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_pkg_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_pkg_name, &desc);
			set_system_flag(tdbb, rpb->rpb_record, f_pkg_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_pkg_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_pkg_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_package_header);
			break;

		case rel_procedures:
			protect_system_table_insert(tdbb, request, relation);

			EVL_field(0, rpb->rpb_record, f_prc_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_prc_name, &desc);

			if (EVL_field(0, rpb->rpb_record, f_prc_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			object_id = set_metadata_id(tdbb, rpb->rpb_record,
										f_prc_id, drq_g_nxt_prc_id, "RDB$PROCEDURES");

			work = DFW_post_work(transaction, dfw_create_procedure, &desc, &schemaDesc, object_id, object_name.package);

			{ // scope
				bool check_blr = true;
				if (EVL_field(0, rpb->rpb_record, f_prc_valid_blr, &desc2))
					check_blr = MOV_get_long(tdbb, &desc2, 0) != 0;

				if (check_blr)
					DFW_post_work_arg(transaction, work, nullptr, nullptr, 0, dfw_arg_check_blr);
			} // scope

			set_system_flag(tdbb, rpb->rpb_record, f_prc_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_prc_owner);

			if (object_name.package.isEmpty())
			{
				if (set_security_class(tdbb, rpb->rpb_record, f_prc_class))
					DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_procedure);
			}
			break;

		case rel_funs:
			protect_system_table_insert(tdbb, request, relation);

			EVL_field(0, rpb->rpb_record, f_fun_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_fun_name, &desc);

			if (EVL_field(0, rpb->rpb_record, f_fun_pkg_name, &desc2))
				MOV_get_metaname(tdbb, &desc2, object_name.package);

			object_id = set_metadata_id(tdbb, rpb->rpb_record,
										f_fun_id, drq_g_nxt_fun_id, "RDB$FUNCTIONS");

			work = DFW_post_work(transaction, dfw_create_function, &desc, &schemaDesc, object_id, object_name.package);

			{ // scope
				bool check_blr = true;
				if (EVL_field(0, rpb->rpb_record, f_fun_valid_blr, &desc2))
					check_blr = MOV_get_long(tdbb, &desc2, 0) != 0;

				if (check_blr)
					DFW_post_work_arg(transaction, work, nullptr, nullptr, 0, dfw_arg_check_blr);
			} // scope

			set_system_flag(tdbb, rpb->rpb_record, f_fun_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_fun_owner);

			if (object_name.package.isEmpty())
			{
				if (set_security_class(tdbb, rpb->rpb_record, f_fun_class))
					DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_udf);
			}
			break;

		case rel_indices:
			protect_system_table_insert(tdbb, request, relation);

			EVL_field(0, rpb->rpb_record, f_idx_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_idx_name, &desc);

			if (EVL_field(0, rpb->rpb_record, f_idx_exp_blr, &desc2))
			{
				DFW_post_work(transaction, dfw_create_expression_index, &desc, &schemaDesc,
							  tdbb->getDatabase()->dbb_max_idx);
			}
			else
				DFW_post_work(transaction, dfw_create_index, &desc, &schemaDesc, tdbb->getDatabase()->dbb_max_idx);

			set_system_flag(tdbb, rpb->rpb_record, f_idx_sys_flag);
			break;

		case rel_rfr:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_rfr_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_rfr_rname, &desc);
			DFW_post_work(transaction, dfw_update_format, &desc, &schemaDesc, 0);
			set_system_flag(tdbb, rpb->rpb_record, f_rfr_sys_flag);
			break;

		case rel_classes:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_cls_class, &desc);
#ifdef DEV_BUILD
			MOV_get_metaname(tdbb, &desc, object_name.object);
			fb_assert(strncmp(object_name.object.c_str(), "SQL$", 4) == 0);
#endif
			DFW_post_work(transaction, dfw_compute_security, &desc, nullptr, 0);
			break;

		case rel_fields:
			EVL_field(0, rpb->rpb_record, f_fld_schema, &schemaDesc);
			MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
			EVL_field(0, rpb->rpb_record, f_fld_name, &desc);
			MOV_get_metaname(tdbb, &desc, object_name.object);
			SCL_check_domain(tdbb, object_name, SCL_create);
			DFW_post_work(transaction, dfw_create_field, &desc, &schemaDesc, 0);
			set_system_flag(tdbb, rpb->rpb_record, f_fld_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_fld_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_fld_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_field);
			break;

		case rel_filters:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_flt_name, &desc);
			if (set_security_class(tdbb, rpb->rpb_record, f_flt_class))
				DFW_post_work(transaction, dfw_grant, &desc, nullptr, obj_blob_filter);
			break;

		case rel_files:
			protect_system_table_insert(tdbb, request, relation);
			{
				const bool nameDefined = EVL_field(0, rpb->rpb_record, f_file_name, &desc);

				const auto shadowNumber = EVL_field(0, rpb->rpb_record, f_file_shad_num, &desc2) ?
					MOV_get_long(tdbb, &desc2, 0) : 0;

				const auto fileFlags = EVL_field(0, rpb->rpb_record, f_file_flags, &desc2) ?
					MOV_get_long(tdbb, &desc2, 0) : 0;

				if (shadowNumber)
				{
					if (!(fileFlags & FILE_inactive))
						DFW_post_work(transaction, dfw_add_shadow, &desc, nullptr, 0);
				}
				else if (fileFlags & FILE_difference)
				{
					if (nameDefined)
						DFW_post_work(transaction, dfw_add_difference, &desc, nullptr, 0);

					if (fileFlags & FILE_backing_up)
						DFW_post_work(transaction, dfw_begin_backup, &desc, nullptr, 0);
				}
			}
			// Nullify the unsupported fields
			rpb->rpb_record->setNull(f_file_seq);
			rpb->rpb_record->setNull(f_file_start);
			rpb->rpb_record->setNull(f_file_length);
			break;

		case rel_triggers:
			EVL_field(0, rpb->rpb_record, f_trg_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_trg_rname, &desc);

			// check if this  request go through without checking permissions
			if (!(request->getStatement()->flags & (Statement::FLAG_IGNORE_PERM | Statement::FLAG_INTERNAL)))
			{
				MOV_get_metaname(tdbb, &schemaDesc, object_name.schema);
				MOV_get_metaname(tdbb, &desc, object_name.object);
				SCL_check_relation(tdbb, object_name, SCL_control | SCL_alter);
			}

			if (EVL_field(0, rpb->rpb_record, f_trg_rname, &desc2))
				DFW_post_work(transaction, dfw_update_format, &desc2, &schemaDesc, 0);

			EVL_field(0, rpb->rpb_record, f_trg_name, &desc);
			work = DFW_post_work(transaction, dfw_create_trigger, &desc, &schemaDesc, 0);

			if (!(desc2.dsc_flags & DSC_null))
				DFW_post_work_arg(transaction, work, &desc2, &schemaDesc, 0, dfw_arg_rel_name);

			if (EVL_field(0, rpb->rpb_record, f_trg_type, &desc2))
			{
				DFW_post_work_arg(transaction, work, &desc2, &schemaDesc,
					(USHORT) MOV_get_int64(tdbb, &desc2, 0), dfw_arg_trg_type);
			}
			set_system_flag(tdbb, rpb->rpb_record, f_trg_sys_flag);
			break;

		case rel_priv:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_prv_rel_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_prv_rname, &desc);
			EVL_field(0, rpb->rpb_record, f_prv_o_type, &desc2);
			object_id = MOV_get_long(tdbb, &desc2, 0);
			DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, object_id);
			break;

		case rel_vrel:
			protect_system_table_insert(tdbb, request, relation);
			// If RDB$CONTEXT_TYPE is NULL, ask DFW to populate it.
			if (!EVL_field(0, rpb->rpb_record, f_vrl_context_type, &desc))
			{
				if (EVL_field(0, rpb->rpb_record, f_vrl_vname, &desc) &&
					EVL_field(0, rpb->rpb_record, f_vrl_context, &desc2))
				{
					EVL_field(0, rpb->rpb_record, f_vrl_schema, &schemaDesc);

					const USHORT id = MOV_get_long(tdbb, &desc2, 0);
					DFW_post_work(transaction, dfw_store_view_context_type, &desc, &schemaDesc, id);
				}
			}
			break;

		case rel_gens:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_gen_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_gen_name, &desc);
			EVL_field(0, rpb->rpb_record, f_gen_id, &desc2);
			object_id = set_metadata_id(tdbb, rpb->rpb_record,
										f_gen_id, drq_g_nxt_gen_id, MASTER_GENERATOR);
			transaction->getGenIdCache()->put(object_id, 0);
			DFW_post_work(transaction, dfw_set_generator, &desc, &schemaDesc, object_id);
			set_system_flag(tdbb, rpb->rpb_record, f_gen_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_gen_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_gen_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_generator);
			break;

		case rel_charsets:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_cs_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_cs_cs_name, &desc);
			set_system_flag(tdbb, rpb->rpb_record, f_cs_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_cs_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_cs_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_charset);
			break;

		case rel_collations:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_coll_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_coll_name, &desc);
			set_system_flag(tdbb, rpb->rpb_record, f_coll_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_coll_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_coll_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_collation);
			break;

		case rel_exceptions:
			protect_system_table_insert(tdbb, request, relation);
			EVL_field(0, rpb->rpb_record, f_xcp_schema, &schemaDesc);
			EVL_field(0, rpb->rpb_record, f_xcp_name, &desc);
			set_metadata_id(tdbb, rpb->rpb_record,
							f_xcp_number, drq_g_nxt_xcp_id, "RDB$EXCEPTIONS");
			set_system_flag(tdbb, rpb->rpb_record, f_xcp_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_xcp_owner);
			if (set_security_class(tdbb, rpb->rpb_record, f_xcp_class))
				DFW_post_work(transaction, dfw_grant, &desc, &schemaDesc, obj_exception);
			break;

		case rel_backup_history:
			if (!tdbb->getAttachment()->locksmith(tdbb, USE_NBACKUP_UTILITY))
				protect_system_table_insert(tdbb, request, relation);
			set_nbackup_id(tdbb, rpb->rpb_record,
							f_backup_id, drq_g_nxt_nbakhist_id, "RDB$BACKUP_HISTORY");
			break;

		case rel_pubs:
			protect_system_table_insert(tdbb, request, relation);
			set_system_flag(tdbb, rpb->rpb_record, f_pub_sys_flag);
			set_owner_name(tdbb, rpb->rpb_record, f_pub_owner);
			break;

		case rel_pub_tables:
			protect_system_table_insert(tdbb, request, relation);
			DFW_post_work(transaction, dfw_change_repl_state, {}, {}, 1);
			break;

		default:    // Shut up compiler warnings
			break;
		}
	}

	// this should be scheduled even in database creation (system transaction)
	switch ((RIDS) relation->rel_id)
	{
		case rel_collations:
			{
				EVL_field(0, rpb->rpb_record, f_coll_schema, &schemaDesc);

				EVL_field(0, rpb->rpb_record, f_coll_cs_id, &desc);
				USHORT id = MOV_get_long(tdbb, &desc, 0);

				EVL_field(0, rpb->rpb_record, f_coll_id, &desc);
				id = INTL_CS_COLL_TO_TTYPE(id, MOV_get_long(tdbb, &desc, 0));

				EVL_field(0, rpb->rpb_record, f_coll_name, &desc);
				DFW_post_work(transaction, dfw_create_collation, &desc, &schemaDesc, id);
			}
			break;

		default:	// Shut up compiler warnings
			break;
	}

	rpb->rpb_b_page = 0;
	rpb->rpb_b_line = 0;
	rpb->rpb_flags = 0;
	rpb->rpb_transaction_nr = transaction->tra_number;
	rpb->getWindow(tdbb).win_flags = 0;
	rpb->rpb_record->pushPrecedence(PageNumber(TRANS_PAGE_SPACE, rpb->rpb_transaction_nr));
	DPM_store(tdbb, rpb, rpb->rpb_record->getPrecedence(), DPM_primary);

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	if (!(transaction->tra_flags & TRA_system) &&
		transaction->tra_save_point && transaction->tra_save_point->isChanging())
	{
		verb_post(tdbb, transaction, rpb, 0);
	}

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_INSERTS, relation->rel_id);

	// for an autocommit transaction, mark a commit as necessary

	if (transaction->tra_flags & TRA_autocommit)
		transaction->tra_flags |= TRA_perform_autocommit;
}


bool VIO_sweep(thread_db* tdbb, jrd_tra* transaction, TraceSweepEvent* traceSweep)
{
/**************************************
 *
 *	V I O _ s w e e p
 *
 **************************************
 *
 * Functional description
 *	Make a garbage collection pass.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* const dbb = tdbb->getDatabase();
	Jrd::Attachment* attachment = tdbb->getAttachment();

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE,
		"VIO_sweep (transaction %" SQUADFORMAT")\n", transaction ? transaction->tra_number : 0);
#endif

	if (transaction->tra_attachment->att_flags & ATT_NO_CLEANUP)
		return false;

	DPM_scan_pages(tdbb);

	if (attachment->att_parallel_workers != 0)
	{
		EngineCheckout cout(tdbb, FB_FUNCTION);

		Coordinator coord(dbb->dbb_permanent);
		SweepTask sweep(tdbb, dbb->dbb_permanent, traceSweep);

		FbLocalStatus local_status;
		local_status->init();

		coord.runSync(&sweep);

		if (!sweep.getResult(&local_status))
			local_status.raise();

		return true;
	}


	// hvlad: restore tdbb->transaction since it can be used later
	tdbb->setTransaction(transaction);

	record_param rpb;
	rpb.rpb_record = NULL;
	rpb.rpb_stream_flags = RPB_s_no_data | RPB_s_sweeper;
	rpb.getWindow(tdbb).win_flags = WIN_large_scan;

	jrd_rel* relation = NULL; // wasn't initialized: memory problem in catch () part.
	vec<jrd_rel*>* vector = NULL;

	GarbageCollector* gc = dbb->dbb_garbage_collector;
	bool ret = true;

	try {

		for (FB_SIZE_T i = 1; (vector = attachment->att_relations) && i < vector->count(); i++)
		{
			relation = (*vector)[i];
			if (relation)
				relation = MET_lookup_relation_id(tdbb, i, false);

			if (relation &&
				!(relation->rel_flags & (REL_deleted | REL_deleting)) &&
				!relation->isTemporary() &&
				relation->getPages(tdbb)->rel_pages)
			{
				jrd_rel::GCShared gcGuard(tdbb, relation);
				if (!gcGuard.gcEnabled())
				{
					ret = false;
					break;
				}

				rpb.rpb_relation = relation;
				rpb.rpb_number.setValue(BOF_NUMBER);
				rpb.rpb_org_scans = relation->rel_scan_count++;

				traceSweep->beginSweepRelation(relation);

				if (gc) {
					gc->sweptRelation(transaction->tra_oldest_active, relation->rel_id);
				}

				while (VIO_next_record(tdbb, &rpb, transaction, 0, DPM_next_all))
				{
					CCH_RELEASE(tdbb, &rpb.getWindow(tdbb));

					if (relation->rel_flags & REL_deleting)
						break;

					JRD_reschedule(tdbb);

					transaction->tra_oldest_active = dbb->dbb_oldest_snapshot;
					if (TipCache* cache = dbb->dbb_tip_cache)
						cache->updateActiveSnapshots(tdbb, &attachment->att_active_snapshots);
				}

				traceSweep->endSweepRelation(relation);

				--relation->rel_scan_count;
			}
		}

		delete rpb.rpb_record;

	}	// try
	catch (const Exception&)
	{
		delete rpb.rpb_record;

		if (relation)
		{
			if (relation->rel_scan_count)
				--relation->rel_scan_count;
		}

		ERR_punt();
	}

	return ret;
}


WriteLockResult VIO_writelock(thread_db* tdbb, record_param* org_rpb, jrd_tra* transaction)
{
/**************************************
 *
 *	V I O _ w r i t e l o c k
 *
 **************************************
 *
 * Functional description
 *	Modify record to make record owned by this transaction
 *
 **************************************/
	SET_TDBB(tdbb);

	jrd_rel* const relation = org_rpb->rpb_relation;
#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES,
		"VIO_writelock (rel_id %u, org_rpb %" QUADFORMAT"d, transaction %" SQUADFORMAT")\n",
		relation->rel_id, org_rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0);

	VIO_trace(DEBUG_WRITES_INFO,
		"   old record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		org_rpb->rpb_page, org_rpb->rpb_line, org_rpb->rpb_transaction_nr,
		org_rpb->rpb_flags, org_rpb->rpb_b_page, org_rpb->rpb_b_line,
		org_rpb->rpb_f_page, org_rpb->rpb_f_line);
#endif

	const bool skipLocked = org_rpb->rpb_stream_flags & RPB_s_skipLocked;

	if (transaction->tra_flags & TRA_system)
	{
		// Explicit locks are not needed in system transactions
		return WriteLockResult::LOCKED;
	}

	if (org_rpb->rpb_runtime_flags & (RPB_refetch | RPB_undo_read))
	{
		if (!VIO_refetch_record(tdbb, org_rpb, transaction, true, true))
			return WriteLockResult::CONFLICTED;

		org_rpb->rpb_runtime_flags &= ~RPB_refetch;
		fb_assert(!(org_rpb->rpb_runtime_flags & RPB_undo_read));
	}

	if (org_rpb->rpb_transaction_nr == transaction->tra_number)
	{
		// We already own this record, thus no writelock is required
		return WriteLockResult::LOCKED;
	}

	transaction->tra_flags |= TRA_write;

	Record* org_record = org_rpb->rpb_record;
	if (!org_record)
	{
		org_record = VIO_record(tdbb, org_rpb, NULL, tdbb->getDefaultPool());
		org_rpb->rpb_address = org_record->getData();
		const Format* const org_format = org_record->getFormat();
		org_rpb->rpb_length = org_format->fmt_length;
		org_rpb->rpb_format_number = org_format->fmt_version;
	}

	// Set up the descriptor for the new record version. Initially,
	// it points to the same record data as the original one.
	record_param new_rpb = *org_rpb;
	new_rpb.rpb_transaction_nr = transaction->tra_number;

	AutoPtr<Record> new_record;
	const Format* const new_format = MET_current(tdbb, relation);

	// If the fetched record is not in the latest format, upgrade it.
	// To do that, allocate new record buffer and make the new record
	// descriptor to point there, then copy the record data.
	if (new_format->fmt_version != new_rpb.rpb_format_number)
	{
		new_rpb.rpb_record = NULL;
		new_record = VIO_record(tdbb, &new_rpb, new_format, tdbb->getDefaultPool());
		new_rpb.rpb_address = new_record->getData();
		new_rpb.rpb_length = new_format->fmt_length;
		new_rpb.rpb_format_number = new_format->fmt_version;

		VIO_copy_record(tdbb, relation, org_record, new_record);
	}

	// We're about to lock the record. Post a refetch request
	// to all the active cursors positioned at this record.

	invalidate_cursor_records(transaction, &new_rpb);

	const bool backVersion = (org_rpb->rpb_b_page != 0);
	record_param temp;
	PageStack stack;
	switch (prepare_update(tdbb, transaction, org_rpb->rpb_transaction_nr, org_rpb, &temp, &new_rpb,
						   stack, true))
	{
		case PrepareResult::SUCCESS:
			break;

		case PrepareResult::DELETED:
			if (skipLocked && (transaction->tra_flags & TRA_read_committed))
				return WriteLockResult::SKIPPED;
			[[fallthrough]];

		case PrepareResult::CONFLICT:
			if ((transaction->tra_flags & TRA_read_consistency))
			{
				Request* top_request = tdbb->getRequest()->req_snapshot.m_owner;
				if (top_request && !(top_request->req_flags & req_update_conflict))
				{
					if (!(top_request->req_flags & req_restart_ready))
					{
						ERR_post(Arg::Gds(isc_deadlock) <<
								 Arg::Gds(isc_update_conflict) <<
								 Arg::Gds(isc_concurrent_transaction) << Arg::Int64(org_rpb->rpb_transaction_nr));
					}

					top_request->req_flags |= req_update_conflict;
					top_request->req_conflict_txn = org_rpb->rpb_transaction_nr;
				}
			}
			org_rpb->rpb_runtime_flags |= RPB_refetch;
			return WriteLockResult::CONFLICTED;

		case PrepareResult::SKIP_LOCKED:
			fb_assert(skipLocked);
			if (skipLocked)
				return WriteLockResult::SKIPPED;
			[[fallthrough]];

		case PrepareResult::LOCK_ERROR:
			// We got some kind of locking error (deadlock, timeout or lock_conflict)
			// Error details should be stuffed into status vector at this point
			// hvlad: we have no details as TRA_wait has already cleared the status vector

			// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
			ERR_post(Arg::Gds(isc_deadlock) <<
						Arg::Gds(isc_update_conflict) <<
						Arg::Gds(isc_concurrent_transaction) << Arg::Int64(org_rpb->rpb_transaction_nr));
	}

	// Old record was restored and re-fetched for write.  Now replace it.

	org_rpb->rpb_transaction_nr = new_rpb.rpb_transaction_nr;
	org_rpb->rpb_format_number = new_rpb.rpb_format_number;
	org_rpb->rpb_b_page = temp.rpb_page;
	org_rpb->rpb_b_line = temp.rpb_line;
	org_rpb->rpb_address = new_rpb.rpb_address;
	org_rpb->rpb_length = new_rpb.rpb_length;
	org_rpb->rpb_flags &= ~(rpb_delta | rpb_uk_modified);
	org_rpb->rpb_flags |= new_rpb.rpb_flags & rpb_delta;

	replace_record(tdbb, org_rpb, &stack, transaction);

	if (!(transaction->tra_flags & TRA_system) && transaction->tra_save_point)
		verb_post(tdbb, transaction, org_rpb, 0);

	// for an autocommit transaction, mark a commit as necessary

	if (transaction->tra_flags & TRA_autocommit)
		transaction->tra_flags |= TRA_perform_autocommit;

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_LOCKS, relation->rel_id);

	// VIO_writelock
	Database* dbb = tdbb->getDatabase();
	if (backVersion && !(tdbb->getAttachment()->att_flags & ATT_no_cleanup) &&
		(dbb->dbb_flags & DBB_gc_cooperative))
	{
		jrd_rel::GCShared gcGuard(tdbb, org_rpb->rpb_relation);
		if (gcGuard.gcEnabled())
		{
			temp.rpb_number = org_rpb->rpb_number;
			if (DPM_get(tdbb, &temp, LCK_read))
				VIO_intermediate_gc(tdbb, &temp, transaction);
		}
	}
	else if ((dbb->dbb_flags & DBB_gc_background) && !org_rpb->rpb_relation->isTemporary())
	{
		notify_garbage_collector(tdbb, org_rpb, transaction->tra_number);
	}

	return WriteLockResult::LOCKED;
}


static int check_precommitted(const jrd_tra* transaction, const record_param* rpb)
{
/*********************************************
 *
 *	c h e c k _ p r e c o m m i t t e d
 *
 *********************************************
 *
 * Functional description
 *	Check if precommitted transaction which created given record version is
 *  current transaction or it is a still active and belongs to the current
 *	attachment. This is needed to detect visibility of records modified in
 *	temporary tables in read-only transactions.
 *
 **************************************/
	if (!(rpb->rpb_flags & rpb_gc_active) && rpb->rpb_relation->isTemporary())
	{
		if (transaction->tra_number == rpb->rpb_transaction_nr)
			return tra_us;

		const jrd_tra* tx = transaction->tra_attachment->att_transactions;
		for (; tx; tx = tx->tra_next)
		{
			if (tx->tra_number == rpb->rpb_transaction_nr)
				return tra_active;
		}
	}

	return tra_committed;
}


static void check_rel_field_class(thread_db* tdbb,
								  record_param* rpb,
								  jrd_tra* transaction)
{
/*********************************************
 *
 *	c h e c k _ r e l _ f i e l d _ c l a s s
 *
 *********************************************
 *
 * Functional description
 *	Given rpb for a record in the nam_r_fields system relation,
 *  containing a security class, check that record itself or
 *	relation, whom it belongs, are OK for given flags.
 *
 **************************************/
	SET_TDBB(tdbb);

	DSC schemaDesc, desc;
	EVL_field(0, rpb->rpb_record, f_rfr_schema, &schemaDesc);
	EVL_field(0, rpb->rpb_record, f_rfr_rname, &desc);
	DFW_post_work(transaction, dfw_update_format, &desc, &schemaDesc, 0);
}

static void check_class(thread_db* tdbb,
						jrd_tra* transaction,
						record_param* org_rpb,
						record_param* new_rpb,
						USHORT id)
{
/**************************************
 *
 *	c h e c k _ c l a s s
 *
 **************************************
 *
 * Functional description
 *	A record in a system relation containing a security class is
 *	being changed.  Check to see if the security class has changed,
 *	and if so, post the change.
 *
 **************************************/
	SET_TDBB(tdbb);

	dsc desc1, desc2;
	const bool flag_org = EVL_field(0, org_rpb->rpb_record, id, &desc1);
	const bool flag_new = EVL_field(0, new_rpb->rpb_record, id, &desc2);

	if (!flag_new || (flag_org && !MOV_compare(tdbb, &desc1, &desc2)))
		return;

	DFW_post_work(transaction, dfw_compute_security, &desc2, nullptr, 0);
}


static bool check_nullify_source(thread_db* tdbb,
								 record_param* org_rpb,
								 record_param* new_rpb,
								 int field_id_1,
								 int field_id_2)
{
/**************************************
 *
 *	c h e c k _ n u l l i f y _ s o u r c e
 *
 **************************************
 *
 * Functional description
 *	A record in a system relation containing a source blob is
 *	being changed.  Check to see if only the source blob has changed,
 *	and if so, validate whether it was an assignment to NULL.
 *
 **************************************/
	if (!tdbb->getAttachment()->locksmith(tdbb, NULL_PRIVILEGE))	// legacy right - no system privilege tuning !!!
		return false;

	bool nullify_found = false;

	dsc org_desc, new_desc;
	for (USHORT iter = 0; iter < org_rpb->rpb_record->getFormat()->fmt_count; ++iter)
	{
		const bool org_null = !EVL_field(NULL, org_rpb->rpb_record, iter, &org_desc);
		const bool new_null = !EVL_field(NULL, new_rpb->rpb_record, iter, &new_desc);

		if ((field_id_1 >= 0 && iter == (USHORT) field_id_1) ||
			(field_id_2 >= 0 && iter == (USHORT) field_id_2))
		{
			fb_assert(org_desc.dsc_dtype == dtype_blob);
			fb_assert(new_desc.dsc_dtype == dtype_blob);

			if (new_null && !org_null)
			{
				nullify_found = true;
				continue;
			}
		}

		if (org_null != new_null || (!new_null && MOV_compare(tdbb, &org_desc, &new_desc)))
			return false;
	}

	return nullify_found;
}


static void check_owner(thread_db* tdbb,
						jrd_tra* transaction,
						record_param* org_rpb,
						record_param* new_rpb,
						USHORT id)
{
/**************************************
 *
 *	c h e c k _ o w n e r
 *
 **************************************
 *
 * Functional description
 *	A record in a system relation containing an owner is
 *	being changed.  Check to see if the owner has changed,
 *	and if so, validate whether this action is allowed.
 *
 **************************************/
	SET_TDBB(tdbb);

	dsc desc1, desc2;
	const bool flag_org = EVL_field(0, org_rpb->rpb_record, id, &desc1);
	const bool flag_new = EVL_field(0, new_rpb->rpb_record, id, &desc2);

	if (!flag_org && !flag_new)
		return;

	if (flag_org && flag_new)
	{
		if (!MOV_compare(tdbb, &desc1, &desc2))
			return;

		const auto attachment = tdbb->getAttachment();
		const MetaString& name = attachment->getEffectiveUserName();

		if (name.hasData())
		{
			desc2.makeText((USHORT) name.length(), CS_METADATA, (UCHAR*) name.c_str());

			if (!MOV_compare(tdbb, &desc1, &desc2))
				return;
		}
	}

	// Note: NULL->USER and USER->NULL changes also cause the error to be raised

	ERR_post(Arg::Gds(isc_protect_ownership));
}


static void check_repl_state(thread_db* tdbb,
							 jrd_tra* transaction,
							 record_param* org_rpb,
							 record_param* new_rpb,
							 USHORT id)
{
/**************************************
 *
 *	c h e c k _ r e p l _ s t a t e
 *
 **************************************
 *
 * Functional description
 *	A record in a system relation containing a replication state is
 *	being changed.  Check to see if the replication state has changed,
 *	and if so, post the change.
 *
 **************************************/
	SET_TDBB(tdbb);

	dsc desc1, desc2;
	const bool flag_org = EVL_field(0, org_rpb->rpb_record, id, &desc1);
	const bool flag_new = EVL_field(0, new_rpb->rpb_record, id, &desc2);

	if (!flag_org && !flag_new)
		return;

	if (flag_org && flag_new && !MOV_compare(tdbb, &desc1, &desc2))
		return;

	DFW_post_work(transaction, dfw_change_repl_state, {}, {}, 0);
}


static void delete_record(thread_db* tdbb, record_param* rpb, ULONG prior_page, MemoryPool* pool)
{
/**************************************
 *
 *	d e l e t e _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Delete a record an all of its fragments.  This assumes the
 *	record has already been fetched for write.  If a pool is given,
 *	the caller has requested that data be fetched as the record is
 *	deleted.
 *
 **************************************/
	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_WRITES,
		"delete_record (rel_id %u, record_param %" QUADFORMAT"d, prior_page %" SLONGFORMAT", pool %p)\n",
		relation->rel_id, rpb->rpb_number.getValue(), prior_page, (void*)pool);

	VIO_trace(DEBUG_WRITES_INFO,
		"   delete_record record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif
	UCHAR* tail = nullptr;
	const UCHAR* tail_end = nullptr;

	Difference difference;

	Record* record = nullptr;
	const Record* prior = nullptr;

	if (pool && !(rpb->rpb_flags & rpb_deleted))
	{
		record = VIO_record(tdbb, rpb, NULL, pool);
		prior = rpb->rpb_prior;

		if (prior)
		{
			tail = difference.getData();
			tail_end = tail + difference.getCapacity();

			if (prior != record)
				record->copyDataFrom(prior);
		}
		else
		{
			tail = record->getData();
			tail_end = tail + record->getLength();
		}

		tail = unpack(rpb, tail_end - tail, tail);
		rpb->rpb_prior = (rpb->rpb_flags & rpb_delta) ? record : nullptr;
	}

	record_param temp_rpb = *rpb;
	DPM_delete(tdbb, &temp_rpb, prior_page);
	tail = delete_tail(tdbb, &temp_rpb, temp_rpb.rpb_page, tail, tail_end);

	if (pool && prior)
	{
		const auto diffLength = tail - difference.getData();
		difference.apply(diffLength, record->getLength(), record->getData());
	}
}


static UCHAR* delete_tail(thread_db* tdbb,
						  record_param* rpb,
						  ULONG prior_page,
						  UCHAR* tail,
						  const UCHAR* tail_end)
{
/**************************************
 *
 *	d e l e t e _ t a i l
 *
 **************************************
 *
 * Functional description
 *	Delete the tail of a record.  If no tail, don't do nuttin'.
 *	If the address of a record tail has been passed, fetch data.
 *
 **************************************/
	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_WRITES,
		"delete_tail (rel_id %u, record_param %" QUADFORMAT"d, prior_page %" SLONGFORMAT", tail %p, length %u)\n",
		relation->rel_id, rpb->rpb_number.getValue(), prior_page, tail, tail_length);

	VIO_trace(DEBUG_WRITES_INFO,
		"   tail of record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	RuntimeStatistics::Accumulator fragments(tdbb, rpb->rpb_relation,
		RuntimeStatistics::RECORD_FRAGMENT_READS);

	while (rpb->rpb_flags & rpb_incomplete)
	{
		rpb->rpb_page = rpb->rpb_f_page;
		rpb->rpb_line = rpb->rpb_f_line;

		// Since the callers are modifying this record, it should not be garbage collected.

		if (!DPM_fetch(tdbb, rpb, LCK_write))
			BUGCHECK(248);		// msg 248 cannot find record fragment

		if (tail)
			tail = unpack(rpb, tail_end - tail, tail);

		DPM_delete(tdbb, rpb, prior_page);
		prior_page = rpb->rpb_page;

		++fragments;
	}

	return tail;
}


static bool dfw_should_know(thread_db* tdbb,
							record_param* org_rpb,
							record_param* new_rpb,
							USHORT irrelevant_field,
							bool void_update_is_relevant)
{
/**************************************
 *
 *	d f w _ s h o u l d _ k n o w
 *
 **************************************
 *
 * Functional description
 * Not all operations on system tables are relevant to inform DFW.
 * In particular, changing comments on objects is irrelevant.
 * Engine often performs empty update to force some tasks (e.g. to
 * recreate index after field type change). So we must return true
 * if relevant field changed or if no fields changed. Or we must
 * return false if only irrelevant field changed.
 *
 **************************************/
	dsc desc2, desc3;
	bool irrelevant_changed = false;
	for (USHORT iter = 0; iter < org_rpb->rpb_record->getFormat()->fmt_count; ++iter)
	{
		const bool flag_org = EVL_field(0, org_rpb->rpb_record, iter, &desc2);
		const bool flag_new = EVL_field(0, new_rpb->rpb_record, iter, &desc3);
		if (flag_org != flag_new || (flag_new && MOV_compare(tdbb, &desc2, &desc3)))
		{
			if (iter != irrelevant_field)
				return true;

			irrelevant_changed = true;
		}
	}
	return void_update_is_relevant ? !irrelevant_changed : false;
}


static void expunge(thread_db* tdbb, record_param* rpb, const jrd_tra* transaction, ULONG prior_page)
{
/**************************************
 *
 *	e x p u n g e
 *
 **************************************
 *
 * Functional description
 *	Expunge a fully mature deleted record.  Get rid of the record
 *	and all of the ancestors.  Be particulary careful since this
 *	can do a lot of damage.
 *
 **************************************/
	SET_TDBB(tdbb);
	Jrd::Attachment* attachment = transaction->tra_attachment;

	fb_assert(assert_gc_enabled(transaction, rpb->rpb_relation));

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_WRITES,
		"expunge (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT
		", prior_page %" SLONGFORMAT")\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0,
		prior_page);
#endif

	if (attachment->att_flags & ATT_no_cleanup)
		return;

	// Re-fetch the record

	if (!DPM_get(tdbb, rpb, LCK_write))
	{
		// expunge
		if (tdbb->getDatabase()->dbb_flags & DBB_gc_background)
			notify_garbage_collector(tdbb, rpb);

		return;
	}

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_WRITES_INFO,
		"   expunge record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	// Make sure it looks kosher and delete the record.

	const TraNumber oldest_snapshot = rpb->rpb_relation->isTemporary() ?
		attachment->att_oldest_snapshot : transaction->tra_oldest_active;

	if (!(rpb->rpb_flags & rpb_deleted) || rpb->rpb_transaction_nr >= oldest_snapshot)
	{

		// expunge
		if (tdbb->getDatabase()->dbb_flags & DBB_gc_background)
			notify_garbage_collector(tdbb, rpb);

		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		return;
	}

	delete_record(tdbb, rpb, prior_page, NULL);

	// If there aren't any old versions, don't worry about garbage collection.

	if (!rpb->rpb_b_page)
		return;

	// Delete old versions fetching data for garbage collection.

	record_param temp = *rpb;
	RecordStack empty_staying;
	garbage_collect(tdbb, &temp, rpb->rpb_page, empty_staying);

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_EXPUNGES, rpb->rpb_relation->rel_id);
}


static void garbage_collect(thread_db* tdbb, record_param* rpb, ULONG prior_page, RecordStack& staying)
{
/**************************************
 *
 *	g a r b a g e _ c o l l e c t
 *
 **************************************
 *
 * Functional description
 *	Garbage collect a chain of back record.  This is called from
 *	"purge" and "expunge."  One enters this routine with an
 *	inactive record_param, describing a records which has either
 *	1) just been deleted or
 *	2) just had its back pointers set to zero
 *	Therefor we can do a fetch on the back pointers we've got
 *	because we have the last existing copy of them.
 *
 **************************************/

	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_WRITES,
		"garbage_collect (rel_id %u, record_param %" QUADFORMAT"d, prior_page %" SLONGFORMAT", staying)\n",
		relation->rel_id, rpb->rpb_number.getValue(), prior_page);

	VIO_trace(DEBUG_WRITES_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	RuntimeStatistics::Accumulator backversions(tdbb, rpb->rpb_relation,
		RuntimeStatistics::RECORD_BACKVERSION_READS);

	// Delete old versions fetching data for garbage collection.

	RecordStack going;

	while (rpb->rpb_b_page)
	{
		rpb->rpb_record = NULL;
		prior_page = rpb->rpb_page;
		rpb->rpb_page = rpb->rpb_b_page;
		rpb->rpb_line = rpb->rpb_b_line;

		if (!DPM_fetch(tdbb, rpb, LCK_write))
			BUGCHECK(291);		// msg 291 cannot find record back version

		delete_record(tdbb, rpb, prior_page, tdbb->getDefaultPool());

		if (rpb->rpb_record)
			going.push(rpb->rpb_record);

		++backversions;

		// Don't monopolize the server while chasing long back version chains.
		JRD_reschedule(tdbb);
	}

	IDX_garbage_collect(tdbb, rpb, going, staying);
	BLB_garbage_collect(tdbb, going, staying, prior_page, rpb->rpb_relation);

	clearRecordStack(going);
}

void VIO_garbage_collect_idx(thread_db* tdbb, jrd_tra* transaction,
								record_param* org_rpb,
								Record* old_data)
{
/**************************************
 *
 *	g a r b a g e _ c o l l e c t _ i d x
 *
 **************************************
 *
 * Functional description
 *	Garbage collect indices for which it is
 *	OK for other transactions to create indices with the same
 *	values.
 *
 **************************************/
	SET_TDBB(tdbb);

	// There is no way to quickly check if there is need to clean indices.

	// The data that is going is passed via old_data.
	if (!old_data) // nothing going, nothing to collect
	{
		return;
	}

	// Garbage collect.  Start by getting all existing old versions from disk

	RecordStack going, staying;
	list_staying(tdbb, org_rpb, staying);
	// Add not-so-old versions from undo log for transaction
	transaction->listStayingUndo(org_rpb->rpb_relation, org_rpb->rpb_number.getValue(), staying);

	// The data that is going is passed via old_data. It is up to caller to make sure that it isn't in one of two lists above

	going.push(old_data);

	IDX_garbage_collect(tdbb, org_rpb, going, staying);
	BLB_garbage_collect(tdbb, going, staying, org_rpb->rpb_page, org_rpb->rpb_relation);

	going.pop();

	clearRecordStack(staying);
}

void Database::garbage_collector(Database* dbb)
{
/**************************************
 *
 *	g a r b a g e _ c o l l e c t o r
 *
 **************************************
 *
 * Functional description
 *	Garbage collect the data pages marked in a
 *	relation's garbage collection bitmap. The
 *	hope is that offloading the computation
 *	and I/O burden of garbage collection will
 *	improve query response time and throughput.
 *
 **************************************/
	FbLocalStatus status_vector;

	try
	{
		UserId user;
		user.setUserName("Garbage Collector");

		Jrd::Attachment* const attachment = Jrd::Attachment::create(dbb, nullptr);
		RefPtr<SysStableAttachment> sAtt(FB_NEW SysStableAttachment(attachment));
		attachment->setStable(sAtt);
		attachment->att_filename = dbb->dbb_filename;
		attachment->att_flags |= ATT_garbage_collector;
		attachment->att_user = &user;

		BackgroundContextHolder tdbb(dbb, attachment, &status_vector, FB_FUNCTION);
		Jrd::Attachment::UseCountHolder use(attachment);
		tdbb->markAsSweeper();

		record_param rpb;
		rpb.getWindow(tdbb).win_flags = WIN_garbage_collector;
		rpb.rpb_stream_flags = RPB_s_no_data | RPB_s_sweeper;

		jrd_rel* relation = NULL;
		jrd_tra* transaction = NULL;

		AutoPtr<GarbageCollector> gc(FB_NEW_POOL(*attachment->att_pool) GarbageCollector(
			*attachment->att_pool, dbb));

		try
		{
			LCK_init(tdbb, LCK_OWNER_attachment);
			INI_init(tdbb);
			PAG_header(tdbb, true);
			PAG_attachment_id(tdbb);
			TRA_init(attachment);

			Monitoring::publishAttachment(tdbb);

			dbb->dbb_garbage_collector = gc;

			sAtt->initDone();

			// Notify our creator that we have started
			dbb->dbb_flags |= DBB_garbage_collector;
			dbb->dbb_flags &= ~DBB_gc_starting;
			dbb->dbb_gc_init.release();

			// The garbage collector flag is cleared to request the thread
			// to finish up and exit.

			bool flush = false;

			while (dbb->dbb_flags & DBB_garbage_collector)
			{
				dbb->dbb_flags |= DBB_gc_active;

				// If background thread activity has been suspended because
				// of I/O errors then idle until the condition is cleared.
				// In particular, make worker threads perform their own
				// garbage collection so that errors are reported to users.

				if (dbb->dbb_flags & DBB_suspend_bgio)
				{
					EngineCheckout cout(tdbb, FB_FUNCTION);
					dbb->dbb_gc_sem.tryEnter(10);
					continue;
				}

				// Scan relation garbage collection bitmaps for candidate data pages.
				// Express interest in the relation to prevent it from being deleted
				// out from under us while garbage collection is in-progress.

				bool found = false, gc_exit = false;
				relation = NULL;

				USHORT relID;
				PageBitmap* gc_bitmap = NULL;

				if ((dbb->dbb_flags & DBB_gc_pending) &&
					(gc_bitmap = gc->getPages(dbb->dbb_oldest_snapshot, relID)))
				{
					relation = MET_lookup_relation_id(tdbb, relID, false);
					if (!relation || (relation->rel_flags & (REL_deleted | REL_deleting)))
					{
						delete gc_bitmap;
						gc_bitmap = NULL;
						gc->removeRelation(relID);
					}

					if (gc_bitmap)
					{
						jrd_rel::GCShared gcGuard(tdbb, relation);
						if (!gcGuard.gcEnabled())
							continue;

						rpb.rpb_relation = relation;

						while (gc_bitmap->getFirst())
						{
							const ULONG dp_sequence = gc_bitmap->current();

							if (!(dbb->dbb_flags & DBB_garbage_collector))
							{
								gc_exit = true;
								break;
							}

							if (gc_exit)
								break;

							gc_bitmap->clear(dp_sequence);

							if (!transaction)
							{
								// Start a "precommitted" transaction by using read-only,
								// read committed. Of particular note is the absence of a
								// transaction lock which means the transaction does not
								// inhibit garbage collection by its very existence.

								transaction = TRA_start(tdbb, sizeof(gc_tpb), gc_tpb);
								tdbb->setTransaction(transaction);
							}

							found = flush = true;
							rpb.rpb_number.setValue(((SINT64) dp_sequence * dbb->dbb_max_records) - 1);
							const RecordNumber last(rpb.rpb_number.getValue() + dbb->dbb_max_records);

							// Attempt to garbage collect all records on the data page.

							bool rel_exit = false;

							while (VIO_next_record(tdbb, &rpb, transaction, NULL, DPM_next_data_page))
							{
								CCH_RELEASE(tdbb, &rpb.getWindow(tdbb));

								if (!(dbb->dbb_flags & DBB_garbage_collector))
								{
									gc_exit = true;
									break;
								}

								if (relation->rel_flags & REL_deleting)
								{
									rel_exit = true;
									break;
								}

								if (relation->rel_flags & REL_gc_disabled)
								{
									rel_exit = true;
									break;
								}

								JRD_reschedule(tdbb);

								if (rpb.rpb_number >= last)
									break;

								// Refresh our notion of the oldest transactions for
								// efficient garbage collection. This is very cheap.

								transaction->tra_oldest = dbb->dbb_oldest_transaction;
								transaction->tra_oldest_active = dbb->dbb_oldest_snapshot;
							}

							if (TipCache* cache = dbb->dbb_tip_cache)
								cache->updateActiveSnapshots(tdbb, &attachment->att_active_snapshots);

							if (gc_exit || rel_exit)
								break;
						}

						if (gc_exit)
							break;

						delete gc_bitmap;
						gc_bitmap = NULL;
					}
				}

				// If there's more work to do voluntarily ask to be rescheduled.
				// Otherwise, wait for event notification.

				if (found)
				{
					JRD_reschedule(tdbb, true);
				}
				else
				{
					dbb->dbb_flags &= ~DBB_gc_pending;

					if (flush)
					{
						// As a last resort, flush garbage collected pages to
						// disk. This isn't strictly necessary but contributes
						// to the supply of free pages available for user
						// transactions. It also reduces the likelihood of
						// orphaning free space on lower precedence pages that
						// haven't been written if a crash occurs.

						CCH_flush(tdbb, FLUSH_SWEEP, 0);
						flush = false;

						attachment->mergeStats();
					}

					dbb->dbb_flags &= ~DBB_gc_active;
					EngineCheckout cout(tdbb, FB_FUNCTION);
					dbb->dbb_gc_sem.tryEnter(10);
				}
			}
		}
		catch (const Firebird::Exception& ex)
		{
			ex.stuffException(&status_vector);
			iscDbLogStatus(dbb->dbb_filename.c_str(), &status_vector);
			// continue execution to clean up
		}

		delete rpb.rpb_record;

		dbb->dbb_garbage_collector = NULL;

		if (transaction)
			TRA_commit(tdbb, transaction, false);

		Monitoring::cleanupAttachment(tdbb);
		attachment->releaseLocks(tdbb);
		LCK_fini(tdbb, LCK_OWNER_attachment);

		attachment->releaseRelations(tdbb);
	}	// try
	catch (const Firebird::Exception& ex)
	{
		dbb->exceptionHandler(ex, NULL);
	}

	dbb->dbb_flags &= ~(DBB_garbage_collector | DBB_gc_active | DBB_gc_pending);

	try
	{
		// Notify the finalization caller that we're finishing.
		if (dbb->dbb_flags & DBB_gc_starting)
		{
			dbb->dbb_flags &= ~DBB_gc_starting;
			dbb->dbb_gc_init.release();
		}
	}
	catch (const Firebird::Exception& ex)
	{
		dbb->exceptionHandler(ex, NULL);
	}
}


void Database::exceptionHandler(const Firebird::Exception& ex,
	ThreadFinishSync<Database*>::ThreadRoutine* /*routine*/)
{
	FbLocalStatus status_vector;
	ex.stuffException(&status_vector);
	iscDbLogStatus(dbb_filename.c_str(), &status_vector);
}


static void gbak_put_search_system_schema_flag(thread_db* tdbb, record_param* rpb, jrd_tra* transaction)
{
	static const std::unordered_map<USHORT, std::vector<USHORT>> schemaBlrFields = {
		{rel_args, {f_arg_default}},
		{rel_fields, {f_fld_v_blr, f_fld_computed, f_fld_default, f_fld_missing}},
		{rel_funs, {f_fun_blr}},
		{rel_indices, {f_idx_exp_blr, f_idx_cond_blr}},
		{rel_prc_prms, {f_prm_default}},
		{rel_procedures, {f_prc_blr}},
		{rel_relations, {f_rel_blr}},
		{rel_rfr, {f_rfr_default}},
		{rel_triggers, {f_trg_blr}}
	};

	static const UCHAR bpb[] = {
		isc_bpb_version1,
		isc_bpb_type, 1, isc_bpb_type_stream
	};

	SET_TDBB(tdbb);

	const auto relation = rpb->rpb_relation;
	dsc desc, desc2;

	if (const auto relBlrFields = schemaBlrFields.find(relation->rel_id); relBlrFields != schemaBlrFields.end())
	{
		UCHAR buffer[BUFFER_MEDIUM];

		for (const auto field : relBlrFields->second)
		{
			if (EVL_field(0, rpb->rpb_record, field, &desc))
			{
				AutoBlb blob(tdbb, blb::open(tdbb, transaction, reinterpret_cast<bid*>(desc.dsc_address)));
				bid newBid;
				const auto newBlob = blb::create2(tdbb, transaction, &newBid, sizeof(bpb), bpb);
				bool firstSegment = true;
				UCHAR newHeader[] = {
					0,
					blr_flags,
					blr_flags_search_system_schema,
					0, 0,
					blr_end
				};

				while (!(blob->blb_flags & BLB_eof))
				{
					const auto len = blob->BLB_get_data(tdbb, buffer, sizeof(buffer), false);

					if (len > 1 && firstSegment)
					{
						newHeader[0] = buffer[0];
						fb_assert(newHeader[0] == blr_version4 || newHeader[0] == blr_version5);

						if ((newHeader[0] == blr_version4 || newHeader[0] == blr_version5) &&
							buffer[1] != blr_flags)
						{
							newBlob->BLB_put_data(tdbb, newHeader, sizeof(newHeader));
							newBlob->BLB_put_data(tdbb, buffer + 1, len - 1);

							firstSegment = false;
						}
						else
						{
							newBid.clear();
							break;
						}
					}
					else
						newBlob->BLB_put_data(tdbb, buffer, len);
				}

				newBlob->BLB_close(tdbb);

				if (!newBid.isEmpty())
				{
					desc2.makeBlob(isc_blob_untyped, 0, reinterpret_cast<ISC_QUAD*>(&newBid));
					blb::move(tdbb, &desc2, &desc, relation, rpb->rpb_record, field);
				}
			}
		}
	}
}


static UndoDataRet get_undo_data(thread_db* tdbb, jrd_tra* transaction,
								 record_param* rpb, MemoryPool* pool)
/**********************************************************
 *
 *  g e t _ u n d o _ d a t a
 *
 **********************************************************
 *
 * This is helper routine for the VIO_chase_record_version. It is used to make
 * cursor stable - i.e. cursor should ignore changes made to the record by the
 * inner code. Of course, it is called only when primary record version was
 * created by current transaction:
 *	rpb->rpb_transaction_nr == transaction->tra_number.
 *
 * Possible cases and actions:
 *
 * - If record was not changed under current savepoint, return udNone.
 *	 VIO_chase_record_version should continue own processing.
 *
 * If record was changed under current savepoint, we should read its previous
 * version:
 *
 * - If previous version data is present at undo-log (after update_in_place,
 *	 for ex.), copy it into rpb and return udExists.
 *	 VIO_chase_record_version should return true.
 *
 * - If record was inserted or updated and then deleted under current savepoint
 *	 we should undo two last actions (delete and insert\update), therefore return
 *	 udForceTwice.
 *	 VIO_chase_record_version should continue and read second available back
 *	 version from disk.
 *
 * - Else we need to undo just a last action, so return udForceBack.
 *	 VIO_chase_record_version should continue and read first available back
 *	 version from disk.
 *
 * If record version was restored from undo log mark rpb with RPB_s_undo_data
 * to let caller know that data page is already released.
 *
 **********************************************************/
{
	if (!transaction->tra_save_point)
		return udNone;

	VerbAction* const action = transaction->tra_save_point->getAction(rpb->rpb_relation);

	if (action)
	{
		const SINT64 recno = rpb->rpb_number.getValue();
		if (!RecordBitmap::test(action->vct_records, recno))
			return udNone;

		rpb->rpb_runtime_flags |= RPB_undo_read;
		if (rpb->rpb_flags & rpb_deleted)
			rpb->rpb_runtime_flags |= RPB_undo_deleted;

		if (!action->vct_undo || !action->vct_undo->locate(recno))
			return udForceBack;

		const UndoItem& undo = action->vct_undo->current();

		rpb->rpb_runtime_flags |= RPB_undo_data;
		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

		AutoTempRecord undoRecord(undo.setupRecord(transaction));

		Record* const record = VIO_record(tdbb, rpb, undoRecord->getFormat(), pool);
		record->copyFrom(undoRecord);

		rpb->rpb_flags &= ~rpb_deleted;
		return udExists;
	}

	return udNone;
}


static void invalidate_cursor_records(jrd_tra* transaction, record_param* mod_rpb)
{
/**************************************
 *
 *	i n v a l i d a t e _ c u r s o r _ r e c o r d s
 *
 **************************************
 *
 * Functional description
 *	Post a refetch request to the records currently fetched
 *  by active cursors of our transaction, because those records
 *  have just been updated or deleted.
 *
 **************************************/
	fb_assert(mod_rpb && mod_rpb->rpb_relation);

	for (Request* request = transaction->tra_requests; request; request = request->req_tra_next)
	{
		if (request->req_flags & req_active)
		{
			for (FB_SIZE_T i = 0; i < request->req_rpb.getCount(); i++)
			{
				record_param* const org_rpb = &request->req_rpb[i];

				if (org_rpb != mod_rpb &&
					org_rpb->rpb_relation && org_rpb->rpb_number.isValid() &&
					org_rpb->rpb_relation->rel_id == mod_rpb->rpb_relation->rel_id &&
					org_rpb->rpb_number == mod_rpb->rpb_number)
				{
					org_rpb->rpb_runtime_flags |= RPB_refetch;
				}
			}
		}
	}
}


static void list_staying_fast(thread_db* tdbb, record_param* rpb, RecordStack& staying,
	record_param* back_rpb, int flags)
{
/**************************************
*
*	l i s t _ s t a y i n g _ f a s t
*
**************************************
*
* Functional description
*	Get all the data that's staying so we can clean up indexes etc.
*	without losing anything. This is fast version of old list_staying.
*   It is used when current transaction owns the record and thus guaranteed
*   that versions chain is not changed during walking.
*
**************************************/
	record_param temp = *rpb;

	if (!(flags & LS_ACTIVE_RPB) && !DPM_fetch(tdbb, &temp, LCK_read))
	{
		// It is impossible as our transaction owns the record
		BUGCHECK(186);	// msg 186 record disappeared
		return;
	}

	fb_assert(temp.rpb_b_page == rpb->rpb_b_page);
	fb_assert(temp.rpb_b_line == rpb->rpb_b_line);

	fb_assert((temp.rpb_flags & ~(rpb_incomplete | rpb_not_packed)) ==
			  (rpb->rpb_flags & ~(rpb_incomplete | rpb_not_packed)));

	Record* backout_rec = NULL;
	RuntimeStatistics::Accumulator backversions(tdbb, rpb->rpb_relation,
		RuntimeStatistics::RECORD_BACKVERSION_READS);

	if (temp.rpb_flags & rpb_deleted)
	{
		CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
	}
	else
	{
		temp.rpb_record = NULL;

		// VIO_data below could change the flags
		const bool backout = (temp.rpb_flags & rpb_gc_active);
		VIO_data(tdbb, &temp, tdbb->getDefaultPool());

		if (!backout)
			staying.push(temp.rpb_record);
		else
		{
			fb_assert(!backout_rec);
			backout_rec = temp.rpb_record;
		}
	}

	///const TraNumber oldest_active = tdbb->getTransaction()->tra_oldest_active;

	while (temp.rpb_b_page)
	{
		///ULONG page = temp.rpb_page = temp.rpb_b_page;
		///USHORT line = temp.rpb_line = temp.rpb_b_line;
		temp.rpb_page = temp.rpb_b_page;
		temp.rpb_line = temp.rpb_b_line;

		temp.rpb_record = NULL;

		if (temp.rpb_flags & rpb_delta)
			fb_assert(temp.rpb_prior != NULL);
		else
			fb_assert(temp.rpb_prior == NULL);

		if (!DPM_fetch(tdbb, &temp, LCK_read))
		{
			fb_assert(false);
			clearRecordStack(staying);
			return;
		}

		if (!(temp.rpb_flags & rpb_chained) || (temp.rpb_flags & (rpb_blob | rpb_fragment)))
		{
			fb_assert(false);
			clearRecordStack(staying);
			CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
			return;
		}

		VIO_data(tdbb, &temp, tdbb->getDefaultPool());
		staying.push(temp.rpb_record);

		++backversions;

		/***
		if (temp.rpb_transaction_nr < oldest_active && temp.rpb_b_page)
		{
			temp.rpb_page = page;
			temp.rpb_line = line;

			record_param temp2 = temp;
			if (DPM_fetch(tdbb, &temp, LCK_write))
			{
				temp.rpb_b_page = 0;
				temp.rpb_b_line = 0;
				temp.rpb_flags &= ~(rpb_delta | rpb_gc_active);
				CCH_MARK(tdbb, &temp.getWindow(tdbb));
				DPM_rewrite_header(tdbb, &temp);
				CCH_RELEASE(tdbb, &temp.getWindow(tdbb));

				garbage_collect(tdbb, &temp2, temp.rpb_page, staying);

				tdbb->bumpRelStats(RuntimeStatistics::RECORD_PURGES, temp.rpb_relation->rel_id);

				if (back_rpb && back_rpb->rpb_page == page && back_rpb->rpb_line == line)
				{
					back_rpb->rpb_b_page = 0;
					back_rpb->rpb_b_line = 0;
				}
				break;
			}
		}
		***/

		// Don't monopolize the server while chasing long back version chains.
		JRD_reschedule(tdbb);
	}

	delete backout_rec;
}


static void list_staying(thread_db* tdbb, record_param* rpb, RecordStack& staying, int flags)
{
/**************************************
 *
 *	l i s t _ s t a y i n g
 *
 **************************************
 *
 * Functional description
 *	Get all the data that's staying so we can clean up indexes etc.
 *	without losing anything.  Note that in the middle somebody could
 *	modify the record -- worse yet, somebody could modify it, commit,
 *	and have somebody else modify it, so if the back pointers on the
 *	original record change throw out what we've got and start over.
 *	"All the data that's staying" is: all the versions of the input
 *	record (rpb) that are stored in the relation.
 *
 **************************************/
	SET_TDBB(tdbb);

	// Use fast way if possible
	if (rpb->rpb_transaction_nr)
	{
		jrd_tra* transaction = tdbb->getTransaction();
		if (transaction && transaction->tra_number == rpb->rpb_transaction_nr)
		{
			list_staying_fast(tdbb, rpb, staying, NULL, flags);
			return;
		}
	}

	Record* data = rpb->rpb_prior;
	Record* backout_rec = NULL;
	ULONG next_page = rpb->rpb_page;
	USHORT next_line = rpb->rpb_line;
	int max_depth = 0;
	int depth = 0;

	// 2014-09-11 NS XXX: This algorithm currently has O(n^2) complexity, but can be
	//   significantly optimized in a case when VIO_data doesn't have to chase fragments.
	//   I won't implement this now, because intermediate GC shall reduce likelihood
	//   of encountering long version chains to almost zero.
	RuntimeStatistics::Accumulator backversions(tdbb, rpb->rpb_relation,
												RuntimeStatistics::RECORD_BACKVERSION_READS);


	// Limit number of "restarts" if primary version constantly changed. Currently,
	// LS_ACTIVE_RPB is passed by VIO_intermediate_gc only and it is ok to return
	// empty staying in this case.
	// Should think on this more before merge it into Firebird tree.

	int n = 0, max = (flags & LS_ACTIVE_RPB) ? 3 : 0;
	for (;;)
	{
		// Each time thru the loop, start from the latest version of the record
		// because during the call to VIO_data (below), things might change.

		record_param temp = *rpb;
		depth = 0;

		// If the entire record disappeared, then there is nothing staying.
		if (!(flags & LS_ACTIVE_RPB) && !DPM_fetch(tdbb, &temp, LCK_read))
		{
			clearRecordStack(staying);
			delete backout_rec;
			backout_rec = NULL;
			return;
		}

		flags &= ~LS_ACTIVE_RPB;

		// If anything changed, then start all over again.  This time with the
		// new, latest version of the record.

		if (temp.rpb_b_page != rpb->rpb_b_page || temp.rpb_b_line != rpb->rpb_b_line ||
			temp.rpb_flags != rpb->rpb_flags)
		{
			clearRecordStack(staying);
			delete backout_rec;
			backout_rec = NULL;

			if ((flags & LS_NO_RESTART) || (max && ++n > max))
			{
				CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
				return;
			}

			next_page = temp.rpb_page;
			next_line = temp.rpb_line;
			max_depth = 0;
			*rpb = temp;
		}

		depth++;

		// Each time thru the for-loop, we process the next older version.
		// The while-loop finds this next older version.

		bool timed_out = false;
		while (temp.rpb_b_page &&
			!(temp.rpb_page == next_page && temp.rpb_line == next_line))
		{
			temp.rpb_prior = (temp.rpb_flags & rpb_delta) ? data : NULL;

			if (!DPM_fetch_back(tdbb, &temp, LCK_read, -1))
			{
				fb_utils::init_status(tdbb->tdbb_status_vector);

				clearRecordStack(staying);
				delete backout_rec;
				backout_rec = NULL;
				next_page = rpb->rpb_page;
				next_line = rpb->rpb_line;
				max_depth = 0;
				timed_out = true;
				break;
			}

			++backversions;
			++depth;

			// Don't monopolize the server while chasing long back version chains.
			JRD_reschedule(tdbb);
		}

		if (timed_out)
			continue;

		// If there is a next older version, then process it: remember that
		// version's data in 'staying'.

		if (temp.rpb_page == next_page && temp.rpb_line == next_line)
		{
			next_page = temp.rpb_b_page;
			next_line = temp.rpb_b_line;
			temp.rpb_record = NULL;

			if (temp.rpb_flags & rpb_deleted)
				CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
			else
			{
				// VIO_data below could change the flags
				const bool backout = (temp.rpb_flags & rpb_gc_active);
				VIO_data(tdbb, &temp, tdbb->getDefaultPool());

				if (!backout)
					staying.push(temp.rpb_record);
				else
				{
					fb_assert(!backout_rec);
					backout_rec = temp.rpb_record;
				}

				data = temp.rpb_record;
			}

			max_depth = depth;

			if (!next_page)
				break;
		}
		else
		{
			CCH_RELEASE(tdbb, &temp.getWindow(tdbb));
			break;
		}
	}

	// If the current number of back versions (depth) is smaller than the number
	// of back versions that we saw in a previous iteration (max_depth), then
	// somebody else must have been garbage collecting also.  Remove the entries
	// in 'staying' that have already been garbage collected.
	while (depth < max_depth--)
	{
		if (staying.hasData())
			delete staying.pop();
	}

	delete backout_rec;
}


static void notify_garbage_collector(thread_db* tdbb, record_param* rpb, TraNumber tranid)
{
/**************************************
 *
 *	n o t i f y _ g a r b a g e _ c o l l e c t o r
 *
 **************************************
 *
 * Functional description
 *	Notify the garbage collector that there is work to be
 *	done. Each relation has a garbage collection sparse
 *	bitmap where each bit corresponds to a data page
 *	sequence number of a data page known to have records
 *	which are candidates for garbage collection.
 *
 **************************************/
	Database* const dbb = tdbb->getDatabase();
	jrd_rel* const relation = rpb->rpb_relation;

	if (dbb->dbb_flags & DBB_suspend_bgio)
		return;

	if (relation->isTemporary())
		return;

	if (tranid == MAX_TRA_NUMBER)
		tranid = rpb->rpb_transaction_nr;

	// system transaction has its own rules
	if (tranid == 0)
		return;

	GarbageCollector* gc = dbb->dbb_garbage_collector;
	if (!gc)
		return;

	// If this is a large sequential scan then defer the release
	// of the data page to the LRU tail until the garbage collector
	// can garbage collect the page.

	if (rpb->getWindow(tdbb).win_flags & WIN_large_scan)
		rpb->getWindow(tdbb).win_flags |= WIN_garbage_collect;

	const ULONG dp_sequence = rpb->rpb_number.getValue() / dbb->dbb_max_records;

	const TraNumber minTranId = gc->addPage(relation->rel_id, dp_sequence, tranid);
	if (tranid > minTranId)
		tranid = minTranId;

	// If the garbage collector isn't active then poke
	// the event on which it sleeps to awaken it.

	dbb->dbb_flags |= DBB_gc_pending;

	if (!(dbb->dbb_flags & DBB_gc_active) &&
		(tranid < (tdbb->getTransaction() ?
			tdbb->getTransaction()->tra_oldest_active : dbb->dbb_oldest_snapshot)) )
	{
		dbb->dbb_gc_sem.release();
	}
}


static PrepareResult prepare_update(thread_db* tdbb, jrd_tra* transaction, TraNumber commit_tid_read,
	record_param* rpb, record_param* temp, record_param* new_rpb, PageStack& stack, bool writelock)
{
/**************************************
 *
 *	p r e p a r e _ u p d a t e
 *
 **************************************
 *
 * Functional description
 *	Prepare for a modify or erase.  Store the old version
 *	of a record, fetch the current version, check transaction
 *	states, etc.
 *
 **************************************/
	SET_TDBB(tdbb);

	jrd_rel* const relation = rpb->rpb_relation;

#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE_ALL,
		"prepare_update (rel_id %u, transaction %" SQUADFORMAT
		", commit_tid read %" SQUADFORMAT", record_param %" QUADFORMAT"d, ",
		relation->rel_id, transaction ? transaction->tra_number : 0, commit_tid_read,
		rpb ? rpb->rpb_number.getValue() : 0);

	VIO_trace(DEBUG_TRACE_ALL,
		" temp_rpb %" QUADFORMAT"d, new_rpb %" QUADFORMAT"d, stack)\n",
		temp ? temp->rpb_number.getValue() : 0,
		new_rpb ? new_rpb->rpb_number.getValue() : 0);

	VIO_trace(DEBUG_TRACE_ALL_INFO,
		"   old record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT
		":%d, prior %p\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line, (void*) rpb->rpb_prior);
#endif

	/* We're almost ready to go.  To erase the record, we must first
	make a copy of the old record someplace else.  Then we must re-fetch
	the record (for write) and verify that it is legal for us to
	erase it -- that it was written by a transaction that was committed
	when we started.  If not, the transaction that wrote the record
	is either active, dead, or in limbo.  If the transaction is active,
	wait for it to finish.  If it commits, we can't procede and must
	return an update conflict.  If the transaction is dead, back out the
	old version of the record and try again.  If in limbo, punt.

	The above is true only for concurrency & consistency mode transactions.
	For read committed transactions, check if the latest commited version
	is the same as the version that was read for the update.  If yes,
	the update can take place.  If some other transaction has modified
	the record and committed, then an update error will be returned.
   */

	*temp = *rpb;
	Record* const record = rpb->rpb_record;

	// Mark the record as chained version, and re-store it

	temp->rpb_address = record->getData();
	const Format* const format = record->getFormat();
	temp->rpb_length = format->fmt_length;
	temp->rpb_format_number = format->fmt_version;
	temp->rpb_flags = rpb_chained;

	if (temp->rpb_prior)
		temp->rpb_flags |= rpb_delta;

	// If it makes sense, store a differences record
	Difference difference;

	if (new_rpb)
	{
		// If both descriptors share the same record, there cannot be any difference.
		// This trick is used by VIO_writelock(), but can be a regular practice as well.
		if (new_rpb->rpb_address == temp->rpb_address)
		{
			fb_assert(new_rpb->rpb_length == temp->rpb_length);

			const ULONG diffLength = difference.makeNoDiff(temp->rpb_length);

			if (diffLength)
			{
				fb_assert(diffLength < temp->rpb_length);

				temp->rpb_address = difference.getData();
				temp->rpb_length = diffLength;
				new_rpb->rpb_flags |= rpb_delta;
			}
		}
		else
		{
			const ULONG diffLength =
				difference.make(new_rpb->rpb_length, new_rpb->rpb_address,
								temp->rpb_length, temp->rpb_address);

			if (diffLength && diffLength < temp->rpb_length)
			{
				temp->rpb_address = difference.getData();
				temp->rpb_length = diffLength;
				new_rpb->rpb_flags |= rpb_delta;
			}
		}
	}

#ifdef VIO_DEBUG
	if (new_rpb)
	{
		VIO_trace(DEBUG_WRITES_INFO,
			"    new record is%sa delta \n",
			(new_rpb->rpb_flags & rpb_delta) ? " " : " NOT ");
	}
#endif

	temp->rpb_number = rpb->rpb_number;
	DPM_store(tdbb, temp, stack, DPM_secondary);

	// Re-fetch the original record for write in anticipation of
	// replacing it with a completely new version.  Make sure it
	// was the same one we stored above.
	record_param org_rpb;
	TraNumber update_conflict_trans = MAX_TRA_NUMBER; //-1;
	const bool skipLocked = rpb->rpb_stream_flags & RPB_s_skipLocked;
	while (true)
	{
		org_rpb.rpb_flags = rpb->rpb_flags;
		org_rpb.rpb_f_line = rpb->rpb_f_line;
		org_rpb.rpb_f_page = rpb->rpb_f_page;

		if (!DPM_get(tdbb, rpb, LCK_write))
		{
			// There is no reason why this record would disappear for a
			// snapshot transaction.
			if (!(transaction->tra_flags & TRA_read_committed) || (transaction->tra_flags & TRA_read_consistency))
				BUGCHECK(186);	// msg 186 record disappeared
			else
			{
				// A read-committed transaction, on the other hand, doesn't
				// insist on the presence of any version, so versions of records
				// and entire records it has already read might be garbage-collected.
				if (!DPM_fetch(tdbb, temp, LCK_write))
					BUGCHECK(291);	// msg 291 cannot find record back version

				delete_record(tdbb, temp, 0, NULL);

				tdbb->bumpRelStats(RuntimeStatistics::RECORD_CONFLICTS, relation->rel_id);
				return PrepareResult::DELETED;
			}
		}

		int state = TRA_snapshot_state(tdbb, transaction, rpb->rpb_transaction_nr);

		// Reset (if appropriate) the garbage collect active flag to reattempt the backout

		if (rpb->rpb_flags & rpb_gc_active)
		{
			if (checkGCActive(tdbb, rpb, state))
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				waitGCActive(tdbb, rpb);
				continue;
			}
		}

		fb_assert(!(rpb->rpb_flags & rpb_gc_active));

		if (state == tra_committed)
			state = check_precommitted(transaction, rpb);

		switch (state)
		{
		case tra_committed:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT
				") is committed (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, transaction->tra_number);
#endif
			if (rpb->rpb_flags & rpb_deleted)
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

				// get rid of the back records we just created
				if (!DPM_fetch(tdbb, temp, LCK_write))
					BUGCHECK(291);	// msg 291 cannot find record back version

				delete_record(tdbb, temp, 0, NULL);

				if (writelock || skipLocked || (transaction->tra_flags & TRA_read_consistency))
				{
					tdbb->bumpRelStats(RuntimeStatistics::RECORD_CONFLICTS, relation->rel_id);
					return PrepareResult::DELETED;
				}

				IBERROR(188);	// msg 188 cannot update erased record
			}

			// For read committed transactions, if the record version we read
			// and started the update
			// has been updated by another transaction which committed in the
			// meantime, we cannot proceed further - update conflict error.

			if ((transaction->tra_flags & TRA_read_committed) &&
				(commit_tid_read != rpb->rpb_transaction_nr))
			{
				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
				if (!DPM_fetch(tdbb, temp, LCK_write))
					BUGCHECK(291);	// msg 291 cannot find record back version

				delete_record(tdbb, temp, 0, NULL);

				tdbb->bumpRelStats(RuntimeStatistics::RECORD_CONFLICTS, relation->rel_id);
				return PrepareResult::CONFLICT;
			}

			/*
			 * The case statement for tra_us has been pushed down to this
			 * current position as we do not want to give update conflict
			 * errors and the "cannot update erased record" within the same
			 * transaction. We were getting these errors in case of triggers.
			 * A pre-delete trigger could update or delete a record which we
			 * are then trying to change.
			 * In order to remove these changes and restore original behaviour,
			 * move this case statement above the 2 "if" statements.
			 * smistry 23-Aug-99
			 */
		case tra_us:
#ifdef VIO_DEBUG
			if (state == tra_us)
			{
				VIO_trace(DEBUG_READS_INFO,
					"    record's transaction (%" SQUADFORMAT
					") is us (my TID - %" SQUADFORMAT")\n",
					rpb->rpb_transaction_nr, transaction->tra_number);
			}
#endif
			if (rpb->rpb_b_page != temp->rpb_b_page || rpb->rpb_b_line != temp->rpb_b_line ||
				rpb->rpb_transaction_nr != temp->rpb_transaction_nr ||
				(rpb->rpb_flags & rpb_delta) != (temp->rpb_flags & rpb_delta) ||
				rpb->rpb_flags != org_rpb.rpb_flags ||
				(rpb->rpb_flags & rpb_incomplete) &&
					(rpb->rpb_f_page != org_rpb.rpb_f_page || rpb->rpb_f_line != org_rpb.rpb_f_line))
			{

				// the primary copy of the record was dead and someone else
				// backed it out for us.  Our data is OK but our pointers
				// aren't, so get rid of the record we created and try again

				CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

				record_param temp2 = *temp;
				if (!DPM_fetch(tdbb, &temp2, LCK_write))
					BUGCHECK(291);	// msg 291 cannot find record back version
				delete_record(tdbb, &temp2, 0, 0);

				temp->rpb_b_page = rpb->rpb_b_page;
				temp->rpb_b_line = rpb->rpb_b_line;
				temp->rpb_flags &= ~rpb_delta;
				temp->rpb_flags |= rpb->rpb_flags & rpb_delta;
				temp->rpb_transaction_nr = rpb->rpb_transaction_nr;

				DPM_store(tdbb, temp, stack, DPM_secondary);
				continue;
			}

			{
				const USHORT pageSpaceID = temp->getWindow(tdbb).win_page.getPageSpaceID();
				stack.push(PageNumber(pageSpaceID, temp->rpb_page));
			}
			return PrepareResult::SUCCESS;

		case tra_active:
		case tra_limbo:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT") is %s (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, (state == tra_active) ? "active" : "limbo",
				transaction->tra_number);
#endif
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

			// Wait as long as it takes (if not skipping locks) for an active
			// transaction which has modified the record.

			state = wait(tdbb, transaction, rpb, skipLocked);

			if (state == tra_committed)
				state = check_precommitted(transaction, rpb);

			// The snapshot says: transaction was active.  The TIP page says: transaction
			// is committed.  Maybe the transaction was rolled back via a transaction
			// level savepoint.  In that case, the record DPM_get-ed via rpb is already
			// backed out.  Try to refetch that record one more time.

			if ((state == tra_committed) && (rpb->rpb_transaction_nr != update_conflict_trans))
			{
				update_conflict_trans = rpb->rpb_transaction_nr;
				continue;
			}

			if (state != tra_dead && !(temp->rpb_flags & rpb_deleted))
			{
				if (!DPM_fetch(tdbb, temp, LCK_write))
					BUGCHECK(291);	// msg 291 cannot find record back version

				delete_record(tdbb, temp, 0, NULL);
			}

			switch (state)
			{
			case tra_committed:
				// For SNAPSHOT mode transactions raise error early
				if (!(transaction->tra_flags & TRA_read_committed))
				{
					tdbb->bumpRelStats(RuntimeStatistics::RECORD_CONFLICTS, relation->rel_id);

					if (skipLocked)
						return PrepareResult::SKIP_LOCKED;

					// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
					ERR_post(Arg::Gds(isc_deadlock) <<
							 Arg::Gds(isc_update_conflict) <<
							 Arg::Gds(isc_concurrent_transaction) << Arg::Int64(update_conflict_trans));
				}
				return PrepareResult::CONFLICT;

			case tra_limbo:
				if (!(transaction->tra_flags & TRA_ignore_limbo))
				{
					// Cannot use Arg::Num here because transaction number is 64-bit unsigned integer
					ERR_post(Arg::Gds(isc_rec_in_limbo) << Arg::Int64(rpb->rpb_transaction_nr));
				}
				[[fallthrough]];

			case tra_active:
				return skipLocked ? PrepareResult::SKIP_LOCKED : PrepareResult::LOCK_ERROR;

			case tra_dead:
				break;

			default:
				fb_assert(false);

			} // switch (state)
			break;

		case tra_dead:
#ifdef VIO_DEBUG
			VIO_trace(DEBUG_READS_INFO,
				"    record's transaction (%" SQUADFORMAT") is dead (my TID - %" SQUADFORMAT")\n",
				rpb->rpb_transaction_nr, transaction->tra_number);
#endif
			CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
			break;
		}

		VIO_backout(tdbb, rpb, transaction);
	}

	return PrepareResult::SUCCESS;
}


static void protect_system_table_insert(thread_db* tdbb,
										const Request* request,
										const jrd_rel* relation,
										bool force_flag)
{
/**************************************
 *
 *	p r o t e c t _ s y s t e m _ t a b l e _ i n s e r t
 *
 **************************************
 *
 * Functional description
 *	Disallow insertions on system tables for everyone except
 *	the GBAK restore process and internal (system) requests used
 *	by the engine itself.
 *
 **************************************/
	const Attachment* const attachment = tdbb->getAttachment();

	if (!force_flag)
	{
		if (attachment->isGbak() || request->hasInternalStatement())
			return;
	}

	status_exception::raise(Arg::Gds(isc_protect_sys_tab) <<
			Arg::Str("INSERT") << relation->rel_name.toQuotedString());
}


static void protect_system_table_delupd(thread_db* tdbb,
								 const jrd_rel* relation,
								 const char* operation,
								 bool force_flag)
{
/**************************************
 *
 *	p r o t e c t _ s y s t e m _ t a b l e _ d e l u p d
 *
 **************************************
 *
 * Functional description
 *	Disallow DELETE and UPDATE on system tables for everyone except
 *	the GBAK restore process and internal (system) requests used
 *	by the engine itself.
 *	Here we include sys triggers and the ones authorized to bypass security.
 *
 **************************************/
	const Attachment* const attachment = tdbb->getAttachment();
	const Request* const request = tdbb->getRequest();

	if (!force_flag)
	{
		if (attachment->isGbak() || request->hasPowerfulStatement())
			return;
	}

	status_exception::raise(Arg::Gds(isc_protect_sys_tab) <<
		Arg::Str(operation) << relation->rel_name.toQuotedString());
}


static void purge(thread_db* tdbb, record_param* rpb)
{
/**************************************
 *
 *	p u r g e
 *
 **************************************
 *
 * Functional description
 *	Purge old versions of a fully mature record.  The record is
 *	guaranteed not to be deleted.  Return true if the record
 *	didn't need to be purged or if the purge was done.  Return false
 *	if the purge couldn't happen because somebody else had the record.
 *	But the function was made void since nobody checks its return value.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	fb_assert(assert_gc_enabled(tdbb->getTransaction(), rpb->rpb_relation));

	jrd_rel* const relation = rpb->rpb_relation;
#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE_ALL,
		"purge (rel_id %u, record_param %" QUADFORMAT"d)\n",
		relation->rel_id, rpb->rpb_number.getValue());

	VIO_trace(DEBUG_TRACE_ALL_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line);
#endif

	// Release and re-fetch the page for write.  Make sure it's still the
	// same record (give up if not).  Then zap the back pointer and release
	// the record.

	record_param temp = *rpb;
	AutoTempRecord gc_rec(VIO_gc_record(tdbb, relation));
	Record* record = rpb->rpb_record = gc_rec;

	VIO_data(tdbb, rpb, relation->rel_pool);

	temp.rpb_prior = rpb->rpb_prior;
	rpb->rpb_record = temp.rpb_record;

	if (!DPM_get(tdbb, rpb, LCK_write))
	{
		// purge
		if (tdbb->getDatabase()->dbb_flags & DBB_gc_background)
			notify_garbage_collector(tdbb, rpb);

		return; //false;
	}

	rpb->rpb_prior = temp.rpb_prior;

	if (temp.rpb_transaction_nr != rpb->rpb_transaction_nr || temp.rpb_b_line != rpb->rpb_b_line ||
		temp.rpb_b_page != rpb->rpb_b_page || rpb->rpb_b_page == 0)
	{
		CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));
		return; // true;
	}

	rpb->rpb_b_page = 0;
	rpb->rpb_b_line = 0;
	rpb->rpb_flags &= ~(rpb_delta | rpb_gc_active);
	CCH_MARK(tdbb, &rpb->getWindow(tdbb));
	DPM_rewrite_header(tdbb, rpb);
	CCH_RELEASE(tdbb, &rpb->getWindow(tdbb));

	RecordStack staying;
	staying.push(record);
	garbage_collect(tdbb, &temp, rpb->rpb_page, staying);

	tdbb->bumpRelStats(RuntimeStatistics::RECORD_PURGES, relation->rel_id);
	return; // true;
}


static void replace_record(thread_db*		tdbb,
						   record_param*	rpb,
						   PageStack*		stack,
						   const jrd_tra*	transaction)
{
/**************************************
 *
 *	r e p l a c e _ r e c o r d
 *
 **************************************
 *
 * Functional description
 *	Replace a record and get rid of the old tail, if any.  If requested,
 *	fetch data for the record on the way out.
 *
 **************************************/
	SET_TDBB(tdbb);

#ifdef VIO_DEBUG
	jrd_rel* relation = rpb->rpb_relation;
	VIO_trace(DEBUG_TRACE_ALL,
		"replace_record (rel_id %u, record_param %" QUADFORMAT"d, transaction %" SQUADFORMAT")\n",
		relation->rel_id, rpb->rpb_number.getValue(), transaction ? transaction->tra_number : 0);

	VIO_trace(DEBUG_TRACE_ALL_INFO,
		"   record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT
		":%d, prior %p\n",
		rpb->rpb_page, rpb->rpb_line, rpb->rpb_transaction_nr,
		rpb->rpb_flags, rpb->rpb_b_page, rpb->rpb_b_line,
		rpb->rpb_f_page, rpb->rpb_f_line, (void*) rpb->rpb_prior);
#endif

	record_param temp = *rpb;
	DPM_update(tdbb, rpb, stack, transaction);
	delete_tail(tdbb, &temp, rpb->rpb_page);

	if ((rpb->rpb_flags & rpb_delta) && !rpb->rpb_prior)
		rpb->rpb_prior = rpb->rpb_record;
}


static void refresh_fk_fields(thread_db* tdbb, Record* old_rec, record_param* cur_rpb,
	record_param* new_rpb)
{
/**************************************
 *
 *	r e f r e s h _ f k _ f i e l d s
 *
 **************************************
 *
 * Functional description
 *	Update new_rpb with foreign key fields values changed by cascade triggers.
 *  Consider self-referenced foreign keys only.
 *
 *  old_rec - old record before modify
 *  cur_rpb - just read record with possibly changed FK fields
 *  new_rpb - new record evaluated by modify statement and before-triggers
 *
 **************************************/
	jrd_rel* relation = cur_rpb->rpb_relation;

	MET_scan_partners(tdbb, relation);

	if (!(relation->rel_foreign_refs.frgn_relations))
		return;

	const FB_SIZE_T frgnCount = relation->rel_foreign_refs.frgn_relations->count();
	if (!frgnCount)
		return;

	RelationPages* relPages = cur_rpb->rpb_relation->getPages(tdbb);

	// Collect all fields of all foreign keys
	SortedArray<int, InlineStorage<int, 16> > fields;

	for (FB_SIZE_T i = 0; i < frgnCount; i++)
	{
		// We need self-referenced FK's only
		if ((*relation->rel_foreign_refs.frgn_relations)[i] == relation->rel_id)
		{
			index_desc idx;
			idx.idx_id = idx_invalid;

			if (BTR_lookup(tdbb, relation, (*relation->rel_foreign_refs.frgn_reference_ids)[i],
					&idx, relPages))
			{
				fb_assert(idx.idx_flags & idx_foreign);

				for (int fld = 0; fld < idx.idx_count; fld++)
				{
					const int fldNum = idx.idx_rpt[fld].idx_field;
					if (!fields.exist(fldNum))
						fields.add(fldNum);
				}
			}
		}
	}

	if (fields.isEmpty())
		return;

	DSC desc1, desc2;
	for (FB_SIZE_T idx = 0; idx < fields.getCount(); idx++)
	{
		// Detect if user changed FK field by himself.
		const int fld = fields[idx];
		const bool flag_old = EVL_field(relation, old_rec, fld, &desc1);
		const bool flag_new = EVL_field(relation, new_rpb->rpb_record, fld, &desc2);

		// If field was not changed by user - pick up possible modification by
		// system cascade trigger
		if (flag_old == flag_new &&
			(!flag_old || (flag_old && !MOV_compare(tdbb, &desc1, &desc2))))
		{
			const bool flag_tmp = EVL_field(relation, cur_rpb->rpb_record, fld, &desc1);
			if (flag_tmp)
				MOV_move(tdbb, &desc1, &desc2);
			else
				new_rpb->rpb_record->setNull(fld);
		}
	}
}


static SSHORT set_metadata_id(thread_db* tdbb, Record* record, USHORT field_id, drq_type_t dyn_id,
	const char* name)
{
/**************************************
 *
 *	s e t _ m e t a d a t a _ i d
 *
 **************************************
 *
 * Functional description
 *	Assign the auto generated ID to a particular field
 *  and return it to the caller.
 *
 **************************************/
	dsc desc1;

	if (EVL_field(0, record, field_id, &desc1))
		return MOV_get_long(tdbb, &desc1, 0);

	SSHORT value = (SSHORT) DYN_UTIL_gen_unique_id(tdbb, dyn_id, name);
	dsc desc2;
	desc2.makeShort(0, &value);
	MOV_move(tdbb, &desc2, &desc1);
	record->clearNull(field_id);
	return value;
}


// Assign the 31-bit auto generated ID to a particular field
static void set_nbackup_id(thread_db* tdbb, Record* record, USHORT field_id, drq_type_t dyn_id,
	const char* name)
{
	dsc desc1;

	if (EVL_field(0, record, field_id, &desc1))
		return;

	SLONG value = (SLONG) DYN_UTIL_gen_unique_id(tdbb, dyn_id, name);
	dsc desc2;
	desc2.makeLong(0, &value);
	MOV_move(tdbb, &desc2, &desc1);
	record->clearNull(field_id);
}


static void set_owner_name(thread_db* tdbb, Record* record, USHORT field_id)
{
/**************************************
 *
 *	s e t _ o w n e r _ n a m e
 *
 **************************************
 *
 * Functional description
 *	Set the owner name for the metadata object.
 *
 **************************************/
	dsc desc1;

	if (!EVL_field(0, record, field_id, &desc1))
	{
		const auto attachment = tdbb->getAttachment();
		const MetaString& name = attachment->getEffectiveUserName();

		if (name.hasData())
		{
			dsc desc2;
			desc2.makeText((USHORT) name.length(), CS_METADATA, (UCHAR*) name.c_str());
			MOV_move(tdbb, &desc2, &desc1);
			record->clearNull(field_id);
		}
	}
}


static bool set_security_class(thread_db* tdbb, Record* record, USHORT field_id)
{
/**************************************
 *
 *	s e t _ s e c u r i t y _ c l a s s
 *
 **************************************
 *
 * Functional description
 *	Generate the security class name.
 *
 **************************************/
	dsc desc1;

	if (!EVL_field(0, record, field_id, &desc1))
	{
		const SINT64 value = DYN_UTIL_gen_unique_id(tdbb, drq_g_nxt_sec_id, SQL_SECCLASS_GENERATOR);
		MetaName name;
		name.printf("%s%" SQUADFORMAT, SQL_SECCLASS_PREFIX, value);
		dsc desc2;
		desc2.makeText((USHORT) name.length(), CS_ASCII, (UCHAR*) name.c_str());
		MOV_move(tdbb, &desc2, &desc1);
		record->clearNull(field_id);

		return true;
	}

	return false;
}


static void set_system_flag(thread_db* tdbb, Record* record, USHORT field_id)
{
/**************************************
 *
 *	s e t _ s y s t e m _ f l a g
 *
 **************************************
 *
 * Functional description
 *	Set the value of a particular field to a known binary value.
 *
 **************************************/
	dsc desc1;

	if (!EVL_field(0, record, field_id, &desc1))
	{
		SSHORT flag = 0;
		dsc desc2;
		desc2.makeShort(0, &flag);
		MOV_move(tdbb, &desc2, &desc1);
		record->clearNull(field_id);
	}
}


void VIO_update_in_place(thread_db* tdbb,
							jrd_tra* transaction, record_param* org_rpb, record_param* new_rpb)
{
/**************************************
 *
 *	u p d a t e _ i n _ p l a c e
 *
 **************************************
 *
 * Functional description
 *	Modify a record in place.  This is used for system transactions
 *	and for multiple modifications of a user record.
 *
 **************************************/
	SET_TDBB(tdbb);
	Database* dbb = tdbb->getDatabase();
	CHECK_DBB(dbb);

	jrd_rel* const relation = org_rpb->rpb_relation;
#ifdef VIO_DEBUG
	VIO_trace(DEBUG_TRACE_ALL,
		"update_in_place (rel_id %u, transaction %" SQUADFORMAT", org_rpb %" QUADFORMAT"d, "
		"new_rpb %" QUADFORMAT"d)\n",
		relation->rel_id, transaction ? transaction->tra_number : 0, org_rpb->rpb_number.getValue(),
		new_rpb ? new_rpb->rpb_number.getValue() : 0);

	VIO_trace(DEBUG_TRACE_ALL_INFO,
		"   old record  %" SLONGFORMAT":%d, rpb_trans %" SQUADFORMAT
		", flags %d, back %" SLONGFORMAT":%d, fragment %" SLONGFORMAT":%d\n",
		org_rpb->rpb_page, org_rpb->rpb_line, org_rpb->rpb_transaction_nr,
		org_rpb->rpb_flags, org_rpb->rpb_b_page, org_rpb->rpb_b_line,
		org_rpb->rpb_f_page, org_rpb->rpb_f_line);
#endif

	PageStack *stack = NULL;
	if (new_rpb->rpb_record) // we apply update to new data
	{
		stack = &new_rpb->rpb_record->getPrecedence();
	}
	else if (org_rpb->rpb_record) // we apply update to delete stub
	{
		stack = &org_rpb->rpb_record->getPrecedence();
	}
	// According to DS on firebird-devel: it is not possible update non-existing record so stack is
	// unavoidable assigned to some value
	fb_assert(stack);

	Record* const old_data = org_rpb->rpb_record;

	// If the old version has been stored as a delta, things get complicated.  Clearly,
	// if we overwrite the current record, the differences from the current version
	// becomes meaningless.  What we need to do is replace the old "delta" record
	// with an old "complete" record, update in placement, then delete the old delta record

	AutoTempRecord gc_rec;

	record_param temp2;
	const Record* prior = org_rpb->rpb_prior;
	if (prior)
	{
		temp2 = *org_rpb;
		temp2.rpb_record = gc_rec = VIO_gc_record(tdbb, relation);
		temp2.rpb_page = org_rpb->rpb_b_page;
		temp2.rpb_line = org_rpb->rpb_b_line;

		if (!DPM_fetch(tdbb, &temp2, LCK_read))
			BUGCHECK(291);	 // msg 291 cannot find record back version

		VIO_data(tdbb, &temp2, relation->rel_pool);

		temp2.rpb_flags = rpb_chained;

		if (temp2.rpb_prior)
			temp2.rpb_flags |= rpb_delta;

		temp2.rpb_number = org_rpb->rpb_number;
		DPM_store(tdbb, &temp2, *stack, DPM_secondary);

		const USHORT pageSpaceID = temp2.getWindow(tdbb).win_page.getPageSpaceID();
		stack->push(PageNumber(pageSpaceID, temp2.rpb_page));
	}

	if (!DPM_get(tdbb, org_rpb, LCK_write))
		BUGCHECK(186);	// msg 186 record disappeared

	if (prior)
	{
		const ULONG page = org_rpb->rpb_b_page;
		const USHORT line = org_rpb->rpb_b_line;
		org_rpb->rpb_b_page = temp2.rpb_page;
		org_rpb->rpb_b_line = temp2.rpb_line;
		org_rpb->rpb_flags &= ~rpb_delta;
		org_rpb->rpb_prior = NULL;
		temp2.rpb_page = page;
		temp2.rpb_line = line;
	}

	UCHAR* const save_address = org_rpb->rpb_address;
	const ULONG length = org_rpb->rpb_length;
	const USHORT format_number = org_rpb->rpb_format_number;
	org_rpb->rpb_address = new_rpb->rpb_address;
	org_rpb->rpb_length = new_rpb->rpb_length;
	org_rpb->rpb_format_number = new_rpb->rpb_format_number;
	org_rpb->rpb_flags &= ~rpb_deleted;
	org_rpb->rpb_flags |= new_rpb->rpb_flags & (rpb_uk_modified|rpb_deleted);

	replace_record(tdbb, org_rpb, stack, transaction);

	org_rpb->rpb_address = save_address;
	org_rpb->rpb_length = length;
	org_rpb->rpb_format_number = format_number;
	org_rpb->rpb_undo = old_data;

	if (transaction->tra_flags & TRA_system)
	{
		// Garbage collect.  Start by getting all existing old versions (other
		// than the immediate two in question).

		RecordStack staying;
		list_staying(tdbb, org_rpb, staying);
		staying.push(new_rpb->rpb_record);

		RecordStack going;
		going.push(org_rpb->rpb_record);

		IDX_garbage_collect(tdbb, org_rpb, going, staying);
		BLB_garbage_collect(tdbb, going, staying, org_rpb->rpb_page, relation);

		staying.pop();
		clearRecordStack(staying);
	}

	if (prior)
	{
		if (!DPM_fetch(tdbb, &temp2, LCK_write))
			BUGCHECK(291);		// msg 291 cannot find record back version

		delete_record(tdbb, &temp2, org_rpb->rpb_page, NULL);
	}
}


static void verb_post(thread_db* tdbb,
					  jrd_tra* transaction,
					  record_param* rpb,
					  Record* old_data)
{
/**************************************
 *
 *	v e r b _ p o s t
 *
 **************************************
 *
 * Functional description
 *	Post a record update under verb control to a transaction.
 *	If the previous version of the record was created by
 *	this transaction in a different verb, save the data as well.
 *
 * Input:
 *  rpb:		New content of the record
 *	old_data:	Only supplied if an in-place operation was performed
 *				(i.e. update_in_place).
 *
 **************************************/
	SET_TDBB(tdbb);

	VerbAction* const action = transaction->tra_save_point->createAction(rpb->rpb_relation);

	if (!RecordBitmap::test(action->vct_records, rpb->rpb_number.getValue()))
	{
		RBM_SET(transaction->tra_pool, &action->vct_records, rpb->rpb_number.getValue());

		if (old_data)
		{
			// An update-in-place is being posted to this savepoint, and this
			// savepoint hasn't seen this record before.

			if (!action->vct_undo)
			{
				action->vct_undo =
					FB_NEW_POOL(*transaction->tra_pool) UndoItemTree(*transaction->tra_pool);
			}

			action->vct_undo->add(UndoItem(transaction, rpb->rpb_number, old_data));
		}
	}
	else if (old_data)
	{
		// Double update us posting. The old_data will not be used,
		// so make sure we garbage collect before we lose track of the
		// in-place-updated record.
		action->garbageCollectIdxLite(tdbb, transaction, rpb->rpb_number.getValue(), action, old_data);
	}
}
