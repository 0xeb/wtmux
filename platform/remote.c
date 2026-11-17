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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux-platform.h"
#include "tmux-remote.h"

#define GLOBAL_WITH_ARG "cfLST"
#define NEW_WITH_ARG "cefFnstxy"

struct buf {
	char	*data;
	size_t	 len;
	size_t	 size;
	int	 failed;
};

static void
buf_add(struct buf *b, const char *s, size_t n)
{
	char	*p;

	if (b->failed)
		return;
	if (b->len + n + 1 > b->size) {
		b->size = (b->len + n + 1) * 2 + 64;
		if ((p = realloc(b->data, b->size)) == NULL) {
			b->failed = 1;
			return;
		}
		b->data = p;
	}
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void
buf_str(struct buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

static char *
buf_done(struct buf *b)
{
	if (b->failed || b->data == NULL) {
		free(b->data);
		return (NULL);
	}
	return (b->data);
}

static int
parse_dest(const char *value, struct tmux_remote *r, char *cause,
    size_t causelen)
{
	const char	*at, *host, *colon, *close;
	char		*end;
	long		 port;
	size_t		 n;

	if (strncmp(value, "ssh://", 6) == 0)
		value += 6;
	else if (strncmp(value, "tmux://", 7) == 0) {
		value += 7;
		r->native = 1;
	}
	if (*value == '\0' || *value == '-') {
		snprintf(cause, causelen, "bad host: %s", value);
		return (-1);
	}
	at = strrchr(value, '@');
	host = at != NULL ? at + 1 : value;
	if (*host == '\0') {
		snprintf(cause, causelen, "bad host: %s", value);
		return (-1);
	}
	colon = NULL;
	if (*host == '[') {
		if ((close = strchr(host, ']')) == NULL ||
		    (close[1] != '\0' && close[1] != ':')) {
			snprintf(cause, causelen, "bad host: %s", value);
			return (-1);
		}
		if (close[1] == ':')
			colon = close + 1;
	} else if ((colon = strchr(host, ':')) != NULL &&
	    strchr(colon + 1, ':') != NULL)
		colon = NULL;

	if (colon != NULL) {
		port = strtol(colon + 1, &end, 10);
		if (colon[1] == '\0' || *end != '\0' || port < 1 ||
		    port > 65535) {
			snprintf(cause, causelen, "bad port: %s", colon + 1);
			return (-1);
		}
		r->port = strdup(colon + 1);
		n = (size_t)(colon - value);
	} else
		n = strlen(value);

	r->dest = malloc(n + 1);
	if (r->dest == NULL || (colon != NULL && r->port == NULL)) {
		snprintf(cause, causelen, "out of memory");
		return (-1);
	}
	if (*host == '[') {
		size_t	user = (size_t)(host - value);

		memcpy(r->dest, value, user);
		memcpy(r->dest + user, host + 1, n - user - 2);
		r->dest[n - 2] = '\0';
	} else {
		memcpy(r->dest, value, n);
		r->dest[n] = '\0';
	}
	return (0);
}

int
tmux_remote_split(int argc, char **argv, struct tmux_remote *r, char *cause,
    size_t causelen)
{
	const char	*arg, *value = NULL;
	char		*kept;
	int		 i, j, k, n = 0, takes_next;

	memset(r, 0, sizeof *r);
	r->argv = calloc((size_t)argc + 1, sizeof *r->argv);
	if (r->argv == NULL) {
		snprintf(cause, causelen, "out of memory");
		return (-1);
	}
	for (i = 1; i < argc; i++) {
		arg = argv[i];
		if (arg[0] != '-' || arg[1] == '\0' || strcmp(arg, "--") == 0)
			break;
		if ((kept = strdup(arg)) == NULL)
			goto nomem;
		k = 1;
		takes_next = 0;
		for (j = 1; arg[j] != '\0'; j++) {
			if (arg[j] == 'H') {
				if (value != NULL) {
					snprintf(cause, causelen,
					    "-H given more than once");
					free(kept);
					goto fail;
				}
				if (arg[j + 1] != '\0')
					value = arg + j + 1;
				else if (i + 1 < argc)
					value = argv[++i];
				else {
					snprintf(cause, causelen,
					    "option requires an argument -- H");
					free(kept);
					goto fail;
				}
				break;
			}
			kept[k++] = arg[j];
			if (strchr(GLOBAL_WITH_ARG, arg[j]) != NULL) {
				if (arg[j + 1] == '\0')
					takes_next = 1;
				for (j++; arg[j] != '\0'; j++)
					kept[k++] = arg[j];
				break;
			}
		}
		kept[k] = '\0';
		if (k > 1)
			r->argv[n++] = kept;
		else
			free(kept);
		if (takes_next && i + 1 < argc) {
			if ((r->argv[n++] = strdup(argv[++i])) == NULL)
				goto nomem;
		}
	}
	if (value == NULL) {
		r->argc = n;
		tmux_remote_free(r);
		return (0);
	}
	for (; i < argc; i++) {
		if ((r->argv[n++] = strdup(argv[i])) == NULL)
			goto nomem;
	}
	r->argc = n;
	if (parse_dest(value, r, cause, causelen) != 0)
		goto fail;
	return (1);

nomem:
	snprintf(cause, causelen, "out of memory");
fail:
	r->argc = n;
	tmux_remote_free(r);
	return (-1);
}
void
tmux_remote_free(struct tmux_remote *r)
{
	int	i;

	if (r->argv != NULL) {
		for (i = 0; i < r->argc; i++)
			free(r->argv[i]);
		free(r->argv);
	}
	free(r->dest);
	free(r->port);
	memset(r, 0, sizeof *r);
}

static int
command_index(int argc, char **argv, const char **label, int *other)
{
	const char	*arg;
	int		 i, j;

	for (i = 0; i < argc; i++) {
		arg = argv[i];
		if (strcmp(arg, "--") == 0)
			return (i + 1);
		if (arg[0] != '-' || arg[1] == '\0')
			return (i);
		for (j = 1; arg[j] != '\0'; j++) {
			if (strchr(GLOBAL_WITH_ARG, arg[j]) == NULL) {
				if (other != NULL)
					*other = 1;
				continue;
			}
			if (arg[j] != 'L' && other != NULL)
				*other = 1;
			if (arg[j + 1] != '\0') {
				if (arg[j] == 'L' && label != NULL)
					*label = arg + j + 1;
			} else if (i + 1 < argc) {
				i++;
				if (arg[j] == 'L' && label != NULL)
					*label = argv[i];
			}
			break;
		}
	}
	return (argc);
}

static int
is_prefix(const char *name, const char *of, size_t min)
{
	size_t	n = strlen(name);

	return (n >= min && n <= strlen(of) && strncmp(name, of, n) == 0);
}

static int
parse_new(int argc, char **argv, int *detached, const char **name,
    int *command)
{
	const char	*arg;
	int		 i, j, simple = 1;

	*detached = 0;
	*name = NULL;
	for (i = 0; i < argc; i++) {
		arg = argv[i];
		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (arg[0] != '-' || arg[1] == '\0')
			break;
		for (j = 1; arg[j] != '\0'; j++) {
			if (arg[j] == 'd') {
				*detached = 1;
				continue;
			}
			if (strchr(NEW_WITH_ARG, arg[j]) == NULL) {
				simple = 0;
				continue;
			}
			if (arg[j] == 's' && j == 1 && arg[j + 1] == '\0' &&
			    i + 1 < argc)
				*name = argv[i + 1];
			else
				simple = 0;
			if (arg[j + 1] == '\0' && i + 1 < argc)
				i++;
			break;
		}
	}
	*command = i;
	return (simple);
}
enum tmux_remote_kind
tmux_remote_kind(int argc, char **argv)
{
	const char	*name, *s;
	int		 i, detached, command;

	i = command_index(argc, argv, NULL, NULL);
	if (i == argc)
		return (TMUX_REMOTE_NEW);
	name = argv[i];
	if (strcmp(name, "attach") == 0 ||
	    is_prefix(name, "attach-session", 1))
		return (TMUX_REMOTE_ATTACH);
	if (strcmp(name, "new") == 0 || is_prefix(name, "new-session", 5)) {
		parse_new(argc - i - 1, argv + i + 1, &detached, &s, &command);
		if (detached)
			return (TMUX_REMOTE_NEW_DETACHED);
		return (TMUX_REMOTE_NEW);
	}
	return (TMUX_REMOTE_OTHER);
}

static int
safe_arg(const char *s)
{
	const char	*p;

	if (*s == '\0' || *s == '@' || *s == '=' || *s == '~')
		return (0);
	for (p = s; *p != '\0'; p++) {
		if (!isalnum((unsigned char)*p) &&
		    strchr("._/:=+-@", *p) == NULL)
			return (0);
	}
	return (1);
}

int
tmux_remote_safe(int argc, char **argv)
{
	int	i;

	for (i = 0; i < argc; i++) {
		if (!safe_arg(argv[i]))
			return (0);
	}
	return (1);
}

char *
tmux_remote_plain(const char *prog, int argc, char **argv)
{
	struct buf	b = { 0 };
	int		i;

	buf_str(&b, prog);
	for (i = 0; i < argc; i++) {
		buf_str(&b, " ");
		buf_str(&b, argv[i]);
	}
	return (buf_done(&b));
}

static void
posix_quote(struct buf *b, const char *s)
{
	const char	*p;

	if (safe_arg(s)) {
		buf_str(b, s);
		return;
	}
	buf_str(b, "'");
	for (p = s; *p != '\0'; p++) {
		if (*p == '\'')
			buf_str(b, "'\\''");
		else
			buf_add(b, p, 1);
	}
	buf_str(b, "'");
}

char *
tmux_remote_posix(const char *prog, int argc, char **argv)
{
	struct buf	b = { 0 };
	int		i;

	posix_quote(&b, prog);
	for (i = 0; i < argc; i++) {
		buf_str(&b, " ");
		posix_quote(&b, argv[i]);
	}
	return (buf_done(&b));
}

static void
ps_quote(struct buf *b, const char *s)
{
	const unsigned char	*p = (const unsigned char *)s;

	buf_str(b, "'");
	for (; *p != '\0'; p++) {
		if (*p == '\'')
			buf_str(b, "''");
		else if (p[0] == 0xe2 && p[1] == 0x80 && p[2] >= 0x98 &&
		    p[2] <= 0x9b) {
			buf_add(b, (const char *)p, 3);
			buf_add(b, (const char *)p, 3);
			p += 2;
		} else
			buf_add(b, (const char *)p, 1);
	}
	buf_str(b, "'");
}

static char *
windows_args(int argc, char **argv)
{
	const char	**list;
	char		 *line, *args;
	int		  i;

	if ((list = calloc((size_t)argc + 2, sizeof *list)) == NULL)
		return (NULL);
	list[0] = "x";
	for (i = 0; i < argc; i++)
		list[i + 1] = argv[i];
	line = tmux_windows_command_line(list);
	free(list);
	if (line == NULL)
		return (NULL);
	args = strdup(line + (argc != 0 ? 4 : 3));
	free(line);
	return (args);
}

char *
tmux_remote_windows_script(const char *prog, int argc, char **argv,
    int desktop)
{
	struct buf	 b = { 0 };
	const char	*label = NULL, *name = NULL, *svc[32];
	char		*all, *line = NULL, *attach = NULL;
	int		 other = 0, i, ci, detached = 0, command, n = 0;
	enum tmux_remote_kind kind;

	if ((all = windows_args(argc, argv)) == NULL)
		return (NULL);
	buf_str(&b, "$ErrorActionPreference = 'Stop'\n"
	    "$ProgressPreference = 'SilentlyContinue'\n"
	    "function Start-Tool($file, $arguments, [switch]$Capture) {\n"
	    "  $i = New-Object Diagnostics.ProcessStartInfo $file, $arguments\n"
	    "  $i.UseShellExecute = $false\n"
	    "  if ($Capture) { $i.RedirectStandardOutput = $true;"
	    " $i.RedirectStandardError = $true }\n"
	    "  $p = [Diagnostics.Process]::Start($i)\n"
	    "  $o = ''\n"
	    "  if ($Capture) { $o = $p.StandardOutput.ReadToEnd() +"
	    " $p.StandardError.ReadToEnd() }\n"
	    "  $p.WaitForExit()\n"
	    "  return @($p.ExitCode, $o)\n"
	    "}\n"
	    "$tmux = ");
	ps_quote(&b, prog);
	buf_str(&b, "\n");

	kind = tmux_remote_kind(argc, argv);
	ci = command_index(argc, argv, &label, &other);
	if (desktop && !other &&
	    (kind == TMUX_REMOTE_NEW || kind == TMUX_REMOTE_NEW_DETACHED) &&
	    (ci == argc ||
	    parse_new(argc - ci - 1, argv + ci + 1, &detached, &name,
	    &command))) {
		svc[n++] = "new";
		if (label != NULL) {
			svc[n++] = "-L";
			svc[n++] = label;
		}
		if (name != NULL) {
			svc[n++] = "-s";
			svc[n++] = name;
		}
		if (ci < argc && ci + 1 + command < argc) {
			svc[n++] = "--";
			for (i = ci + 1 + command; i < argc && n < 31; i++)
				svc[n++] = argv[i];
		}
		svc[n] = NULL;
		line = windows_args(n, (char **)svc);
		svc[0] = "attach-session";
		svc[1] = "-t";
		n = 2;
		if (label != NULL) {
			svc[0] = "-L";
			svc[1] = label;
			svc[2] = "attach-session";
			svc[3] = "-t";
			n = 4;
		}
		attach = windows_args(n, (char **)svc);
	}
	if (line != NULL && attach != NULL) {
		buf_str(&b, "$cmd = Get-Command $tmux -CommandType Application"
		    " -ErrorAction SilentlyContinue | Select-Object -First 1\n"
		    "$svc = $null\n"
		    "if ($cmd) { $svc = Join-Path (Split-Path $cmd.Source)"
		    " 'wtmux-service.exe' }\n"
		    "if ($svc -and (Test-Path $svc) -and"
		    " (Get-Service wtmux-service"
		    " -ErrorAction SilentlyContinue |"
		    " Where-Object Status -eq Running)) {\n"
		    "  $r = Start-Tool $svc ");
		ps_quote(&b, line);
		buf_str(&b, " -Capture\n"
		    "  if ($r[0] -eq 0 -and $r[1] -match"
		    " '(?m)^started (.+) for (\\S+) in Windows session (\\d+)')"
		    " {\n"
		    "    $name = $Matches[1]\n"
		    "    [Console]::Error.WriteLine(\"tmux: $name is on the"
		    " desktop of $($Matches[2]) (Windows session"
		    " $($Matches[3]))\")\n");
		if (kind == TMUX_REMOTE_NEW_DETACHED)
			buf_str(&b, "    exit 0\n");
		else {
			buf_str(&b, "    $q = '\"=' + ($name -replace"
			    " '(\\\\*)\"', '$1$1\\\"' -replace '(\\\\+)$',"
			    " '$1$1') + '\"'\n"
			    "    $r = Start-Tool $tmux (");
			ps_quote(&b, attach);
			buf_str(&b, " + ' ' + $q)\n"
			    "    exit $r[0]\n");
		}
		buf_str(&b, "  }\n"
		    "  [Console]::Error.WriteLine('tmux: not on the desktop: ' +"
		    " $r[1].Trim())\n"
		    "}\n");
	}
	buf_str(&b, "$r = Start-Tool $tmux ");
	ps_quote(&b, all);
	buf_str(&b, "\nexit $r[0]\n");
	free(all);
	free(line);
	free(attach);
	return (buf_done(&b));
}

char *
tmux_remote_encode_script(const char *script)
{
	static const char	 b64[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char	*p = (const unsigned char *)script;
	unsigned char		*u16;
	unsigned int		 c, need;
	size_t			 n = 0, i, len = strlen(script);
	char			*out, *o;

	if ((u16 = malloc(len * 4 + 4)) == NULL)
		return (NULL);
	while (*p != '\0') {
		c = *p++;
		need = 0;
		if (c >= 0xf0 && c < 0xf8) {
			c &= 0x07;
			need = 3;
		} else if (c >= 0xe0) {
			c &= 0x0f;
			need = 2;
		} else if (c >= 0xc0) {
			c &= 0x1f;
			need = 1;
		} else if (c >= 0x80)
			c = 0xfffd;
		for (; need != 0; need--) {
			if ((*p & 0xc0) != 0x80) {
				c = 0xfffd;
				break;
			}
			c = (c << 6) | (*p++ & 0x3f);
		}
		if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
			c = 0xfffd;
		if (c >= 0x10000) {
			c -= 0x10000;
			u16[n++] = (0xd800 + (c >> 10)) & 0xff;
			u16[n++] = (0xd800 + (c >> 10)) >> 8;
			c = 0xdc00 + (c & 0x3ff);
		}
		u16[n++] = c & 0xff;
		u16[n++] = (c >> 8) & 0xff;
	}
	if ((out = malloc((n + 2) / 3 * 4 + 1)) == NULL) {
		free(u16);
		return (NULL);
	}
	o = out;
	for (i = 0; i + 2 < n; i += 3) {
		*o++ = b64[u16[i] >> 2];
		*o++ = b64[((u16[i] & 3) << 4) | (u16[i + 1] >> 4)];
		*o++ = b64[((u16[i + 1] & 15) << 2) | (u16[i + 2] >> 6)];
		*o++ = b64[u16[i + 2] & 63];
	}
	if (i < n) {
		*o++ = b64[u16[i] >> 2];
		if (i + 1 < n) {
			*o++ = b64[((u16[i] & 3) << 4) | (u16[i + 1] >> 4)];
			*o++ = b64[(u16[i + 1] & 15) << 2];
		} else {
			*o++ = b64[(u16[i] & 3) << 4];
			*o++ = '=';
		}
		*o++ = '=';
	}
	*o = '\0';
	free(u16);
	return (out);
}

int
tmux_remote_probe_is_windows(const char *output)
{
	return (strstr(output, "Windows_NT") != NULL);
}

void
tmux_remote_frame_header(unsigned char *h, int channel, size_t len)
{
	h[0] = (unsigned char)channel;
	h[1] = 0;
	h[2] = (unsigned char)((len >> 8) & 0xff);
	h[3] = (unsigned char)(len & 0xff);
}

int
tmux_remote_frame_parse(const unsigned char *h, int *channel, size_t *len)
{
	if (h[0] > TMUX_REMOTE_CONTROL || h[1] != 0)
		return (-1);
	*channel = h[0];
	*len = ((size_t)h[2] << 8) | h[3];
	return (0);
}

static const char *
next_line(const char **p, size_t *len)
{
	const char	*line = *p, *end;

	if (**p == '\0')
		return (NULL);
	end = strchr(line, '\n');
	if (end == NULL)
		end = line + strlen(line);
	*p = (*end == '\n') ? end + 1 : end;
	*len = (size_t)(end - line);
	if (*len > 0 && line[*len - 1] == '\r')
		(*len)--;
	return (line);
}

static int
next_word(const char **p, const char *end, const char *s)
{
	const char	*w;
	size_t		 n;

	while (*p < end && (**p == ' ' || **p == '\t'))
		(*p)++;
	w = *p;
	while (*p < end && **p != ' ' && **p != '\t')
		(*p)++;
	n = (size_t)(*p - w);
	return (n != 0 && n == strlen(s) && strncmp(w, s, n) == 0);
}

int
tmux_remote_known_host(const char *contents, const char *host,
    const char *fingerprint)
{
	const char	*p = contents, *line, *q;
	size_t		 len;
	int		 known = 0;

	while ((line = next_line(&p, &len)) != NULL) {
		if (len == 0 || *line == '#')
			continue;
		q = line;
		if (!next_word(&q, line + len, host))
			continue;
		known = 1;
		if (next_word(&q, line + len, fingerprint))
			return (TMUX_REMOTE_HOST_MATCH);
	}
	return (known ? TMUX_REMOTE_HOST_MISMATCH : TMUX_REMOTE_HOST_UNKNOWN);
}

int
tmux_remote_key_authorized(const char *contents, const char *key)
{
	const char	*p = contents, *line, *q;
	size_t		 len;

	while ((line = next_line(&p, &len)) != NULL) {
		if (len == 0 || *line == '#')
			continue;
		q = line;
		if (next_word(&q, line + len, TMUX_REMOTE_KEY_TYPE) &&
		    next_word(&q, line + len, key))
			return (1);
	}
	return (0);
}

unsigned char *
tmux_remote_key_message(const unsigned char *challenge, size_t clen,
    const unsigned char *binding, size_t blen, const char *user,
    size_t *outlen)
{
	static const char	 label[] = "tmux-remote-key-v1";
	unsigned char		*m, *p;
	size_t			 ulen = strlen(user);

	*outlen = sizeof label + 4 + clen + 4 + blen + ulen;
	if ((m = malloc(*outlen)) == NULL)
		return (NULL);
	p = m;
	memcpy(p, label, sizeof label);
	p += sizeof label;
	p[0] = (clen >> 24) & 0xff; p[1] = (clen >> 16) & 0xff;
	p[2] = (clen >> 8) & 0xff; p[3] = clen & 0xff;
	p += 4;
	memcpy(p, challenge, clen);
	p += clen;
	p[0] = (blen >> 24) & 0xff; p[1] = (blen >> 16) & 0xff;
	p[2] = (blen >> 8) & 0xff; p[3] = blen & 0xff;
	p += 4;
	memcpy(p, binding, blen);
	p += blen;
	memcpy(p, user, ulen);
	return (m);
}

static unsigned int
get32(const unsigned char *p)
{
	return ((unsigned int)p[0] | (unsigned int)p[1] << 8 |
	    (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24);
}

static void
put32(unsigned char *p, unsigned int v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static int
environ_allowed(const unsigned char *s, size_t n)
{
	static const char	*names[] = { "TERM=", "COLORTERM=", "LANG=",
				    "LC_", NULL };
	int			 i;

	for (i = 0; names[i] != NULL; i++) {
		if (n >= strlen(names[i]) &&
		    memcmp(s, names[i], strlen(names[i])) == 0)
			return (1);
	}
	return (0);
}

static int
filter_add(struct buf *b, const unsigned char *hdr, unsigned int pid,
    const void *data, size_t n)
{
	unsigned char	h[16];

	memcpy(h, hdr, 16);
	put32(h + 4, (unsigned int)(16 + n));
	put32(h + 12, pid);
	buf_add(b, (const char *)h, 16);
	buf_add(b, data, n);
	return (b->failed ? -1 : 0);
}

int
tmux_remote_filter(struct tmux_remote_filter *f, const void *in, size_t n,
    unsigned char **out, size_t *outlen)
{
	struct buf	 b = { 0 }, acc;
	unsigned char	*m, pid[4];
	unsigned int	 type, len;
	size_t		 off = 0;
	char		 empty = '\0';

	*out = NULL;
	*outlen = 0;
	acc.data = (char *)f->buf;
	acc.len = f->len;
	acc.size = f->size;
	acc.failed = 0;
	buf_add(&acc, in, n);
	if (acc.failed)
		return (-1);
	f->buf = (unsigned char *)acc.data;
	f->len = acc.len;
	f->size = acc.size;

	while (f->len - off >= 16) {
		m = f->buf + off;
		type = get32(m);
		len = get32(m + 4);
		if (len < 16 || len > 16384 + 16)
			goto bad;
		if (f->len - off < len)
			break;
		if (type == f->type_clientpid) {
			put32(pid, f->pid);
			if (filter_add(&b, m, f->pid, pid, sizeof pid) != 0)
				goto bad;
		} else if (type == f->type_cwd) {
			if (filter_add(&b, m, f->pid, &empty, 1) != 0)
				goto bad;
		} else if (type != f->type_environ ||
		    environ_allowed(m + 16, len - 16)) {
			if (filter_add(&b, m, f->pid, m + 16, len - 16) != 0)
				goto bad;
		}
		off += len;
	}
	memmove(f->buf, f->buf + off, f->len - off);
	f->len -= off;
	if (b.failed)
		goto bad;
	*out = (unsigned char *)b.data;
	*outlen = b.len;
	return (0);
bad:
	free(b.data);
	return (-1);
}

void
tmux_remote_filter_free(struct tmux_remote_filter *f)
{
	free(f->buf);
	f->buf = NULL;
	f->len = f->size = 0;
}

static int
limit_find(struct tmux_remote_limit *l, const char *key, int create)
{
	int	i, oldest = 0;

	for (i = 0; i < TMUX_REMOTE_LIMIT_SLOTS; i++) {
		if (l->slot[i].key[0] != '\0' &&
		    strcmp(l->slot[i].key, key) == 0)
			return (i);
		if (l->slot[i].last < l->slot[oldest].last)
			oldest = i;
	}
	if (!create)
		return (-1);
	memset(&l->slot[oldest], 0, sizeof l->slot[oldest]);
	snprintf(l->slot[oldest].key, sizeof l->slot[oldest].key, "%s", key);
	return (oldest);
}

unsigned long long
tmux_remote_limit_wait(struct tmux_remote_limit *l, const char *key,
    unsigned long long now)
{
	unsigned long long	wait, until;
	int			i;

	if ((i = limit_find(l, key, 0)) == -1 || l->slot[i].failures < 3)
		return (0);
	wait = 1000ULL << (l->slot[i].failures - 3 > 9 ? 9 :
	    l->slot[i].failures - 3);
	if (wait > 300000)
		wait = 300000;
	until = l->slot[i].last + wait;
	return (until > now ? until - now : 0);
}

void
tmux_remote_limit_fail(struct tmux_remote_limit *l, const char *key,
    unsigned long long now)
{
	int	i = limit_find(l, key, 1);

	l->slot[i].failures++;
	l->slot[i].last = now;
}

void
tmux_remote_limit_ok(struct tmux_remote_limit *l, const char *key)
{
	int	i;

	if ((i = limit_find(l, key, 0)) != -1)
		memset(&l->slot[i], 0, sizeof l->slot[i]);
}