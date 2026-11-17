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
#include <winternl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef TMUX_WIN32_CONPTY_DEFAULT
#define TMUX_WIN32_CONPTY_DEFAULT "inbox"
#endif
#ifndef PSEUDOCONSOLE_GLYPH_WIDTH_WCSWIDTH
#define PSEUDOCONSOLE_GLYPH_WIDTH_WCSWIDTH 0x10
#endif
typedef HRESULT(
    WINAPI *conpty_create_fn)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT(WINAPI *conpty_resize_fn)(HPCON, COORD);
typedef VOID(WINAPI *conpty_close_fn)(HPCON);
struct conpty_provider {
	conpty_create_fn create;
	conpty_resize_fn resize;
	conpty_close_fn close;
	DWORD flags;
	char name[MAX_PATH * 3 + 16];
};
static struct conpty_provider conpty_api, conpty_inbox;
static DWORD conpty_error;
static int conpty_auto;
static INIT_ONCE conpty_once = INIT_ONCE_STATIC_INIT;

static int
conpty_host_present(const wchar_t *dll)
{
	typedef BOOL(WINAPI * wow64_fn)(HANDLE, USHORT *, USHORT *);
	wow64_fn wow64;
	USHORT process_machine = 0, native = 0;
	SYSTEM_INFO si;
	const wchar_t *arch;
	wchar_t host[MAX_PATH * 2], *slash;

	wow64 =
	    (wow64_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
	        "IsWow64Process2");
	if (wow64 == NULL ||
	    !wow64(GetCurrentProcess(), &process_machine, &native)) {
		GetNativeSystemInfo(&si);
		native =
		    si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64
		    ? IMAGE_FILE_MACHINE_ARM64
		    : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
		    ? IMAGE_FILE_MACHINE_AMD64
		    : IMAGE_FILE_MACHINE_I386;
	}
	arch = native == IMAGE_FILE_MACHINE_ARM64 ? L"arm64"
	    : native == IMAGE_FILE_MACHINE_AMD64  ? L"x64"
	                                          : L"x86";
	if (wcslen(dll) >= sizeof host / sizeof host[0] - 32)
		return (0);
	wcscpy(host, dll);
	slash = wcsrchr(host, L'\\');
	if (slash == NULL)
		return (0);
	swprintf(slash + 1,
	    sizeof host / sizeof host[0] - (size_t)(slash + 1 - host),
	    L"%ls\\OpenConsole.exe", arch);
	return (GetFileAttributesW(host) != INVALID_FILE_ATTRIBUTES);
}

static int
conpty_load_package(struct conpty_provider *p, const wchar_t *path)
{
	HMODULE module;
	if (!conpty_host_present(path))
		return (0);
	module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (module == NULL)
		return (0);
	p->create = (conpty_create_fn)(void *)GetProcAddress(module,
	    "ConptyCreatePseudoConsole");
	p->resize = (conpty_resize_fn)(void *)GetProcAddress(module,
	    "ConptyResizePseudoConsole");
	p->close = (conpty_close_fn)(void *)GetProcAddress(module,
	    "ConptyClosePseudoConsole");
	if (p->create == NULL || p->resize == NULL || p->close == NULL) {
		FreeLibrary(module);
		memset(p, 0, sizeof *p);
		return (0);
	}
	p->flags = PSEUDOCONSOLE_GLYPH_WIDTH_WCSWIDTH;
	strcpy(p->name, "package:");
	WideCharToMultiByte(CP_UTF8, 0, path, -1, p->name + 8,
	    (int)sizeof p->name - 8, NULL, NULL);
	return (1);
}

static int
conpty_load_inbox(struct conpty_provider *p)
{
	HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
	if (kernel == NULL)
		return (0);
	p->create = (conpty_create_fn)(void *)GetProcAddress(kernel,
	    "CreatePseudoConsole");
	p->resize = (conpty_resize_fn)(void *)GetProcAddress(kernel,
	    "ResizePseudoConsole");
	p->close = (conpty_close_fn)(void *)GetProcAddress(kernel,
	    "ClosePseudoConsole");
	if (p->create == NULL || p->resize == NULL || p->close == NULL) {
		memset(p, 0, sizeof *p);
		return (0);
	}
	p->flags = 0;
	strcpy(p->name, "inbox");
	return (1);
}

