/* $OpenBSD$ */

/*
 * Copyright (c) 2026 arndawg <https://github.com/arndawg>
 * Copyright (c) 2026 Elias Bachaalany <elias.bachaalany@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"
#include "platform/win32-internal.h"

void win32_child_exited(pid_t pid, int status);

struct watched_process {
	HANDLE process;
	pid_t pid;
	struct watched_process *next;
};
static struct watched_process *watched;
static CRITICAL_SECTION watch_lock;
static HANDLE watcher_thread, watcher_event;
static volatile LONG watcher_shutdown;

static DWORD WINAPI
process_watcher_thread(LPVOID arg)
{
	struct watched_process **at, *done;
	DWORD code;
	(void)arg;
	while (!InterlockedCompareExchange(&watcher_shutdown, 0, 0)) {
		WaitForSingleObject(watcher_event, 25);
		for (;;) {
			done = NULL;
			EnterCriticalSection(&watch_lock);
			for (at = &watched; *at != NULL; at = &(*at)->next) {
				if (WaitForSingleObject((*at)->process, 0) ==
				    WAIT_OBJECT_0) {
					done = *at;
					*at = done->next;
					break;
				}
			}
			if (done != NULL) {
				if (!GetExitCodeProcess(done->process, &code))
					code = 1;
				CloseHandle(done->process);
			}
			LeaveCriticalSection(&watch_lock);
			if (done == NULL)
				break;
			win32_child_exited(done->pid, (int)((code & 255) << 8));
			win32_signal_notify(SIGCHLD);
			free(done);
		}
	}
	return (0);
}

void
win32_process_init(void)
{
	InitializeCriticalSection(&watch_lock);
	watcher_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	InterlockedExchange(&watcher_shutdown, 0);
	watched = NULL;
	if (watcher_event == NULL)
		abort();
	watcher_thread =
	    CreateThread(NULL, 0, process_watcher_thread, NULL, 0, NULL);
	if (watcher_thread == NULL)
		abort();
}

void
win32_process_cleanup(void)
{
	struct watched_process *next;
	InterlockedExchange(&watcher_shutdown, 1);
	SetEvent(watcher_event);
	WaitForSingleObject(watcher_thread, INFINITE);
	CloseHandle(watcher_thread);
	watcher_thread = NULL;
	CloseHandle(watcher_event);
	watcher_event = NULL;
	EnterCriticalSection(&watch_lock);
	while (watched != NULL) {
		next = watched->next;
		CloseHandle(watched->process);
		free(watched);
		watched = next;
	}
	LeaveCriticalSection(&watch_lock);
	DeleteCriticalSection(&watch_lock);
}

void
win32_process_watch(HANDLE process, pid_t pid)
{
	struct watched_process *item = calloc(1, sizeof *item);
	if (item == NULL)
		abort();
	if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
	        &item->process, 0, FALSE, DUPLICATE_SAME_ACCESS))
		abort();
	item->pid = pid;
	EnterCriticalSection(&watch_lock);
	item->next = watched;
	watched = item;
	LeaveCriticalSection(&watch_lock);
	SetEvent(watcher_event);
}

HANDLE
win32_launch_server(const char *path, const char *const *extra,
    HANDLE *errors)
{
	STARTUPINFOEXW startup;
	PROCESS_INFORMATION process;
	SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
	HANDLE read_end = NULL, write_end = NULL, result = NULL;
	SIZE_T size = 0;
	int handles = 0;
	wchar_t *executable = calloc(32768, sizeof *executable),
	        *command = NULL;
	char *executable8 = NULL, *command8 = NULL;
	const char **argv = NULL;
	size_t n = 0, i;
	DWORD length, flags;
	struct tmux_os_error error;
	*errors = NULL;
	memset(&startup, 0, sizeof startup);
	if (executable == NULL)
		return (NULL);
	length = GetModuleFileNameW(NULL, executable, 32768);
	if (length == 0 || length >= 32768)
		goto done;
	if ((executable8 = tmux_win32_utf8(executable, &error)) == NULL)
		goto done;
	while (extra != NULL && extra[n] != NULL)
		n++;
	if ((argv = calloc(n + 5, sizeof *argv)) == NULL)
		goto done;
	argv[0] = executable8;
	argv[1] = "-D";
	argv[2] = "-S";
	argv[3] = path;
	for (i = 0; i < n; i++)
		argv[4 + i] = extra[i];
	if ((command8 = tmux_windows_command_line(argv)) == NULL)
		goto done;
	if ((command = tmux_win32_utf16(command8, &error)) == NULL)
		goto done;
	startup.StartupInfo.cb = sizeof startup;
	memset(&process, 0, sizeof process);
	flags = DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;

	if (CreatePipe(&read_end, &write_end, &inherit, 0) &&
	    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
		InitializeProcThreadAttributeList(NULL, 1, 0, &size);
		startup.lpAttributeList = malloc(size);
		if (startup.lpAttributeList != NULL &&
		    InitializeProcThreadAttributeList(startup.lpAttributeList,
		        1, 0, &size) &&
		    UpdateProcThreadAttribute(startup.lpAttributeList, 0,
		        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write_end,
		        sizeof write_end, NULL, NULL)) {
			startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startup.StartupInfo.hStdError = write_end;
			flags |= EXTENDED_STARTUPINFO_PRESENT;
			handles = 1;
		}
	}
	if (!CreateProcessW(executable, command, NULL, NULL, handles, flags,
	        NULL, NULL, &startup.StartupInfo, &process)) {
		if (GetLastError() != ERROR_ACCESS_DENIED)
			goto done;
		free(command);
		if ((command = tmux_win32_utf16(command8, &error)) == NULL)
			goto done;
		if (!CreateProcessW(executable, command, NULL, NULL, handles,
		        flags & ~CREATE_BREAKAWAY_FROM_JOB, NULL, NULL,
		        &startup.StartupInfo, &process))
			goto done;
	}
	CloseHandle(process.hThread);
	result = process.hProcess;
	if (handles) {
		*errors = read_end;
		read_end = NULL;
	}
done:
	if (startup.lpAttributeList != NULL) {
		if (handles)
			DeleteProcThreadAttributeList(startup.lpAttributeList);
		free(startup.lpAttributeList);
	}
	if (read_end != NULL)
		CloseHandle(read_end);
	if (write_end != NULL)
		CloseHandle(write_end);
	free(command);
	free(command8);
	free(argv);
	free(executable8);
	free(executable);
	return (result);
}

void
win32_process_unwatch(pid_t pid)
{
	struct watched_process **at, *item;
	EnterCriticalSection(&watch_lock);
	for (at = &watched; *at != NULL; at = &(*at)->next) {
		if ((*at)->pid == pid) {
			item = *at;
			*at = item->next;
			CloseHandle(item->process);
			free(item);
			break;
		}
	}
	LeaveCriticalSection(&watch_lock);
	SetEvent(watcher_event);
}
