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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "tmux-platform.h"
#include "wtmux-service.h"
#include "service-internal.h"
#include "wtmux-version.h"

#ifndef TMUX_VERSION
#define TMUX_VERSION "unknown"
#endif

#define REQUEST_TIMEOUT		10000
#define REPLY_TIMEOUT		120000
#define TMUX_TIMEOUT		30000
#define MAX_CLIENTS		32
#define EVENT_SOURCE_KEY	L"SYSTEM\\CurrentControlSet\\Services\\EventLog" \
				L"\\Application\\wtmux-service"
#define TRUSTED_INSTALLER	L"S-1-5-80-956008885-3418522649-1831038044-" \
				L"1853292631-2271478464"

static SERVICE_STATUS_HANDLE	 status_handle;
static SERVICE_STATUS		 service_status;
static HANDLE			 stop_event;
static HANDLE			 event_source;
static CRITICAL_SECTION		 log_lock;
static volatile LONG		 active_clients;
wchar_t				 self_path[MAX_PATH];
wchar_t				 tmux_path[MAX_PATH];

void
fatal(const char *msg)
{
	fprintf(stderr, "wtmux-service: %s\n", msg);
	exit(1);
}

void *
xcalloc(size_t n, size_t size)
{
	void	*p;

	if ((p = calloc(n == 0 ? 1 : n, size == 0 ? 1 : size)) == NULL)
		fatal("out of memory");
	return (p);
}

static void
buf_add(struct buf *b, const char *data, size_t len)
{
	char	*p;

	if (b->len + len + 1 > b->size) {
		b->size = (b->len + len + 1) * 2;
		if ((p = realloc(b->data, b->size)) == NULL)
			fatal("out of memory");
		b->data = p;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
}

static void
buf_printf(struct buf *b, const char *fmt, ...)
{
	va_list	 ap;
	char	 tmp[4096];
	int	 n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0)
		buf_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n :
		    sizeof tmp - 1);
}

wchar_t *
to_wide(const char *s)
{
	wchar_t	*w;
	int	 n;

	n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	w = xcalloc(n, sizeof *w);
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return (w);
}

char *
to_utf8(const wchar_t *w)
{
	char	*s;
	int	 n;

	n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	s = xcalloc(n, 1);
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return (s);
}

static int
pipe_io(HANDLE h, void *data, DWORD len, int writing, DWORD timeout)
{
	OVERLAPPED	 ov;
	DWORD		 done, total = 0;
	BOOL		 ok;
	int		 rc = -1;

	memset(&ov, 0, sizeof ov);
	if ((ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)) == NULL)
		return (-1);
	while (total < len) {
		ResetEvent(ov.hEvent);
		if (writing)
			ok = WriteFile(h, (char *)data + total, len - total,
			    NULL, &ov);
		else
			ok = ReadFile(h, (char *)data + total, len - total,
			    NULL, &ov);
		if (!ok && GetLastError() != ERROR_IO_PENDING)
			goto out;
		if (WaitForSingleObject(ov.hEvent, timeout) != WAIT_OBJECT_0) {
			CancelIoEx(h, &ov);
			GetOverlappedResult(h, &ov, &done, TRUE);
			goto out;
		}
		if (!GetOverlappedResult(h, &ov, &done, FALSE) || done == 0)
			goto out;
		total += done;
	}
	rc = 0;
out:
	CloseHandle(ov.hEvent);
	return (rc);
}

static uint32_t
get_u32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static void
put_u32(unsigned char *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

int
is_trusted_sid(PSID sid)
{
	wchar_t	*s;
	int	 trusted;

	if (IsWellKnownSid(sid, WinLocalSystemSid) ||
	    IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
		return (1);
	if (!ConvertSidToStringSidW(sid, &s))
		return (0);
	trusted = wcscmp(s, TRUSTED_INSTALLER) == 0;
	LocalFree(s);
	return (trusted);
}

void
sid_name(PSID sid, wchar_t *out, size_t outlen)
{
	wchar_t		 user[256], domain[256], *s;
	DWORD		 ulen = 256, dlen = 256;
	SID_NAME_USE	 use;

	if (LookupAccountSidW(NULL, sid, user, &ulen, domain, &dlen, &use)) {
		if (*domain != L'\0')
			_snwprintf_s(out, outlen, _TRUNCATE, L"%s\\%s", domain,
			    user);
		else
			_snwprintf_s(out, outlen, _TRUNCATE, L"%s", user);
	} else if (ConvertSidToStringSidW(sid, &s)) {
		_snwprintf_s(out, outlen, _TRUNCATE, L"%s", s);
		LocalFree(s);
	} else
		_snwprintf_s(out, outlen, _TRUNCATE, L"?");
}

PSID
copy_sid(PSID sid)
{
	PSID	copy;
	DWORD	len = GetLengthSid(sid);

	copy = xcalloc(1, len);
	CopySid(len, copy, sid);
	return (copy);
}

PSID
token_user(HANDLE token)
{
	union {
		TOKEN_USER	tu;
		char		space[512];
	} u;
	DWORD	len;

	if (!GetTokenInformation(token, TokenUser, &u, sizeof u, &len))
		return (NULL);
	return (copy_sid(u.tu.User.Sid));
}

static int
token_is_admin(HANDLE token)
{
	BYTE	 sid[SECURITY_MAX_SID_SIZE];
	DWORD	 len = sizeof sid;
	BOOL	 member = FALSE;

	if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &len))
		return (0);
	if (!CheckTokenMembership(token, sid, &member))
		return (0);
	return (member != FALSE);
}

int
process_is_elevated(void)
{
	TOKEN_ELEVATION	 e;
	HANDLE		 token;
	DWORD		 len;
	int		 elevated = 0;

	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		if (GetTokenInformation(token, TokenElevation, &e, sizeof e,
		    &len))
			elevated = e.TokenIsElevated != 0;
		CloseHandle(token);
	}
	return (elevated);
}

