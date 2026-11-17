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

#include "win32-tls.h"
#include <shellapi.h>
#include <shlobj.h>
#include <ws2tcpip.h>
#include <errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/win32-internal.h"
#include "tmux-platform.h"
#include "tmux-remote.h"

enum family {
	FAMILY_UNKNOWN,
	FAMILY_POSIX,
	FAMILY_WINDOWS
};

static BOOL WINAPI
remote_ctrl_handler(DWORD type)
{
	(void)type;
	return (TRUE);
}

static char *
cache_path(void)
{
	PWSTR	 base;
	char	*dir, *path;
	size_t	 n;
	struct tmux_os_error error;

	if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL,
	    &base)))
		return (NULL);
	dir = tmux_win32_utf8(base, &error);
	CoTaskMemFree(base);
	if (dir == NULL)
		return (NULL);
	n = strlen(dir) + 32;
	if ((path = malloc(n)) != NULL)
		snprintf(path, n, "%s\\tmux\\remote-hosts", dir);
	free(dir);
	return (path);
}

static char *
host_key(struct tmux_remote *r)
{
	char	*key;
	size_t	 n = strlen(r->dest) + (r->port != NULL ? strlen(r->port) : 0) +
		    2;

	if ((key = malloc(n)) == NULL)
		return (NULL);
	snprintf(key, n, "%s%s%s", r->dest, r->port != NULL ? ":" : "",
	    r->port != NULL ? r->port : "");
	return (key);
}

static enum family
cache_read(const char *key)
{
	char		*path, line[1024], *space;
	FILE		*f;
	enum family	 family = FAMILY_UNKNOWN;

	if ((path = cache_path()) == NULL)
		return (FAMILY_UNKNOWN);
	f = fopen(path, "r");
	free(path);
	if (f == NULL)
		return (FAMILY_UNKNOWN);
	while (fgets(line, sizeof line, f) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		if ((space = strrchr(line, ' ')) == NULL)
			continue;
		*space++ = '\0';
		if (strcmp(line, key) != 0)
			continue;
		if (strcmp(space, "windows") == 0)
			family = FAMILY_WINDOWS;
		else if (strcmp(space, "posix") == 0)
			family = FAMILY_POSIX;
	}
	fclose(f);
	return (family);
}

static void
cache_write(const char *key, enum family family)
{
	char	*path, *slash;
	FILE	*f;

	if ((path = cache_path()) == NULL)
		return;
	if ((slash = strrchr(path, '\\')) != NULL) {
		*slash = '\0';
		CreateDirectoryA(path, NULL);
		*slash = '\\';
	}
	if ((f = fopen(path, "a")) != NULL) {
		fprintf(f, "%s %s\n", key,
		    family == FAMILY_WINDOWS ? "windows" : "posix");
		fclose(f);
	}
	free(path);
}

