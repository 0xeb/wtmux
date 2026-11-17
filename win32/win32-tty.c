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
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"

static HANDLE hConsoleIn = INVALID_HANDLE_VALUE;
static HANDLE hConsoleOut = INVALID_HANDLE_VALUE;
static DWORD saved_in_mode = 0;
static DWORD saved_out_mode = 0;
static int mode_saved = 0;

int
win32_tty_raw_mode(void)
{
	DWORD in_mode, out_mode;

	hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
	hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);

	if (hConsoleIn == INVALID_HANDLE_VALUE ||
	    hConsoleOut == INVALID_HANDLE_VALUE)
		return (-1);

	GetConsoleMode(hConsoleIn, &saved_in_mode);
	GetConsoleMode(hConsoleOut, &saved_out_mode);
	mode_saved = 1;

	in_mode = ENABLE_VIRTUAL_TERMINAL_INPUT;

	out_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
	    ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;

	SetConsoleMode(hConsoleIn, in_mode);
	SetConsoleMode(hConsoleOut, out_mode);

	return (0);
}

void
win32_tty_restore(void)
{
	if (!mode_saved)
		return;

	SetConsoleMode(hConsoleIn, saved_in_mode);
	SetConsoleMode(hConsoleOut, saved_out_mode);
	mode_saved = 0;
}

int
win32_tty_control_mode(void)
{
	DWORD mode;

	hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);
	hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!GetConsoleMode(hConsoleIn, &mode))
		return (-1);
	saved_in_mode = mode;
	if (!GetConsoleMode(hConsoleOut, &saved_out_mode))
		saved_out_mode = 0;
	mode_saved = 1;
	SetConsoleMode(hConsoleIn,
	    mode &
	        ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
	            ENABLE_PROCESSED_INPUT));
	return (0);
}

int
win32_tty_get_size(int *cols, int *rows)
{
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	HANDLE h;

	if (win32_test_terminal() != NULL)
		return (win32_test_terminal_size(cols, rows));

	h = GetStdHandle(STD_OUTPUT_HANDLE);
	if (h == INVALID_HANDLE_VALUE)
		return (-1);

	if (!GetConsoleScreenBufferInfo(h, &csbi))
		return (-1);

	*cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
	*rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
	return (0);
}

void
win32_tty_init_utf8(void)
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
}

int	b64_ntop(const unsigned char *, size_t, char *, size_t);

const char *
win32_test_terminal(void)
{
	static char	*path;
	static int	 checked;
	const char	*value;

	if (!checked) {
		checked = 1;
		value = getenv("TMUX_WIN32_TEST_TERMINAL");
		if (value != NULL && *value != '\0')
			path = _strdup(value);
		_putenv("TMUX_WIN32_TEST_TERMINAL=");
		SetEnvironmentVariableA("TMUX_WIN32_TEST_TERMINAL", NULL);
	}
	return (path);
}

int
win32_test_terminal_size(int *cols, int *rows)
{
	FILE	*f;
	int	 c, r, n;

	if (win32_test_terminal() == NULL ||
	    (f = fopen(win32_test_terminal(), "r")) == NULL)
		return (-1);
	n = fscanf(f, "%d %d", &c, &r);
	fclose(f);
	if (n != 2 || c <= 0 || r <= 0 || c > 10000 || r > 10000)
		return (-1);
	*cols = c;
	*rows = r;
	return (0);
}

int
win32_test_terminal_changed(void)
{
	static char	 last[128];
	static int	 seen;
	char		 now[128];
	FILE		*f;
	size_t		 n = 0;
	int		 changed;

	if (win32_test_terminal() == NULL)
		return (0);
	if ((f = fopen(win32_test_terminal(), "r")) != NULL) {
		n = fread(now, 1, sizeof now - 1, f);
		fclose(f);
	}
	now[n] = '\0';
	if (n == 0)
		return (0);
	changed = seen && strcmp(now, last) != 0;
	memcpy(last, now, n + 1);
	seen = 1;
	return (changed);
}

static SRWLOCK	relay_send_lock = SRWLOCK_INIT;

static char	relay_held[32];
static size_t	relay_heldlen;

static int	relay_clipboard = -1;

int
win32_tty_relay_send(int fd, const char *buf, size_t len)
{
	fd_set	ready;
	int	n, error = 0;

	AcquireSRWLockExclusive(&relay_send_lock);
	while (len != 0) {
		n = send((SOCKET)fd, buf, len > INT_MAX ? INT_MAX : (int)len,
		    0);
		if (n > 0) {
			buf += n;
			len -= n;
			continue;
		}
		if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
			error = -1;
			break;
		}
		FD_ZERO(&ready);
		FD_SET((SOCKET)fd, &ready);
		select(0, NULL, &ready, NULL, NULL);
	}
	ReleaseSRWLockExclusive(&relay_send_lock);
	return (error);
}