static void
find_paths(void)
{
	wchar_t	*slash;

	if (GetModuleFileNameW(NULL, self_path, MAX_PATH) == 0 ||
	    GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		fatal("cannot find own path");
	wcscpy_s(tmux_path, MAX_PATH, self_path);
	if ((slash = wcsrchr(tmux_path, L'\\')) == NULL)
		fatal("bad own path");
	slash[1] = L'\0';
	wcscat_s(tmux_path, MAX_PATH, L"tmux.exe");
}

void
service_log(WORD type, const char *fmt, ...)
{
	static wchar_t	 path[MAX_PATH];
	SYSTEMTIME	 now;
	va_list		 ap;
	char		 msg[2048], line[2200];
	wchar_t		*wide, *base;
	const wchar_t	*strings[1];
	HANDLE		 h;
	DWORD		 written;
	SECURITY_ATTRIBUTES sa;
	int		 n;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	for (n = 0; msg[n] != '\0'; n++) {
		if ((unsigned char)msg[n] < 0x20)
			msg[n] = '?';
	}

	EnterCriticalSection(&log_lock);
	if (*path == L'\0' &&
	    SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramData, 0, NULL,
	    &base))) {
		_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\wtmux-service",
		    base);
		CoTaskMemFree(base);
		memset(&sa, 0, sizeof sa);
		sa.nLength = sizeof sa;
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
		    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
		    &sa.lpSecurityDescriptor, NULL)) {
			CreateDirectoryW(path, &sa);
			LocalFree(sa.lpSecurityDescriptor);
		}
		wcscat_s(path, MAX_PATH, L"\\service.log");
	}
	GetLocalTime(&now);
	n = snprintf(line, sizeof line,
	    "%04u-%02u-%02u %02u:%02u:%02u %s\r\n", now.wYear, now.wMonth,
	    now.wDay, now.wHour, now.wMinute, now.wSecond, msg);
	if (*path != L'\0' && n > 0) {
		h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
		    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			WriteFile(h, line, (DWORD)strlen(line), &written,
			    NULL);
			CloseHandle(h);
		}
	}
	if (event_source != NULL) {
		wide = to_wide(msg);
		strings[0] = wide;
		ReportEventW(event_source, type, 0, 1000, NULL, 1, 0, strings,
		    NULL);
		free(wide);
	}
	LeaveCriticalSection(&log_lock);
}

static void
enable_privilege(const wchar_t *name)
{
	TOKEN_PRIVILEGES	 tp;
	HANDLE			 token;

	if (!OpenProcessToken(GetCurrentProcess(),
	    TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		return;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid))
		AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
	CloseHandle(token);
}

static int
run_as_user(HANDLE token, const wchar_t *cmdline, struct buf *output,
    int show, DWORD timeout)
{
	STARTUPINFOEXW			 si;
	PROCESS_INFORMATION		 pi;
	SECURITY_ATTRIBUTES		 sa;
	LPPROC_THREAD_ATTRIBUTE_LIST	 attrs = NULL;
	SIZE_T				 attrlen = 0;
	HANDLE				 rd = NULL, wr = NULL;
	void				*env = NULL;
	wchar_t				 profile[MAX_PATH], *line;
	DWORD				 plen = MAX_PATH, avail, n, code;
	DWORD				 flags, start;
	char				 chunk[4096];
	int				 rc = -1, exited;

	if (!CreateEnvironmentBlock(&env, token, FALSE))
		env = NULL;
	if (!GetUserProfileDirectoryW(token, profile, &plen))
		profile[0] = L'\0';
	line = _wcsdup(cmdline);

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.lpDesktop = L"winsta0\\default";
	flags = CREATE_UNICODE_ENVIRONMENT;
	if (show) {
		si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
		si.StartupInfo.wShowWindow = SW_SHOWNORMAL;
		flags |= CREATE_NEW_CONSOLE;
		if (!CreateProcessAsUserW(token, NULL, line, NULL, NULL, FALSE,
		    flags, env, *profile != L'\0' ? profile : NULL,
		    &si.StartupInfo, &pi))
			goto out;
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		rc = 0;
		goto out;
	}

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(&rd, &wr, &sa, 0))
		goto out;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrlen);
	attrs = xcalloc(1, attrlen);
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrlen) ||
	    !UpdateProcThreadAttribute(attrs, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &wr, sizeof wr, NULL, NULL)) {
		free(attrs);
		attrs = NULL;
		goto out;
	}
	si.lpAttributeList = attrs;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_HIDE;
	si.StartupInfo.hStdOutput = wr;
	si.StartupInfo.hStdError = wr;
	flags |= CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
	if (!CreateProcessAsUserW(token, NULL, line, NULL, NULL, TRUE, flags,
	    env, *profile != L'\0' ? profile : NULL, &si.StartupInfo, &pi))
		goto out;
	CloseHandle(pi.hThread);
	CloseHandle(wr);
	wr = NULL;

	start = GetTickCount();
	for (;;) {
		exited = WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0;
		while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) &&
		    avail != 0) {
			if (!ReadFile(rd, chunk, sizeof chunk, &n, NULL) ||
			    n == 0)
				break;
			if (output->len < TMUX_SERVICE_MAX)
				buf_add(output, chunk, n);
		}
		if (exited)
			break;
		if (GetTickCount() - start > timeout) {
			TerminateProcess(pi.hProcess, 1);
			buf_printf(output, "(timed out)\n");
			break;
		}
		Sleep(20);
	}
	if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE)
		rc = (int)code;
	CloseHandle(pi.hProcess);
out:
	if (rc == -1 && output != NULL && GetLastError() != 0)
		buf_printf(output, "cannot start process (error %lu)\n",
		    GetLastError());
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (rd != NULL)
		CloseHandle(rd);
	if (wr != NULL)
		CloseHandle(wr);
	if (env != NULL)
		DestroyEnvironmentBlock(env);
	free(line);
	return (rc);
}

static wchar_t *
make_cmdline(const wchar_t *program, const char **args, int nargs)
{
	const char	**argv;
	char		 *line;
	wchar_t		 *wide;
	int		  i;

	argv = xcalloc(nargs + 2, sizeof *argv);
	argv[0] = to_utf8(program);
	for (i = 0; i < nargs; i++)
		argv[i + 1] = args[i];
	line = tmux_windows_command_line(argv);
	free((char *)argv[0]);
	free(argv);
	if (line == NULL)
		return (NULL);
	wide = to_wide(line);
	free(line);
	return (wide);
}