static BOOL CALLBACK
conpty_init(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	wchar_t mode[16], path[MAX_PATH * 2], *slash;
	DWORD n;
	int want_package, need_package;
	(void)once;
	(void)parameter;
	(void)context;
	n = GetEnvironmentVariableW(L"TMUX_WINDOWS_CONPTY", mode,
	    sizeof mode / sizeof mode[0]);
	if (n == 0 || n >= sizeof mode / sizeof mode[0])
		MultiByteToWideChar(CP_UTF8, 0, TMUX_WIN32_CONPTY_DEFAULT, -1,
		    mode, sizeof mode / sizeof mode[0]);
	need_package = _wcsicmp(mode, L"package") == 0;
	want_package = need_package || _wcsicmp(mode, L"auto") == 0;
	conpty_load_inbox(&conpty_inbox);
	if (want_package) {
		n = GetEnvironmentVariableW(L"TMUX_WINDOWS_CONPTY_DLL", path,
		    sizeof path / sizeof path[0]);
		if (n == 0 || n >= sizeof path / sizeof path[0]) {
			n = GetModuleFileNameW(NULL, path,
			    sizeof path / sizeof path[0]);
			if (n == 0 || n >= sizeof path / sizeof path[0] - 16)
				path[0] = L'\0';
			else {
				slash = wcsrchr(path, L'\\');
				if (slash != NULL)
					wcscpy(slash + 1, L"conpty.dll");
			}
		}
		for (slash = path; *slash != L'\0'; slash++) {
			if (*slash == L'/')
				*slash = L'\\';
		}
		if (path[0] != L'\0' &&
		    GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
		    conpty_load_package(&conpty_api, path)) {
			conpty_auto = !need_package;
			return (TRUE);
		}
		if (need_package) {
			conpty_error = ERROR_MOD_NOT_FOUND;
			return (TRUE);
		}
	}
	conpty_api = conpty_inbox;
	if (conpty_api.create == NULL)
		conpty_error = ERROR_PROC_NOT_FOUND;
	return (TRUE);
}

const char *
tmux_win32_conpty_provider(void)
{
	InitOnceExecuteOnce(&conpty_once, conpty_init, NULL, NULL);
	return (conpty_api.create != NULL ? conpty_api.name : NULL);
}

static HRESULT
conpty_create(COORD size, HANDLE in, HANDLE out, HPCON *console,
    const struct conpty_provider **used)
{
	HRESULT hr;
	*used = &conpty_api;
	hr = conpty_api.create(size, in, out, conpty_api.flags, console);
	if (FAILED(hr) && conpty_auto && conpty_inbox.create != NULL) {
		*used = &conpty_inbox;
		hr = conpty_inbox.create(size, in, out, conpty_inbox.flags,
		    console);
	}
	return (hr);
}

#define OUTPUT_CAPACITY (4u * 1024u * 1024u)
#define INPUT_CAPACITY (1u * 1024u * 1024u)
struct byte_queue {
	unsigned char *data;
	size_t capacity, head, used;
};
struct tmux_pty {
	HPCON console;
	const struct conpty_provider
	    *provider;
	HANDLE process, job, input, output, reader, writer, reaper;
	SRWLOCK console_lock;
	DWORD pid;
	CRITICAL_SECTION lock;
	CONDITION_VARIABLE condition;
	struct byte_queue in, out;
	int closing, eof, input_eof, aborted;
	DWORD read_error, write_error;
};
static size_t
queue_put(struct byte_queue *q, const unsigned char *data, size_t length)
{
	size_t tail = (q->head + q->used) % q->capacity;
	size_t first;
	if (length > q->capacity - q->used)
		length = q->capacity - q->used;
	first = q->capacity - tail;
	if (first > length)
		first = length;
	memcpy(q->data + tail, data, first);
	memcpy(q->data, data + first, length - first);
	q->used += length;
	return (length);
}

static size_t
queue_take(struct byte_queue *q, unsigned char *data, size_t length)
{
	size_t first;
	if (length > q->used)
		length = q->used;
	first = q->capacity - q->head;
	if (first > length)
		first = length;
	memcpy(data, q->data + q->head, first);
	memcpy(data + first, q->data, length - first);
	q->head = (q->head + length) % q->capacity;
	q->used -= length;
	return (length);
}

static DWORD WINAPI
reader_thread(LPVOID arg)
{
	struct tmux_pty *pty = arg;
	unsigned char buffer[16384];
	DWORD count, saved = ERROR_SUCCESS;
	size_t offset;
	for (;;) {
		if (!ReadFile(pty->output, buffer, sizeof buffer, &count,
		        NULL)) {
			saved = GetLastError();
			if (saved == ERROR_BROKEN_PIPE ||
			    saved == ERROR_OPERATION_ABORTED)
				saved = ERROR_SUCCESS;
			break;
		}
		if (count == 0)
			break;
		offset = 0;
		EnterCriticalSection(&pty->lock);
		while (offset < count && !pty->closing) {
			if (pty->out.used == pty->out.capacity)
				SleepConditionVariableCS(&pty->condition,
				    &pty->lock, INFINITE);
			else {
				offset += queue_put(&pty->out, buffer + offset,
				    count - offset);
				WakeAllConditionVariable(&pty->condition);
			}
		}
		LeaveCriticalSection(&pty->lock);
	}
	EnterCriticalSection(&pty->lock);
	pty->read_error = saved;
	pty->eof = 1;
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
	return (0);
}

