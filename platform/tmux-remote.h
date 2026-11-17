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

#ifndef TMUX_REMOTE_H
#define TMUX_REMOTE_H

#include <stddef.h>

struct tmux_remote {
	char	 *dest;
	char	 *port;
	int	  native;
	int	  argc;
	char	**argv;
};

enum tmux_remote_kind {
	TMUX_REMOTE_OTHER,
	TMUX_REMOTE_ATTACH,
	TMUX_REMOTE_NEW,
	TMUX_REMOTE_NEW_DETACHED
};

int	 tmux_remote_split(int, char **, struct tmux_remote *, char *, size_t);
void	 tmux_remote_free(struct tmux_remote *);
enum tmux_remote_kind tmux_remote_kind(int, char **);
int	 tmux_remote_safe(int, char **);
char	*tmux_remote_plain(const char *, int, char **);
char	*tmux_remote_posix(const char *, int, char **);
char	*tmux_remote_windows_script(const char *, int, char **, int);
char	*tmux_remote_encode_script(const char *);
int	 tmux_remote_probe_is_windows(const char *);

#define TMUX_REMOTE_PROBE "echo $env:OS %OS%"

#define TMUX_REMOTE_PORT		7380
#define TMUX_REMOTE_FRAME_HEADER	4
#define TMUX_REMOTE_FRAME_MAX		65535
enum tmux_remote_channel {
	TMUX_REMOTE_IMSG = 0,
	TMUX_REMOTE_TTY = 1,
	TMUX_REMOTE_CONTROL = 2
};
void	 tmux_remote_frame_header(unsigned char *, int, size_t);
int	 tmux_remote_frame_parse(const unsigned char *, int *, size_t *);

int	 tmux_remote_known_host(const char *, const char *, const char *);
#define TMUX_REMOTE_HOST_UNKNOWN	0
#define TMUX_REMOTE_HOST_MATCH		1
#define TMUX_REMOTE_HOST_MISMATCH	-1

#define TMUX_REMOTE_KEY_TYPE	"tmux-ecdsa-p256"
int	 tmux_remote_key_authorized(const char *, const char *);

unsigned char *tmux_remote_key_message(const unsigned char *, size_t,
	    const unsigned char *, size_t, const char *, size_t *);

struct tmux_remote_filter {
	unsigned char	*buf;
	size_t		 len;
	size_t		 size;
	unsigned int	 pid;
	unsigned int	 type_cwd;
	unsigned int	 type_environ;
	unsigned int	 type_clientpid;
};
int	 tmux_remote_filter(struct tmux_remote_filter *, const void *, size_t,
	    unsigned char **, size_t *);
void	 tmux_remote_filter_free(struct tmux_remote_filter *);

#define TMUX_REMOTE_LIMIT_SLOTS	256
struct tmux_remote_limit {
	struct {
		char		key[128];
		unsigned int	failures;
		unsigned long long last;
	}		slot[TMUX_REMOTE_LIMIT_SLOTS];
};
unsigned long long tmux_remote_limit_wait(struct tmux_remote_limit *,
	    const char *, unsigned long long);
void	 tmux_remote_limit_fail(struct tmux_remote_limit *, const char *,
	    unsigned long long);
void	 tmux_remote_limit_ok(struct tmux_remote_limit *, const char *);
#endif
