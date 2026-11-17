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

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "win32-platform.h"
#include "platform/win32-internal.h"
#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <userenv.h>

char **environ = NULL;

void
win32_init_environ(void)
{
	wchar_t *block = GetEnvironmentStringsW(), *at;
	char **entries;
	size_t count = 0, i = 0;
	struct tmux_os_error error;
	if (block == NULL)
		abort();
	for (at = block; *at != L'\0'; at += wcslen(at) + 1)
		if (*at != L'=')
			count++;
	entries = calloc(count + 1, sizeof *entries);
	if (entries == NULL)
		abort();
	for (at = block; *at != L'\0'; at += wcslen(at) + 1) {
		if (*at == L'=')
			continue;
		entries[i] = tmux_win32_utf8(at, &error);
		if (entries[i++] == NULL)
			abort();
	}
	FreeEnvironmentStringsW(block);
	environ = entries;
}

char *
win32_getcwd(char *buffer, size_t capacity)
{
	DWORD units = GetCurrentDirectoryW(0, NULL), actual;
	wchar_t *wide;
	char *text;
	struct tmux_os_error error;
	size_t length;
	if (units == 0) {
		errno = EIO;
		return (NULL);
	}
	wide = calloc(units, sizeof *wide);
	if (wide == NULL) {
		errno = ENOMEM;
		return (NULL);
	}
	actual = GetCurrentDirectoryW(units, wide);
	if (actual == 0 || actual >= units) {
		free(wide);
		errno = EIO;
		return (NULL);
	}
	text = tmux_win32_utf8(wide, &error);
	free(wide);
	if (text == NULL) {
		errno = EILSEQ;
		return (NULL);
	}
	if (buffer == NULL)
		return (text);
	length = strlen(text) + 1;
	if (length > capacity) {
		free(text);
		errno = ERANGE;
		return (NULL);
	}
	memcpy(buffer, text, length);
	free(text);
	return (buffer);
}

int
win32_flock(int fd, int operation)
{
	HANDLE h;
	OVERLAPPED ov;
	DWORD flags = 0;

	h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return (-1);
	}

	memset(&ov, 0, sizeof ov);

	if (operation & LOCK_UN)
		return (UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) ? 0 : -1);

	if (operation & LOCK_EX)
		flags |= LOCKFILE_EXCLUSIVE_LOCK;
	if (operation & LOCK_NB)
		flags |= LOCKFILE_FAIL_IMMEDIATELY;

	if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
		if (GetLastError() == ERROR_LOCK_VIOLATION)
			errno = EAGAIN;
		else
			errno = EIO;
		return (-1);
	}
	return (0);
}

void
win32_closefrom(int lowfd)
{
	int maxfd, fd;

	maxfd = _getmaxstdio();
	if (maxfd < 256)
		maxfd = 256;
	for (fd = lowfd; fd < maxfd; fd++)
		_close(fd);
}

int
win32_clock_gettime(int clk_id, struct timespec *tp)
{
	static LARGE_INTEGER freq;
	static int freq_init = 0;
	LARGE_INTEGER counter;
	FILETIME ft;
	ULARGE_INTEGER uli;

	if (clk_id == CLOCK_MONOTONIC) {
		if (!freq_init) {
			QueryPerformanceFrequency(&freq);
			freq_init = 1;
		}
		QueryPerformanceCounter(&counter);
		tp->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
		tp->tv_nsec =
		    (long)(((counter.QuadPart % freq.QuadPart) * 1000000000LL) /
		        freq.QuadPart);
		return (0);
	}

	GetSystemTimeAsFileTime(&ft);
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	uli.QuadPart -= 116444736000000000ULL;
	tp->tv_sec = (time_t)(uli.QuadPart / 10000000ULL);
	tp->tv_nsec = (long)((uli.QuadPart % 10000000ULL) * 100);
	return (0);
}

int
win32_gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;
	ULARGE_INTEGER uli;

	(void)tz;
	GetSystemTimeAsFileTime(&ft);
	uli.LowPart = ft.dwLowDateTime;
	uli.HighPart = ft.dwHighDateTime;
	uli.QuadPart -= 116444736000000000ULL;
	tv->tv_sec = (long)(uli.QuadPart / 10000000ULL);
	tv->tv_usec = (long)((uli.QuadPart % 10000000ULL) / 10);
	return (0);
}

struct win32_user {
	uid_t	 uid;
	PSID	 sid;
};
static struct win32_user	*win32_users;
static u_int			 win32_nusers;

uid_t
win32_sid_uid(PSID sid)
{
	struct win32_user	*u;
	uid_t			 uid;
	DWORD			 len;
	u_int			 i;

	for (i = 0; i < win32_nusers; i++) {
		if (EqualSid(win32_users[i].sid, sid))
			return (win32_users[i].uid);
	}
	uid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
	for (i = 0; i < win32_nusers; i++) {
		if (uid == 0 || win32_users[i].uid == uid) {
			uid = 0x40000000 + win32_nusers;
			break;
		}
	}
	if (uid == 0)
		uid = 0x40000000 + win32_nusers;
	len = GetLengthSid(sid);
	u = realloc(win32_users, (win32_nusers + 1) * sizeof *win32_users);
	if (u == NULL)
		return (uid);
	win32_users = u;
	u = &win32_users[win32_nusers];
	if ((u->sid = malloc(len)) == NULL)
		return (uid);
	CopySid(len, u->sid, sid);
	u->uid = uid;
	win32_nusers++;
	return (uid);
}

PSID
win32_uid_sid(uid_t uid)
{
	u_int	i;

	for (i = 0; i < win32_nusers; i++) {
		if (win32_users[i].uid == uid)
			return (win32_users[i].sid);
	}
	return (NULL);
}

uid_t
win32_getuid(void)
{
	static uid_t	 uid = (uid_t)-1;
	HANDLE		 token;
	TOKEN_USER	*user;
	DWORD		 len = 0;

	if (uid != (uid_t)-1)
		return (uid);
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return (0x40000000);
	GetTokenInformation(token, TokenUser, NULL, 0, &len);
	if ((user = malloc(len)) != NULL &&
	    GetTokenInformation(token, TokenUser, user, len, &len))
		uid = win32_sid_uid(user->User.Sid);
	free(user);
	CloseHandle(token);
	if (uid == (uid_t)-1)
		return (0x40000000);
	return (uid);
}

static struct passwd	pw_entry;
static char		pw_name_buf[257 * 3];
static char		pw_dir_buf[MAX_PATH * 3];