static DWORD WINAPI
writer_thread(LPVOID arg)
{
	struct tmux_pty *pty = arg;
	unsigned char buffer[16384];
	DWORD count, saved = ERROR_SUCCESS;
	size_t length, offset;
	for (;;) {
		EnterCriticalSection(&pty->lock);
		while (!pty->closing && !pty->input_eof && pty->in.used == 0)
			SleepConditionVariableCS(&pty->condition, &pty->lock,
			    INFINITE);
		if (pty->closing || (pty->input_eof && pty->in.used == 0)) {
			LeaveCriticalSection(&pty->lock);
			break;
		}
		length = queue_take(&pty->in, buffer, sizeof buffer);
		WakeAllConditionVariable(&pty->condition);
		LeaveCriticalSection(&pty->lock);
		for (offset = 0; offset < length; offset += count) {
			if (!WriteFile(pty->input, buffer + offset,
			        (DWORD)(length - offset), &count, NULL) ||
			    count == 0) {
				saved = GetLastError();
				if (saved == ERROR_SUCCESS)
					saved = ERROR_BROKEN_PIPE;
				goto done;
			}
		}
	}
done:
	EnterCriticalSection(&pty->lock);
	pty->write_error = saved;
	WakeAllConditionVariable(&pty->condition);
	if (pty->input_eof && !pty->closing) {
		CloseHandle(pty->input);
		pty->input = NULL;
	}
	LeaveCriticalSection(&pty->lock);
	return (0);
}

static DWORD WINAPI
reaper_thread(LPVOID arg)
{
	struct tmux_pty *pty = arg;
	HPCON console;
	WaitForSingleObject(pty->process, INFINITE);
	AcquireSRWLockExclusive(&pty->console_lock);
	console = pty->console;
	pty->console = NULL;
	ReleaseSRWLockExclusive(&pty->console_lock);
	if (console != NULL)
		pty->provider->close(console);
	return (0);
}

#define PTY_PARAMS_NORMALIZED	0x1
#define PTY_PARAMS_FLAGS	0x8
#define PTY_PARAMS32_CURDIR	0x24
#define PTY_PARAMS32_TITLE	0x70
#define PTY_PEB32_PARAMS	0x10
#ifdef _WIN64
#define PTY_PARAMS_CURDIR	0x38
#define PTY_PARAMS_TITLE	0xb0
#else
#define PTY_PARAMS_CURDIR	PTY_PARAMS32_CURDIR
#define PTY_PARAMS_TITLE	PTY_PARAMS32_TITLE
#endif

typedef LONG (NTAPI *pty_query_process_fn)(HANDLE, ULONG, PVOID, ULONG,
    PULONG);

static int
pty_peek(HANDLE h, uint64_t address, void *buffer, size_t length)
{
	SIZE_T	got;

	if (address == 0 || address > (uint64_t)(ULONG_PTR)-1)
		return (-1);
	if (!ReadProcessMemory(h, (LPCVOID)(ULONG_PTR)address, buffer, length,
	    &got) || got != length)
		return (-1);
	return (0);
}