static int
run_ssh(const char **argv, char *output, size_t size)
{
	STARTUPINFOW		 si;
	PROCESS_INFORMATION	 pi;
	SECURITY_ATTRIBUTES	 sa;
	HANDLE			 rd = NULL, wr = NULL;
	struct tmux_os_error	 error;
	char			*line;
	wchar_t			*wline;
	DWORD			 code = (DWORD)-1, n;
	size_t			 used = 0;
	int			 ok;

	if ((line = tmux_windows_command_line(argv)) == NULL)
		return (-1);
	wline = tmux_win32_utf16(line, &error);
	free(line);
	if (wline == NULL)
		return (-1);

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	if (output != NULL) {
		memset(&sa, 0, sizeof sa);
		sa.nLength = sizeof sa;
		sa.bInheritHandle = TRUE;
		if (!CreatePipe(&rd, &wr, &sa, 0)) {
			free(wline);
			return (-1);
		}
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
		si.hStdOutput = wr;
	}
	ok = CreateProcessW(NULL, wline, NULL, NULL, TRUE, 0, NULL, NULL, &si,
	    &pi);
	free(wline);
	if (wr != NULL)
		CloseHandle(wr);
	if (!ok) {
		if (rd != NULL)
			CloseHandle(rd);
		fprintf(stderr, "tmux: cannot run %s (error %lu)\n", argv[0],
		    GetLastError());
		return (-1);
	}
	CloseHandle(pi.hThread);
	if (output != NULL) {
		while (used + 1 < size &&
		    ReadFile(rd, output + used, (DWORD)(size - 1 - used), &n,
		    NULL) && n != 0)
			used += n;
		output[used] = '\0';
		CloseHandle(rd);
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return ((int)code);
}

static int
ssh_command(const char **argv, int max)
{
	static char	**words;
	static int	  nwords;
	struct tmux_os_error error;
	const wchar_t	 *env;
	wchar_t		**wide;
	int		  i, n;

	if (words == NULL) {
		env = _wgetenv(L"TMUX_SSH_COMMAND");
		if (env != NULL && *env != L'\0' &&
		    (wide = CommandLineToArgvW(env, &n)) != NULL) {
			words = calloc((size_t)n + 1, sizeof *words);
			for (i = 0; words != NULL && i < n; i++) {
				if ((words[nwords] = tmux_win32_utf8(wide[i],
				    &error)) != NULL)
					nwords++;
			}
			LocalFree(wide);
		}
		if (nwords == 0) {
			static char	*ssh[] = { "ssh", NULL };

			words = ssh;
			nwords = 1;
		}
	}
	for (i = 0; i < nwords && i < max; i++)
		argv[i] = words[i];
	return (i);
}

static enum family
find_family(struct tmux_remote *r)
{
	const char	*argv[40], *env;
	char		*key, output[4096];
	enum family	 family;
	int		 n = 0, status;

	env = getenv("TMUX_REMOTE_SHELL");
	if (env != NULL && strcmp(env, "windows") == 0)
		return (FAMILY_WINDOWS);
	if (env != NULL && strcmp(env, "posix") == 0)
		return (FAMILY_POSIX);
	if ((key = host_key(r)) == NULL)
		return (FAMILY_UNKNOWN);
	if ((family = cache_read(key)) != FAMILY_UNKNOWN) {
		free(key);
		return (family);
	}
	n = ssh_command(argv, 32);
	argv[n++] = "-T";
	if (r->port != NULL) {
		argv[n++] = "-p";
		argv[n++] = r->port;
	}
	argv[n++] = r->dest;
	argv[n++] = TMUX_REMOTE_PROBE;
	argv[n] = NULL;
	status = run_ssh(argv, output, sizeof output);
	if (status != 0) {
		fprintf(stderr, "tmux: %s: could not run a command over ssh "
		    "(exit %d)\n", key, status);
		free(key);
		return (FAMILY_UNKNOWN);
	}
	family = tmux_remote_probe_is_windows(output) ? FAMILY_WINDOWS :
	    FAMILY_POSIX;
	cache_write(key, family);
	free(key);
	return (family);
}

int
win32_remote_main(struct tmux_remote *r)
{
	enum tmux_remote_kind	 kind = tmux_remote_kind(r->argc, r->argv);
	enum family		 family = FAMILY_UNKNOWN;
	const char		*prog, *env, *argv[44];
	char			*command = NULL, *script, *encoded;
	int			 desktop, newsession, safe, n = 0, status;
	DWORD			 mode;
	size_t			 len;

	prog = getenv("TMUX_REMOTE_TMUX");
	if (prog == NULL || *prog == '\0')
		prog = "tmux";
	env = getenv("TMUX_REMOTE_DESKTOP");
	desktop = env == NULL || (strcmp(env, "0") != 0 &&
	    strcmp(env, "no") != 0 && strcmp(env, "off") != 0);
	newsession = (kind == TMUX_REMOTE_NEW ||
	    kind == TMUX_REMOTE_NEW_DETACHED);
	safe = tmux_remote_safe(r->argc, r->argv) &&
	    tmux_remote_safe(1, (char **)&prog);

	if (!safe || (desktop && newsession)) {
		family = find_family(r);
		if (family == FAMILY_UNKNOWN)
			return (255);
	}
	if (family == FAMILY_WINDOWS && (!safe || (desktop && newsession))) {
		script = tmux_remote_windows_script(prog, r->argc, r->argv,
		    desktop);
		encoded = script != NULL ?
		    tmux_remote_encode_script(script) : NULL;
		free(script);
		if (encoded == NULL)
			return (255);
		len = strlen(encoded) + 128;
		if ((command = malloc(len)) != NULL)
			snprintf(command, len, "powershell -NoLogo -NoProfile "
			    "-NonInteractive -ExecutionPolicy Bypass -OutputFormat Text "
			    "-EncodedCommand %s", encoded);
		free(encoded);
	} else if (family == FAMILY_POSIX)
		command = tmux_remote_posix(prog, r->argc, r->argv);
	else
		command = tmux_remote_plain(prog, r->argc, r->argv);
	if (command == NULL) {
		fprintf(stderr, "tmux: out of memory\n");
		return (255);
	}

	n = ssh_command(argv, 32);
	if ((kind == TMUX_REMOTE_ATTACH || kind == TMUX_REMOTE_NEW) &&
	    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
		argv[n++] = "-t";
	else
		argv[n++] = "-T";
	if (r->port != NULL) {
		argv[n++] = "-p";
		argv[n++] = r->port;
	}
	argv[n++] = r->dest;
	argv[n++] = command;
	argv[n] = NULL;

	SetConsoleCtrlHandler(remote_ctrl_handler, TRUE);
	status = run_ssh(argv, NULL, 0);
	SetConsoleCtrlHandler(remote_ctrl_handler, FALSE);
	free(command);
	return (status < 0 ? 255 : status);
}

int	 win32_ipc_connect(const char *);
int	 win32_ipc_connect_tty(const char *, const char *);
int	 win32_ipc_server_alive(const char *);
HANDLE	 win32_launch_server(const char *, const char *const *, HANDLE *);
int	 win32_socketpair(int, int, int, int *);

static struct {
	int			 active;
	char			*user;
	char			*host;
	char			*port;
	char			 key[300];
	int			 connected;
	struct win32_tls	 tls;
	int			 imsg[2];
	int			 tty[2];
	HANDLE			 replied;
	char			 reply[1024];
} native = { .imsg = { -1, -1 }, .tty = { -1, -1 } };

int
win32_remote_native_active(void)
{
	return (native.active);
}

int
win32_remote_native_setup(struct tmux_remote *r)
{
	WSADATA	 wsa;
	char	*at;

	WSAStartup(MAKEWORD(2, 2), &wsa);
	if ((at = strrchr(r->dest, '@')) != NULL) {
		*at = '\0';
		native.user = strdup(r->dest);
		native.host = strdup(at + 1);
	} else
		native.host = strdup(r->dest);
	if (r->port != NULL)
		native.port = strdup(r->port);
	else {
		native.port = malloc(8);
		if (native.port != NULL)
			snprintf(native.port, 8, "%d", TMUX_REMOTE_PORT);
	}
	if (native.host == NULL || native.port == NULL)
		return (-1);
	snprintf(native.key, sizeof native.key, "%s:%s", native.host,
	    native.port);
	native.active = 1;
	return (0);
}

char *
win32_remote_native_label(const char *label)
{
	char	*s;
	size_t	 n = strlen(native.key) + strlen(label) + 16;

	if ((s = malloc(n)) != NULL)
		snprintf(s, n, "tmux://%s/%s", native.key, label);
	return (s);
}

int
win32_remote_environ_allowed(const char *s)
{
	return (strncmp(s, "TERM=", 5) == 0 ||
	    strncmp(s, "COLORTERM=", 10) == 0 ||
	    strncmp(s, "LANG=", 5) == 0 || strncmp(s, "LC_", 3) == 0);
}

static int
read_answer(const char *prompt, int echo, char *buf, size_t size)
{
	HANDLE	 in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	 mode, n;
	wchar_t	 w[512];
	char	 c;
	size_t	 len = 0;
	int	 console;

	console = GetConsoleMode(in, &mode);
	fprintf(stderr, "%s", prompt);
	fflush(stderr);
	if (console) {
		if (!echo)
			SetConsoleMode(in, (mode | ENABLE_LINE_INPUT) &
			    ~ENABLE_ECHO_INPUT);
		if (!ReadConsoleW(in, w, 511, &n, NULL))
			n = 0;
		SetConsoleMode(in, mode);
		if (!echo)
			fprintf(stderr, "\n");
		while (n > 0 && (w[n - 1] == L'\n' || w[n - 1] == L'\r'))
			n--;
		w[n] = L'\0';
		if (WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)size,
		    NULL, NULL) == 0)
			return (-1);
		SecureZeroMemory(w, sizeof w);
		return (0);
	}
	while (len + 1 < size && ReadFile(in, &c, 1, &n, NULL) && n == 1 &&
	    c != '\n')
		buf[len++] = c;
	if (len > 0 && buf[len - 1] == '\r')
		len--;
	buf[len] = '\0';
	if (!echo)
		fprintf(stderr, "\n");
	return (0);
}

