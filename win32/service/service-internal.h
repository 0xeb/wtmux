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

#ifndef SERVICE_INTERNAL_H
#define SERVICE_INTERNAL_H

struct buf {
	char	*data;
	size_t	 len;
	size_t	 size;
};

struct caller {
	PSID	 sid;
	wchar_t	 name[512];
	int	 admin;
};

struct wts_entry {
	DWORD	 id;
	int	 active;
	int	 locked;
	PSID	 sid;
	wchar_t	 name[512];
};

extern wchar_t	 self_path[MAX_PATH];
extern wchar_t	 tmux_path[MAX_PATH];

void		 fatal(const char *);
void		*xcalloc(size_t, size_t);
wchar_t		*to_wide(const char *);
char		*to_utf8(const wchar_t *);
int		 is_trusted_sid(PSID);
void		 sid_name(PSID, wchar_t *, size_t);
PSID		 copy_sid(PSID);
PSID		 token_user(HANDLE);
int		 process_is_elevated(void);
void		 service_log(WORD, const char *, ...);
struct wts_entry *wts_list(DWORD *);
void		 wts_free(struct wts_entry *, DWORD);
int		 client_request(int, char **);

void		 remote_reload(char *, size_t);
void		 remote_stop(void);
void		 remote_forget(void);
int		 remote_fingerprint(char *, size_t);
int		 cmd_listen(int, wchar_t **);
int		 cmd_keygen(int);

#endif