static wchar_t *
pty_process_parameter(HANDLE h, int title)
{
	static pty_query_process_fn	 query;
	PROCESS_BASIC_INFORMATION	 basic;
	ULONG_PTR			 peb32 = 0, params = 0;
	ULONG				 flags, params32;
	UNICODE_STRING			 string;
	struct {
		USHORT	Length;
		USHORT	MaximumLength;
		ULONG	Buffer;
	}				 string32;
	uint64_t			 address;
	USHORT				 length;
	wchar_t				*s;

	if (query == NULL) {
		query = (pty_query_process_fn)(void *)GetProcAddress(
		    GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
		if (query == NULL)
			return (NULL);
	}
#ifdef _WIN64
	if (query(h, ProcessWow64Information, &peb32, sizeof peb32,
	    NULL) < 0)
		peb32 = 0;
#endif
	if (peb32 != 0) {
		if (pty_peek(h, peb32 + PTY_PEB32_PARAMS, &params32,
		    sizeof params32) != 0)
			return (NULL);
		params = params32;
		if (pty_peek(h, params + (title ? PTY_PARAMS32_TITLE :
		    PTY_PARAMS32_CURDIR), &string32, sizeof string32) != 0)
			return (NULL);
		length = string32.Length;
		address = string32.Buffer;
	} else {
		if (query(h, ProcessBasicInformation, &basic, sizeof basic,
		    NULL) < 0)
			return (NULL);
		if (pty_peek(h, (ULONG_PTR)basic.PebBaseAddress +
		    offsetof(PEB, ProcessParameters), &params,
		    sizeof params) != 0)
			return (NULL);
		if (pty_peek(h, params + (title ? PTY_PARAMS_TITLE :
		    PTY_PARAMS_CURDIR), &string, sizeof string) != 0)
			return (NULL);
		length = string.Length;
		address = (ULONG_PTR)string.Buffer;
	}
	if (pty_peek(h, params + PTY_PARAMS_FLAGS, &flags, sizeof flags) != 0)
		return (NULL);
	if (~flags & PTY_PARAMS_NORMALIZED)
		address += params;
	if (length == 0 || length % sizeof (wchar_t) != 0)
		return (NULL);
	if ((s = malloc(length + sizeof (wchar_t))) == NULL)
		return (NULL);
	if (pty_peek(h, address, s, length) != 0) {
		free(s);
		return (NULL);
	}
	s[length / sizeof (wchar_t)] = L'\0';
	return (s);
}

char *
tmux_win32_process_cwd(uint64_t pid)
{
	struct tmux_os_error	 error;
	wchar_t			 image[MAX_PATH * 4], *wide;
	DWORD			 size = MAX_PATH * 4;
	HANDLE			 h;
	char			*path, *base, *cwd = NULL;
	size_t			 n;

	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ,
	    FALSE, (DWORD)pid);
	if (h == NULL)
		return (NULL);
	if (QueryFullProcessImageNameW(h, 0, image, &size) &&
	    (path = tmux_win32_utf8(image, &error)) != NULL) {
		base = strrchr(path, '\\');
		base = (base == NULL ? path : base + 1);
		n = (_stricmp(base, "pwsh.exe") == 0 ||
		    _stricmp(base, "powershell.exe") == 0 ||
		    tmux_windows_is_cygwin_program(path));
		free(path);
		if (n) {
			CloseHandle(h);
			return (NULL);
		}
	}
	wide = pty_process_parameter(h, 0);
	CloseHandle(h);
	if (wide == NULL)
		return (NULL);
	n = wcslen(wide);
	if (n > 3 && wide[n - 1] == L'\\')
		wide[n - 1] = L'\0';
	cwd = tmux_win32_utf8(wide, &error);
	free(wide);
	return (cwd);
}

