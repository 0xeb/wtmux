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

static SOCKET signal_pipe[2] = {INVALID_SOCKET, INVALID_SOCKET};
static HANDLE resize_thread = NULL;
static volatile int signal_shutdown = 0;
static SHORT last_cols = 0, last_rows = 0;

static void (*signal_callback)(int) = NULL;

static BOOL WINAPI
ctrl_handler(DWORD type)
{
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		win32_signal_notify(SIGINT);
		return (TRUE);
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		win32_signal_notify(SIGTERM);
		return (TRUE);
	}
	return (FALSE);
}

static DWORD WINAPI
resize_poll_thread(LPVOID arg)
{
	HANDLE hConsole;
	CONSOLE_SCREEN_BUFFER_INFO csbi;

	(void)arg;
	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

	while (!signal_shutdown) {
		if (win32_test_terminal() != NULL) {
			if (win32_test_terminal_changed())
				win32_signal_notify(SIGWINCH);
		} else if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
			SHORT cols =
			    csbi.srWindow.Right - csbi.srWindow.Left + 1;
			SHORT rows =
			    csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
			if ((cols != last_cols || rows != last_rows) &&
			    last_cols != 0) {
				last_cols = cols;
				last_rows = rows;
				win32_signal_notify(SIGWINCH);
			} else {
				last_cols = cols;
				last_rows = rows;
			}
		}
		Sleep(100);
	}
	return (0);
}

void
win32_signal_init(void)
{
	int sv[2];
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	HANDLE hConsole;

	win32_wsa_init();

	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0) {
		signal_pipe[0] = (SOCKET)sv[0];
		signal_pipe[1] = (SOCKET)sv[1];
	}

	SetConsoleCtrlHandler(NULL, FALSE);
	SetConsoleCtrlHandler(ctrl_handler, TRUE);

	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	if (win32_test_terminal() != NULL)
		(void)win32_test_terminal_changed();
	else if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
		last_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
		last_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	}

	signal_shutdown = 0;
	resize_thread =
	    CreateThread(NULL, 0, resize_poll_thread, NULL, 0, NULL);
}

void
win32_signal_cleanup(void)
{
	signal_shutdown = 1;

	if (resize_thread != NULL) {
		WaitForSingleObject(resize_thread, 2000);
		CloseHandle(resize_thread);
		resize_thread = NULL;
	}

	SetConsoleCtrlHandler(ctrl_handler, FALSE);

	if (signal_pipe[0] != INVALID_SOCKET) {
		closesocket(signal_pipe[0]);
		closesocket(signal_pipe[1]);
		signal_pipe[0] = signal_pipe[1] = INVALID_SOCKET;
	}
}

static DWORD WINAPI
usr_thread(LPVOID arg)
{
	HANDLE	*events = arg;
	DWORD	 n;

	for (;;) {
		n = WaitForMultipleObjects(2, events, FALSE, INFINITE);
		if (n == WAIT_OBJECT_0)
			win32_signal_notify(SIGUSR1);
		else if (n == WAIT_OBJECT_0 + 1)
			win32_signal_notify(SIGUSR2);
		else
			break;
	}
	return (0);
}

void
win32_signal_events(void)
{
	static HANDLE	 events[2];
	HANDLE		 thread;
	wchar_t		 name[64];
	int		 i;

	if (events[0] != NULL)
		return;
	for (i = 0; i < 2; i++) {
		swprintf(name, sizeof name / sizeof name[0],
		    L"Local\\tmux-%lu-USR%d", GetCurrentProcessId(), i + 1);
		if ((events[i] = CreateEventW(NULL, FALSE, FALSE, name)) ==
		    NULL)
			return;
	}
	thread = CreateThread(NULL, 0, usr_thread, events, 0, NULL);
	if (thread != NULL)
		CloseHandle(thread);
}
int
win32_signal_get_fd(void)
{
	return (int)signal_pipe[0];
}

void
win32_signal_notify(int signo)
{
	char c = (char)signo;

	if (signal_pipe[1] != INVALID_SOCKET)
		send(signal_pipe[1], &c, 1, 0);
}

void
win32_signal_set_callback(void (*cb)(int))
{
	signal_callback = cb;
}

void
win32_signal_dispatch(void)
{
	char buf[32];
	int n, i;

	if (signal_pipe[0] == INVALID_SOCKET)
		return;

	n = recv(signal_pipe[0], buf, sizeof buf, 0);
	if (n <= 0)
		return;

	for (i = 0; i < n; i++) {
		if (signal_callback != NULL)
			signal_callback((int)buf[i]);
	}
}