struct wts_entry *
wts_list(DWORD *count)
{
	WTS_SESSION_INFOW	*info;
	WTSINFOEXW		*ex;
	struct wts_entry	*list;
	HANDLE			 token;
	DWORD			 n, i, bytes;

	*count = 0;
	if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info,
	    &n))
		return (NULL);
	list = xcalloc(n, sizeof *list);
	for (i = 0; i < n; i++) {
		if (info[i].SessionId == 0 ||
		    (info[i].State != WTSActive &&
		    info[i].State != WTSDisconnected))
			continue;
		if (!WTSQueryUserToken(info[i].SessionId, &token))
			continue;
		list[*count].sid = token_user(token);
		CloseHandle(token);
		if (list[*count].sid == NULL)
			continue;
		list[*count].id = info[i].SessionId;
		list[*count].active = info[i].State == WTSActive;
		sid_name(list[*count].sid, list[*count].name,
		    sizeof list[*count].name / sizeof (wchar_t));
		if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
		    info[i].SessionId, WTSSessionInfoEx, (LPWSTR *)&ex,
		    &bytes)) {
			if (ex->Level == 1)
				list[*count].locked =
				    ex->Data.WTSInfoExLevel1.SessionFlags ==
				    WTS_SESSIONSTATE_LOCK;
			WTSFreeMemory(ex);
		}
		(*count)++;
	}
	WTSFreeMemory(info);
	return (list);
}

void
wts_free(struct wts_entry *list, DWORD count)
{
	DWORD	i;

	for (i = 0; i < count; i++)
		free(list[i].sid);
	free(list);
}

static void
list_tmux_sessions(DWORD session, struct buf *out)
{
	HANDLE		 token;
	struct buf	 res = { 0 };
	wchar_t		*line;
	const char	*args[2];
	char		*p, *next, *f[4], *base;
	int		 i, any = 0;

	if (!WTSQueryUserToken(session, &token))
		return;
	args[0] = "user-ls";
	args[1] = to_utf8(tmux_path);
	line = make_cmdline(self_path, args, 2);
	free((char *)args[1]);
	if (line != NULL && run_as_user(token, line, &res, 0,
	    TMUX_TIMEOUT) == 0 && res.data != NULL) {
		for (p = res.data; p != NULL && *p != '\0'; p = next) {
			if ((next = strchr(p, '\n')) != NULL)
				*next++ = '\0';
			p[strcspn(p, "\r")] = '\0';
			for (i = 0; i < 4; i++) {
				f[i] = p;
				if (p != NULL && (p = strchr(p, '|')) != NULL)
					*p++ = '\0';
			}
			if (f[3] == NULL)
				continue;
			base = strrchr(f[0], '/');
			if (base == NULL)
				base = strrchr(f[0], '\\');
			buf_printf(out, "%-8s %-13s   tmux -L %s: %s (%s windows%s)\n",
			    "", "", base != NULL ? base + 1 : f[0], f[1], f[2],
			    strcmp(f[3], "0") != 0 ? ", attached" : "");
			any = 1;
		}
	}
	if (!any)
		buf_printf(out, "%-8s %-13s   (no tmux sessions)\n", "", "");
	free(line);
	free(res.data);
	CloseHandle(token);
}

static int
do_ls(struct caller *c, struct buf *out)
{
	struct wts_entry	*list;
	DWORD			 n, i, j;
	char			*name;
	const char		*state;
	int			 seen;

	list = wts_list(&n);
	if (n == 0) {
		buf_printf(out, "no users are logged on\n");
		free(list);
		return (0);
	}
	buf_printf(out, "%-8s %-13s %s\n", "SESSION", "STATE", "USER");
	for (i = 0; i < n; i++) {
		if (list[i].active)
			state = list[i].locked ? "active,locked" : "active";
		else
			state = "disconnected";
		name = to_utf8(list[i].name);
		buf_printf(out, "%-8lu %-13s %s\n", list[i].id, state, name);
		free(name);
		if (!c->admin && !EqualSid(c->sid, list[i].sid))
			continue;
		for (seen = 0, j = 0; j < i; j++)
			seen |= EqualSid(list[j].sid, list[i].sid);
		if (!seen)
			list_tmux_sessions(list[i].id, out);
	}
	wts_free(list, n);
	return (0);
}

static int
open_window(HANDLE token, const char *socket, const char *name)
{
	const char	*args[6];
	wchar_t		 wt[MAX_PATH], *line, *p, *q, *esc;
	void		*env;
	int		 rc, have_wt = 0;

	args[0] = "-S";
	args[1] = socket;
	args[2] = "attach-session";
	args[3] = "-t";
	args[4] = name;
	*wt = L'\0';
	if (CreateEnvironmentBlock(&env, token, FALSE)) {
		for (p = env; *p != L'\0'; p += wcslen(p) + 1) {
			if (_wcsnicmp(p, L"LOCALAPPDATA=", 13) == 0) {
				_snwprintf_s(wt, MAX_PATH, _TRUNCATE,
				    L"%s\\Microsoft\\WindowsApps\\wt.exe",
				    p + 13);
				break;
			}
		}
		DestroyEnvironmentBlock(env);
	}
	if (*wt != L'\0' && GetFileAttributesW(wt) != INVALID_FILE_ATTRIBUTES)
		have_wt = 1;
	if ((line = make_cmdline(tmux_path, args, 5)) == NULL)
		return (-1);
	if (have_wt) {
		esc = xcalloc(wcslen(wt) + 2 * wcslen(line) + 8, sizeof *esc);
		_snwprintf_s(esc, wcslen(wt) + 4, _TRUNCATE, L"\"%s\" ", wt);
		q = esc + wcslen(esc);
		for (p = line; *p != L'\0'; p++) {
			if (*p == L';')
				*q++ = L'\\';
			*q++ = *p;
		}
		*q = L'\0';
		rc = run_as_user(token, esc, NULL, 1, 0);
		free(esc);
		if (rc == 0) {
			free(line);
			return (0);
		}
	}
	rc = run_as_user(token, line, NULL, 1, 0);
	free(line);
	return (rc);
}