static void
pty_set_codepage(HPCON console, HANDLE child, unsigned int codepage)
{
	STARTUPINFOEXW		 startup;
	PROCESS_INFORMATION	 process;
	SECURITY_ATTRIBUTES	 security;
	SIZE_T			 size = 0;
	HANDLE			 null;
	wchar_t			 title[MAX_PATH * 2], path[MAX_PATH + 16];
	wchar_t			 command[32], *s;
	DWORD			 n = MAX_PATH * 2;
	UINT			 len;

	if (!IsValidCodePage(codepage))
		return;
	len = GetSystemDirectoryW(path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return;
	wcscat_s(path, MAX_PATH + 16, L"\\chcp.com");
	swprintf(command, 32, L"chcp.com %u", codepage);
	if ((s = pty_process_parameter(child, 1)) != NULL) {
		wcsncpy_s(title, MAX_PATH * 2, s, _TRUNCATE);
		free(s);
	} else if (!QueryFullProcessImageNameW(child, 0, title, &n))
		title[0] = L'\0';

	memset(&security, 0, sizeof security);
	security.nLength = sizeof security;
	security.bInheritHandle = TRUE;
	null = CreateFileW(L"NUL", GENERIC_READ|GENERIC_WRITE,
	    FILE_SHARE_READ|FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, NULL);
	if (null == INVALID_HANDLE_VALUE)
		return;

	memset(&startup, 0, sizeof startup);
	memset(&process, 0, sizeof process);
	InitializeProcThreadAttributeList(NULL, 2, 0, &size);
	startup.lpAttributeList = malloc(size);
	if (startup.lpAttributeList == NULL)
		goto out;
	if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0,
	    &size)) {
		free(startup.lpAttributeList);
		goto out;
	}
	if (UpdateProcThreadAttribute(startup.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof console, NULL,
	    NULL) &&
	    UpdateProcThreadAttribute(startup.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &null, sizeof null, NULL,
	    NULL)) {
		startup.StartupInfo.cb = sizeof startup;
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = null;
		startup.StartupInfo.hStdOutput = null;
		startup.StartupInfo.hStdError = null;
		if (title[0] != L'\0')
			startup.StartupInfo.lpTitle = title;
		if (CreateProcessW(path, command, NULL, NULL, TRUE,
		    EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
		    &startup.StartupInfo, &process)) {
			if (WaitForSingleObject(process.hProcess, 5000) !=
			    WAIT_OBJECT_0)
				TerminateProcess(process.hProcess, 1);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
	}
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	free(startup.lpAttributeList);
out:
	CloseHandle(null);
}
static int
pty_spawn_impl(struct tmux_pty **out, const struct tmux_pty_options *o,
    const char *raw, const wchar_t *rawenv, int use_pipes,
    struct tmux_os_error *error)
{
	struct tmux_pty *pty = NULL;
	HANDLE con_in = NULL, con_out = NULL;
	STARTUPINFOEXW startup;
	PROCESS_INFORMATION process;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	SIZE_T attribute_size = 0;
	COORD size;
	HRESULT hr;
	DWORD saved;
	wchar_t *application = NULL, *command = NULL, *cwd = NULL, *env = NULL;
	char *command8 = NULL;
	int attr_initialized = 0;
	SECURITY_ATTRIBUTES security;
	HANDLE inherited[2];
	memset(&security, 0, sizeof security);
	security.nLength = sizeof security;
	security.bInheritHandle = TRUE;
	memset(&startup, 0, sizeof startup);
	memset(&process, 0, sizeof process);
	if (out == NULL)
		return tmux_win32_error(error, ERROR_INVALID_PARAMETER,
		    "output pointer");
	*out = NULL;
	if (o == NULL ||
	    (raw == NULL &&
	        (o->argv == NULL || o->argv[0] == NULL ||
	            o->argv[0][0] == '\0')) ||
	    o->columns == 0 || o->rows == 0 || o->columns > 32767 ||
	    o->rows > 32767)
		return tmux_win32_error(error, ERROR_INVALID_PARAMETER,
		    "pty options");
	if (raw == NULL) {
		application = tmux_win32_executable(o->argv[0], error);
		if (application == NULL)
			goto fail;
		command8 = tmux_windows_command_line(o->argv);
	} else
		command8 = _strdup(raw);
	if (command8 == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "command line allocation");
		goto fail;
	}
	command = tmux_win32_utf16(command8, error);
	if (command == NULL)
		goto fail;
	if (wcslen(command) >= 32767) {
		tmux_win32_error(error, ERROR_FILENAME_EXCED_RANGE,
		    "command line length");
		goto fail;
	}
	if (o->cwd != NULL && (cwd = tmux_win32_utf16(o->cwd, error)) == NULL)
		goto fail;
	if (o->env != NULL &&
	    (env = tmux_win32_environment(o->env, error)) == NULL)
		goto fail;
	pty = calloc(1, sizeof *pty);
	if (pty == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "pty allocation");
		goto fail;
	}
	InitializeCriticalSection(&pty->lock);
	InitializeSRWLock(&pty->console_lock);
	InitializeConditionVariable(&pty->condition);
	pty->in.capacity = INPUT_CAPACITY;
	pty->out.capacity = OUTPUT_CAPACITY;
	pty->in.data = malloc(pty->in.capacity);
	pty->out.data = malloc(pty->out.capacity);
	if (pty->in.data == NULL || pty->out.data == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "pty IO allocation");
		goto fail;
	}
	if (!CreatePipe(&con_in, &pty->input, use_pipes ? &security : NULL,
	        0) ||
	    !CreatePipe(&pty->output, &con_out, use_pipes ? &security : NULL,
	        0)) {
		tmux_win32_error(error, GetLastError(), "create ConPTY pipes");
		goto fail;
	}
	if (use_pipes) {
		if (!SetHandleInformation(pty->input, HANDLE_FLAG_INHERIT, 0) ||
		    !SetHandleInformation(pty->output, HANDLE_FLAG_INHERIT,
		        0)) {
			tmux_win32_error(error, GetLastError(),
			    "private pipe handles");
			goto fail;
		}
	} else {
		if (tmux_win32_conpty_provider() == NULL) {
			tmux_win32_error(error, conpty_error,
			    conpty_error == ERROR_MOD_NOT_FOUND
			        ? "TMUX_WINDOWS_CONPTY=package but conpty.dll "
			          "or its OpenConsole.exe host is missing"
			        : "ConPTY is unavailable (Windows 10 1809 or "
			          "later is required)");
			goto fail;
		}
		size.X = (SHORT)o->columns;
		size.Y = (SHORT)o->rows;
		hr = conpty_create(size, con_in, con_out, &pty->console,
		    &pty->provider);
		if (FAILED(hr)) {
			tmux_win32_error(error, (DWORD)hr,
			    "CreatePseudoConsole");
			goto fail;
		}
		CloseHandle(con_in);
		con_in = NULL;
		CloseHandle(con_out);
		con_out = NULL;
	}
	pty->reader = CreateThread(NULL, 0, reader_thread, pty, 0, NULL);
	if (pty->reader == NULL) {
		tmux_win32_error(error, GetLastError(), "ConPTY reader thread");
		goto fail;
	}
	pty->writer = CreateThread(NULL, 0, writer_thread, pty, 0, NULL);
	if (pty->writer == NULL) {
		tmux_win32_error(error, GetLastError(), "ConPTY writer thread");
		goto fail;
	}
	InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
	startup.lpAttributeList = malloc(attribute_size);
	if (startup.lpAttributeList == NULL) {
		tmux_win32_error(error, ERROR_NOT_ENOUGH_MEMORY,
		    "process attributes");
		goto fail;
	}
	if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
	        &attribute_size)) {
		tmux_win32_error(error, GetLastError(),
		    "initialize process attributes");
		goto fail;
	}
	attr_initialized = 1;
	inherited[0] = con_in;
	inherited[1] = con_out;
	if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
	        use_pipes ? PROC_THREAD_ATTRIBUTE_HANDLE_LIST
	                  : PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
	        use_pipes ? (void *)inherited : pty->console,
	        use_pipes ? sizeof inherited : sizeof pty->console, NULL,
	        NULL)) {
		tmux_win32_error(error, GetLastError(), "attach child IO");
		goto fail;
	}
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	if (use_pipes) {
		startup.StartupInfo.hStdInput = con_in;
		startup.StartupInfo.hStdOutput = con_out;
		startup.StartupInfo.hStdError = con_out;
	} else {
		startup.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
		startup.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
		startup.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
	}
	pty->job = CreateJobObjectW(NULL, NULL);
	memset(&limits, 0, sizeof limits);
	limits.BasicLimitInformation.LimitFlags =
	    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (pty->job == NULL ||
	    !SetInformationJobObject(pty->job,
	        JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
		tmux_win32_error(error, GetLastError(), "process tree job");
		goto fail;
	}
	startup.StartupInfo.cb = sizeof startup;
	if (!CreateProcessW(application, command, NULL, NULL,
	        use_pipes ? TRUE : FALSE,
	        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
	            CREATE_SUSPENDED | (use_pipes ? CREATE_NO_WINDOW : 0),
	        rawenv != NULL ? (void *)rawenv : env, cwd,
	        &startup.StartupInfo, &process)) {
		tmux_win32_error(error, GetLastError(), "CreateProcessW");
		goto fail;
	}
	if (con_in != NULL) {
		CloseHandle(con_in);
		con_in = NULL;
	}
	if (con_out != NULL) {
		CloseHandle(con_out);
		con_out = NULL;
	}
	pty->process = process.hProcess;
	pty->pid = process.dwProcessId;
	if (!AssignProcessToJobObject(pty->job, pty->process)) {
		saved = GetLastError();
		TerminateProcess(pty->process, 127);
		tmux_win32_error(error, saved,
		    "assign child to process tree job");
		goto fail;
	}
	if (!use_pipes && o->codepage != 0)
		pty_set_codepage(pty->console, process.hProcess, o->codepage);
	if (ResumeThread(process.hThread) == (DWORD)-1) {
		tmux_win32_error(error, GetLastError(), "resume child");
		goto fail;
	}
	CloseHandle(process.hThread);
	process.hThread = NULL;
	pty->reaper = CreateThread(NULL, 0, reaper_thread, pty, 0, NULL);
	if (pty->reaper == NULL) {
		tmux_win32_error(error, GetLastError(), "ConPTY reaper thread");
		goto fail;
	}
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	free(startup.lpAttributeList);
	free(application);
	free(command8);
	free(command);
	free(cwd);
	free(env);
	*out = pty;
	return (0);
fail:
	if (process.hThread != NULL)
		CloseHandle(process.hThread);
	if (attr_initialized)
		DeleteProcThreadAttributeList(startup.lpAttributeList);
	free(startup.lpAttributeList);
	if (con_in != NULL)
		CloseHandle(con_in);
	if (con_out != NULL)
		CloseHandle(con_out);
	free(application);
	free(command8);
	free(command);
	free(cwd);
	free(env);
	tmux_pty_destroy(pty);
	return (-1);
}