static int
win32_profile_dir(PSID sid, char *buf, size_t size)
{
	wchar_t	 key[256], dir[MAX_PATH], *s;
	DWORD	 len = sizeof dir;

	if (!ConvertSidToStringSidW(sid, &s))
		return (-1);
	swprintf(key, 256, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
	    L"\\ProfileList\\%ls", s);
	LocalFree(s);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProfileImagePath",
	    RRF_RT_REG_SZ|RRF_RT_REG_EXPAND_SZ, NULL, dir, &len) !=
	    ERROR_SUCCESS)
		return (-1);
	if (WideCharToMultiByte(CP_UTF8, 0, dir, -1, buf, (int)size, NULL,
	    NULL) == 0)
		return (-1);
	return (0);
}

struct passwd *
win32_getpwuid(uid_t uid)
{
	wchar_t		 wide[MAX_PATH], domain[MAX_PATH];
	DWORD		 size, dsize;
	SID_NAME_USE	 use;
	PSID		 sid;

	if (uid == win32_getuid()) {
		size = sizeof wide / sizeof wide[0];
		if (!GetUserNameW(wide, &size) ||
		    WideCharToMultiByte(CP_UTF8, 0, wide, -1, pw_name_buf,
		    sizeof pw_name_buf, NULL, NULL) == 0)
			strncpy(pw_name_buf, "user", sizeof pw_name_buf);
		size = GetEnvironmentVariableW(L"USERPROFILE", wide,
		    sizeof wide / sizeof wide[0]);
		if (size == 0 || size >= sizeof wide / sizeof wide[0] ||
		    WideCharToMultiByte(CP_UTF8, 0, wide, -1, pw_dir_buf,
		    sizeof pw_dir_buf, NULL, NULL) == 0)
			strncpy(pw_dir_buf, "C:\\", sizeof pw_dir_buf);
	} else {
		if ((sid = win32_uid_sid(uid)) == NULL)
			return (NULL);
		size = sizeof wide / sizeof wide[0];
		dsize = sizeof domain / sizeof domain[0];
		if (!LookupAccountSidW(NULL, sid, wide, &size, domain, &dsize,
		    &use) ||
		    WideCharToMultiByte(CP_UTF8, 0, wide, -1, pw_name_buf,
		    sizeof pw_name_buf, NULL, NULL) == 0)
			return (NULL);
		if (win32_profile_dir(sid, pw_dir_buf, sizeof pw_dir_buf) != 0)
			pw_dir_buf[0] = '\0';
	}
	pw_entry.pw_name = pw_name_buf;
	pw_entry.pw_dir = pw_dir_buf;
	pw_entry.pw_shell = _PATH_BSHELL;
	pw_entry.pw_uid = uid;
	pw_entry.pw_gid = 0;
	return (&pw_entry);
}

struct passwd *
win32_getpwnam(const char *name)
{
	wchar_t		 wide[MAX_PATH], domain[MAX_PATH];
	BYTE		 sid[SECURITY_MAX_SID_SIZE];
	DWORD		 size = sizeof sid, dsize = MAX_PATH;
	SID_NAME_USE	 use;

	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, MAX_PATH) == 0)
		return (NULL);
	if (!LookupAccountNameW(NULL, wide, sid, &size, domain, &dsize, &use) ||
	    use != SidTypeUser)
		return (NULL);
	return (win32_getpwuid(win32_sid_uid(sid)));
}
static char progname_buf[MAX_PATH * 3];
static int progname_init = 0;

const char *
win32_getprogname(void)
{
	char *p;
	wchar_t wide[MAX_PATH];

	if (!progname_init) {
		if (GetModuleFileNameW(NULL, wide, MAX_PATH) == 0 ||
		    WideCharToMultiByte(CP_UTF8, 0, wide, -1, progname_buf,
		        sizeof progname_buf, NULL, NULL) == 0)
			strncpy(progname_buf, "tmux", sizeof progname_buf);
		p = strrchr(progname_buf, '\\');
		if (p != NULL) {
			memmove(progname_buf, p + 1, strlen(p + 1) + 1);
		}
		p = strrchr(progname_buf, '.');
		if (p != NULL && _stricmp(p, ".exe") == 0)
			*p = '\0';
		progname_init = 1;
	}
	return (progname_buf);
}

struct win32_child_exit {
	pid_t pid;
	int status;
	struct win32_child_exit *next;
};
static struct win32_child_exit *child_exit_list = NULL;
static CRITICAL_SECTION child_exit_lock;
static INIT_ONCE child_exit_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK
child_exit_init(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	(void)once;
	(void)parameter;
	(void)context;
	InitializeCriticalSection(&child_exit_lock);
	return (TRUE);
}

static void
child_exit_ensure_init(void)
{
	InitOnceExecuteOnce(&child_exit_once, child_exit_init, NULL, NULL);
}

void
win32_child_exited(pid_t pid, int status)
{
	struct win32_child_exit *e;

	child_exit_ensure_init();
	e = calloc(1, sizeof *e);
	if (e == NULL)
		return;
	e->pid = pid;
	e->status = status;

	EnterCriticalSection(&child_exit_lock);
	e->next = child_exit_list;
	child_exit_list = e;
	LeaveCriticalSection(&child_exit_lock);
}

pid_t
win32_waitpid(pid_t pid, int *status, int options)
{
	struct win32_child_exit **pp, *e;

	(void)options;
	child_exit_ensure_init();

	EnterCriticalSection(&child_exit_lock);
	for (pp = &child_exit_list; *pp != NULL; pp = &(*pp)->next) {
		e = *pp;
		if (pid == WAIT_ANY || e->pid == pid) {
			*pp = e->next;
			LeaveCriticalSection(&child_exit_lock);
			if (status != NULL)
				*status = e->status;
			pid = e->pid;
			free(e);
			return (pid);
		}
	}
	LeaveCriticalSection(&child_exit_lock);

	errno = ECHILD;
	return ((pid_t)-1);
}