static int
do_new(struct caller *c, int argc, char **argv, struct buf *out)
{
	struct tmux_service_new		 opts;
	struct tmux_service_session	*mine;
	struct wts_entry		*list = NULL;
	enum tmux_service_verdict	 verdict;
	HANDLE				 token = NULL;
	PSID				 target = NULL;
	struct buf			 res = { 0 }, grant = { 0 };
	const char			*args[TMUX_SERVICE_MAXARGS + 12];
	char				 cause[256], *tname, *cname, *sep;
	char				*socket = NULL, *name = NULL, *nl;
	wchar_t				*wuser, domain[256], *line = NULL;
	BYTE				 sidbuf[SECURITY_MAX_SID_SIZE];
	DWORD				 sidlen = sizeof sidbuf;
	DWORD				 domlen = 256, n = 0, i, nmine = 0;
	SID_NAME_USE			 use;
	int				 nargs = 0, idx, rc = 1, j;

	if (tmux_service_parse_new(argc, argv, &opts, cause,
	    sizeof cause) != 0) {
		buf_printf(out, "%s\n", cause);
		return (1);
	}
	if (opts.user != NULL) {
		wuser = to_wide(opts.user);
		if (!LookupAccountNameW(NULL, wuser, sidbuf, &sidlen, domain,
		    &domlen, &use) || use != SidTypeUser) {
			buf_printf(out, "unknown user: %s\n", opts.user);
			free(wuser);
			return (1);
		}
		free(wuser);
		target = copy_sid(sidbuf);
	} else
		target = copy_sid(c->sid);
	{
		wchar_t	wname[512];

		sid_name(target, wname, 512);
		tname = to_utf8(wname);
	}
	cname = to_utf8(c->name);

	list = wts_list(&n);
	mine = xcalloc(n, sizeof *mine);
	for (i = 0; i < n; i++) {
		if (!EqualSid(list[i].sid, target))
			continue;
		mine[nmine].id = list[i].id;
		mine[nmine].active = list[i].active;
		nmine++;
	}
	verdict = tmux_service_policy(EqualSid(c->sid, target), c->admin,
	    nmine);
	if (verdict == TMUX_SERVICE_DENY_OTHER_USER) {
		buf_printf(out, "permission denied: only an administrator "
		    "(elevated) may start a session for another user\n");
		service_log(EVENTLOG_WARNING_TYPE,
		    "denied: %s asked for a session for %s", cname, tname);
		goto out;
	}
	if (verdict == TMUX_SERVICE_DENY_NOT_LOGGED_ON) {
		buf_printf(out, "%s is not logged on: there is no desktop to "
		    "start a session on\n", tname);
		service_log(EVENTLOG_WARNING_TYPE,
		    "refused: %s asked for a session for %s, not logged on",
		    cname, tname);
		goto out;
	}
	if ((idx = tmux_service_pick_session(mine, nmine,
	    opts.session_id)) == -1) {
		buf_printf(out, "%s has no Windows session %ld\n", tname,
		    opts.session_id);
		goto out;
	}
	if (!WTSQueryUserToken(mine[idx].id, &token)) {
		buf_printf(out, "cannot get the user's token (error %lu)\n",
		    GetLastError());
		goto out;
	}

	nargs = 0;
	if (opts.label != NULL) {
		args[nargs++] = "-L";
		args[nargs++] = opts.label;
	}
	args[nargs++] = "display-message";
	args[nargs++] = "-p";
	args[nargs++] = "#{pid}";
	if ((line = make_cmdline(tmux_path, args, nargs)) != NULL &&
	    run_as_user(token, line, &res, 0, TMUX_TIMEOUT) == 0 &&
	    res.data != NULL) {
		DWORD	pid = strtoul(res.data, NULL, 10), where;

		if (pid != 0 && ProcessIdToSessionId(pid, &where) &&
		    where != mine[idx].id) {
			buf_printf(out, "the tmux server for this socket is "
			    "already running in Windows session %lu, not on "
			    "the desktop (session %lu): use another -L, or "
			    "kill that server first\n", where, mine[idx].id);
			goto out;
		}
	}
	free(line);
	line = NULL;
	free(res.data);
	memset(&res, 0, sizeof res);
	nargs = 0;
	if (opts.label != NULL) {
		args[nargs++] = "-L";
		args[nargs++] = opts.label;
	}
	args[nargs++] = "new-session";
	args[nargs++] = "-d";
	args[nargs++] = "-P";
	args[nargs++] = "-F";
	args[nargs++] = "#{socket_path}|#{session_name}";
	if (opts.name != NULL) {
		args[nargs++] = "-s";
		args[nargs++] = opts.name;
	}
	if (opts.ncommand > 0) {
		args[nargs++] = "--";
		for (j = 0; j < opts.ncommand; j++)
			args[nargs++] = opts.command[j];
	}
	if ((line = make_cmdline(tmux_path, args, nargs)) == NULL) {
		buf_printf(out, "bad arguments\n");
		goto out;
	}
	if (run_as_user(token, line, &res, 0, TMUX_TIMEOUT) != 0 ||
	    res.data == NULL || (sep = strchr(res.data, '|')) == NULL) {
		buf_printf(out, "tmux did not start: %s",
		    res.data != NULL ? res.data : "no output\n");
		if (res.len == 0 || res.data[res.len - 1] != '\n')
			buf_printf(out, "\n");
		service_log(EVENTLOG_WARNING_TYPE,
		    "failed: %s asked for a session for %s in session %lu: %s",
		    cname, tname, mine[idx].id,
		    res.data != NULL ? res.data : "");
		goto out;
	}
	*sep = '\0';
	socket = res.data;
	name = sep + 1;
	if ((nl = strpbrk(name, "\r\n")) != NULL)
		*nl = '\0';

	if (!EqualSid(c->sid, target)) {
		args[0] = "-S";
		args[1] = socket;
		args[2] = "server-access";
		args[3] = "-a";
		args[4] = cname;
		free(line);
		line = make_cmdline(tmux_path, args, 5);
		if (line == NULL ||
		    run_as_user(token, line, &grant, 0, TMUX_TIMEOUT) != 0)
			buf_printf(out, "warning: could not allow %s into the "
			    "server: %s", cname, grant.data != NULL ?
			    grant.data : "\n");
	}
	if (opts.window && open_window(token, socket, name) != 0)
		buf_printf(out, "warning: could not open a window (error "
		    "%lu)\n", GetLastError());

	buf_printf(out, "started %s for %s in Windows session %lu (%s)\n",
	    name, tname, mine[idx].id, mine[idx].active ? "active" :
	    "disconnected");
	buf_printf(out, "socket: %s\n", socket);
	buf_printf(out, "attach: tmux -S \"%s\" attach -t \"%s\"\n", socket,
	    name);
	service_log(EVENTLOG_INFORMATION_TYPE,
	    "%s started session %s for %s in Windows session %lu%s",
	    cname, name, tname, mine[idx].id, opts.window ? " with a window" :
	    "");
	rc = 0;
out:
	if (token != NULL)
		CloseHandle(token);
	if (list != NULL)
		wts_free(list, n);
	free(mine);
	free(target);
	free(tname);
	free(cname);
	free(line);
	free(res.data);
	free(grant.data);
	return (rc);
}