static char *
data_path(const char *name)
{
	PWSTR		 base;
	char		*dir, *path;
	size_t		 n;
	struct tmux_os_error error;

	if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL,
	    &base)))
		return (NULL);
	dir = tmux_win32_utf8(base, &error);
	CoTaskMemFree(base);
	if (dir == NULL)
		return (NULL);
	n = strlen(dir) + strlen(name) + 16;
	if ((path = malloc(n)) != NULL) {
		snprintf(path, n, "%s\\tmux", dir);
		CreateDirectoryA(path, NULL);
		snprintf(path, n, "%s\\tmux\\%s", dir, name);
	}
	free(dir);
	return (path);
}

static char *
read_file(const char *path)
{
	FILE	*f;
	char	*s;
	long	 n;

	if ((f = fopen(path, "rb")) == NULL)
		return (strdup(""));
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || n > 1048576 || (s = malloc((size_t)n + 1)) == NULL) {
		fclose(f);
		return (NULL);
	}
	n = (long)fread(s, 1, (size_t)n, f);
	s[n] = '\0';
	fclose(f);
	return (s);
}

static int
check_host(char *err, size_t errlen)
{
	const char	*env;
	char		 fp[128], answer[16], *path, *known;
	FILE		*f;
	int		 result;

	if (win32_tls_peer_fingerprint(&native.tls, fp, sizeof fp) != 0) {
		snprintf(err, errlen, "no host key");
		return (-1);
	}
	env = getenv("TMUX_REMOTE_KNOWN_HOSTS");
	path = (env != NULL && *env != '\0') ? strdup(env) :
	    data_path("known_hosts");
	if (path == NULL || (known = read_file(path)) == NULL) {
		free(path);
		snprintf(err, errlen, "cannot read known_hosts");
		return (-1);
	}
	result = tmux_remote_known_host(known, native.key, fp);
	free(known);
	if (result == TMUX_REMOTE_HOST_MATCH) {
		free(path);
		return (0);
	}
	if (result == TMUX_REMOTE_HOST_MISMATCH) {
		fprintf(stderr, "tmux: THE HOST KEY OF %s HAS CHANGED!\n"
		    "It is now %s, not the one in %s.\nSomeone may be in "
		    "the middle, or the host's wtmux-service was reinstalled; "
		    "check with \"wtmux-service fingerprint\" there, then "
		    "remove the old line.\n", native.key, fp, path);
		free(path);
		snprintf(err, errlen, "host key changed");
		return (-1);
	}
	fprintf(stderr, "tmux: the host key of %s is not known.\n"
	    "Its fingerprint is %s; compare it with \"wtmux-service "
	    "fingerprint\" on that machine.\n", native.key, fp);
	if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), (DWORD[]){ 0 })) {
		fprintf(stderr, "Not asking without a console; add \"%s %s\" "
		    "to %s to trust it.\n", native.key, fp, path);
		free(path);
		snprintf(err, errlen, "host key not known");
		return (-1);
	}
	if (read_answer("Trust it (yes/no)? ", 1, answer, sizeof answer) != 0 ||
	    strcmp(answer, "yes") != 0) {
		free(path);
		snprintf(err, errlen, "host key not trusted");
		return (-1);
	}
	if ((f = fopen(path, "ab")) != NULL) {
		fprintf(f, "%s %s\n", native.key, fp);
		fclose(f);
	}
	free(path);
	return (0);
}