int
tmux_pty_spawn(struct tmux_pty **out, const struct tmux_pty_options *o,
    struct tmux_os_error *error)
{
	return (pty_spawn_impl(out, o, NULL, NULL, 0, error));
}

int
tmux_win32_pty_spawn_raw(struct tmux_pty **out, const char *command,
    const char *cwd, const wchar_t *env, unsigned short columns,
    unsigned short rows, int use_pipes, unsigned int codepage,
    struct tmux_os_error *error)
{
	struct tmux_pty_options o;
	memset(&o, 0, sizeof o);
	o.cwd = cwd;
	o.codepage = codepage;
	o.columns = columns;
	o.rows = rows;
	if (command == NULL || *command == '\0')
		return tmux_win32_error(error, ERROR_INVALID_PARAMETER,
		    "raw command line");
	return (pty_spawn_impl(out, &o, command, env, use_pipes, error));
}

void
tmux_win32_pty_end_input(struct tmux_pty *pty)
{
	EnterCriticalSection(&pty->lock);
	pty->input_eof = 1;
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
}

HANDLE
tmux_win32_pty_process(struct tmux_pty *pty)
{
	return (pty->process);
}

int
tmux_win32_pty_key_records(struct tmux_pty *pty)
{
	static int		 inbox = -1;
	OSVERSIONINFOEXW	 v;
	DWORDLONG		 mask = 0;

	if (pty->console == NULL || pty->provider == NULL)
		return (0);
	if (strcmp(pty->provider->name, "inbox") != 0)
		return (1);
	if (inbox == -1) {
		memset(&v, 0, sizeof v);
		v.dwOSVersionInfoSize = sizeof v;
		v.dwMajorVersion = 10;
		v.dwBuildNumber = 22000;
		VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
		VER_SET_CONDITION(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
		inbox = VerifyVersionInfoW(&v, VER_MAJORVERSION |
		    VER_BUILDNUMBER, mask) ? 1 : 0;
	}
	return (inbox);
}
uint64_t
tmux_win32_pty_foreground(struct tmux_pty *pty)
{
	JOBOBJECT_BASIC_PROCESS_ID_LIST *list;
	DWORD size = sizeof *list + 255 * sizeof(ULONG_PTR), i;
	FILETIME created, exited, kernel, user;
	ULONGLONG best = 0, when;
	uint64_t pid = 0;
	HANDLE h;
	if (pty == NULL || pty->job == NULL)
		return (0);
	if ((list = malloc(size)) == NULL)
		return (0);
	if (!QueryInformationJobObject(pty->job, JobObjectBasicProcessIdList,
	        list, size, NULL) &&
	    GetLastError() != ERROR_MORE_DATA) {
		free(list);
		return (0);
	}
	for (i = 0; i < list->NumberOfProcessIdsInList; i++) {
		h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
		    FALSE, (DWORD)list->ProcessIdList[i]);
		if (h == NULL)
			continue;
		if (GetProcessTimes(h, &created, &exited, &kernel, &user) &&
		    WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
			when = ((ULONGLONG)created.dwHighDateTime << 32) |
			    created.dwLowDateTime;
			if (when >= best) {
				best = when;
				pid = list->ProcessIdList[i];
			}
		}
		CloseHandle(h);
	}
	free(list);
	return (pid);
}