static int
dispatch(struct caller *c, int argc, char **argv, struct buf *out)
{
	if (strcmp(argv[0], "ls") == 0 && argc == 1)
		return (do_ls(c, out));
	if (strcmp(argv[0], "new") == 0)
		return (do_new(c, argc, argv, out));
	if (strcmp(argv[0], "reload") == 0 && argc == 1) {
		char	status[512];

		if (!c->admin) {
			buf_printf(out, "permission denied\n");
			return (1);
		}
		remote_reload(status, sizeof status);
		buf_printf(out, "%s\n", status);
		return (0);
	}
	if (strcmp(argv[0], "fingerprint") == 0 && argc == 1) {
		char	fp[128];

		if (remote_fingerprint(fp, sizeof fp) != 0) {
			buf_printf(out, "no host key (wtmux-service listen "
			    "makes one)\n");
			return (1);
		}
		buf_printf(out, "%s\n", fp);
		return (0);
	}
	if (strcmp(argv[0], "version") == 0 && argc == 1) {
		buf_printf(out, WTMUX_BANNER "\n", TMUX_VERSION);
		return (0);
	}
	buf_printf(out, "unknown request: %s\n", argv[0]);
	return (1);
}

static DWORD WINAPI
client_thread(LPVOID arg)
{
	HANDLE		 pipe = arg, token = NULL;
	struct caller	 c;
	struct buf	 out = { 0 };
	unsigned char	 hdr[8];
	char		*body = NULL, **argv = NULL, byte, *name;
	uint32_t	 len;
	int		 argc = 0, status = 1;

	memset(&c, 0, sizeof c);
	if (pipe_io(pipe, hdr, 4, 0, REQUEST_TIMEOUT) != 0)
		goto out;
	len = get_u32(hdr);
	if (len == 0 || len > TMUX_SERVICE_MAX)
		goto out;
	body = xcalloc(1, len);
	if (pipe_io(pipe, body, len, 0, REQUEST_TIMEOUT) != 0)
		goto out;

	if (!ImpersonateNamedPipeClient(pipe))
		goto out;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
		token = NULL;
	RevertToSelf();
	if (token == NULL || (c.sid = token_user(token)) == NULL)
		goto out;
	c.admin = token_is_admin(token);
	sid_name(c.sid, c.name, sizeof c.name / sizeof c.name[0]);

	if (tmux_service_decode(body, len, &argc, &argv) != 0) {
		buf_printf(&out, "bad request\n");
	} else {
		status = dispatch(&c, argc, argv, &out);
		if (strcmp(argv[0], "ls") == 0) {
			name = to_utf8(c.name);
			service_log(EVENTLOG_INFORMATION_TYPE, "%s listed "
			    "sessions", name);
			free(name);
		}
	}

	put_u32(hdr, (uint32_t)status);
	put_u32(hdr + 4, (uint32_t)out.len);
	if (pipe_io(pipe, hdr, 8, 1, REQUEST_TIMEOUT) == 0 &&
	    (out.len == 0 ||
	    pipe_io(pipe, out.data, (DWORD)out.len, 1, REQUEST_TIMEOUT) == 0))
		pipe_io(pipe, &byte, 1, 0, REQUEST_TIMEOUT);
out:
	if (token != NULL)
		CloseHandle(token);
	tmux_service_free_args(argc, argv);
	free(body);
	free(c.sid);
	free(out.data);
	DisconnectNamedPipe(pipe);
	CloseHandle(pipe);
	InterlockedDecrement(&active_clients);
	return (0);
}

static void
service_loop(void)
{
	SECURITY_ATTRIBUTES	 sa;
	OVERLAPPED		 ov;
	HANDLE			 pipe, events[2], thread;
	DWORD			 flags, err, i;
	int			 first = 1;

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	    L"O:SYG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;AU)",
	    SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
		service_log(EVENTLOG_ERROR_TYPE, "bad pipe security");
		return;
	}
	memset(&ov, 0, sizeof ov);
	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	events[0] = ov.hEvent;
	events[1] = stop_event;
	for (;;) {
		flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
		if (first)
			flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
		pipe = CreateNamedPipeW(TMUX_SERVICE_PIPE, flags,
		    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
		    PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
		    4096, 4096, 0, &sa);
		if (pipe == INVALID_HANDLE_VALUE) {
			service_log(EVENTLOG_ERROR_TYPE,
			    "cannot create the pipe (error %lu)",
			    GetLastError());
			if (first ||
			    WaitForSingleObject(stop_event, 1000) == 0)
				break;
			continue;
		}
		first = 0;
		ResetEvent(ov.hEvent);
		if (!ConnectNamedPipe(pipe, &ov)) {
			err = GetLastError();
			if (err == ERROR_IO_PENDING) {
				if (WaitForMultipleObjects(2, events, FALSE,
				    INFINITE) != WAIT_OBJECT_0) {
					CancelIoEx(pipe, &ov);
					GetOverlappedResult(pipe, &ov, &i,
					    TRUE);
					CloseHandle(pipe);
					break;
				}
				if (!GetOverlappedResult(pipe, &ov, &i,
				    FALSE)) {
					CloseHandle(pipe);
					continue;
				}
			} else if (err != ERROR_PIPE_CONNECTED) {
				CloseHandle(pipe);
				continue;
			}
		}
		InterlockedIncrement(&active_clients);
		if (active_clients > MAX_CLIENTS) {
			InterlockedDecrement(&active_clients);
			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
			continue;
		}
		thread = CreateThread(NULL, 0, client_thread, pipe, 0, NULL);
		if (thread == NULL) {
			InterlockedDecrement(&active_clients);
			CloseHandle(pipe);
		} else
			CloseHandle(thread);
	}
	for (i = 0; i < 100 && active_clients > 0; i++)
		Sleep(100);
	CloseHandle(ov.hEvent);
	LocalFree(sa.lpSecurityDescriptor);
}