static int
read_control(char *line, size_t size)
{
	static unsigned char	 buf[TMUX_REMOTE_FRAME_MAX + 1];
	size_t			 n;
	int			 channel;

	for (;;) {
		if (win32_tls_frame_read(&native.tls, &channel, buf, &n) != 0)
			return (-1);
		if (channel == TMUX_REMOTE_CONTROL) {
			snprintf(line, size, "%s", (char *)buf);
			return (0);
		}
	}
}

static const unsigned char *
binding_data(SecPkgContext_Bindings *b, size_t *n)
{
	SEC_CHANNEL_BINDINGS	*cb = b->Bindings;

	*n = cb->cbApplicationDataLength;
	return ((const unsigned char *)cb + cb->dwApplicationDataOffset);
}

static int
auth_key(const char *user, char *err, size_t errlen)
{
	NCRYPT_KEY_HANDLE	 key;
	SecPkgContext_Bindings	 b;
	const unsigned char	*bind;
	unsigned char		*challenge, *msg, sig[256];
	char			*pub, *b64, line[1024];
	size_t			 clen, blen, mlen;
	DWORD			 siglen = sizeof sig;
	int			 rc = -1;

	if (win32_tls_user_key(0, &key, NULL, 0) != 0)
		return (1);
	pub = win32_tls_user_key_public(key);
	if (pub == NULL || win32_tls_line(&native.tls, "key %s", pub) != 0 ||
	    read_control(line, sizeof line) != 0)
		goto out;
	if (strncmp(line, "challenge ", 10) != 0) {
		snprintf(err, errlen, "%s", line);
		rc = 1;
		goto out;
	}
	if ((challenge = win32_tls_unbase64(line + 10, &clen)) == NULL)
		goto out;
	if (win32_tls_binding(&native.tls, &b) != 0) {
		free(challenge);
		goto out;
	}
	bind = binding_data(&b, &blen);
	msg = tmux_remote_key_message(challenge, clen, bind, blen, user,
	    &mlen);
	free(challenge);
	FreeContextBuffer(b.Bindings);
	if (msg == NULL)
		goto out;
	if (win32_tls_user_key_sign(key, msg, mlen, sig, &siglen) != 0) {
		free(msg);
		snprintf(err, errlen, "cannot sign with the key");
		goto out;
	}
	free(msg);
	b64 = win32_tls_base64(sig, siglen);
	if (b64 == NULL || win32_tls_line(&native.tls, "sign %s", b64) != 0 ||
	    read_control(line, sizeof line) != 0) {
		free(b64);
		goto out;
	}
	free(b64);
	if (strncmp(line, "ok", 2) == 0)
		rc = 0;
	else {
		snprintf(err, errlen, "%s", line);
		rc = 1;
	}
out:
	free(pub);
	NCryptFreeObject(key);
	return (rc);
}