pid_t
win32_getppid(void)
{
	PROCESSENTRY32W pe;
	HANDLE snap, h;
	DWORD self = GetCurrentProcessId(), parent = 0;
	FILETIME mine, theirs, x1, x2, x3;
	pid_t result = 1;

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return (1);
	pe.dwSize = sizeof pe;
	if (Process32FirstW(snap, &pe)) {
		do {
			if (pe.th32ProcessID == self) {
				parent = pe.th32ParentProcessID;
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	if (parent == 0)
		return (1);
	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
	    parent);
	if (h == NULL)
		return (1);
	if (GetProcessTimes(GetCurrentProcess(), &mine, &x1, &x2, &x3) &&
	    GetProcessTimes(h, &theirs, &x1, &x2, &x3) &&
	    CompareFileTime(&theirs, &mine) <= 0 &&
	    WaitForSingleObject(h, 0) == WAIT_TIMEOUT)
		result = (pid_t)parent;
	CloseHandle(h);
	return (result);
}

int
win32_kill(pid_t pid, int sig)
{
	HANDLE h;

	if (sig == 0) {
		h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h == NULL) {
			errno = ESRCH;
			return (-1);
		}
		CloseHandle(h);
		return (0);
	}

	if (sig == SIGTERM || sig == SIGKILL || sig == SIGHUP) {
		if (pid == (pid_t)GetCurrentProcessId() && sig != SIGKILL) {
			win32_signal_notify(sig);
			return (0);
		}
		h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
		if (h == NULL) {
			errno = EPERM;
			return (-1);
		}
		TerminateProcess(h, 1);
		CloseHandle(h);
		return (0);
	}

	return (0);
}

int
win32_socketpair(int domain, int type, int protocol, int sv[2])
{
	SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET,
	       peer = INVALID_SOCKET;
	struct sockaddr_in address, local, remote;
	int size, attempts;
	DWORD timeout = 3000;
	BOOL exclusive = TRUE;
	(void)domain;
	(void)type;
	(void)protocol;
	win32_wsa_init();
	listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
	    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (listener == INVALID_SOCKET)
		goto fail;
	if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
	        (const char *)&exclusive, sizeof exclusive))
		goto fail;
	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, (struct sockaddr *)&address, sizeof address))
		goto fail;
	size = sizeof address;
	if (getsockname(listener, (struct sockaddr *)&address, &size) ||
	    listen(listener, 16))
		goto fail;
	client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
	    WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
	if (client == INVALID_SOCKET ||
	    connect(client, (struct sockaddr *)&address, sizeof address))
		goto fail;
	size = sizeof local;
	if (getsockname(client, (struct sockaddr *)&local, &size))
		goto fail;
	for (attempts = 0; attempts < 64; attempts++) {
		fd_set readable;
		struct timeval wait;
		FD_ZERO(&readable);
		FD_SET(listener, &readable);
		wait.tv_sec = 3;
		wait.tv_usec = 0;
		if (select(0, &readable, NULL, NULL, &wait) <= 0)
			goto fail;
		size = sizeof remote;
		peer = accept(listener, (struct sockaddr *)&remote, &size);
		if (peer == INVALID_SOCKET)
			goto fail;
		if (remote.sin_addr.s_addr == local.sin_addr.s_addr &&
		    remote.sin_port == local.sin_port)
			break;
		closesocket(peer);
		peer = INVALID_SOCKET;
	}
	if (peer == INVALID_SOCKET)
		goto fail;
	if ((uintptr_t)client > INT_MAX || (uintptr_t)peer > INT_MAX) {
		WSASetLastError(WSAEMFILE);
		goto fail;
	}
	if (!SetHandleInformation((HANDLE)peer, HANDLE_FLAG_INHERIT, 0))
		goto fail;
	setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout,
	    sizeof timeout);
	setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout,
	    sizeof timeout);
	closesocket(listener);
	sv[0] = (int)client;
	sv[1] = (int)peer;
	return (0);
fail:
	if (peer != INVALID_SOCKET)
		closesocket(peer);
	if (client != INVALID_SOCKET)
		closesocket(client);
	if (listener != INVALID_SOCKET)
		closesocket(listener);
	errno = EIO;
	return (-1);
}

int
win32_getpeereid(int fd, uid_t *uid, gid_t *gid)
{
	PSID	sid;

	*uid = win32_getuid();
	*gid = (gid_t)-1;
	if ((sid = win32_ipc_peer_sid(fd)) != NULL)
		*uid = win32_sid_uid(sid);
	return (0);
}

ssize_t
win32_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	ssize_t total = 0;
	int i;

	for (i = 0; i < msg->msg_iovlen; i++) {
		ssize_t n =
		    send((SOCKET)fd, (const char *)msg->msg_iov[i].iov_base,
		        (int)msg->msg_iov[i].iov_len, flags);
		if (n < 0) {
			if (total > 0)
				return (total);
			errno = WSAGetLastError();
			return (-1);
		}
		total += n;
		if ((size_t)n < msg->msg_iov[i].iov_len)
			break;
	}
	return (total);
}

ssize_t
win32_recvmsg(int fd, struct msghdr *msg, int flags)
{
	ssize_t n;

	if (msg->msg_iovlen < 1)
		return (0);

	n = recv((SOCKET)fd, (char *)msg->msg_iov[0].iov_base,
	    (int)msg->msg_iov[0].iov_len, flags);
	if (n < 0)
		errno = WSAGetLastError();

	msg->msg_controllen = 0;
	return (n);
}

char *
strsep(char **stringp, const char *delim)
{
	char *s, *tok;
	const char *spanp;
	int c, sc;

	if ((s = *stringp) == NULL)
		return (NULL);
	for (tok = s;;) {
		c = *s++;
		spanp = delim;
		do {
			if ((sc = *spanp++) == c) {
				if (c == 0)
					s = NULL;
				else
					s[-1] = 0;
				*stringp = s;
				return (tok);
			}
		} while (sc != 0);
	}
}


int
win32_mkstemp(char *tmpl)
{
	if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0)
		return (-1);
	return (_open(tmpl, _O_CREAT | _O_EXCL | _O_RDWR, _S_IREAD | _S_IWRITE));
}

int
win32_wcwidth(wchar_t wc)
{
	if (wc == 0)
		return (0);
	if (wc < 32 || (wc >= 0x7f && wc < 0xa0))
		return (-1);
	if ((wc >= 0x1100 && wc <= 0x115f) || wc == 0x2329 || wc == 0x232a ||
	    (wc >= 0x2e80 && wc <= 0xa4cf && wc != 0x303f) ||
	    (wc >= 0xac00 && wc <= 0xd7a3) || (wc >= 0xf900 && wc <= 0xfaff) ||
	    (wc >= 0xfe10 && wc <= 0xfe19) || (wc >= 0xfe30 && wc <= 0xfe6f) ||
	    (wc >= 0xff01 && wc <= 0xff60) || (wc >= 0xffe0 && wc <= 0xffe6))
		return (2);
	return (1);
}