ptrdiff_t
tmux_pty_read(struct tmux_pty *pty, void *buffer, size_t capacity,
    struct tmux_os_error *error)
{
	size_t n;
	int eof;
	DWORD saved;
	if (capacity == 0)
		return (0);
	EnterCriticalSection(&pty->lock);
	n = queue_take(&pty->out, buffer, capacity);
	eof = pty->eof;
	saved = pty->read_error;
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
	if (n != 0)
		return (ptrdiff_t)n;
	if (eof) {
		if (saved != ERROR_SUCCESS)
			return (tmux_win32_error(error, saved, "read ConPTY"));
		if (error != NULL) {
			error->code = 0;
			error->message[0] = '\0';
		}
		return (-1);
	}
	return (0);
}

ptrdiff_t
tmux_pty_write(struct tmux_pty *pty, const void *buffer, size_t length,
    struct tmux_os_error *error)
{
	size_t n;
	DWORD saved;
	if (length == 0)
		return (0);
	EnterCriticalSection(&pty->lock);
	saved = pty->write_error;
	if ((pty->closing || pty->input_eof) && saved == ERROR_SUCCESS)
		saved = ERROR_BROKEN_PIPE;
	n = saved != ERROR_SUCCESS ? 0 : queue_put(&pty->in, buffer, length);
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
	if (saved != ERROR_SUCCESS)
		return (tmux_win32_error(error, saved, "write ConPTY"));
	return (ptrdiff_t)n;
}

