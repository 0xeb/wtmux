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

#define main tmux_native_main
#include "../tmux.c"
#undef main
#include "platform/win32-internal.h"
#include "tmux-remote.h"
#include "wtmux-version.h"

int win32_remote_main(struct tmux_remote *);
int win32_remote_native_setup(struct tmux_remote *);
int win32_remote_relay(char *(*)(const char *, char **), unsigned int,
    unsigned int, unsigned int);

#ifndef TMUX_WIN32_MIN_BUILD
#define TMUX_WIN32_MIN_BUILD 17763
#endif
static int
win32_wants_banner(int argc, char **argv)
{
	const char	*cp;
	int		 i, n = 0;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-' || argv[i][1] == '\0')
			return (0);
		for (cp = argv[i] + 1; *cp != '\0'; cp++) {
			if (*cp != 'V')
				return (0);
			n++;
		}
	}
	return (n >= 2);
}

static int
win32_build_supported(void)
{
	OSVERSIONINFOEXW v;
	DWORDLONG mask = 0;
	memset(&v, 0, sizeof v);
	v.dwOSVersionInfoSize = sizeof v;
	v.dwMajorVersion = 10;
	v.dwBuildNumber = TMUX_WIN32_MIN_BUILD;
	VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
	VER_SET_CONDITION(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
	return (VerifyVersionInfoW(&v, VER_MAJORVERSION | VER_BUILDNUMBER, mask));
}

int
wmain(int argc, wchar_t **wide_argv)
{
	char **utf8_argv;
	struct tmux_os_error error;
	int i, result;
	if (!win32_build_supported()) {
		fprintf(stderr,
		    "tmux: this build requires Windows 10 build %u or later\n",
		    (unsigned)TMUX_WIN32_MIN_BUILD);
		return (1);
	}
	utf8_argv = calloc((size_t)argc + 1, sizeof *utf8_argv);
	if (utf8_argv == NULL)
		return (1);
	for (i = 0; i < argc; i++) {
		utf8_argv[i] = tmux_win32_utf8(wide_argv[i], &error);
		if (utf8_argv[i] == NULL) {
			fprintf(stderr, "%s\n", error.message);
			while (i-- > 0)
				free(utf8_argv[i]);
			free(utf8_argv);
			return (1);
		}
	}
	if (win32_wants_banner(argc, utf8_argv)) {
		printf(WTMUX_BANNER "\n", getversion());
		result = 0;
		goto out;
	}
	(void)win32_test_terminal();
	if (getenv("TMUX_WIN32_RELAY") != NULL) {
		result = win32_remote_relay(make_label, MSG_IDENTIFY_CWD,
		    MSG_IDENTIFY_ENVIRON, MSG_IDENTIFY_CLIENTPID);
		goto out;
	}
	{
		struct tmux_remote	r;
		char			cause[256];

		switch (tmux_remote_split(argc, utf8_argv, &r, cause,
		    sizeof cause)) {
		case 1:
			if (r.native) {
				char	**args;

				if (win32_remote_native_setup(&r) != 0 ||
				    (args = calloc((size_t)r.argc + 2,
				    sizeof *args)) == NULL) {
					result = 1;
					goto out;
				}
				args[0] = utf8_argv[0];
				for (i = 0; i < r.argc; i++)
					args[i + 1] = r.argv[i];
				result = tmux_native_main(r.argc + 1, args);
				goto out;
			}
			result = win32_remote_main(&r);
			tmux_remote_free(&r);
			goto out;
		case -1:
			fprintf(stderr, "tmux: %s\nusage: tmux -H "
			    "[user@]host[:port] [flags] [command [flags]]\n",
			    cause);
			result = 1;
			goto out;
		}
	}
	result = tmux_native_main(argc, utf8_argv);
out:
	for (i = 0; i < argc; i++)
		free(utf8_argv[i]);
	free(utf8_argv);
	return (result);
}