static int
fnmatch_fold(int c, int flags)
{
	return ((flags & FNM_CASEFOLD) ? tolower(c) : c);
}

static int
fnmatch_bracket(const char **patp, int c, int flags)
{
	const char *pat = *patp;
	int negate = 0, match = 0, first = 1, lo, hi;

	if (*pat == '!' || *pat == '^') {
		negate = 1;
		pat++;
	}
	for (;;) {
		if (*pat == '\0')
			return (-1);
		if (*pat == ']' && !first)
			break;
		first = 0;
		if (*pat == '\\' && !(flags & FNM_NOESCAPE) && pat[1] != '\0')
			pat++;
		lo = (unsigned char)*pat++;
		hi = lo;
		if (*pat == '-' && pat[1] != ']' && pat[1] != '\0') {
			pat++;
			if (*pat == '\\' && !(flags & FNM_NOESCAPE) &&
			    pat[1] != '\0')
				pat++;
			hi = (unsigned char)*pat++;
		}
		if ((flags & FNM_PATHNAME) && c == '/')
			continue;
		if (fnmatch_fold(c, flags) >= fnmatch_fold(lo, flags) &&
		    fnmatch_fold(c, flags) <= fnmatch_fold(hi, flags))
			match = 1;
	}
	*patp = pat + 1;
	return (match != negate);
}

static int
fnmatch_internal(const char *pat, const char *str, const char *start, int flags)
{
	const char *save;
	int pc, sc, r;

	for (;;) {
		pc = (unsigned char)*pat++;
		sc = (unsigned char)*str;

		if ((flags & FNM_PERIOD) && sc == '.' && pc != '.' &&
		    (str == start ||
		        ((flags & FNM_PATHNAME) && str[-1] == '/')) &&
		    (pc == '*' || pc == '?' || pc == '['))
			return (FNM_NOMATCH);

		switch (pc) {
		case '\0':
			return (sc == '\0' ? 0 : FNM_NOMATCH);
		case '?':
			if (sc == '\0')
				return (FNM_NOMATCH);
			if ((flags & FNM_PATHNAME) && sc == '/')
				return (FNM_NOMATCH);
			str++;
			break;
		case '*':
			while (*pat == '*')
				pat++;
			if (*pat == '\0') {
				if (flags & FNM_PATHNAME)
					return (strchr(str, '/') == NULL
					        ? 0
					        : FNM_NOMATCH);
				return (0);
			}
			for (; *str != '\0'; str++) {
				if (fnmatch_internal(pat, str, start,
				        flags & ~FNM_PERIOD) == 0)
					return (0);
				if ((flags & FNM_PATHNAME) && *str == '/')
					break;
			}
			return (fnmatch_internal(pat, str, start,
			    flags & ~FNM_PERIOD));
		case '[':
			if (sc == '\0')
				return (FNM_NOMATCH);
			save = pat;
			r = fnmatch_bracket(&pat, sc, flags);
			if (r == -1) {
				pat = save;
				if (sc != '[')
					return (FNM_NOMATCH);
			} else if (r == 0)
				return (FNM_NOMATCH);
			str++;
			break;
		case '\\':
			if (!(flags & FNM_NOESCAPE)) {
				pc = (unsigned char)*pat++;
				if (pc == '\0') {
					pc = '\\';
					pat--;
				}
			}
		default:
			if (fnmatch_fold(pc, flags) != fnmatch_fold(sc, flags))
				return (FNM_NOMATCH);
			str++;
			break;
		}
	}
}

int
win32_fnmatch(const char *pattern, const char *string, int flags)
{
	return (fnmatch_internal(pattern, string, string, flags));
}



#define WIN32_TIME_CYCLE (146097LL * 86400)

static struct tm *
win32_time_convert(const time_t *t, struct tm *result, int local)
{
	long long	cycles, years;
	time_t		moved;
	errno_t		error;

	cycles = 0;
	moved = *t;
	if (moved < WIN32_TIME_CYCLE || moved >= 2 * WIN32_TIME_CYCLE) {
		cycles = moved / WIN32_TIME_CYCLE - 1;
		if (moved % WIN32_TIME_CYCLE < 0)
			cycles--;
		moved -= cycles * WIN32_TIME_CYCLE;
	}
	if (local)
		error = localtime_s(result, &moved);
	else
		error = gmtime_s(result, &moved);
	if (error != 0) {
		errno = error;
		return (NULL);
	}
	years = result->tm_year + cycles * 400;
	if (years > INT_MAX || years < INT_MIN) {
		errno = EOVERFLOW;
		return (NULL);
	}
	result->tm_year = (int)years;
	return (result);
}

struct tm *
localtime_r(const time_t *t, struct tm *result)
{
	if (*t >= 0 && *t < 32535215999LL && localtime_s(result, t) == 0)
		return (result);
	return (win32_time_convert(t, result, 1));
}

struct tm *
gmtime_r(const time_t *t, struct tm *result)
{
	if (*t >= 0 && *t < 32535215999LL && gmtime_s(result, t) == 0)
		return (result);
	return (win32_time_convert(t, result, 0));
}

char *
ctime_r(const time_t *t, char *buf)
{
	static const char	 days[] = "SunMonTueWedThuFriSat";
	static const char	 months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	struct tm		 tm;

	if (localtime_r(t, &tm) == NULL)
		return (NULL);
	if (tm.tm_year + 1900 < 1000 || tm.tm_year + 1900 > 9999) {
		errno = EOVERFLOW;
		return (NULL);
	}
	snprintf(buf, 26, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n",
	    days + tm.tm_wday * 3, months + tm.tm_mon * 3, tm.tm_mday,
	    tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
	return (buf);
}

static DWORD
win32_local_temp(WCHAR *buf, size_t size)
{
	WCHAR	 local[MAX_PATH + 1], *known;
	HANDLE	 token;
	BOOL	 found = FALSE;
	int	 n;

	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		found = ExpandEnvironmentStringsForUserW(token,
		    L"%LOCALAPPDATA%", local, sizeof local / sizeof local[0]) &&
		    local[0] != L'%' && local[0] != L'\0';
		CloseHandle(token);
	}
	if (!found) {
		if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL,
		    &known) != S_OK)
			return (0);
		wcsncpy_s(local, sizeof local / sizeof local[0], known,
		    _TRUNCATE);
		CoTaskMemFree(known);
	}
	n = _snwprintf_s(buf, size, _TRUNCATE, L"%ls\\Temp\\", local);
	return (n > 0 ? (DWORD)n : 0);
}