static int
auth_windows(char *err, size_t errlen)
{
	SEC_WINNT_AUTH_IDENTITY_W	 id, *idp = NULL;
	CredHandle			 cred;
	CtxtHandle			 ctx;
	SecPkgContext_Bindings		 b;
	SecPkgContext_Sizes		 sizes;
	SecBuffer			 inb[2], outb[1], mb[2];
	SecBufferDesc			 ind, outd, md;
	SECURITY_STATUS			 st;
	const unsigned char		*bind;
	unsigned char			*token = NULL, *sig = NULL;
	wchar_t				 target[300], wuser[256], wdomain[256];
	wchar_t				 wpass[256], *bs;
	char				 pass[256], line[16384], *b64;
	unsigned char			 mic[512];
	size_t				 tlen = 0, blen;
	ULONG				 attrs;
	int				 first = 1, rc = -1, have_ctx = 0;

	if (native.user != NULL) {
		snprintf(line, sizeof line, "%s@%s's password: ", native.user,
		    native.host);
		if (read_answer(line, 0, pass, sizeof pass) != 0)
			return (-1);
		MultiByteToWideChar(CP_UTF8, 0, native.user, -1, wuser, 256);
		MultiByteToWideChar(CP_UTF8, 0, pass, -1, wpass, 256);
		SecureZeroMemory(pass, sizeof pass);
		wdomain[0] = L'\0';
		if ((bs = wcschr(wuser, L'\\')) != NULL) {
			*bs = L'\0';
			wcscpy_s(wdomain, 256, wuser);
			memmove(wuser, bs + 1, (wcslen(bs + 1) + 1) *
			    sizeof (wchar_t));
		}
		memset(&id, 0, sizeof id);
		id.User = (unsigned short *)wuser;
		id.UserLength = (unsigned long)wcslen(wuser);
		id.Domain = (unsigned short *)wdomain;
		id.DomainLength = (unsigned long)wcslen(wdomain);
		id.Password = (unsigned short *)wpass;
		id.PasswordLength = (unsigned long)wcslen(wpass);
		id.Flags = SEC_WINNT_AUTH_IDENTITY_UNICODE;
		idp = &id;
	}
	st = AcquireCredentialsHandleW(NULL, L"Negotiate",
	    SECPKG_CRED_OUTBOUND, NULL, idp, NULL, NULL, &cred, NULL);
	SecureZeroMemory(wpass, sizeof wpass);
	if (st != SEC_E_OK) {
		snprintf(err, errlen, "no Windows credentials (0x%08lx); name "
		    "an account: tmux://user@%s", (unsigned long)st,
		    native.host);
		return (-1);
	}
	if (win32_tls_binding(&native.tls, &b) != 0) {
		FreeCredentialsHandle(&cred);
		return (-1);
	}
	_snwprintf_s(target, 300, _TRUNCATE, L"HOST/%S", native.host);
	if (win32_tls_line(&native.tls, "negotiate") != 0)
		goto out;
	for (;;) {
		inb[0].BufferType = SECBUFFER_TOKEN;
		inb[0].pvBuffer = token;
		inb[0].cbBuffer = (unsigned long)tlen;
		inb[1].BufferType = SECBUFFER_CHANNEL_BINDINGS;
		inb[1].pvBuffer = b.Bindings;
		inb[1].cbBuffer = b.BindingsLength;
		ind.ulVersion = SECBUFFER_VERSION;
		ind.cBuffers = 2;
		ind.pBuffers = token != NULL ? inb : inb + 1;
		if (token == NULL)
			ind.cBuffers = 1;
		outb[0].BufferType = SECBUFFER_TOKEN;
		outb[0].pvBuffer = NULL;
		outb[0].cbBuffer = 0;
		outd.ulVersion = SECBUFFER_VERSION;
		outd.cBuffers = 1;
		outd.pBuffers = outb;
		st = InitializeSecurityContextW(&cred, first ? NULL : &ctx,
		    target, ISC_REQ_INTEGRITY|ISC_REQ_CONNECTION|
		    ISC_REQ_ALLOCATE_MEMORY, 0, SECURITY_NATIVE_DREP, &ind, 0,
		    &ctx, &outd, &attrs, NULL);
		first = 0;
		free(token);
		token = NULL;
		if (st == SEC_I_COMPLETE_NEEDED ||
		    st == SEC_I_COMPLETE_AND_CONTINUE)
			CompleteAuthToken(&ctx, &outd);
		if (st == SEC_E_NO_CREDENTIALS && native.user == NULL) {
			snprintf(err, errlen, "this account cannot log on to "
			    "%s without a password; name an account: "
			    "tmux://user@%s", native.host, native.host);
			goto out;
		}
		if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED &&
		    st != SEC_I_COMPLETE_NEEDED &&
		    st != SEC_I_COMPLETE_AND_CONTINUE) {
			snprintf(err, errlen, "Windows logon failed "
			    "(0x%08lx)", (unsigned long)st);
			goto out;
		}
		have_ctx = 1;
		if (outb[0].cbBuffer != 0) {
			b64 = win32_tls_base64(outb[0].pvBuffer,
			    outb[0].cbBuffer);
			FreeContextBuffer(outb[0].pvBuffer);
			if (b64 == NULL ||
			    win32_tls_line(&native.tls, "token %s", b64) != 0) {
				free(b64);
				goto out;
			}
			free(b64);
		}
		if (st == SEC_E_OK || st == SEC_I_COMPLETE_NEEDED)
			break;
		if (read_control(line, sizeof line) != 0)
			goto out;
		if (strncmp(line, "token ", 6) != 0) {
			snprintf(err, errlen, "%s", line);
			goto out;
		}
		if ((token = win32_tls_unbase64(line + 6, &tlen)) == NULL)
			goto out;
	}

	if (QueryContextAttributesW(&ctx, SECPKG_ATTR_SIZES, &sizes) !=
	    SEC_E_OK || sizes.cbMaxSignature > sizeof mic)
		goto out;
	bind = binding_data(&b, &blen);
	if ((sig = malloc(blen + 32)) == NULL)
		goto out;
	memcpy(sig, "tmux-remote-cb-v1", 17);
	memcpy(sig + 17, bind, blen);
	mb[0].BufferType = SECBUFFER_DATA;
	mb[0].pvBuffer = sig;
	mb[0].cbBuffer = (unsigned long)(17 + blen);
	mb[1].BufferType = SECBUFFER_TOKEN;
	mb[1].pvBuffer = mic;
	mb[1].cbBuffer = sizes.cbMaxSignature;
	md.ulVersion = SECBUFFER_VERSION;
	md.cBuffers = 2;
	md.pBuffers = mb;
	if ((st = MakeSignature(&ctx, 0, &md, 0)) != SEC_E_OK) {
		snprintf(err, errlen, "cannot sign the channel (0x%08lx)",
		    (unsigned long)st);
		goto out;
	}
	b64 = win32_tls_base64(mic, mb[1].cbBuffer);
	if (b64 == NULL || win32_tls_line(&native.tls, "mic %s", b64) != 0 ||
	    read_control(line, sizeof line) != 0) {
		free(b64);
		goto out;
	}
	free(b64);
	if (strncmp(line, "ok", 2) == 0)
		rc = 0;
	else
		snprintf(err, errlen, "%s", line);