static void
set_status(DWORD state)
{
	service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	service_status.dwCurrentState = state;
	service_status.dwControlsAccepted = state == SERVICE_RUNNING ?
	    SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
	service_status.dwWin32ExitCode = NO_ERROR;
	service_status.dwWaitHint = state == SERVICE_RUNNING ? 0 : 10000;
	SetServiceStatus(status_handle, &service_status);
}

static DWORD WINAPI
service_control(DWORD control, DWORD type, LPVOID data, LPVOID context)
{
	(void)type;
	(void)data;
	(void)context;
	switch (control) {
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		set_status(SERVICE_STOP_PENDING);
		SetEvent(stop_event);
		return (NO_ERROR);
	case SERVICE_CONTROL_INTERROGATE:
		return (NO_ERROR);
	}
	return (ERROR_CALL_NOT_IMPLEMENTED);
}

static void WINAPI
service_main(DWORD argc, LPWSTR *argv)
{
	(void)argc;
	(void)argv;
	status_handle = RegisterServiceCtrlHandlerExW(TMUX_SERVICE_NAME,
	    service_control, NULL);
	if (status_handle == NULL)
		return;
	set_status(SERVICE_START_PENDING);
	stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	event_source = RegisterEventSourceW(NULL, TMUX_SERVICE_NAME);
	enable_privilege(L"SeTcbPrivilege");
	enable_privilege(L"SeAssignPrimaryTokenPrivilege");
	enable_privilege(L"SeIncreaseQuotaPrivilege");
	set_status(SERVICE_RUNNING);
	service_log(EVENTLOG_INFORMATION_TYPE, "started (wtmux-service %s)",
	    TMUX_VERSION);
	{
		char	status[512];

		remote_reload(status, sizeof status);
	}
	service_loop();
	remote_stop();
	service_log(EVENTLOG_INFORMATION_TYPE, "stopped");
	if (event_source != NULL)
		DeregisterEventSource(event_source);
	set_status(SERVICE_STOPPED);
}

static const char *
service_call(int argc, char **argv, char **body, uint32_t *len,
    uint32_t *status)
{
	PSECURITY_DESCRIPTOR	 sd;
	PSID			 owner;
	HANDLE			 pipe;
	unsigned char		 hdr[8];
	char			*req;
	const char		*error = NULL;
	size_t			 reqlen;
	int			 ok;

	*body = NULL;
	for (;;) {
		pipe = CreateFileW(TMUX_SERVICE_PIPE,
		    GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
		    SECURITY_IDENTIFICATION, NULL);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetLastError() == ERROR_FILE_NOT_FOUND)
			return ("the service is not running (as an "
			    "administrator: wtmux-service install)");
		if (GetLastError() != ERROR_PIPE_BUSY ||
		    !WaitNamedPipeW(TMUX_SERVICE_PIPE, 5000))
			return ("cannot connect to the service");
	}
	if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT,
	    OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL,
	    &sd) != ERROR_SUCCESS) {
		CloseHandle(pipe);
		return ("cannot check the service's pipe");
	}
	ok = IsWellKnownSid(owner, WinLocalSystemSid);
	LocalFree(sd);
	if (!ok) {
		CloseHandle(pipe);
		return ("the service's pipe is not owned by SYSTEM; refusing");
	}

	if (tmux_service_encode(argc, argv, &req, &reqlen) != 0) {
		CloseHandle(pipe);
		return ("request too long");
	}
	if (pipe_io(pipe, req, (DWORD)reqlen, 1, REQUEST_TIMEOUT) != 0 ||
	    pipe_io(pipe, hdr, 8, 0, REPLY_TIMEOUT) != 0)
		error = "no reply from the service";
	free(req);
	if (error == NULL) {
		*status = get_u32(hdr);
		*len = get_u32(hdr + 4);
		if (*len > TMUX_SERVICE_MAX)
			error = "bad reply from the service";
	}
	if (error == NULL) {
		*body = xcalloc(1, *len + 1);
		if (*len != 0 &&
		    pipe_io(pipe, *body, *len, 0, REQUEST_TIMEOUT) != 0) {
			free(*body);
			*body = NULL;
			error = "short reply from the service";
		}
	}
	CloseHandle(pipe);
	return (error);
}

int
client_request(int argc, char **argv)
{
	char		*body;
	const char	*error;
	uint32_t	 status, len;

	if ((error = service_call(argc, argv, &body, &len, &status)) != NULL)
		fatal(error);
	fwrite(body, 1, len, status == 0 ? stdout : stderr);
	free(body);
	return ((int)status);
}

static int
cmd_version(void)
{
	SC_HANDLE		 scm, svc;
	SERVICE_STATUS		 st;
	QUERY_SERVICE_CONFIGW	*cfg = NULL;
	DWORD			 need = 0;
	wchar_t			 path[MAX_PATH] = L"", *start, *end;
	char			*body = NULL, *dash;
	const char		*state, *error, *args[1] = { "version" };
	uint32_t		 status, len;

	printf(WTMUX_BANNER "\n", TMUX_VERSION);

	if ((scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT)) == NULL) {
		printf("service: unknown (cannot open the service control "
		    "manager)\n");
		return (0);
	}
	svc = OpenServiceW(scm, TMUX_SERVICE_NAME,
	    SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
	if (svc == NULL) {
		if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
			printf("service: not installed\n");
		else
			printf("service: unknown (cannot query it)\n");
		CloseServiceHandle(scm);
		return (0);
	}
	if (!QueryServiceConfigW(svc, NULL, 0, &need) &&
	    GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
		cfg = xcalloc(1, need);
		if (QueryServiceConfigW(svc, cfg, need, &need) &&
		    cfg->lpBinaryPathName != NULL) {
			start = cfg->lpBinaryPathName;
			if (*start == L'"' &&
			    (end = wcschr(start + 1, L'"')) != NULL) {
				*end = L'\0';
				start++;
			}
			wcscpy_s(path, MAX_PATH, start);
		}
		free(cfg);
	}
	if (!QueryServiceStatus(svc, &st))
		st.dwCurrentState = 0;
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);

	if (st.dwCurrentState != SERVICE_RUNNING) {
		switch (st.dwCurrentState) {
		case SERVICE_STOPPED:
			state = "stopped";
			break;
		case SERVICE_START_PENDING:
			state = "starting";
			break;
		case SERVICE_STOP_PENDING:
			state = "stopping";
			break;
		default:
			state = "unknown";
			break;
		}
		printf("service: installed, %s (%ls)\n", state, path);
		return (0);
	}
	error = service_call(1, (char **)args, &body, &len, &status);
	if (error != NULL || status != 0) {
		printf("service: running, version unknown (%ls)\n", path);
		free(body);
		return (0);
	}
	body[strcspn(body, "\r\n")] = '\0';
	if ((dash = strstr(body, " - ")) != NULL)
		*dash = '\0';
	printf("service: running, %s (%ls)\n", body, path);
	free(body);
	return (0);
}

