/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		dfw_proto.h
 *	DESCRIPTION:	Prototype header file for dfw.cpp
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

#ifndef JRD_DFW_PROTO_H
#define JRD_DFW_PROTO_H

#include "../jrd/btr.h"	// defines SelectivityList

namespace Jrd
{
	enum dfw_t;
}

USHORT DFW_assign_index_type(Jrd::thread_db*, const Jrd::QualifiedName&, SSHORT, SSHORT);
void DFW_delete_deferred(Jrd::jrd_tra*, SavNumber);
Firebird::SortedArray<int>& DFW_get_ids(Jrd::DeferredWork* work);
void DFW_merge_work(Jrd::jrd_tra*, SavNumber, SavNumber);
void DFW_perform_work(Jrd::thread_db*, Jrd::jrd_tra*);
void DFW_perform_post_commit_work(Jrd::jrd_tra*);
Jrd::DeferredWork* DFW_post_work(Jrd::jrd_tra*, Jrd::dfw_t, const dsc* nameDesc, const dsc* schemaDesc, USHORT,
	const Jrd::MetaName& package = {});
Jrd::DeferredWork* DFW_post_work(Jrd::jrd_tra*, Jrd::dfw_t, const Firebird::string&, const Jrd::MetaName& schema,
	USHORT, const Jrd::MetaName& package = {});
Jrd::DeferredWork* DFW_post_work_arg(Jrd::jrd_tra*, Jrd::DeferredWork*, const dsc* nameDesc, const dsc* schemaDesc,
	USHORT);
Jrd::DeferredWork* DFW_post_work_arg(Jrd::jrd_tra*, Jrd::DeferredWork*, const dsc* nameDesc, const dsc* schemaDesc,
	USHORT, Jrd::dfw_t);
void DFW_update_index(const Jrd::QualifiedName&, USHORT, const Jrd::SelectivityList&, Jrd::jrd_tra*);
void DFW_reset_icu(Jrd::thread_db*);

#endif // JRD_DFW_PROTO_H