ptrdiff_t
tmux_win32_pty_read_wait(struct tmux_pty *pty, void *buffer, size_t capacity)
{
	size_t n = 0;
	EnterCriticalSection(&pty->lock);
	while (!pty->aborted && pty->out.used == 0 && !pty->eof)
		SleepConditionVariableCS(&pty->condition, &pty->lock, INFINITE);
	if (!pty->aborted)
		n = queue_take(&pty->out, buffer, capacity);
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
	return (n == 0 ? -1 : (ptrdiff_t)n);
}

ptrdiff_t
tmux_win32_pty_write_wait(struct tmux_pty *pty, const void *buffer,
    size_t length)
{
	size_t n = 0;
	EnterCriticalSection(&pty->lock);
	while (!pty->aborted && !pty->closing && !pty->input_eof &&
	    pty->write_error == ERROR_SUCCESS &&
	    pty->in.used == pty->in.capacity)
		SleepConditionVariableCS(&pty->condition, &pty->lock, INFINITE);
	if (!pty->aborted && !pty->closing && !pty->input_eof &&
	    pty->write_error == ERROR_SUCCESS)
		n = queue_put(&pty->in, buffer, length);
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
	return (n == 0 ? -1 : (ptrdiff_t)n);
}

void
tmux_win32_pty_abort(struct tmux_pty *pty)
{
	EnterCriticalSection(&pty->lock);
	pty->aborted = 1;
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
}

int
tmux_pty_resize(struct tmux_pty *pty, unsigned short columns,
    unsigned short rows, struct tmux_os_error *error)
{
	COORD size;
	HRESULT hr;
	if (columns == 0 || rows == 0 || columns > 32767 || rows > 32767)
		return tmux_win32_error(error, ERROR_INVALID_PARAMETER,
		    "pty size");
	size.X = (SHORT)columns;
	size.Y = (SHORT)rows;
	AcquireSRWLockShared(&pty->console_lock);
	if (pty->console == NULL) {
		ReleaseSRWLockShared(&pty->console_lock);
		return tmux_win32_error(error, ERROR_BROKEN_PIPE,
		    "resize exited ConPTY");
	}
	hr = pty->provider->resize(pty->console, size);
	ReleaseSRWLockShared(&pty->console_lock);
	return FAILED(hr)
	    ? tmux_win32_error(error, (DWORD)hr, "ResizePseudoConsole")
	    : 0;
}

int
tmux_pty_poll(struct tmux_pty *pty, uint32_t *status,
    struct tmux_os_error *error)
{
	DWORD result = WaitForSingleObject(pty->process, 0), code;
	if (result == WAIT_TIMEOUT)
		return (0);
	if (result == WAIT_FAILED)
		return (tmux_win32_error(error, GetLastError(), "wait child"));
	if (!GetExitCodeProcess(pty->process, &code))
		return tmux_win32_error(error, GetLastError(),
		    "child exit status");
	if (status != NULL)
		*status = code;
	return (1);
}

uint64_t
tmux_pty_pid(const struct tmux_pty *pty)
{
	return (pty->pid);
}

void
tmux_pty_destroy(struct tmux_pty *pty)
{
	if (pty == NULL)
		return;
	EnterCriticalSection(&pty->lock);
	pty->closing = 1;
	WakeAllConditionVariable(&pty->condition);
	LeaveCriticalSection(&pty->lock);
	if (pty->job != NULL)
		TerminateJobObject(pty->job, 1);
	if (pty->writer != NULL) {
		CancelSynchronousIo(pty->writer);
		while (WaitForSingleObject(pty->writer, 10) == WAIT_TIMEOUT)
			CancelSynchronousIo(pty->writer);
		CloseHandle(pty->writer);
	}
	if (pty->input != NULL)
		CloseHandle(pty->input);
	if (pty->reaper != NULL) {
		WaitForSingleObject(pty->reaper, INFINITE);
		CloseHandle(pty->reaper);
	} else if (pty->console != NULL)
		pty->provider->close(pty->console);
	if (pty->reader != NULL) {
		CancelSynchronousIo(pty->reader);
		WaitForSingleObject(pty->reader, INFINITE);
		CloseHandle(pty->reader);
	}
	if (pty->output != NULL)
		CloseHandle(pty->output);
	if (pty->process != NULL) {
		WaitForSingleObject(pty->process, INFINITE);
		CloseHandle(pty->process);
	}
	if (pty->job != NULL)
		CloseHandle(pty->job);
	DeleteCriticalSection(&pty->lock);
	free(pty->in.data);
	free(pty->out.data);
	free(pty);
}