static int
user_ls(const wchar_t *tmux)
{
	WIN32_FIND_DATAW	 dirs, files;
	HANDLE			 hd, hf, nul;
	STARTUPINFOW		 si;
	PROCESS_INFORMATION	 pi;
	wchar_t			 base[MAX_PATH], pattern[MAX_PATH];
	wchar_t			 socket[MAX_PATH], line[3 * MAX_PATH];
	wchar_t			*local;
	SECURITY_ATTRIBUTES	 sa;
	DWORD			 n;

	n = GetEnvironmentVariableW(L"TMUX_TMPDIR", base, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0,
		    NULL, &local)))
			return (1);
		_snwprintf_s(base, MAX_PATH, _TRUNCATE, L"%s\\Temp", local);
		CoTaskMemFree(local);
	}
	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
	    OPEN_EXISTING, 0, NULL);
	_snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\tmux-*", base);
	if ((hd = FindFirstFileW(pattern, &dirs)) == INVALID_HANDLE_VALUE)
		return (0);
	do {
		if (~dirs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		_snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\%s\\*", base,
		    dirs.cFileName);
		if ((hf = FindFirstFileW(pattern, &files)) ==
		    INVALID_HANDLE_VALUE)
			continue;
		do {
			if ((files.dwFileAttributes &
			    FILE_ATTRIBUTE_DIRECTORY) ||
			    (~files.dwFileAttributes &
			    FILE_ATTRIBUTE_REPARSE_POINT))
				continue;
			_snwprintf_s(socket, MAX_PATH, _TRUNCATE, L"%s\\%s\\%s",
			    base, dirs.cFileName, files.cFileName);
			_snwprintf_s(line, sizeof line / sizeof line[0],
			    _TRUNCATE, L"\"%s\" -S \"%s\" ls -F "
			    L"\"#{socket_path}|#{session_name}|"
			    L"#{session_windows}|#{session_attached}\"", tmux,
			    socket);
			memset(&si, 0, sizeof si);
			si.cb = sizeof si;
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
			si.hStdError = nul;
			if (CreateProcessW(NULL, line, NULL, NULL, TRUE,
			    CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				WaitForSingleObject(pi.hProcess, TMUX_TIMEOUT);
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
			}
		} while (FindNextFileW(hf, &files));
		FindClose(hf);
	} while (FindNextFileW(hd, &dirs));
	FindClose(hd);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	return (0);
}

static int
path_is_protected(const wchar_t *path, wchar_t *why, size_t whylen)
{
	PSECURITY_DESCRIPTOR	 sd;
	PSID			 owner;
	PACL			 dacl;
	ACE_HEADER		*ace;
	ACCESS_ALLOWED_ACE	*allow;
	wchar_t			 name[512];
	DWORD			 i, mask;
	int			 ok = 1;

	mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
	    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
	    GENERIC_WRITE | GENERIC_ALL;
	if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
	    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
	    NULL, &dacl, NULL, &sd) != ERROR_SUCCESS) {
		_snwprintf_s(why, whylen, _TRUNCATE, L"%s: cannot read its "
		    L"permissions", path);
		return (0);
	}
	if (!is_trusted_sid(owner)) {
		sid_name(owner, name, 512);
		_snwprintf_s(why, whylen, _TRUNCATE, L"%s is owned by %s",
		    path, name);
		ok = 0;
	} else if (dacl == NULL) {
		_snwprintf_s(why, whylen, _TRUNCATE, L"%s has no permissions "
		    L"(anyone may change it)", path);
		ok = 0;
	} else {
		for (i = 0; ok && i < dacl->AceCount; i++) {
			if (!GetAce(dacl, i, (void **)&ace) ||
			    ace->AceType != ACCESS_ALLOWED_ACE_TYPE ||
			    (ace->AceFlags & INHERIT_ONLY_ACE))
				continue;
			allow = (ACCESS_ALLOWED_ACE *)ace;
			if ((allow->Mask & mask) == 0 ||
			    is_trusted_sid(&allow->SidStart))
				continue;
			sid_name(&allow->SidStart, name, 512);
			_snwprintf_s(why, whylen, _TRUNCATE,
			    L"%s may be changed by %s", path, name);
			ok = 0;
		}
	}
	LocalFree(sd);
	return (ok);
}

static void
install_advice(const wchar_t *dir)
{
	wchar_t	pf[MAX_PATH];

	if (ExpandEnvironmentStringsW(L"%ProgramFiles%\\wtmux", pf,
	    MAX_PATH) == 0 || pf[0] == L'%')
		wcscpy_s(pf, MAX_PATH, L"C:\\Program Files\\wtmux");
	fwprintf(stderr,
	    L"wtmux-service: refusing to install from %s: the service runs as "
	    L"SYSTEM and starts the programs in this folder, so only "
	    L"administrators may be able to change it.\n"
	    L"From an elevated prompt, either copy the folder under Program "
	    L"Files:\n"
	    L"  robocopy \"%s\" \"%s\" /E\n"
	    L"  \"%s\\wtmux-service.exe\" install\n"
	    L"or lock this folder down:\n"
	    L"  icacls \"%s\" /setowner *S-1-5-32-544 /T /C /Q\n"
	    L"  icacls \"%s\" /inheritance:r /remove:g *S-1-1-0 *S-1-5-11 "
	    L"*S-1-5-32-545 *S-1-3-0 /T /C /Q\n"
	    L"  icacls \"%s\" /grant:r *S-1-5-18:(OI)(CI)F "
	    L"*S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX /T /C /Q\n"
	    L"  \"%s\\wtmux-service.exe\" install\n",
	    dir, dir, pf, pf, dir, dir, dir, dir);
}

