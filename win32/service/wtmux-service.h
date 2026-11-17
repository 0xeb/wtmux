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

#ifndef TMUX_SERVICE_H
#define TMUX_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#define TMUX_SERVICE_NAME	L"wtmux-service"
#define TMUX_SERVICE_PIPE	L"\\\\.\\pipe\\wtmux-service"
#define TMUX_SERVICE_MAX	65536
#define TMUX_SERVICE_MAXARGS	64

int	tmux_service_encode(int, char *const *, char **, size_t *);
int	tmux_service_decode(const char *, size_t, int *, char ***);
void	tmux_service_free_args(int, char **);

struct tmux_service_new {
	const char	 *user;
	const char	 *name;
	const char	 *label;
	int		  window;
	long		  session_id;
	int		  ncommand;
	char *const	 *command;
};
int	tmux_service_parse_new(int, char *const *, struct tmux_service_new *,
	    char *, size_t);

enum tmux_service_verdict {
	TMUX_SERVICE_ALLOW,
	TMUX_SERVICE_DENY_OTHER_USER,
	TMUX_SERVICE_DENY_NOT_LOGGED_ON
};
enum tmux_service_verdict tmux_service_policy(int, int, unsigned int);

struct tmux_service_session {
	unsigned long	id;
	int		active;
};
int	tmux_service_pick_session(const struct tmux_service_session *,
	    unsigned int, long);

#endif
