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

#include "win32-platform.h"
#include "platform/win32-internal.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct win32_pty {
	struct tmux_pty *backend;
	SOCKET sock, peer;
	HANDLE output_pump, input_pump;
	volatile LONG closing;
	int onlcr, pipes;
};
static size_t
pty_onlcr(const unsigned char *in, size_t length, unsigned char *out,
    size_t *out_length)
{
	size_t i, o = 0;
	for (i = 0; i < length; i++) {
		if (in[i] == '\n')
			out[o++] = '\r';
		out[o++] = in[i];
	}
	*out_length = o;
	return (length);
}

static DWORD WINAPI
pty_output_pump(LPVOID arg)
{
	struct win32_pty *pty = arg;
	unsigned char raw[16384], cooked[2 * 16384], *p;
	size_t length;
	ptrdiff_t got;
	int n;
	for (;;) {
		got = tmux_win32_pty_read_wait(pty->backend, raw, sizeof raw);
		if (got < 0)
			break;
		if (pty->onlcr) {
			pty_onlcr(raw, (size_t)got, cooked, &length);
			p = cooked;
		} else {
			p = raw;
			length = (size_t)got;
		}
		while (length > 0) {
			n = send(pty->peer, (const char *)p, (int)length, 0);
			if (n <= 0)
				goto out;
			p += n;
			length -= (size_t)n;
		}
	}
out:
	shutdown(pty->peer, SD_SEND);
	return (0);
}

static DWORD WINAPI
pty_input_pump(LPVOID arg)
{
	struct win32_pty *pty = arg;
	char input[16384];
	ptrdiff_t got;
	int n, offset;
	for (;;) {
		n = recv(pty->peer, input, sizeof input, 0);
		if (n == 0)
			tmux_win32_pty_end_input(pty->backend);
		if (n <= 0)
			break;
		for (offset = 0; offset < n; offset += (int)got) {
			got = tmux_win32_pty_write_wait(pty->backend,
			    input + offset, (size_t)(n - offset));
			if (got < 0)
				return (0);
		}
	}
	return (0);
}

static struct win32_pty *
spawn_child(const char *cmd, const char *cwd, char *env, int cols, int rows,
    u_int codepage, pid_t *out_pid, int use_pipes, int onlcr)
{
	struct win32_pty *pty;
	struct tmux_os_error error;
	int pair[2], one = 1;
	DWORD saved, forever = 0;
	if (cols <= 0 || rows <= 0 || cols > 32767 || rows > 32767) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return (NULL);
	}
	pty = calloc(1, sizeof *pty);
	if (pty == NULL)
		return (NULL);
	pty->sock = pty->peer = INVALID_SOCKET;
	pty->onlcr = onlcr;
	pty->pipes = use_pipes;
	if (tmux_win32_pty_spawn_raw(&pty->backend, cmd, cwd,
	        (const wchar_t *)env, (unsigned short)cols,
	        (unsigned short)rows, use_pipes, codepage, &error) != 0) {
		saved = error.code;
		free(pty);
		SetLastError(saved);
		return (NULL);
	}
	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, pair) != 0)
		goto fail;
	pty->sock = (SOCKET)pair[0];
	pty->peer = (SOCKET)pair[1];
	if (setsockopt(pty->peer, SOL_SOCKET, SO_SNDTIMEO,
	        (const char *)&forever, sizeof forever) != 0)
		goto fail;
	setsockopt(pty->peer, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
	    sizeof one);
	setsockopt(pty->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
	    sizeof one);
	pty->output_pump = CreateThread(NULL, 0, pty_output_pump, pty, 0, NULL);
	if (pty->output_pump == NULL)
		goto fail;
	pty->input_pump = CreateThread(NULL, 0, pty_input_pump, pty, 0, NULL);
	if (pty->input_pump == NULL)
		goto fail;
	if (out_pid != NULL)
		*out_pid = (pid_t)tmux_pty_pid(pty->backend);
	return (pty);
fail:
	saved = GetLastError();
	win32_pty_close(pty);
	SetLastError(saved);
	return (NULL);
}

struct win32_pty *
win32_pty_spawn(const char *cmd, const char *cwd, char *env, int cols, int rows,
    u_int codepage, pid_t *pid)
{
	return (spawn_child(cmd, cwd, env, cols, rows, codepage, pid, 0, 0));
}

u_int
win32_pane_codepage(const char *value)
{
	char	*end;
	u_long	 cp;

	if (value == NULL || *value == '\0')
		return (CP_UTF8);
	if (strcmp(value, "system") == 0)
		return (0);
	cp = strtoul(value, &end, 10);
	if (*end != '\0' || cp == 0 || cp > 65535 || !IsValidCodePage(cp))
		return (CP_UTF8);
	return ((u_int)cp);
}

struct win32_pty *
win32_pipe_spawn(const char *cmd, const char *cwd, char *env, pid_t *pid)
{
	return (spawn_child(cmd, cwd, env, 80, 24, 0, pid, 1, 0));
}