static int
cmd_install(void)
{
	SC_HANDLE		 scm, svc;
	SERVICE_DESCRIPTIONW	 desc;
	SERVICE_STATUS		 st;
	HKEY			 key;
	wchar_t			 dir[MAX_PATH], *slash, why[1024];
	wchar_t			 cmd[MAX_PATH + 16];
	const wchar_t		*check[3];
	const wchar_t		*dll = L"%SystemRoot%\\Microsoft.NET\\Framework"
				    L"\\v4.0.30319\\EventLogMessages.dll";
	DWORD			 types = 7, i;

	if (!process_is_elevated())
		fatal("install needs an elevated (administrator) prompt");
	if (GetFileAttributesW(tmux_path) == INVALID_FILE_ATTRIBUTES)
		fatal("tmux.exe is not beside wtmux-service.exe");
	wcscpy_s(dir, MAX_PATH, self_path);
	if ((slash = wcsrchr(dir, L'\\')) != NULL)
		*slash = L'\0';
	check[0] = dir;
	check[1] = self_path;
	check[2] = tmux_path;
	for (i = 0; i < 3; i++) {
		if (!path_is_protected(check[i], why,
		    sizeof why / sizeof why[0])) {
			fwprintf(stderr, L"wtmux-service: %s\n", why);
			install_advice(dir);
			exit(1);
		}
	}

	if ((scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE)) ==
	    NULL)
		fatal("cannot open the service control manager");
	_snwprintf_s(cmd, sizeof cmd / sizeof cmd[0], _TRUNCATE,
	    L"\"%s\" run", self_path);
	svc = CreateServiceW(scm, TMUX_SERVICE_NAME, L"tmux session service",
	    SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
	    SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL);
	if (svc == NULL) {
		if (GetLastError() == ERROR_SERVICE_EXISTS)
			fatal("already installed (wtmux-service uninstall "
			    "first)");
		fatal("cannot create the service");
	}
	desc.lpDescription = L"Starts tmux sessions inside logged-on users' "
	    L"own sessions on request (wtmux-service new).";
	ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, EVENT_SOURCE_KEY, 0, NULL, 0,
	    KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
		RegSetValueExW(key, L"EventMessageFile", 0, REG_EXPAND_SZ,
		    (const BYTE *)dll, (DWORD)((wcslen(dll) + 1) *
		    sizeof (wchar_t)));
		RegSetValueExW(key, L"TypesSupported", 0, REG_DWORD,
		    (const BYTE *)&types, sizeof types);
		RegCloseKey(key);
	}
	if (!StartServiceW(svc, 0, NULL))
		fatal("installed, but the service did not start");
	for (i = 0; i < 100; i++) {
		if (QueryServiceStatus(svc, &st) &&
		    st.dwCurrentState == SERVICE_RUNNING)
			break;
		Sleep(100);
	}
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	fwprintf(stdout, L"wtmux-service installed and started (%s)\n",
	    self_path);
	return (0);
}

static int
cmd_uninstall(void)
{
	SC_HANDLE	 scm, svc;
	SERVICE_STATUS	 st;
	int		 i;

	if (!process_is_elevated())
		fatal("uninstall needs an elevated (administrator) prompt");
	if ((scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT)) == NULL)
		fatal("cannot open the service control manager");
	svc = OpenServiceW(scm, TMUX_SERVICE_NAME,
	    SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
	if (svc == NULL)
		fatal("not installed");
	if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
		for (i = 0; i < 100; i++) {
			if (QueryServiceStatus(svc, &st) &&
			    st.dwCurrentState == SERVICE_STOPPED)
				break;
			Sleep(100);
		}
	}
	if (!DeleteService(svc))
		fatal("cannot delete the service");
	remote_forget();
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, EVENT_SOURCE_KEY);
	printf("wtmux-service uninstalled\n");
	return (0);
}

static void
usage(void)
{
	fprintf(stderr, WTMUX_BANNER "\n", TMUX_VERSION);
	fprintf(stderr,
	    "usage: wtmux-service ls\n"
	    "       wtmux-service new [-w] [-i session-id] [-L socket-name] "
	    "[-s session-name]\n"
	    "                        [-u user] [--] [command ...]\n"
	    "       wtmux-service install | uninstall | version | -V\n"
	    "       wtmux-service listen [-a address] [-p port] "
	    "[-auth keys|windows|both]\n"
	    "                           [-no-ntlm] | listen off\n"
	    "       wtmux-service fingerprint | keygen | pubkey\n");
	exit(1);
}

int
wmain(int argc, wchar_t **wargv)
{
	SERVICE_TABLE_ENTRYW	 table[] = {
		{ TMUX_SERVICE_NAME, service_main },
		{ NULL, NULL }
	};
	char			**argv;
	int			  i;

	InitializeCriticalSection(&log_lock);
	find_paths();
	if (argc < 2)
		usage();
	if (wcscmp(wargv[1], L"run") == 0) {
		if (!StartServiceCtrlDispatcherW(table))
			fatal("run is for the service control manager");
		return (0);
	}
	if (wcscmp(wargv[1], L"install") == 0 && argc == 2)
		return (cmd_install());
	if (argc == 2 && (wcscmp(wargv[1], L"version") == 0 ||
	    wcscmp(wargv[1], L"-V") == 0 || wcscmp(wargv[1], L"--version") == 0))
		return (cmd_version());
	if (wcscmp(wargv[1], L"uninstall") == 0 && argc == 2)
		return (cmd_uninstall());
	if (wcscmp(wargv[1], L"user-ls") == 0 && argc == 3)
		return (user_ls(wargv[2]));
	if (wcscmp(wargv[1], L"listen") == 0)
		return (cmd_listen(argc, wargv));
	if (wcscmp(wargv[1], L"keygen") == 0 && argc == 2)
		return (cmd_keygen(1));
	if (wcscmp(wargv[1], L"pubkey") == 0 && argc == 2)
		return (cmd_keygen(0));
	if (wargv[1][0] == L'-')
		usage();
	argv = xcalloc(argc, sizeof *argv);
	for (i = 1; i < argc; i++)
		argv[i - 1] = to_utf8(wargv[i]);
	return (client_request(argc - 1, argv));
}
