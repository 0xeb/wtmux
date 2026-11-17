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

#include "win32-internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>

int
tmux_win32_error(struct tmux_os_error *error, DWORD code, const char *operation)
{
	wchar_t *wide = NULL;
	char message[384];
	int n = 0;
	if (error == NULL)
		return (-1);
	error->code = code;
	if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
	            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	        NULL, code, 0, (wchar_t *)&wide, 0, NULL) != 0) {
		n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, message,
		    sizeof message, NULL, NULL);
		LocalFree(wide);
	}
	if (n == 0)
		snprintf(message, sizeof message, "Windows error %lu",
		    (unsigned long)code);
	n = (int)strlen(message);
	while (n > 0 &&
	    (message[n - 1] == '\r' || message[n - 1] == '\n' ||
	        message[n - 1] == ' '))
		message[--n] = '\0';
	snprintf(error->message, sizeof error->message, "%s: %s", operation,
	    message);
	return (-1);
}

wchar_t *
tmux_win32_utf16(const char *text, struct tmux_os_error *error)
{
	wchar_t *wide;
	int n;
	if (text == NULL) {
		tmux_win32_error(error, ERROR_INVALID_PARAMETER, "UTF-8 input");
		return (NULL);
	}
	n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL,
	    0);
	if (n == 0) {
		tmux_win32_error(error, GetLastError(), "decode UTF-8");
		return (NULL);
	}
	wide = calloc((size_t)n, sizeof *wide);
	if (wide == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "UTF-16 allocation");
		return (NULL);
	}
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
	        n)) {
		tmux_win32_error(error, GetLastError(), "decode UTF-8");
		free(wide);
		return (NULL);
	}
	return (wide);
}

char *
tmux_win32_utf8(const wchar_t *wide, struct tmux_os_error *error)
{
	char *text;
	int n;
	if (wide == NULL) {
		tmux_win32_error(error, ERROR_INVALID_PARAMETER,
		    "UTF-16 input");
		return (NULL);
	}
	n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL,
	    0, NULL, NULL);
	if (n == 0) {
		tmux_win32_error(error, GetLastError(), "decode UTF-16");
		return (NULL);
	}
	text = malloc((size_t)n);
	if (text == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "UTF-8 allocation");
		return (NULL);
	}
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text,
	        n, NULL, NULL)) {
		tmux_win32_error(error, GetLastError(), "decode UTF-16");
		free(text);
		return (NULL);
	}
	return (text);
}

static int
compare_env(const void *left, const void *right)
{
	const wchar_t *a = *(const wchar_t *const *)left;
	const wchar_t *b = *(const wchar_t *const *)right;
	const wchar_t *ae = wcschr(a + (*a == L'='), L'=');
	const wchar_t *be = wcschr(b + (*b == L'='), L'=');
	int result =
	    CompareStringOrdinal(a, (int)(ae - a), b, (int)(be - b), TRUE);
	return result == CSTR_LESS_THAN   ? -1
	    : result == CSTR_GREATER_THAN ? 1
	                                  : 0;
}

wchar_t *
tmux_win32_environment(const char *const *env, struct tmux_os_error *error)
{
	size_t count = 0, i, units = 1, n;
	wchar_t **entries, *block = NULL, *at;
	DWORD code = ERROR_INVALID_PARAMETER;
	if (env == NULL)
		return NULL;
	while (env[count] != NULL) {
		if (++count > 32767) {
			tmux_win32_error(error, ERROR_BAD_ENVIRONMENT,
			    "environment count");
			return (NULL);
		}
	}
	entries = calloc(count + 1, sizeof *entries);
	if (entries == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "environment allocation");
		return (NULL);
	}
	for (i = 0; i < count; i++) {
		entries[i] = tmux_win32_utf16(env[i], error);
		if (entries[i] == NULL)
			goto done;
		if (entries[i][0] == L'\0' ||
		    wcschr(entries[i] + (entries[i][0] == L'='), L'=') ==
		        NULL) {
			tmux_win32_error(error, ERROR_BAD_ENVIRONMENT,
			    "environment entry");
			goto done;
		}
		n = wcslen(entries[i]) + 1;
		if (n > SIZE_MAX / sizeof(wchar_t) - units) {
			tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
			    "environment size");
			goto done;
		}
		units += n;
	}
	qsort(entries, count, sizeof *entries, compare_env);
	for (i = 1; i < count; i++) {
		if (compare_env(&entries[i - 1], &entries[i]) == 0) {
			tmux_win32_error(error, ERROR_BAD_ENVIRONMENT,
			    "duplicate environment name");
			goto done;
		}
	}
	block = calloc(units < 2 ? 2 : units, sizeof *block);
	if (block == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "environment allocation");
		goto done;
	}
	at = block;
	for (i = 0; i < count; i++) {
		n = wcslen(entries[i]) + 1;
		memcpy(at, entries[i], n * sizeof *at);
		at += n;
	}
	code = ERROR_SUCCESS;
done:
	for (i = 0; i < count; i++)
		free(entries[i]);
	free(entries);
	(void)code;
	return (block);
}

wchar_t *
tmux_win32_executable(const char *name, struct tmux_os_error *error)
{
	wchar_t *wide = tmux_win32_utf16(name, error), *path;
	DWORD count, actual;
	if (wide == NULL)
		return (NULL);
	if (wcschr(wide, L'\\') != NULL || wcschr(wide, L'/') != NULL ||
	    wcschr(wide, L':') != NULL)
		return (wide);
	count = SearchPathW(NULL, wide, L".exe", 0, NULL, NULL);
	if (count == 0) {
		tmux_win32_error(error, GetLastError(), "find executable");
		free(wide);
		return (NULL);
	}
	path = calloc((size_t)count + 1, sizeof *path);
	if (path == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "executable allocation");
		free(wide);
		return (NULL);
	}
	actual = SearchPathW(NULL, wide, L".exe", count + 1, path, NULL);
	free(wide);
	if (actual == 0 || actual > count) {
		tmux_win32_error(error,
		    actual == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER,
		    "find executable");
		free(path);
		return (NULL);
	}
	return (path);
}
