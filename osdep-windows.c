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
#include <sys/stat.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#include <windows.h>
#include <psapi.h>

char *
osdep_get_name(int fd, char *tty)
{
	struct window_pane *wp;

	(void)tty;
	if (fd == -1)
		return (NULL);
	RB_FOREACH(wp, window_pane_tree, &all_window_panes)
	{
		if (wp->fd == fd && wp->win32_pty != NULL)
			return (win32_pty_foreground_name(wp->win32_pty));
	}
	return (NULL);
}

char *
osdep_get_cwd(int fd)
{
	static char *cwd;
	struct window_pane *wp;

	if (fd == -1)
		return (NULL);
	RB_FOREACH(wp, window_pane_tree, &all_window_panes)
	{
		if (wp->fd != fd)
			continue;
		free(cwd);
		cwd = win32_pty_foreground_cwd(wp->win32_pty);
		if (cwd != NULL)
			return (cwd);
		if (wp->win32_cwd != NULL)
			cwd = xstrdup(wp->win32_cwd);
		else if (wp->cwd != NULL)
			cwd = xstrdup(wp->cwd);
		else
			cwd = NULL;
		return (cwd);
	}
	return (NULL);
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	return (-1);
}

static int
win32_is_local_host(const char *host, size_t len)
{
	static const COMPUTER_NAME_FORMAT	 formats[] = {
		ComputerNameDnsHostname,
		ComputerNameDnsFullyQualified,
		ComputerNameNetBIOS
	};
	char					 name[256];
	DWORD					 size;
	u_int					 i;

	for (i = 0; i < nitems(formats); i++) {
		size = sizeof name;
		if (!GetComputerNameExA(formats[i], name, &size))
			continue;
		if (strlen(name) == len && strncasecmp(host, name, len) == 0)
			return (1);
	}
	return (0);
}

int
win32_pane_report_cwd(struct window_pane *wp, const char *value, int is_uri)
{
	char *path, *out;
	const char *in;
	struct stat sb;
	size_t len;
	int hi, lo, unc;

	if (is_uri) {
		if (strncasecmp(value, "file://", 7) != 0)
			return (0);
		value += 7;
		if ((in = strchr(value, '/')) == NULL)
			return (0);
		if (in != value && strncasecmp(value, "localhost/", 10) != 0 &&
		    !win32_is_local_host(value, in - value)) {
			log_debug("%s: %%%u: ignoring \"%s\"", __func__, wp->id,
			    value - 7);
			return (0);
		}
		path = out = xmalloc(strlen(in) + 1);
		for (; *in != '\0'; in++) {
			if (*in == '%' && (hi = hexval(in[1])) != -1 &&
			    (lo = hexval(in[2])) != -1) {
				*out++ = (char)(hi * 16 + lo);
				in += 2;
			} else
				*out++ = *in;
		}
		*out = '\0';
	} else {
		len = strlen(value);
		if (len >= 2 && value[0] == '"' && value[len - 1] == '"')
			path = xstrndup(value + 1, len - 2);
		else
			path = xstrdup(value);
	}

	if (path[0] == '/' && isalpha((u_char)path[1]) && path[2] == ':')
		memmove(path, path + 1, strlen(path));
	else if (path[0] == '/' && isalpha((u_char)path[1]) &&
	    (path[2] == '/' || path[2] == '\0')) {
		out = NULL;
		xasprintf(&out, "%c:%s", toupper((u_char)path[1]),
		    path[2] == '\0' ? "/" : path + 2);
		free(path);
		path = out;
	}
	for (out = path; *out != '\0'; out++) {
		if (*out == '/')
			*out = '\\';
	}
	len = strlen(path);
	if (len > 3 && path[len - 1] == '\\')
		path[len - 1] = '\0';

	unc = (path[0] == '\\' && path[1] == '\\');
	if (*path == '\0' ||
	    (!unc && (stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode)))) {
		log_debug("%s: %%%u: ignoring \"%s\"", __func__, wp->id, path);
		free(path);
		return (0);
	}
	log_debug("%s: %%%u: %s", __func__, wp->id, path);
	free(wp->win32_cwd);
	wp->win32_cwd = path;
	return (1);
}

struct event_base *
osdep_event_init(void)
{
	return (event_init());
}