static const char *
win32_tmpdir_utf8(char *path, size_t size, const WCHAR *wide)
{
	char	*s;

	if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, (int)size, NULL,
	    NULL) == 0) {
		snprintf(path, size, "%s", "C:/Windows/Temp/");
		return (path);
	}
	for (s = path; *s != '\0'; s++) {
		if (*s == '\\')
			*s = '/';
	}
	return (path);
}

const char *
win32_tmpdir(void)
{
	static char	 path[MAX_PATH * 3 + 1];
	WCHAR		 wide[MAX_PATH + 1];
	DWORD		 n = 0;

	if (*path != '\0')
		return (path);
	if (GetEnvironmentVariableW(L"TMP", NULL, 0) == 0 &&
	    GetEnvironmentVariableW(L"TEMP", NULL, 0) == 0)
		n = win32_local_temp(wide, sizeof wide / sizeof wide[0]);
	if (n == 0)
		n = GetTempPathW(sizeof wide / sizeof wide[0], wide);
	if (n == 0 || n > MAX_PATH)
		wcscpy_s(wide, sizeof wide / sizeof wide[0], L"C:\\Windows\\Temp\\");
	return (win32_tmpdir_utf8(path, sizeof path, wide));
}

const char *
win32_socket_tmpdir(void)
{
	static char	 path[MAX_PATH * 3 + 1];
	WCHAR		 wide[MAX_PATH + 1];

	if (*path != '\0')
		return (path);
	if (win32_local_temp(wide, sizeof wide / sizeof wide[0]) == 0)
		return (win32_tmpdir());
	return (win32_tmpdir_utf8(path, sizeof path, wide));
}
int
win32_isatty(int fd)
{
	HANDLE	h;
	DWORD	mode;

	if (fd >= 0 && fd <= 2 && win32_test_terminal() != NULL)
		return (1);
	h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE)
		return (0);
	return (GetConsoleMode(h, &mode) != 0);
}

char *
win32_ttyname(int fd)
{
	static char	 name[TTY_NAME_MAX];
	const char	*value;

	if (!isatty(fd))
		return (NULL);
	value = getenv("TMUX_WINDOWS_TTY");
	if (value != NULL && *value != '\0' && strlen(value) < sizeof name)
		snprintf(name, sizeof name, "%s", value);
	else if (win32_test_terminal() != NULL) {
		snprintf(name, sizeof name, "test-terminal-%lu",
		    (unsigned long)GetCurrentProcessId());
	} else {
		snprintf(name, sizeof name, "console-%lu",
		    (unsigned long)GetCurrentProcessId());
	}
	return (name);
}

static void
win32_qsort_swap(char *a, char *b, size_t size)
{
	char	c;

	while (size-- != 0) {
		c = *a;
		*a++ = *b;
		*b++ = c;
	}
}

void
win32_qsort(void *base, size_t nmemb, size_t size,
    int (*compar)(const void *, const void *))
{
	char	*src = base, *dst, *tmp, *p;
	size_t	 width, lo, mid, hi, i, j, k;

	if (nmemb < 2 || size == 0)
		return;
	if (nmemb > SIZE_MAX / size || (tmp = malloc(nmemb * size)) == NULL) {
		for (i = 1; i < nmemb; i++) {
			p = src + i * size;
			while (p != src && compar(p - size, p) > 0) {
				win32_qsort_swap(p - size, p, size);
				p -= size;
			}
		}
		return;
	}
	dst = tmp;
	for (width = 1; width < nmemb; width *= 2) {
		for (lo = 0; lo < nmemb; lo += 2 * width) {
			mid = lo + width < nmemb ? lo + width : nmemb;
			hi = mid + width < nmemb ? mid + width : nmemb;
			i = lo;
			j = mid;
			for (k = lo; k < hi; k++) {
				if (j == hi ||
				    (i < mid && compar(src + j * size,
				        src + i * size) >= 0))
					memcpy(dst + k * size, src + i++ * size,
					    size);
				else
					memcpy(dst + k * size, src + j++ * size,
					    size);
			}
		}
		p = src;
		src = dst;
		dst = p;
		if (width > nmemb / 2)
			break;
	}
	if (src != base)
		memcpy(base, src, nmemb * size);
	free(tmp);
}

static int
win32_strftime_append(char *out, size_t max, size_t *used, const char *s,
    size_t len)
{
	if (*used + len >= max)
		return (-1);
	memcpy(out + *used, s, len);
	*used += len;
	out[*used] = '\0';
	return (0);
}

size_t
win32_strftime(char *out, size_t max, const char *fmt, const struct tm *tm)
{
	const char	*p, *start;
	char		 conv[4], buf[128];
	size_t		 used = 0;
	struct tm	 copy;
	int		 nopad, hour;

	if (max == 0)
		return (0);
	out[0] = '\0';
	for (p = fmt; *p != '\0'; p++) {
		if (*p != '%') {
			if (win32_strftime_append(out, max, &used, p, 1) != 0)
				return (0);
			continue;
		}
		start = p++;
		nopad = (*p == '-');
		if (nopad)
			p++;
		if (*p == 'E' || *p == 'O')
			p++;
		if (*p == '\0') {
			if (win32_strftime_append(out, max, &used, start,
			    strlen(start)) != 0)
				return (0);
			break;
		}
		switch (*p) {
		case 'k':
		case 'l':
			hour = tm->tm_hour;
			if (*p == 'l' && (hour %= 12) == 0)
				hour = 12;
			snprintf(buf, sizeof buf, nopad ? "%d" : "%2d", hour);
			break;
		case 'P':
			snprintf(buf, sizeof buf, "%s", tm->tm_hour < 12 ? "am" :
			    "pm");
			break;
		case 's':
			copy = *tm;
			snprintf(buf, sizeof buf, "%lld", (long long)mktime(&copy));
			break;
		default:
			if (strchr("aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%",
			    *p) == NULL) {
				if (win32_strftime_append(out, max, &used, start,
				    p - start + 1) != 0)
					return (0);
				continue;
			}
			snprintf(conv, sizeof conv, nopad ? "%%#%c" : "%%%c", *p);
			if ((strftime)(buf, sizeof buf, conv, tm) == 0)
				buf[0] = '\0';
			break;
		}
		if (win32_strftime_append(out, max, &used, buf, strlen(buf)) != 0)
			return (0);
	}
	return (used);
}
struct win32_glob_state {
	char **paths;
	size_t count, alloc;
	char **parts;
	size_t nparts;
	char sep;
};

