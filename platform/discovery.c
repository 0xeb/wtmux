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
#include <string.h>

int
tmux_discovery_decode(const char *bytes, size_t length, char nonce[65])
{
	size_t i;
	unsigned char ch;
	if (bytes == NULL || nonce == NULL || length > 65)
		return (-1);
	for (i = 0; i < length && i < 64; i++) {
		ch = (unsigned char)bytes[i];
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
		        (ch >= 'A' && ch <= 'F')))
			return (-1);
	}
	if (length <= 64)
		return (0);
	if (bytes[64] != '\n')
		return (-1);
	memcpy(nonce, bytes, 64);
	nonce[64] = '\0';
	return (1);
}