out:
	free(sig);
	free(token);
	FreeContextBuffer(b.Bindings);
	if (have_ctx)
		DeleteSecurityContext(&ctx);
	FreeCredentialsHandle(&cred);
	return (rc);
}

static void
account_name(char *out, size_t size)
{
	wchar_t			 name[256], domain[256], computer[256];
	DWORD			 n = 256, dn = 256, cn = 256;
	SID_NAME_USE		 use;
	BYTE			 sid[SECURITY_MAX_SID_SIZE];
	DWORD			 sidlen = sizeof sid;

	if (native.user != NULL) {
		snprintf(out, size, "%s", native.user);
		return;
	}
	*out = '\0';
	if (!GetUserNameW(name, &n))
		return;
	GetComputerNameW(computer, &cn);
	if (LookupAccountNameW(NULL, name, sid, &sidlen, domain, &dn, &use) &&
	    _wcsicmp(domain, computer) != 0)
		snprintf(out, size, "%ls\\%ls", domain, name);
	else
		snprintf(out, size, "%ls", name);
}

static int
native_open(char *err, size_t errlen)
{
	struct addrinfo	 hints, *res, *ai;
	SOCKET		 s = INVALID_SOCKET;
	char		 line[1024], user[600];
	const char	*env;
	int		 r;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(native.host, native.port, &hints, &res) != 0) {
		snprintf(err, errlen, "%s: host not found", native.host);
		return (-1);
	}
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == INVALID_SOCKET)
			continue;
		if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
			break;
		closesocket(s);
		s = INVALID_SOCKET;
	}
	freeaddrinfo(res);
	if (s == INVALID_SOCKET) {
		snprintf(err, errlen, "%s: cannot connect (is wtmux-service "
		    "listening there?)", native.key);
		return (-1);
	}
	if (win32_tls_client(&native.tls, s, native.host, err, errlen) != 0)
		return (-1);
	if (check_host(err, errlen) != 0)
		return (-1);

	account_name(user, sizeof user);
	if (win32_tls_line(&native.tls, "hello 1 user=%s", user) != 0 ||
	    read_control(line, sizeof line) != 0) {
		snprintf(err, errlen, "connection closed");
		return (-1);
	}
	if (strncmp(line, "methods ", 8) != 0) {
		snprintf(err, errlen, "%s", line);
		return (-1);
	}
	env = getenv("TMUX_REMOTE_AUTH");
	r = 1;
	if (strstr(line, " key") != NULL &&
	    (env == NULL || strcmp(env, "windows") != 0)) {
		if ((r = auth_key(user, err, errlen)) < 0)
			return (-1);
	}
	if (r != 0 && strstr(line, " windows") != NULL &&
	    (env == NULL || strcmp(env, "key") != 0))
		r = auth_windows(err, errlen);
	if (r != 0) {
		if (*err == '\0')
			snprintf(err, errlen, "no way to log on");
		return (-1);
	}
	return (0);
}

static DWORD WINAPI
native_writer(LPVOID arg)
{
	int	 channel = (int)(INT_PTR)arg, fd;
	char	 buf[16384];
	int	 n;

	fd = channel == TMUX_REMOTE_IMSG ? native.imsg[1] : native.tty[1];
	while ((n = recv((SOCKET)fd, buf, sizeof buf, 0)) > 0) {
		if (win32_tls_frame_write(&native.tls, channel, buf,
		    (size_t)n) != 0)
			break;
	}
	if (channel == TMUX_REMOTE_IMSG)
		shutdown(native.tls.s, SD_BOTH);
	return (0);
}

static int
send_fd(int fd, const unsigned char *p, size_t n)
{
	int	r;

	while (n > 0) {
		r = send((SOCKET)fd, (const char *)p, (int)n, 0);
		if (r <= 0)
			return (-1);
		p += r;
		n -= (size_t)r;
	}
	return (0);
}

static DWORD WINAPI
native_reader(LPVOID arg)
{
	static unsigned char	 buf[TMUX_REMOTE_FRAME_MAX + 1];
	size_t			 n;
	int			 channel;

	(void)arg;
	while (win32_tls_frame_read(&native.tls, &channel, buf, &n) == 0) {
		if (channel == TMUX_REMOTE_IMSG)
			send_fd(native.imsg[1], buf, n);
		else if (channel == TMUX_REMOTE_TTY && native.tty[1] != -1)
			send_fd(native.tty[1], buf, n);
		else if (channel == TMUX_REMOTE_CONTROL) {
			snprintf(native.reply, sizeof native.reply, "%s",
			    (char *)buf);
			SetEvent(native.replied);
		}
	}
	snprintf(native.reply, sizeof native.reply, "error connection closed");
	SetEvent(native.replied);
	if (native.imsg[1] != -1)
		shutdown((SOCKET)native.imsg[1], SD_BOTH);
	if (native.tty[1] != -1)
		shutdown((SOCKET)native.tty[1], SD_BOTH);
	return (0);
}