static int
win32_glob_magic(const char *s)
{
	return (strpbrk(s, "*?[") != NULL);
}

static void
win32_glob_add(struct win32_glob_state *st, const char *path)
{
	if (st->count == st->alloc) {
		st->alloc = st->alloc == 0 ? 8 : st->alloc * 2;
		st->paths = realloc(st->paths, st->alloc * sizeof *st->paths);
		if (st->paths == NULL)
			abort();
	}
	st->paths[st->count++] = _strdup(path);
}

static char *
win32_glob_join(const char *prefix, const char *name, char sep)
{
	size_t n = strlen(prefix);
	char *out = malloc(n + strlen(name) + 2);

	if (out == NULL)
		abort();
	memcpy(out, prefix, n);
	if (n != 0 && prefix[n - 1] != '/' && prefix[n - 1] != '\\' &&
	    !(n == 2 && prefix[1] == ':'))
		out[n++] = sep;
	strcpy(out + n, name);
	return (out);
}

static DWORD
win32_glob_attributes(const char *path)
{
	wchar_t *w;
	DWORD attr;
	int n;

	n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if (n == 0)
		return (INVALID_FILE_ATTRIBUTES);
	w = malloc(n * sizeof *w);
	if (w == NULL)
		abort();
	MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
	attr = GetFileAttributesW(w);
	free(w);
	return (attr);
}

