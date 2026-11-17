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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wtmux-service.h"

int
tmux_service_encode(int argc, char *const *argv, char **out, size_t *outlen)
{
	size_t	 len = 4, n;
	char	*buf, *p;
	int	 i;

	if (argc < 1 || argc > TMUX_SERVICE_MAXARGS)
		return (-1);
	for (i = 0; i < argc; i++) {
		n = strlen(argv[i]) + 1;
		if (n > TMUX_SERVICE_MAX || len + n > TMUX_SERVICE_MAX + 4)
			return (-1);
		len += n;
	}
	if ((buf = malloc(len)) == NULL)
		return (-1);
	n = len - 4;
	buf[0] = (char)(n & 0xff);
	buf[1] = (char)((n >> 8) & 0xff);
	buf[2] = (char)((n >> 16) & 0xff);
	buf[3] = (char)((n >> 24) & 0xff);
	p = buf + 4;
	for (i = 0; i < argc; i++) {
		n = strlen(argv[i]) + 1;
		memcpy(p, argv[i], n);
		p += n;
	}
	*out = buf;
	*outlen = len;
	return (0);
}

int
tmux_service_decode(const char *buf, size_t len, int *argc, char ***argv)
{
	char		**list;
	const char	 *p, *end = buf + len;
	int		  n = 0, i;

	if (len == 0 || len > TMUX_SERVICE_MAX || buf[len - 1] != '\0')
		return (-1);
	for (p = buf; p < end; p += strlen(p) + 1)
		n++;
	if (n > TMUX_SERVICE_MAXARGS)
		return (-1);
	if ((list = calloc(n + 1, sizeof *list)) == NULL)
		return (-1);
	for (p = buf, i = 0; i < n; p += strlen(p) + 1, i++) {
		if ((list[i] = strdup(p)) == NULL) {
			tmux_service_free_args(i, list);
			return (-1);
		}
	}
	*argc = n;
	*argv = list;
	return (0);
}

void
tmux_service_free_args(int argc, char **argv)
{
	int	i;

	if (argv == NULL)
		return;
	for (i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);
}

static int
tmux_service_valid_label(const char *label)
{
	const char	*p;

	if (*label == '\0' || strcmp(label, ".") == 0 ||
	    strcmp(label, "..") == 0)
		return (0);
	for (p = label; *p != '\0'; p++) {
		if (strchr("/\\:*?\"<>|", *p) != NULL ||
		    (unsigned char)*p < 0x20)
			return (0);
	}
	return (1);
}

int
tmux_service_parse_new(int argc, char *const *argv,
    struct tmux_service_new *opts, char *cause, size_t causelen)
{
	const char	*arg, **slot;
	char		*end;
	int		 i;

	memset(opts, 0, sizeof *opts);
	opts->session_id = -1;
	for (i = 1; i < argc; i++) {
		arg = argv[i];
		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (arg[0] != '-' || arg[1] == '\0')
			break;
		if (strcmp(arg, "-w") == 0) {
			opts->window = 1;
			continue;
		}
		if (arg[2] != '\0' || strchr("iLsu", arg[1]) == NULL) {
			snprintf(cause, causelen, "unknown option: %s", arg);
			return (-1);
		}
		if (i + 1 == argc || argv[i + 1][0] == '\0') {
			snprintf(cause, causelen, "%s needs an argument", arg);
			return (-1);
		}
		arg = argv[++i];
		switch (argv[i - 1][1]) {
		case 'i':
			opts->session_id = strtol(arg, &end, 10);
			if (*end != '\0' || opts->session_id < 0) {
				snprintf(cause, causelen,
				    "bad session id: %s", arg);
				return (-1);
			}
			continue;
		case 'L':
			if (!tmux_service_valid_label(arg)) {
				snprintf(cause, causelen,
				    "bad socket name: %s", arg);
				return (-1);
			}
			slot = &opts->label;
			break;
		case 's':
			slot = &opts->name;
			break;
		default:
			slot = &opts->user;
			break;
		}
		*slot = arg;
	}
	opts->ncommand = argc - i;
	opts->command = opts->ncommand > 0 ? argv + i : NULL;
	return (0);
}

enum tmux_service_verdict
tmux_service_policy(int same_user, int admin, unsigned int nsessions)
{
	if (!same_user && !admin)
		return (TMUX_SERVICE_DENY_OTHER_USER);
	if (nsessions == 0)
		return (TMUX_SERVICE_DENY_NOT_LOGGED_ON);
	return (TMUX_SERVICE_ALLOW);
}

int
tmux_service_pick_session(const struct tmux_service_session *s,
    unsigned int n, long want)
{
	unsigned int	i;
	int		best = -1;

	for (i = 0; i < n; i++) {
		if (want >= 0) {
			if (s[i].id == (unsigned long)want)
				return ((int)i);
			continue;
		}
		if (best == -1 ||
		    (s[i].active && !s[best].active) ||
		    (s[i].active == s[best].active && s[i].id < s[best].id))
			best = (int)i;
	}
	return (best);
}