int
win32_remote_native_connect(const char *path, int start)
{
	const char	*label;
	char		 err[512] = "", line[1024];
	HANDLE		 h;

	if (!native.connected) {
		if (native_open(err, sizeof err) != 0) {
			if (strncmp(err, "denied ", 7) == 0)
				memmove(err, err + 7, strlen(err + 7) + 1);
			fprintf(stderr, "tmux: %s: %s\n", native.key, err);
			exit(1);
		}
		native.connected = 1;
	}
	if (strncmp(path, "tmux://", 7) == 0 &&
	    (label = strrchr(path, '/')) != NULL)
		win32_tls_line(&native.tls, "connect label=%s start=%d",
		    label + 1, start);
	else
		win32_tls_line(&native.tls, "connect path=%s start=%d", path,
		    start);
	if (read_control(line, sizeof line) != 0) {
		errno = EIO;
		return (-1);
	}
	if (strcmp(line, "refused") == 0) {
		errno = ECONNREFUSED;
		return (-1);
	}
	if (strcmp(line, "denied") == 0) {
		errno = EACCES;
		return (-1);
	}
	if (strcmp(line, "connected") != 0) {
		fprintf(stderr, "tmux: %s: %s\n", native.key,
		    strncmp(line, "error ", 6) == 0 ? line + 6 : line);
		exit(1);
	}
	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, native.imsg) != 0) {
		errno = EIO;
		return (-1);
	}
	native.replied = CreateEventW(NULL, FALSE, FALSE, NULL);
	h = CreateThread(NULL, 0, native_reader, NULL, 0, NULL);
	if (h != NULL)
		CloseHandle(h);
	h = CreateThread(NULL, 0, native_writer,
	    (LPVOID)(INT_PTR)TMUX_REMOTE_IMSG, 0, NULL);
	if (h != NULL)
		CloseHandle(h);
	return (native.imsg[0]);
}

int
win32_remote_native_tty(const char *token)
{
	HANDLE	h;

	if (native.tty[0] != -1 ||
	    win32_socketpair(AF_INET, SOCK_STREAM, 0, native.tty) != 0)
		return (-1);
	ResetEvent(native.replied);
	if (win32_tls_line(&native.tls, "tty %s", token) != 0 ||
	    WaitForSingleObject(native.replied, 30000) != WAIT_OBJECT_0 ||
	    strcmp(native.reply, "connected") != 0) {
		closesocket((SOCKET)native.tty[0]);
		closesocket((SOCKET)native.tty[1]);
		native.tty[0] = native.tty[1] = -1;
		return (-1);
	}
	h = CreateThread(NULL, 0, native_writer,
	    (LPVOID)(INT_PTR)TMUX_REMOTE_TTY, 0, NULL);
	if (h != NULL)
		CloseHandle(h);
	return (native.tty[0]);
}

static HANDLE		 relay_out;
static CRITICAL_SECTION	 relay_lock;
static int		 relay_imsg = -1, relay_tty = -1;
static int		 relay_may_start;

static int
relay_write(int channel, const void *data, size_t n)
{
	unsigned char	h[TMUX_REMOTE_FRAME_HEADER];
	DWORD		w;
	int		rc = 0;

	tmux_remote_frame_header(h, channel, n);
	EnterCriticalSection(&relay_lock);
	if (!WriteFile(relay_out, h, sizeof h, &w, NULL) || w != sizeof h ||
	    (n != 0 && (!WriteFile(relay_out, data, (DWORD)n, &w, NULL) ||
	    w != n)))
		rc = -1;
	LeaveCriticalSection(&relay_lock);
	return (rc);
}

static void
relay_line(const char *fmt, ...)
{
	char	line[2048];
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n > 0)
		relay_write(TMUX_REMOTE_CONTROL, line, strlen(line));
}

static DWORD WINAPI
relay_socket_reader(LPVOID arg)
{
	int	channel = (int)(INT_PTR)arg, fd;
	char	buf[16384];
	int	n;

	fd = channel == TMUX_REMOTE_IMSG ? relay_imsg : relay_tty;
	while ((n = recv((SOCKET)fd, buf, sizeof buf, 0)) > 0) {
		if (relay_write(channel, buf, (size_t)n) != 0)
			break;
	}
	if (channel == TMUX_REMOTE_IMSG)
		ExitProcess(0);
	return (0);
}

static int
read_all(HANDLE h, void *buf, size_t n)
{
	unsigned char	*p = buf;
	DWORD		 r;

	while (n > 0) {
		if (!ReadFile(h, p, (DWORD)n, &r, NULL) || r == 0)
			return (-1);
		p += r;
		n -= r;
	}
	return (0);
}