struct win32_pty *
win32_pipe_pane_spawn(const char *cmd, const char *cwd, char *env, pid_t *pid)
{
	return (spawn_child(cmd, cwd, env, 80, 24, 0, pid, 1, 1));
}

int
win32_pty_resize(struct win32_pty *pty, int cols, int rows)
{
	struct tmux_os_error error;
	if (pty == NULL || cols <= 0 || rows <= 0 || cols > 32767 ||
	    rows > 32767)
		return (-1);
	if (pty->onlcr)
		return 0;
	return tmux_pty_resize(pty->backend, (unsigned short)cols,
	    (unsigned short)rows, &error);
}

void
win32_pty_close(struct win32_pty *pty)
{
	if (pty == NULL)
		return;
	InterlockedExchange(&pty->closing, 1);
	if (pty->backend != NULL)
		tmux_win32_pty_abort(pty->backend);
	if (pty->sock != INVALID_SOCKET)
		shutdown(pty->sock, SD_BOTH);
	if (pty->peer != INVALID_SOCKET)
		shutdown(pty->peer, SD_BOTH);
	if (pty->output_pump != NULL) {
		if (WaitForSingleObject(pty->output_pump, 5000) ==
		        WAIT_TIMEOUT &&
		    pty->peer != INVALID_SOCKET) {
			closesocket(pty->peer);
			pty->peer = INVALID_SOCKET;
		}
		WaitForSingleObject(pty->output_pump, INFINITE);
		CloseHandle(pty->output_pump);
	}
	if (pty->input_pump != NULL) {
		if (WaitForSingleObject(pty->input_pump, 5000) ==
		        WAIT_TIMEOUT &&
		    pty->peer != INVALID_SOCKET) {
			closesocket(pty->peer);
			pty->peer = INVALID_SOCKET;
		}
		WaitForSingleObject(pty->input_pump, INFINITE);
		CloseHandle(pty->input_pump);
	}
	tmux_pty_destroy(pty->backend);
	if (pty->sock != INVALID_SOCKET)
		closesocket(pty->sock);
	if (pty->peer != INVALID_SOCKET)
		closesocket(pty->peer);
	free(pty);
}

int
win32_pty_get_fd(struct win32_pty *pty)
{
	return (pty == NULL ? -1 : (int)pty->sock);
}

int
win32_pty_is_console(struct win32_pty *pty)
{
	return (pty != NULL && !pty->onlcr && !pty->pipes);
}

int
win32_pty_key_records(struct win32_pty *pty)
{
	return (win32_pty_is_console(pty) && pty->backend != NULL &&
	    tmux_win32_pty_key_records(pty->backend));
}

size_t
win32_key_alt_record(unsigned char ch, char *buf, size_t size)
{
	SHORT scan = VkKeyScanW((WCHAR)ch);
	UINT vk = 0, sc = 0, state = LEFT_ALT_PRESSED;
	int n;
	if (scan != -1) {
		vk = LOBYTE(scan);
		if (HIBYTE(scan) & 1)
			state |= SHIFT_PRESSED;
		sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	}
	n = _snprintf_s(buf, size, _TRUNCATE,
	    "\033[%u;%u;%u;1;%u;1_\033[%u;%u;%u;0;%u;1_", vk, sc, (unsigned)ch,
	    state, vk, sc, (unsigned)ch, state);
	return (n < 0 ? 0 : (size_t)n);
}

HANDLE
win32_pty_get_process(struct win32_pty *pty)
{
	return (pty == NULL ? NULL : tmux_win32_pty_process(pty->backend));
}

char *
win32_pty_foreground_name(struct win32_pty *pty)
{
	struct tmux_os_error error;
	wchar_t path[MAX_PATH * 4], *base, *s;
	DWORD size = sizeof path / sizeof path[0];
	HANDLE h;
	uint64_t pid;
	size_t n;
	if (pty == NULL || (pid = tmux_win32_pty_foreground(pty->backend)) == 0)
		return (NULL);
	if ((h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
	         (DWORD)pid)) == NULL)
		return (NULL);
	if (!QueryFullProcessImageNameW(h, 0, path, &size)) {
		CloseHandle(h);
		return (NULL);
	}
	CloseHandle(h);
	for (base = s = path; *s != L'\0'; s++)
		if (*s == L'\\' || *s == L'/')
			base = s + 1;
	n = wcslen(base);
	if (n > 4 && _wcsicmp(base + n - 4, L".exe") == 0)
		base[n - 4] = L'\0';
	return (tmux_win32_utf8(base, &error));
}

char *
win32_pty_foreground_cwd(struct win32_pty *pty)
{
	uint64_t	pid;

	if (pty == NULL ||
	    (pid = tmux_win32_pty_foreground(pty->backend)) == 0)
		return (NULL);
	return (tmux_win32_process_cwd(pid));
}

int
win32_pty_write(struct win32_pty *pty, const void *data, size_t length)
{
	if (pty == NULL || length > INT_MAX)
		return (-1);
	return (send(pty->sock, data, (int)length, 0));
}
