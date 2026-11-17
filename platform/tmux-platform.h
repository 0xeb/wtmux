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

#ifndef TMUX_PLATFORM_PTY_H
#define TMUX_PLATFORM_PTY_H
#include <stddef.h>
#include <stdint.h>

struct tmux_pty;
struct tmux_os_error {
	unsigned long code;
	char message[512];
};
struct tmux_pty_options {
	const char *const
	    *argv;
	const char *cwd;
	const char *const
	    *env;
	unsigned short columns, rows;
	unsigned int codepage;
};
int tmux_pty_spawn(struct tmux_pty **, const struct tmux_pty_options *,
    struct tmux_os_error *);
ptrdiff_t
tmux_pty_read(struct tmux_pty *, void *, size_t, struct tmux_os_error *);
ptrdiff_t
tmux_pty_write(struct tmux_pty *, const void *, size_t, struct tmux_os_error *);
int tmux_pty_resize(struct tmux_pty *, unsigned short, unsigned short,
    struct tmux_os_error *);
int tmux_pty_poll(struct tmux_pty *, uint32_t *, struct tmux_os_error *);
uint64_t tmux_pty_pid(const struct tmux_pty *);
void tmux_pty_destroy(struct tmux_pty *);

char *tmux_windows_command_line(const char *const *);
char *tmux_windows_shell_command(const char *, const char *);
char *tmux_windows_shell_command_for(const char *, const char *, int);
char *tmux_windows_program_command_line(const char *const *);
int tmux_windows_is_cygwin_program(const char *);
char *tmux_windows_login_shell(const char *);
int tmux_discovery_decode(const char *, size_t, char[65]);
#endif