static void
win32_glob_expand(struct win32_glob_state *st, const char *prefix, size_t i)
{
	WIN32_FIND_DATAW fd;
	HANDLE h;
	wchar_t *wpattern;
	char *search, *path, name[MAX_PATH * 4];
	const char *part;
	DWORD attr;
	int last, n;

	if (i == st->nparts) {
		win32_glob_add(st, prefix);
		return;
	}
	part = st->parts[i];
	last = (i + 1 == st->nparts);

	if (!win32_glob_magic(part)) {
		path = win32_glob_join(prefix, part, st->sep);
		attr = win32_glob_attributes(path);
		if (attr != INVALID_FILE_ATTRIBUTES &&
		    (last || (attr & FILE_ATTRIBUTE_DIRECTORY)))
			win32_glob_expand(st, path, i + 1);
		free(path);
		return;
	}

	search = win32_glob_join(prefix, "*", st->sep);
	n = MultiByteToWideChar(CP_UTF8, 0, search, -1, NULL, 0);
	wpattern = malloc((n != 0 ? n : 1) * sizeof *wpattern);
	if (wpattern == NULL)
		abort();
	MultiByteToWideChar(CP_UTF8, 0, search, -1, wpattern, n);
	free(search);
	h = FindFirstFileExW(wpattern, FindExInfoBasic, &fd,
	    FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	free(wpattern);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
		        sizeof name, NULL, NULL) == 0)
			continue;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		if (!last && (~fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		if (win32_fnmatch(part, name, FNM_PERIOD | FNM_CASEFOLD) != 0)
			continue;
		path = win32_glob_join(prefix, name, st->sep);
		win32_glob_expand(st, path, i + 1);
		free(path);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static int
win32_glob_compare(const void *a, const void *b)
{
	return (strcmp(*(char *const *)a, *(char *const *)b));
}

int
win32_glob(const char *base, const char *pattern, char ***paths, size_t *count)
{
	struct win32_glob_state st;
	char *copy, *p, *root, *start;
	size_t rootlen = 0;

	memset(&st, 0, sizeof st);
	st.sep = (strchr(pattern, '\\') != NULL ||
	             (base != NULL && strchr(base, '\\') != NULL))
	    ? '\\'
	    : '/';

	if ((pattern[0] == '\\' || pattern[0] == '/') &&
	    (pattern[1] == '\\' || pattern[1] == '/')) {
		p = (char *)pattern + 2;
		p += strcspn(p, "\\/");
		if (*p != '\0')
			p += 1 + strcspn(p + 1, "\\/");
		rootlen = p - pattern;
	} else if (isalpha((u_char)pattern[0]) && pattern[1] == ':')
		rootlen = (pattern[2] == '\\' || pattern[2] == '/') ? 3 : 2;
	else if (pattern[0] == '\\' || pattern[0] == '/')
		rootlen = 1;

	if (rootlen != 0) {
		root = malloc(rootlen + 1);
		if (root == NULL)
			abort();
		memcpy(root, pattern, rootlen);
		root[rootlen] = '\0';
	} else
		root = _strdup(base != NULL ? base : ".");
	copy = _strdup(pattern + rootlen);
	if (root == NULL || copy == NULL)
		abort();

	for (start = p = copy;; p++) {
		if (*p == '\\' || *p == '/' || *p == '\0') {
			int end = (*p == '\0');
			*p = '\0';
			if (*start != '\0') {
				st.parts = realloc(st.parts,
				    (st.nparts + 1) * sizeof *st.parts);
				if (st.parts == NULL)
					abort();
				st.parts[st.nparts++] = start;
			}
			if (end)
				break;
			start = p + 1;
		}
	}
	win32_glob_expand(&st, root, 0);
	free(st.parts);
	free(copy);
	free(root);

	if (st.count > 1)
		qsort(st.paths, st.count, sizeof *st.paths, win32_glob_compare);
	*paths = st.paths;
	*count = st.count;
	return (st.count == 0 ? -1 : 0);
}

void
win32_globfree(char **paths, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		free(paths[i]);
	free(paths);
}

#define TPARM_STACK_SIZE 32
#define TPARM_BUF_SIZE 4096

char *
tparm(const char *str, intptr_t p1, intptr_t p2, intptr_t p3, intptr_t p4,
    intptr_t p5, intptr_t p6, intptr_t p7, intptr_t p8, intptr_t p9)
{
	static char buf[TPARM_BUF_SIZE];
	intptr_t params[9] = {p1, p2, p3, p4, p5, p6, p7, p8, p9};
	intptr_t stack[TPARM_STACK_SIZE];
	intptr_t vars[52];
	int sp = 0;
	size_t pos = 0;
	const char *p;
	int n;

	if (str == NULL)
		return (NULL);

	memset(stack, 0, sizeof stack);
	memset(vars, 0, sizeof vars);

	for (p = str; *p != '\0' && pos < sizeof buf - 16; p++) {
		if (*p != '%') {
			buf[pos++] = *p;
			continue;
		}
		p++;
		if (*p == '\0')
			break;

		switch (*p) {
		case '%':
			buf[pos++] = '%';
			break;

		case 'p':
			p++;
			if (*p >= '1' && *p <= '9') {
				if (sp < TPARM_STACK_SIZE)
					stack[sp++] = params[*p - '1'];
			}
			break;

		case 'd':
			if (sp > 0)
				n = (int)stack[--sp];
			else
				n = 0;
			pos += snprintf(buf + pos, sizeof buf - pos, "%d", n);
			break;

		case 'c':
			if (sp > 0)
				n = (int)stack[--sp];
			else
				n = 0;
			buf[pos++] = (char)n;
			break;

		case 's':
			if (sp > 0) {
				const char *sv =
				    (const char *)(uintptr_t)stack[--sp];
				if (sv != NULL) {
					while (
					    *sv != '\0' && pos < sizeof buf - 1)
						buf[pos++] = *sv++;
				}
			}
			break;

		case '{':
			p++;
			n = 0;
			while (*p >= '0' && *p <= '9') {
				n = n * 10 + (*p - '0');
				p++;
			}
			if (*p == '}') {
				if (sp < TPARM_STACK_SIZE)
					stack[sp++] = n;
			} else {
				p--;
			}
			break;

		case 'i':
			params[0]++;
			params[1]++;
			break;

		case '+':
			if (sp >= 2) {
				sp--;
				stack[sp - 1] += stack[sp];
			}
			break;

		case '-':
			if (sp >= 2) {
				sp--;
				stack[sp - 1] -= stack[sp];
			}
			break;

		case '*':
			if (sp >= 2) {
				sp--;
				stack[sp - 1] *= stack[sp];
			}
			break;

		case '/':
			if (sp >= 2) {
				sp--;
				if (stack[sp] != 0)
					stack[sp - 1] /= stack[sp];
			}
			break;

		case '<':
			if (sp >= 2) {
				sp--;
				stack[sp - 1] =
				    (stack[sp - 1] < stack[sp]) ? 1 : 0;
			}
			break;

		case '>':
			if (sp >= 2) {
				sp--;
				stack[sp - 1] =
				    (stack[sp - 1] > stack[sp]) ? 1 : 0;
			}
			break;

		case '=':
			if (sp >= 2) {
				sp--;
				stack[sp - 1] =
				    (stack[sp - 1] == stack[sp]) ? 1 : 0;
			}
			break;

		case 'm':
			if (sp >= 2) {
				sp--;
				if (stack[sp] != 0)
					stack[sp - 1] %= stack[sp];
			}
			break;

		case '&':
		case '|':
		case '^':
		case 'A':
		case 'O':
			if (sp >= 2) {
				sp--;
				if (*p == '&')
					stack[sp - 1] &= stack[sp];
				else if (*p == '|')
					stack[sp - 1] |= stack[sp];
				else if (*p == '^')
					stack[sp - 1] ^= stack[sp];
				else if (*p == 'A')
					stack[sp - 1] = stack[sp - 1] && stack[sp];
				else
					stack[sp - 1] = stack[sp - 1] || stack[sp];
			}
			break;

		case '~':
			if (sp >= 1)
				stack[sp - 1] = ~stack[sp - 1];
			break;

		case 'l':
			if (sp >= 1) {
				const char *sv =
				    (const char *)(uintptr_t)stack[sp - 1];
				stack[sp - 1] = (sv == NULL ? 0 : strlen(sv));
			}
			break;

		case '\'':
			if (p[1] != '\0' && p[2] == '\'') {
				if (sp < TPARM_STACK_SIZE)
					stack[sp++] = (u_char)p[1];
				p += 2;
			}
			break;

		case 'P':
		case 'g':
			n = -1;
			if (p[1] >= 'a' && p[1] <= 'z')
				n = p[1] - 'a';
			else if (p[1] >= 'A' && p[1] <= 'Z')
				n = 26 + p[1] - 'A';
			if (n == -1)
				break;
			p++;
			if (p[-1] == 'P') {
				if (sp > 0)
					vars[n] = stack[--sp];
			} else if (sp < TPARM_STACK_SIZE)
				stack[sp++] = vars[n];
			break;

		case '!':
			if (sp >= 1)
				stack[sp - 1] = !stack[sp - 1];
			break;

		case '?':
			break;

		case 't': {
			int cond = 0;
			if (sp > 0)
				cond = (int)stack[--sp];
			if (!cond) {
				int depth = 0;
				while (*++p != '\0') {
					if (*p != '%')
						continue;
					p++;
					if (*p == '?')
						depth++;
					else if (*p == ';') {
						if (depth == 0)
							break;
						depth--;
					} else if (*p == 'e' && depth == 0)
						break;
				}
				if (*p == '\0')
					p--;
			}
			break;
		}

		case 'e': {
			int depth = 0;
			while (*++p != '\0') {
				if (*p != '%')
					continue;
				p++;
				if (*p == '?')
					depth++;
				else if (*p == ';') {
					if (depth == 0)
						break;
					depth--;
				}
			}
			if (*p == '\0')
				p--;
			break;
		}

		case ';':
			break;

		default:
			buf[pos++] = '%';
			if (pos < sizeof buf - 1)
				buf[pos++] = *p;
			break;
		}
	}

	buf[pos] = '\0';
	return (buf);
}

struct fd_bridge {
	SOCKET sock;
	int writing;
	HANDLE thread;
	struct fd_bridge *next;
};
static struct fd_bridge *fd_bridges;
static SRWLOCK fd_bridges_lock = SRWLOCK_INIT;

struct fd_bridge_args {
	SOCKET peer;
	int fd;
	int writing;
};

static DWORD WINAPI
fd_bridge_thread(LPVOID arg)
{
	struct fd_bridge_args *b = arg;
	HANDLE h = (HANDLE)_get_osfhandle(b->fd);
	char buf[16384];
	DWORD n, mode, done;
	int got, console = GetConsoleMode(h, &mode);

	if (b->writing) {
		while ((got = recv(b->peer, buf, sizeof buf, 0)) > 0) {
			for (done = 0; done < (DWORD)got; done += n) {
				int w = _write(b->fd, buf + done, got - done);
				if (w <= 0)
					goto out;
				n = (DWORD)w;
			}
		}
	} else {
		while (ReadFile(h, buf, sizeof buf, &n, NULL) && n != 0) {
			if (console && buf[0] == 0x1a)
				break;
			for (done = 0; done < n; done += got) {
				got = send(b->peer, buf + done, n - done, 0);
				if (got <= 0)
					goto out;
			}
		}
		shutdown(b->peer, SD_SEND);
	}
out:
	closesocket(b->peer);
	_close(b->fd);
	free(b);
	return (0);
}

int
win32_fd_bridge(int fd, int writing)
{
	struct fd_bridge *b;
	struct fd_bridge_args *a;
	int sv[2];

	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0)
		return (-1);
	b = calloc(1, sizeof *b);
	a = calloc(1, sizeof *a);
	if (b == NULL || a == NULL)
		abort();
	b->sock = (SOCKET)sv[0];
	b->writing = writing;
	a->peer = (SOCKET)sv[1];
	a->fd = fd;
	a->writing = writing;
	b->thread = CreateThread(NULL, 0, fd_bridge_thread, a, 0, NULL);
	if (b->thread == NULL) {
		closesocket(b->sock);
		closesocket(a->peer);
		free(a);
		free(b);
		return (-1);
	}
	AcquireSRWLockExclusive(&fd_bridges_lock);
	b->next = fd_bridges;
	fd_bridges = b;
	ReleaseSRWLockExclusive(&fd_bridges_lock);
	return ((int)b->sock);
}

int
win32_close_any(int fd)
{
	struct fd_bridge **at, *b = NULL;
	int type, len = sizeof type;

	AcquireSRWLockExclusive(&fd_bridges_lock);
	for (at = &fd_bridges; *at != NULL; at = &(*at)->next) {
		if ((int)(*at)->sock == fd) {
			b = *at;
			*at = b->next;
			break;
		}
	}
	ReleaseSRWLockExclusive(&fd_bridges_lock);
	if (b != NULL) {
		if (b->writing) {
			shutdown(b->sock, SD_SEND);
			WaitForSingleObject(b->thread, 10000);
		}
		closesocket(b->sock);
		CloseHandle(b->thread);
		free(b);
		return (0);
	}
	if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_TYPE, (char *)&type, &len) ==
	    0)
		return (closesocket((SOCKET)fd) == 0 ? 0 : -1);
	return (_close(fd));
}

