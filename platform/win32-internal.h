/* $OpenBSD$ */

/*
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

#ifndef TMUX_WIN32_INTERNAL_H
#define TMUX_WIN32_INTERNAL_H
#include <windows.h>
#include "tmux-platform.h"
int tmux_win32_error(struct tmux_os_error *, DWORD, const char *);
wchar_t *tmux_win32_utf16(const char *, struct tmux_os_error *);
char *tmux_win32_utf8(const wchar_t *, struct tmux_os_error *);
wchar_t *tmux_win32_environment(const char *const *, struct tmux_os_error *);
wchar_t *tmux_win32_executable(const char *, struct tmux_os_error *);
int tmux_win32_pty_spawn_raw(struct tmux_pty **, const char *, const char *,
    const wchar_t *, unsigned short, unsigned short, int, unsigned int,
    struct tmux_os_error *);
void tmux_win32_pty_end_input(struct tmux_pty *);
HANDLE tmux_win32_pty_process(struct tmux_pty *);
int tmux_win32_pty_key_records(struct tmux_pty *);
uint64_t tmux_win32_pty_foreground(struct tmux_pty *);
char *tmux_win32_process_cwd(uint64_t);
const char *tmux_win32_conpty_provider(void);
ptrdiff_t tmux_win32_pty_read_wait(struct tmux_pty *, void *, size_t);
ptrdiff_t tmux_win32_pty_write_wait(struct tmux_pty *, const void *, size_t);
void tmux_win32_pty_abort(struct tmux_pty *);
#endif