static int
relay_connect(const char *path, int start, char *err, size_t errlen)
{
	HANDLE		 server, errors = NULL;
	ULONGLONG	 begin;
	char		 pid[32], user[256];
	DWORD		 n;
	int		 fd;

	fd = win32_ipc_connect(path);
	if (fd != -1 || errno != ECONNREFUSED || !start)
		return (fd);
	if (!relay_may_start) {
		n = sizeof user;
		if (!GetUserNameA(user, &n))
			snprintf(user, sizeof user, "this user");
		snprintf(err, errlen, "no server running, and %s is not "
		    "logged on to start one", user);
		errno = EIO;
		return (-1);
	}
	snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
	SetEnvironmentVariableA("TMUX_WIN32_LAUNCHER_PID", pid);
	server = win32_launch_server(path, NULL, &errors);
	SetEnvironmentVariableA("TMUX_WIN32_LAUNCHER_PID", NULL);
	if (server == NULL) {
		snprintf(err, errlen, "cannot start a server");
		errno = EIO;
		return (-1);
	}
	begin = GetTickCount64();
	for (;;) {
		Sleep(100);
		if ((fd = win32_ipc_connect(path)) != -1)
			break;
		if (WaitForSingleObject(server, 0) == WAIT_OBJECT_0 &&
		    !win32_ipc_server_alive(path)) {
			n = 0;
			if (errors != NULL && ReadFile(errors, err,
			    (DWORD)errlen - 1, &n, NULL))
				err[n] = '\0';
			while (n > 0 && (err[n - 1] == '\n' ||
			    err[n - 1] == '\r'))
				err[--n] = '\0';
			errno = EIO;
			break;
		}
		if (GetTickCount64() - begin > 30000) {
			snprintf(err, errlen, "the server did not start");
			errno = EIO;
			break;
		}
	}
	CloseHandle(server);
	if (errors != NULL)
		CloseHandle(errors);
	return (fd);
}

int
win32_remote_relay(char *(*label_path)(const char *, char **),
    unsigned int type_cwd, unsigned int type_environ,
    unsigned int type_clientpid)
{
	static unsigned char		 buf[TMUX_REMOTE_FRAME_MAX + 1];
	struct tmux_remote_filter	 filter;
	unsigned char			 h[TMUX_REMOTE_FRAME_HEADER], *out;
	HANDLE				 in, t;
	char				*path = NULL, *cause, *p, err[512];
	size_t				 n, outlen;
	int				 channel, start, fd;

	p = getenv("TMUX_WIN32_RELAY");
	relay_may_start = (p != NULL && strcmp(p, "0") != 0);
	SetEnvironmentVariableA("TMUX_WIN32_RELAY", NULL);
	InitializeCriticalSection(&relay_lock);
	in = GetStdHandle(STD_INPUT_HANDLE);
	relay_out = GetStdHandle(STD_OUTPUT_HANDLE);
	memset(&filter, 0, sizeof filter);
	filter.pid = GetCurrentProcessId();
	filter.type_cwd = type_cwd;
	filter.type_environ = type_environ;
	filter.type_clientpid = type_clientpid;

	for (;;) {
		if (read_all(in, h, sizeof h) != 0 ||
		    tmux_remote_frame_parse(h, &channel, &n) != 0 ||
		    (n != 0 && read_all(in, buf, n) != 0))
			break;
		buf[n] = '\0';
		if (channel == TMUX_REMOTE_IMSG && relay_imsg != -1) {
			if (tmux_remote_filter(&filter, buf, n, &out,
			    &outlen) != 0)
				break;
			if (outlen != 0 && send_fd(relay_imsg, out, outlen) != 0)
				break;
			free(out);
			continue;
		}
		if (channel == TMUX_REMOTE_TTY && relay_tty != -1) {
			if (send_fd(relay_tty, buf, n) != 0)
				break;
			continue;
		}
		if (channel != TMUX_REMOTE_CONTROL)
			break;

		if (strncmp((char *)buf, "connect ", 8) == 0 &&
		    relay_imsg == -1) {
			p = strstr((char *)buf, " start=");
			start = (p != NULL && p[7] == '1');
			if (p != NULL)
				*p = '\0';
			free(path);
			path = NULL;
			*err = '\0';
			if (strncmp((char *)buf + 8, "label=", 6) == 0) {
				path = label_path((char *)buf + 14, &cause);
				if (path == NULL) {
					relay_line("error %s", cause != NULL ?
					    cause : "bad socket name");
					continue;
				}
			} else if (strncmp((char *)buf + 8, "path=", 5) == 0)
				path = strdup((char *)buf + 13);
			if (path == NULL) {
				relay_line("error bad request");
				continue;
			}
			fd = relay_connect(path, start, err, sizeof err);
			if (fd == -1) {
				if (*err != '\0')
					relay_line("error %s", err);
				else if (errno == ECONNREFUSED)
					relay_line("refused");
				else if (errno == EACCES)
					relay_line("denied");
				else
					relay_line("error %s", strerror(errno));
				continue;
			}
			relay_imsg = fd;
			relay_line("connected");
			t = CreateThread(NULL, 0, relay_socket_reader,
			    (LPVOID)(INT_PTR)TMUX_REMOTE_IMSG, 0, NULL);
			if (t != NULL)
				CloseHandle(t);
		} else if (strncmp((char *)buf, "tty ", 4) == 0 &&
		    path != NULL && relay_tty == -1) {
			fd = win32_ipc_connect_tty(path, (char *)buf + 4);
			if (fd == -1) {
				relay_line("error no tty channel");
				continue;
			}
			relay_tty = fd;
			relay_line("connected");
			t = CreateThread(NULL, 0, relay_socket_reader,
			    (LPVOID)(INT_PTR)TMUX_REMOTE_TTY, 0, NULL);
			if (t != NULL)
				CloseHandle(t);
		} else
			relay_line("error bad request");
	}
	if (relay_imsg != -1)
		closesocket((SOCKET)relay_imsg);
	if (relay_tty != -1)
		closesocket((SOCKET)relay_tty);
	return (0);
}