static wchar_t *
win32_path_utf16(const char *path)
{
	struct tmux_os_error error;
	wchar_t *wide;

	if (path == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	if ((wide = tmux_win32_utf16(path, &error)) == NULL)
		errno = EILSEQ;
	return (wide);
}

int
win32_open(const char *path, int flags, ...)
{
	wchar_t *w;
	va_list ap;
	int mode = 0, fd;

	if (flags & _O_CREAT) {
		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	if (!(flags & (_O_TEXT | _O_WTEXT | _O_U8TEXT | _O_U16TEXT)))
		flags |= _O_BINARY;
	if ((w = win32_path_utf16(path)) == NULL)
		return (-1);
	fd = _wopen(w, flags, (mode & 0222) ? _S_IREAD | _S_IWRITE : _S_IREAD);
	free(w);
	return (fd);
}

static const char *
win32_fopen_mode(const char *mode, char *buf, size_t len)
{
	if (strchr(mode, 'b') != NULL || strchr(mode, 't') != NULL)
		return (mode);
	snprintf(buf, len, "%sb", mode);
	return (buf);
}

FILE *
win32_fopen(const char *path, const char *mode)
{
	wchar_t *w, *wm;
	FILE *f = NULL;
	char buf[16];

	mode = win32_fopen_mode(mode, buf, sizeof buf);
	if ((w = win32_path_utf16(path)) == NULL)
		return (NULL);
	if ((wm = win32_path_utf16(mode)) != NULL) {
		f = _wfopen(w, wm);
		free(wm);
	}
	free(w);
	return (f);
}

FILE *
win32_fdopen(int fd, const char *mode)
{
	char buf[16];

	_setmode(fd, _O_BINARY);
	return (_fdopen(fd, win32_fopen_mode(mode, buf, sizeof buf)));
}

int
win32_access(const char *path, int mode)
{
	wchar_t *w;
	int r;

	if ((w = win32_path_utf16(path)) == NULL)
		return (-1);
	r = _waccess(w, mode & 06);
	free(w);
	return (r);
}

int
win32_stat(const char *path, struct stat *sb)
{
	wchar_t *w;
	int r;

	if ((w = win32_path_utf16(path)) == NULL)
		return (-1);
	r = _wstat(w, (struct _stat *)sb);
	free(w);
	return (r);
}

#define WIN32_PATH_FN(name, wfn) \
	int name(const char *path) \
	{ \
		wchar_t *w; \
		int r; \
\
		if ((w = win32_path_utf16(path)) == NULL) \
			return (-1); \
		r = wfn(w); \
		free(w); \
		return (r); \
	}
WIN32_PATH_FN(win32_chdir, _wchdir)
WIN32_PATH_FN(win32_mkdir, _wmkdir)
WIN32_PATH_FN(win32_rmdir, _wrmdir)
WIN32_PATH_FN(win32_unlink, _wunlink)

char *
win32_realpath(const char *path, char *resolved)
{
	struct tmux_os_error error;
	wchar_t *w, *full;
	char *text;
	size_t n;

	if ((w = win32_path_utf16(path)) == NULL)
		return (NULL);
	full = _wfullpath(NULL, w, 0);
	free(w);
	if (full == NULL)
		return (NULL);
	if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
		free(full);
		errno = ENOENT;
		return (NULL);
	}
	text = tmux_win32_utf8(full, &error);
	free(full);
	if (text == NULL) {
		errno = EILSEQ;
		return (NULL);
	}
	if (resolved == NULL)
		return (text);
	n = strlen(text) + 1;
	if (n > PATH_MAX) {
		free(text);
		errno = ENAMETOOLONG;
		return (NULL);
	}
	memcpy(resolved, text, n);
	free(text);
	return (resolved);
}

void
win32_fd_bridges_finish(void)
{
	struct fd_bridge *b;
	int fd;

	for (;;) {
		AcquireSRWLockShared(&fd_bridges_lock);
		for (b = fd_bridges; b != NULL && !b->writing; b = b->next)
			;
		fd = (b != NULL) ? (int)b->sock : -1;
		ReleaseSRWLockShared(&fd_bridges_lock);
		if (fd == -1)
			break;
		win32_close_any(fd);
	}
}