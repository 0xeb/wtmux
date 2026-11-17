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

#include "tmux-platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

char *
tmux_windows_command_line(const char *const *argv)
{
	size_t length = 1, i, n, slashes;
	const char *s;
	char *result, *out;
	if (argv == NULL || argv[0] == NULL || argv[0][0] == '\0') {
		errno = EINVAL;
		return (NULL);
	}
	for (i = 0; argv[i] != NULL; i++) {
		n = strlen(argv[i]);
		if (n > (SIZE_MAX - length - 3) / 2) {
			errno = ENOMEM;
			return (NULL);
		}
		length += n * 2 + 3;
	}
	result = malloc(length);
	if (result == NULL)
		return (NULL);
	out = result;
	for (i = 0; argv[i] != NULL; i++) {
		if (i != 0)
			*out++ = ' ';
		*out++ = '"';
		slashes = 0;
		for (s = argv[i]; *s != '\0'; s++) {
			if (*s == '\\') {
				slashes++;
				continue;
			}
			if (*s == '"') {
				while (slashes != 0) {
					*out++ = '\\';
					*out++ = '\\';
					slashes--;
				}
				*out++ = '\\';
			} else {
				while (slashes != 0) {
					*out++ = '\\';
					slashes--;
				}
			}
			*out++ = *s;
		}
		while (slashes != 0) {
			*out++ = '\\';
			*out++ = '\\';
			slashes--;
		}
		*out++ = '"';
	}
	*out = '\0';
	return (result);
}

static char *
quote_cygwin(const char *const *argv)
{
	size_t length = 1, i, n;
	const char *s;
	char *result, *out;
	for (i = 0; argv[i] != NULL; i++) {
		n = strlen(argv[i]);
		if (n > (SIZE_MAX - length - 3) / 2) {
			errno = ENOMEM;
			return (NULL);
		}
		length += n * 2 + 3;
	}
	if ((result = malloc(length)) == NULL)
		return (NULL);
	out = result;
	for (i = 0; argv[i] != NULL; i++) {
		if (i != 0)
			*out++ = ' ';
		*out++ = '"';
		for (s = argv[i]; *s != '\0'; s++) {
			if (i != 0 && (*s == '\\' || *s == '"'))
				*out++ = '\\';
			*out++ = *s;
		}
		*out++ = '"';
	}
	*out = '\0';
	return (result);
}

enum shell_kind { SHELL_CMD, SHELL_POWERSHELL, SHELL_POSIX, SHELL_OTHER };
static enum shell_kind
shell_kind(const char *shell)
{
	static const char *const posix[] = {"sh", "bash", "zsh", "ksh", "mksh",
	    "dash", "ash", "fish", NULL};
	const char *base = shell, *s;
	char name[64];
	size_t n, i;
	for (s = shell; *s != '\0'; s++)
		if (*s == '\\' || *s == '/' || *s == ':')
			base = s + 1;
	n = strlen(base);
	if (n >= 4 && (base[n - 4] == '.') && (base[n - 3] | 0x20) == 'e' &&
	    (base[n - 2] | 0x20) == 'x' && (base[n - 1] | 0x20) == 'e')
		n -= 4;
	if (n == 0 || n >= sizeof name)
		return (SHELL_OTHER);
	for (i = 0; i < n; i++)
		name[i] = (base[i] >= 'A' && base[i] <= 'Z')
		    ? (char)(base[i] | 0x20)
		    : base[i];
	name[n] = '\0';
	if (strcmp(name, "cmd") == 0)
		return (SHELL_CMD);
	if (strcmp(name, "pwsh") == 0 || strcmp(name, "powershell") == 0)
		return (SHELL_POWERSHELL);
	for (i = 0; posix[i] != NULL; i++)
		if (strcmp(name, posix[i]) == 0)
			return (SHELL_POSIX);
	return (SHELL_OTHER);
}

char *
tmux_windows_shell_command_for(const char *shell, const char *command,
    int cygwin)
{
	const char *argv[6];
	char *result;
	size_t n;
	if (shell == NULL || *shell == '\0' || command == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	if (shell_kind(shell) == SHELL_CMD) {
		n = strlen(shell) + strlen(command) + 16;
		if ((result = malloc(n)) == NULL)
			return (NULL);
		snprintf(result, n, "\"%s\" /d /s /c \"%s\"", shell, command);
		return (result);
	}
	argv[0] = shell;
	if (shell_kind(shell) == SHELL_POWERSHELL) {
		argv[1] = "-NoLogo";
		argv[2] = "-NoProfile";
		argv[3] = "-Command";
		argv[4] = command;
		argv[5] = NULL;
	} else {
		argv[1] = "-c";
		argv[2] = command;
		argv[3] = NULL;
	}
	return (cygwin ? quote_cygwin(argv) : tmux_windows_command_line(argv));
}

char *
tmux_windows_shell_command(const char *shell, const char *command)
{
	return tmux_windows_shell_command_for(shell, command,
	    tmux_windows_is_cygwin_program(shell));
}

char *
tmux_windows_program_command_line(const char *const *argv)
{
	if (argv != NULL && argv[0] != NULL &&
	    tmux_windows_is_cygwin_program(argv[0]))
		return (quote_cygwin(argv));
	return (tmux_windows_command_line(argv));
}

#include <windows.h>
#include "win32-internal.h"
int
tmux_windows_is_cygwin_program(const char *program)
{
	static const wchar_t *const dlls[] = {L"msys-2.0.dll", L"cygwin1.dll",
	    NULL};
	struct tmux_os_error error;
	wchar_t path[MAX_PATH * 2], *wide, *base, *s;
	size_t i;
	DWORD n;
	if (program == NULL || *program == '\0')
		return (0);
	if ((wide = tmux_win32_utf16(program, &error)) == NULL)
		return (0);
	if (wcspbrk(wide, L"\\/:") != NULL) {
		if (wcslen(wide) >= sizeof path / sizeof path[0]) {
			free(wide);
			return (0);
		}
		wcscpy(path, wide);
	} else {
		n = SearchPathW(NULL, wide, L".exe",
		    sizeof path / sizeof path[0], path, NULL);
		if (n == 0 || n >= sizeof path / sizeof path[0]) {
			free(wide);
			return (0);
		}
	}
	free(wide);
	for (base = s = path; *s != L'\0'; s++)
		if (*s == L'\\' || *s == L'/')
			base = s + 1;
	for (i = 0; dlls[i] != NULL; i++) {
		if ((size_t)(base - path) + wcslen(dlls[i]) >=
		    sizeof path / sizeof path[0])
			return (0);
		wcscpy(base, dlls[i]);
		if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
			return (1);
	}
	return (0);
}

char *
tmux_windows_login_shell(const char *shell)
{
	const char *argv[3];
	if (shell == NULL || *shell == '\0') {
		errno = EINVAL;
		return (NULL);
	}
	argv[0] = shell;
	argv[1] = shell_kind(shell) == SHELL_POSIX ? "-l" : NULL;
	argv[2] = NULL;
	return (tmux_windows_command_line(argv));
}
