/*
 *	PROGRAM:	JRD Remote Server
 *	MODULE:		cntl.cpp
 *	DESCRIPTION:	Windows NT service control panel interface
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

#include "firebird.h"
#include <stdio.h>
#include "../remote/remote.h"
#include "../common/ThreadStart.h"
#include "../utilities/install/install_nt.h"
#include "../remote/server/serve_proto.h"
#include "../../remote/server/os/win32/cntl_proto.h"
#include "../yvalve/gds_proto.h"
#include "../common/isc_proto.h"
#include "../jrd/jrd_proto.h"
#include "../common/classes/init.h"
#include "ibase.h"

#include <windows.h>

using namespace Firebird;

const unsigned int SHUTDOWN_TIMEOUT = 10 * 1000;	// 10 seconds

static void WINAPI control_thread(DWORD);

static USHORT report_status(DWORD, DWORD, DWORD, DWORD);

static ThreadEntryPoint* main_handler;
static SERVICE_STATUS_HANDLE service_handle;
static GlobalPtr<string> service_name;
static GlobalPtr<string> mutex_name;
static HANDLE stop_event_handle;
static HANDLE hMutex = NULL;


void CNTL_init(ThreadEntryPoint* handler, const TEXT* name)
{
/**************************************
 *
 *	C N T L _ i n i t
 *
 **************************************
 *
 * Functional description
 *
 **************************************/

	main_handler = handler;
	service_name->printf(REMOTE_SERVICE, name);
	mutex_name->printf(GUARDIAN_MUTEX, name);
}


void WINAPI CNTL_main_thread( DWORD /*argc*/, char* /*argv*/[])
{
/**************************************
 *
 *	C N T L _ m a i n _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	service_handle = RegisterServiceCtrlHandler(service_name->c_str(), control_thread);
	if (!service_handle)
		return;

	int status = 1;
	DWORD temp = 0;

	if (report_status(SERVICE_START_PENDING, NO_ERROR, 1, 3000) &&
		(stop_event_handle = CreateEvent(NULL, TRUE, FALSE, NULL)) != NULL &&
		report_status(SERVICE_START_PENDING, NO_ERROR, 2, 3000))
	{
		try
		{
			Thread::start(main_handler, NULL, THREAD_medium);
			if (report_status(SERVICE_RUNNING, NO_ERROR, 0, 0))
			{
				status = 0;
				temp = WaitForSingleObject(stop_event_handle, INFINITE);
			}
		}
		catch (const Exception& ex)
		{
			iscLogException("CNTL: cannot start service handler thread", ex);
		}
	}

	DWORD last_error = 0;
	if (temp == WAIT_FAILED || status)
		last_error = GetLastError();

	if (stop_event_handle)
		CloseHandle(stop_event_handle);

	report_status(SERVICE_STOP_PENDING, NO_ERROR, 1, SHUTDOWN_TIMEOUT);

	fb_shutdown(SHUTDOWN_TIMEOUT, fb_shutrsn_svc_stopped);

	report_status(SERVICE_STOPPED, last_error, 0, 0);
}


void CNTL_shutdown_service( const TEXT* message)
{
/**************************************
 *
 *	C N T L _ s h u t d o w n _ s e r v i c e
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	const char* strings[2];

	char buffer[BUFFER_LARGE];
	snprintf(buffer, sizeof(buffer), "%s error: %lu", service_name->c_str(), GetLastError());

	HANDLE event_source = RegisterEventSource(NULL, service_name->c_str());
	if (event_source)
	{
		strings[0] = buffer;
		strings[1] = message;
		ReportEvent(event_source,
					EVENTLOG_ERROR_TYPE,
					0,
					0,
					NULL,
					2,
					0,
					const_cast<const char**>(strings), NULL);
		DeregisterEventSource(event_source);
	}

	if (stop_event_handle)
		SetEvent(stop_event_handle);
}


static void WINAPI control_thread( DWORD action)
{
/**************************************
 *
 *	c o n t r o l _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *	Process a service control request.
 *
 **************************************/
	const DWORD state = SERVICE_RUNNING;

	switch (action)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		report_status(SERVICE_STOP_PENDING, NO_ERROR, 1, 3000);
		if (hMutex)
			ReleaseMutex(hMutex);
		SetEvent(stop_event_handle);
		return;

	case SERVICE_CONTROL_INTERROGATE:
		break;

	case SERVICE_CREATE_GUARDIAN_MUTEX:
		hMutex = OpenMutex(SYNCHRONIZE, FALSE, mutex_name->c_str());
		if (hMutex)
		{
			UINT error_mode = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
				SEM_NOOPENFILEERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT;
			SetErrorMode(error_mode);
			WaitForSingleObject(hMutex, INFINITE);
		}
		break;

	default:
		break;
	}

	report_status(state, NO_ERROR, 0, 0);
}

static USHORT report_status(DWORD state, DWORD exit_code, DWORD checkpoint, DWORD hint)
{
/**************************************
 *
 *	r e p o r t _ s t a t u s
 *
 **************************************
 *
 * Functional description
 *	Report our status to the control manager.
 *
 **************************************/
	SERVICE_STATUS status;
	status.dwServiceType = (SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS);
	status.dwServiceSpecificExitCode = 0;

	if (state == SERVICE_START_PENDING)
		status.dwControlsAccepted = 0;
	else
		status.dwControlsAccepted = SERVICE_ACCEPT_STOP;

	status.dwCurrentState = state;
	status.dwWin32ExitCode = exit_code;
	status.dwCheckPoint = checkpoint;
	status.dwWaitHint = hint;

	const USHORT ret = SetServiceStatus(service_handle, &status);
	if (!ret)
		CNTL_shutdown_service("SetServiceStatus");

	return ret;
}