static int
relay_osc52_query(const char *buf, size_t len, size_t *size, size_t *sellen)
{
	const char	*prefix = "\033]52;";
	size_t		 i, j;

	for (i = 0; i < 5; i++) {
		if (i == len)
			return (1);
		if (buf[i] != prefix[i])
			return (-1);
	}
	for (j = i; j < len && buf[j] != ';'; j++) {
		if (j - i == 8 || !isalnum((unsigned char)buf[j]))
			return (-1);
	}
	if (j == len)
		return (1);
	*sellen = j - i;
	if (++j == len)
		return (1);
	if (buf[j] != '?')
		return (-1);
	if (++j == len)
		return (1);
	if (buf[j] == '\007') {
		*size = j + 1;
		return (0);
	}
	if (buf[j] != '\033')
		return (-1);
	if (++j == len)
		return (1);
	if (buf[j] != '\\')
		return (-1);
	*size = j + 1;
	return (0);
}

static void
relay_osc52_answer(int fd, const char *sel, size_t sellen)
{
	HANDLE	 h;
	wchar_t	*w;
	char	*text = NULL, *b64, *reply;
	int	 n = 0, tries;
	size_t	 size;

	for (tries = 0; tries < 10 && !OpenClipboard(NULL); tries++)
		Sleep(10);
	if (tries != 10) {
		h = GetClipboardData(CF_UNICODETEXT);
		if (h != NULL && (w = GlobalLock(h)) != NULL) {
			n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL,
			    NULL);
			if (n > 1 && (text = malloc(n)) != NULL)
				WideCharToMultiByte(CP_UTF8, 0, w, -1, text, n,
				    NULL, NULL);
			n = (text == NULL) ? 0 : n - 1;
			GlobalUnlock(h);
		}
		CloseClipboard();
	}
	size = ((size_t)n + 2) / 3 * 4 + 1;
	reply = malloc(sellen + size + 8);
	if (reply != NULL) {
		memcpy(reply, "\033]52;", 5);
		memcpy(reply + 5, sel, sellen);
		reply[5 + sellen] = ';';
		b64 = reply + 6 + sellen;
		*b64 = '\0';
		if (n != 0)
			b64_ntop((unsigned char *)text, n, b64, size);
		strcat(b64, "\007");
		win32_tty_relay_send(fd, reply, strlen(reply));
		free(reply);
	}
	free(text);
}

void
win32_tty_relay_output(int fd, const char *data, size_t len)
{
	HANDLE	 out = GetStdHandle(STD_OUTPUT_HANDLE);
	char	*buf;
	size_t	 n, i, start = 0, size, sellen;
	DWORD	 written;
	int	 found;

	if (relay_clipboard == -1) {
		relay_clipboard = (!win32_remote_native_active() &&
		    win32_test_terminal() == NULL &&
		    getenv("SSH_CONNECTION") == NULL &&
		    getenv("SSH_CLIENT") == NULL);
	}
	if (!relay_clipboard) {
		WriteFile(out, data, (DWORD)len, &written, NULL);
		return;
	}
	n = relay_heldlen + len;
	if ((buf = malloc(n)) == NULL) {
		WriteFile(out, relay_held, (DWORD)relay_heldlen, &written,
		    NULL);
		relay_heldlen = 0;
		WriteFile(out, data, (DWORD)len, &written, NULL);
		return;
	}
	memcpy(buf, relay_held, relay_heldlen);
	memcpy(buf + relay_heldlen, data, len);
	relay_heldlen = 0;

	for (i = 0; i < n; i++) {
		if (buf[i] != '\033')
			continue;
		found = relay_osc52_query(buf + i, n - i, &size, &sellen);
		if (found == 1 && n - i <= sizeof relay_held) {
			relay_heldlen = n - i;
			memcpy(relay_held, buf + i, relay_heldlen);
			n = i;
			break;
		}
		if (found != 0)
			continue;
		WriteFile(out, buf + start, (DWORD)(i - start), &written,
		    NULL);
		relay_osc52_answer(fd, buf + i + 5, sellen);
		i += size - 1;
		start = i + 1;
	}
	if (start < n)
		WriteFile(out, buf + start, (DWORD)(n - start), &written, NULL);
	free(buf);
}
