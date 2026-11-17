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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"
#include "platform/tmux-platform.h"
#include <sddl.h>
#include <aclapi.h>
size_t strlcpy(char *, const char *, size_t);

#include <bcrypt.h>

static int
ipc_random(void *buffer, ULONG length)
{
	return (BCRYPT_SUCCESS(BCryptGenRandom(NULL, buffer, length,
	    BCRYPT_USE_SYSTEM_PREFERRED_RNG)));
}

#define AUTH_TOKEN_LEN 32
#define MAX_PENDING_NONCES 16
#define NONCE_EXPIRY_MS 30000
#define PIPE_WAIT_TIMEOUT_MS 5000

struct pending_nonce {
	char nonce[AUTH_TOKEN_LEN * 2 + 1];
	DWORD created;
	int used;
	PSID sid;
};

#define MAX_PEERS 256
static struct {
	int fd;
	PSID sid;
} peers[MAX_PEERS];

static char *pipe_users_sddl;

static struct pending_nonce pending_nonces[MAX_PENDING_NONCES];
static CRITICAL_SECTION nonce_lock;
static int nonce_lock_init;

static HANDLE discovery_pipe = INVALID_HANDLE_VALUE;
static HANDLE pipe_thread_handle;
static HANDLE pipe_shutdown_event;
static volatile int pipe_thread_running;
static char server_socket_path[MAX_PATH * 3];
static BY_HANDLE_FILE_INFORMATION server_socket_id;
static int server_socket_id_valid;

static int
pipe_io(HANDLE pipe, void *buffer, DWORD length, DWORD *transferred,
    int writing, DWORD timeout)
{
	OVERLAPPED operation;
	DWORD code, waited;
	BOOL ok;
	memset(&operation, 0, sizeof operation);
	operation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (operation.hEvent == NULL)
		return (-1);
	ok = writing ? WriteFile(pipe, buffer, length, transferred, &operation)
	             : ReadFile(pipe, buffer, length, transferred, &operation);
	if (!ok && GetLastError() == ERROR_IO_PENDING) {
		waited = WaitForSingleObject(operation.hEvent, timeout);
		if (waited != WAIT_OBJECT_0) {
			CancelIoEx(pipe, &operation);
			GetOverlappedResult(pipe, &operation, transferred,
			    TRUE);
			CloseHandle(operation.hEvent);
			return (-1);
		}
		ok = GetOverlappedResult(pipe, &operation, transferred, FALSE);
	}
	code = ok ? ERROR_SUCCESS : GetLastError();
	CloseHandle(operation.hEvent);
	SetLastError(code);
	return (ok ? 0 : -1);
}



static int
pipe_server_owns_socket(HANDLE pipe, const char *path)
{
	PSECURITY_DESCRIPTOR	 pipe_sd = NULL, file_sd = NULL;
	PSID			 pipe_owner, file_owner;
	wchar_t			 wide[MAX_PATH * 2];
	DWORD			 error;
	int			 ok = 0;

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
	        sizeof wide / sizeof wide[0]) == 0)
		return (0);
	if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
	        &pipe_owner, NULL, NULL, NULL, &pipe_sd) == ERROR_SUCCESS) {
		error = GetNamedSecurityInfoW(wide, SE_FILE_OBJECT,
		    OWNER_SECURITY_INFORMATION, &file_owner, NULL, NULL, NULL,
		    &file_sd);
		if (error == ERROR_SUCCESS)
			ok = EqualSid(pipe_owner, file_owner);
		else if (error == ERROR_FILE_NOT_FOUND ||
		    error == ERROR_PATH_NOT_FOUND)
			ok = -1;
	}
	if (pipe_sd != NULL)
		LocalFree(pipe_sd);
	if (file_sd != NULL)
		LocalFree(file_sd);
	return (ok);
}
static int
send_all(SOCKET socket, const char *bytes, size_t length)
{
	int count;
	while (length != 0) {
		count = send(socket, bytes, (int)length, 0);
		if (count <= 0)
			return (-1);
		bytes += count;
		length -= (size_t)count;
	}
	return (0);
}

static const char *
get_pipe_name(const char *path)
{
	static char	 name[64];
	wchar_t		 wide[MAX_PATH * 2], full[MAX_PATH * 2], *p;
	uint64_t	 hash = 0xcbf29ce484222325ULL;
	DWORD		 n;

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
	        sizeof wide / sizeof wide[0]) == 0)
		wide[0] = L'\0';
	n = GetFullPathNameW(wide, sizeof full / sizeof full[0], full, NULL);
	if (n == 0 || n >= sizeof full / sizeof full[0])
		wcscpy_s(full, sizeof full / sizeof full[0], wide);
	for (p = full; *p != L'\0'; p++) {
		if (*p == L'/')
			*p = L'\\';
	}
	CharLowerW(full);
	for (p = full; *p != L'\0'; p++) {
		hash ^= (uint16_t)*p;
		hash *= 0x100000001b3ULL;
	}
	snprintf(name, sizeof name, "\\\\.\\pipe\\tmux-%016llx",
	    (unsigned long long)hash);
	return (name);
}

static const wchar_t *
pipe_name_wide(const char *name)
{
	static wchar_t wide[256];

	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 256) == 0)
		wide[0] = L'\0';
	return (wide);
}

static char *
ipc_user_sid_string(PSID *copy)
{
	HANDLE		 token;
	DWORD		 len = 0;
	TOKEN_USER	*tu;
	char		*s = NULL;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return (NULL);
	GetTokenInformation(token, TokenUser, NULL, 0, &len);
	if ((tu = malloc(len)) != NULL &&
	    GetTokenInformation(token, TokenUser, tu, len, &len) &&
	    ConvertSidToStringSidA(tu->User.Sid, &s) && copy != NULL &&
	    (*copy = malloc(GetLengthSid(tu->User.Sid))) != NULL)
		CopySid(GetLengthSid(tu->User.Sid), *copy, tu->User.Sid);
	free(tu);
	CloseHandle(token);
	return (s);
}

static PSECURITY_DESCRIPTOR
ipc_security(void)
{
	char			*sid_str, sddl[4096];
	PSECURITY_DESCRIPTOR	 sd = NULL;

	if ((sid_str = ipc_user_sid_string(NULL)) == NULL)
		return (NULL);

	snprintf(sddl, sizeof sddl, "O:%sD:P(A;;GA;;;%s)%s", sid_str, sid_str,
	    pipe_users_sddl == NULL ? "" : pipe_users_sddl);
	LocalFree(sid_str);
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,
	        SDDL_REVISION_1, &sd, NULL))
		return (NULL);
	return (sd);
}

static int
ipc_secure_socket(const char *path)
{
	PSECURITY_DESCRIPTOR	 sd;
	PSID			 owner;
	PACL			 dacl;
	BOOL			 present, defaulted;
	wchar_t			 wide[MAX_PATH * 2];
	DWORD			 error = ERROR_INVALID_PARAMETER;

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
	        sizeof wide / sizeof wide[0]) == 0 ||
	    (sd = ipc_security()) == NULL)
		return (-1);
	if (GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
	    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted))
		error = SetNamedSecurityInfoW(wide, SE_FILE_OBJECT,
		    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
		        PROTECTED_DACL_SECURITY_INFORMATION,
		    owner, NULL, dacl, NULL);
	LocalFree(sd);
	return (error == ERROR_SUCCESS ? 0 : -1);
}

static int
ipc_file_id(const char *path, BY_HANDLE_FILE_INFORMATION *id)
{
	wchar_t	wide[MAX_PATH * 2];
	HANDLE	h;
	int	ok;

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
	        sizeof wide / sizeof wide[0]) == 0)
		return (0);
	h = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ |
	    FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
	    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return (0);
	ok = GetFileInformationByHandle(h, id);
	CloseHandle(h);
	return (ok);
}

static HANDLE
create_discovery_pipe(const char *pipe_name)
{
	HANDLE			 h;
	SECURITY_ATTRIBUTES	 sa;
	PSECURITY_DESCRIPTOR	 sd;

	if ((sd = ipc_security()) == NULL)
		return (INVALID_HANDLE_VALUE);
	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = FALSE;

	h = CreateNamedPipeW(pipe_name_wide(pipe_name),
	    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
	        FILE_FLAG_FIRST_PIPE_INSTANCE | WRITE_DAC,
	    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
	        PIPE_REJECT_REMOTE_CLIENTS,
	    1,
	    256, 256,
	    0, &sa);

	LocalFree(sd);
	return (h);
}

static int
add_pending_nonce(char *nonce_out, PSID sid)
{
	unsigned char random_bytes[AUTH_TOKEN_LEN];
	int i;
	DWORD now;

	if (!ipc_random(random_bytes, sizeof random_bytes))
		return (-1);

	for (i = 0; i < AUTH_TOKEN_LEN; i++)
		snprintf(nonce_out + i * 2, 3, "%02x", random_bytes[i]);
	nonce_out[AUTH_TOKEN_LEN * 2] = '\0';

	EnterCriticalSection(&nonce_lock);
	now = GetTickCount();

	for (i = 0; i < MAX_PENDING_NONCES; i++) {
		if (pending_nonces[i].used &&
		    (now - pending_nonces[i].created) > NONCE_EXPIRY_MS)
			pending_nonces[i].used = 0;
	}

	for (i = 0; i < MAX_PENDING_NONCES; i++) {
		if (!pending_nonces[i].used) {
			strlcpy(pending_nonces[i].nonce, nonce_out,
			    sizeof pending_nonces[i].nonce);
			pending_nonces[i].created = now;
			pending_nonces[i].used = 1;
			free(pending_nonces[i].sid);
			pending_nonces[i].sid = sid;
			LeaveCriticalSection(&nonce_lock);
			return (0);
		}
	}

	LeaveCriticalSection(&nonce_lock);
	return (-1);
}

static int
consume_pending_nonce(const char *nonce, PSID *sid)
{
	int i;
	DWORD now;

	EnterCriticalSection(&nonce_lock);
	now = GetTickCount();

	for (i = 0; i < MAX_PENDING_NONCES; i++) {
		if (!pending_nonces[i].used)
			continue;
		if ((now - pending_nonces[i].created) > NONCE_EXPIRY_MS) {
			pending_nonces[i].used = 0;
			continue;
		}
		if (strcmp(pending_nonces[i].nonce, nonce) == 0) {
			pending_nonces[i].used = 0;
			*sid = pending_nonces[i].sid;
			pending_nonces[i].sid = NULL;
			LeaveCriticalSection(&nonce_lock);
			return (0);
		}
	}

	LeaveCriticalSection(&nonce_lock);
	return (-1);
}

static PSID
pipe_client_sid(HANDLE pipe)
{
	HANDLE token;
	TOKEN_USER *user = NULL;
	DWORD len = 0;
	PSID sid = NULL;
	if (!ImpersonateNamedPipeClient(pipe))
		return (NULL);
	if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
		GetTokenInformation(token, TokenUser, NULL, 0, &len);
		if ((user = malloc(len)) != NULL &&
		    GetTokenInformation(token, TokenUser, user, len, &len) &&
		    (sid = malloc(GetLengthSid(user->User.Sid))) != NULL)
			CopySid(GetLengthSid(user->User.Sid), sid,
			    user->User.Sid);
		free(user);
		CloseHandle(token);
	}
	RevertToSelf();
	return (sid);
}

static DWORD WINAPI
pipe_accept_thread(LPVOID arg)
{
	OVERLAPPED ov;
	HANDLE events[2];
	char nonce[AUTH_TOKEN_LEN * 2 + 1];
	char response[128];
	DWORD wait_result, bytes;
	PSID sid;

	(void)arg;

	memset(&ov, 0, sizeof ov);
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == NULL)
		return (1);

	events[0] = ov.hEvent;
	events[1] = pipe_shutdown_event;

	while (pipe_thread_running) {
		ResetEvent(ov.hEvent);

		if (ConnectNamedPipe(discovery_pipe, &ov)) {
		} else {
			DWORD err = GetLastError();
			if (err == ERROR_IO_PENDING) {
				wait_result = WaitForMultipleObjects(2, events,
				    FALSE, INFINITE);
				if (wait_result != WAIT_OBJECT_0) {
					CancelIo(discovery_pipe);
					GetOverlappedResult(discovery_pipe, &ov,
					    &bytes, TRUE);
					break;
				}
			} else if (err == ERROR_NO_DATA) {
				DisconnectNamedPipe(discovery_pipe);
				continue;
			} else if (err != ERROR_PIPE_CONNECTED) {
				break;
			}
		}

		if (!pipe_thread_running) {
			DisconnectNamedPipe(discovery_pipe);
			break;
		}

		sid = NULL;
		{
			DWORD received;
			char hello;
			if (pipe_io(discovery_pipe, &hello, 1, &received, 0,
			        PIPE_WAIT_TIMEOUT_MS) == 0 &&
			    received == 1)
				sid = pipe_client_sid(discovery_pipe);
		}

		if (sid != NULL && add_pending_nonce(nonce, sid) == 0) {
			DWORD written, received;
			char acknowledgement;
			snprintf(response, sizeof response, "%s\n", nonce);
			if (pipe_io(discovery_pipe, response,
			        (DWORD)strlen(response), &written, 1,
			        PIPE_WAIT_TIMEOUT_MS) == 0 &&
			    written == strlen(response))
				pipe_io(discovery_pipe, &acknowledgement, 1,
				    &received, 0, PIPE_WAIT_TIMEOUT_MS);
		}

		DisconnectNamedPipe(discovery_pipe);
	}

	CloseHandle(ov.hEvent);
	return (0);
}

static SOCKET
pipe_discover_and_connect(const char *path, char *nonce_out, size_t nonce_size)
{
	const char		*pipe_name;
	HANDLE			 pipe;
	char			 buf[128];
	DWORD			 nread;
	ULONGLONG		 deadline = GetTickCount64() + PIPE_WAIT_TIMEOUT_MS;
	char			 decoded_nonce[65];
	int			 decoded;
	SOCKET			 s;
	struct sockaddr_un	 sa;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, path, sizeof sa.sun_path) >=
	    sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (INVALID_SOCKET);
	}
	errno = ECONNREFUSED;
	pipe_name = get_pipe_name(path);

	for (;;) {
		pipe = CreateFileW(pipe_name_wide(pipe_name),
		    GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
		        SECURITY_IDENTIFICATION, NULL);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetLastError() == ERROR_ACCESS_DENIED) {
			errno = EACCES;
			return (INVALID_SOCKET);
		}
		if (GetLastError() != ERROR_PIPE_BUSY)
			return (INVALID_SOCKET);
		{
			ULONGLONG now = GetTickCount64();
			if (now >= deadline ||
			    !WaitNamedPipeW(pipe_name_wide(pipe_name),
			        (DWORD)(deadline - now)))
				return (INVALID_SOCKET);
		}
	}

	switch (pipe_server_owns_socket(pipe, path)) {
	case 1:
		break;
	case -1:
		CloseHandle(pipe);
		errno = ECONNREFUSED;
		return (INVALID_SOCKET);
	default:
		CloseHandle(pipe);
		errno = EACCES;
		return (INVALID_SOCKET);
	}
	{
		DWORD written;
		char hello = 'H';
		ULONGLONG now = GetTickCount64();
		if (now >= deadline ||
		    pipe_io(pipe, &hello, 1, &written, 1,
		        (DWORD)(deadline - now)) != 0 ||
		    written != 1) {
			CloseHandle(pipe);
			return (INVALID_SOCKET);
		}
	}
	nread = 0;
	for (;;) {
		DWORD received;
		ULONGLONG now = GetTickCount64();
		if (now >= deadline || nread == sizeof buf) {
			CloseHandle(pipe);
			return (INVALID_SOCKET);
		}
		if (pipe_io(pipe, buf + nread, sizeof buf - nread, &received, 0,
		        (DWORD)(deadline - now)) != 0 ||
		    received == 0) {
			CloseHandle(pipe);
			return (INVALID_SOCKET);
		}
		nread += received;
		decoded = tmux_discovery_decode(buf, nread, decoded_nonce);
		if (decoded < 0) {
			CloseHandle(pipe);
			return (INVALID_SOCKET);
		}
		if (decoded == 1)
			break;
	}
	{
		DWORD acknowledged;
		char ack = 'K';
		ULONGLONG now = GetTickCount64();
		if (now >= deadline ||
		    pipe_io(pipe, &ack, 1, &acknowledged, 1,
		        (DWORD)(deadline - now)) != 0 ||
		    acknowledged != 1 || nonce_size < sizeof decoded_nonce) {
			CloseHandle(pipe);
			return (INVALID_SOCKET);
		}
	}
	CloseHandle(pipe);
	memcpy(nonce_out, decoded_nonce, sizeof decoded_nonce);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET) {
		errno = EAFNOSUPPORT;
		return (INVALID_SOCKET);
	}
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
		errno = WSAGetLastError() == WSAEACCES ? EACCES : ECONNREFUSED;
		closesocket(s);
		return (INVALID_SOCKET);
	}
	return (s);
}

int
win32_ipc_create_server(const char *path)
{
	SOCKET			 s;
	struct sockaddr_un	 sa;
	int			 i;

	win32_wsa_init();
	if (!nonce_lock_init) {
		InitializeCriticalSection(&nonce_lock);
		memset(pending_nonces, 0, sizeof pending_nonces);
		nonce_lock_init = 1;
	}

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, path, sizeof sa.sun_path) >=
	    sizeof sa.sun_path) {
		errno = ENAMETOOLONG;
		return (-1);
	}

	if (discovery_pipe == INVALID_HANDLE_VALUE) {
		for (i = 0; i < 40; i++) {
			discovery_pipe = create_discovery_pipe(
			    get_pipe_name(path));
			if (discovery_pipe != INVALID_HANDLE_VALUE ||
			    (GetLastError() != ERROR_ACCESS_DENIED &&
			    GetLastError() != ERROR_PIPE_BUSY))
				break;
			Sleep(50);
		}
		if (discovery_pipe == INVALID_HANDLE_VALUE) {
			errno = EADDRINUSE;
			return (-1);
		}
	}

	unlink(path);
	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == INVALID_SOCKET) {
		errno = EAFNOSUPPORT;
		return (-1);
	}
	if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
		errno = WSAGetLastError() == WSAEACCES ? EACCES :
		    GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES &&
		    GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EADDRINUSE;
		closesocket(s);
		return (-1);
	}
	if (ipc_secure_socket(path) != 0 || listen(s, 128) != 0) {
		errno = EACCES;
		closesocket(s);
		return (-1);
	}
	strlcpy(server_socket_path, path, sizeof server_socket_path);
	server_socket_id_valid = ipc_file_id(path, &server_socket_id);

	if (pipe_thread_handle != NULL)
		return ((int)s);
	pipe_shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	pipe_thread_running = 1;
	pipe_thread_handle =
	    CreateThread(NULL, 0, pipe_accept_thread, NULL, 0, NULL);
	if (pipe_thread_handle == NULL) {
		if (pipe_shutdown_event != NULL) {
			CloseHandle(pipe_shutdown_event);
			pipe_shutdown_event = NULL;
		}
		closesocket(s);
		errno = ENOMEM;
		return (-1);
	}
	return ((int)s);
}

int
win32_ipc_server_alive(const char *path)
{
	if (WaitNamedPipeW(pipe_name_wide(get_pipe_name(path)), 1))
		return (1);
	return (GetLastError() != ERROR_FILE_NOT_FOUND);
}
int
win32_mkdir_private(const char *path)
{
	char			*sid, sddl[256];
	wchar_t			 wide[MAX_PATH * 2];
	SECURITY_ATTRIBUTES	 sa;
	PSECURITY_DESCRIPTOR	 sd;
	DWORD			 error;

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
	        sizeof wide / sizeof wide[0]) == 0 ||
	    (sid = ipc_user_sid_string(NULL)) == NULL) {
		errno = EINVAL;
		return (-1);
	}
	snprintf(sddl, sizeof sddl, "O:%sD:P(A;OICI;FA;;;%s)", sid, sid);
	LocalFree(sid);
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl,
	        SDDL_REVISION_1, &sd, NULL)) {
		errno = EINVAL;
		return (-1);
	}
	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = sd;
	error = CreateDirectoryW(wide, &sa) ? 0 : GetLastError();
	LocalFree(sd);
	if (error == 0)
		return (0);
	errno = error == ERROR_ALREADY_EXISTS ? EEXIST :
	    error == ERROR_ACCESS_DENIED ? EACCES :
	    error == ERROR_PATH_NOT_FOUND ? ENOENT : EINVAL;
	return (-1);
}

int
win32_owned_by_user(const char *path)
{
	wchar_t			 wide[MAX_PATH * 2];
	PSECURITY_DESCRIPTOR	 sd = NULL;
	PSID			 owner, self = NULL;
	char			*sid;
	DWORD			 attributes;
	int			 ok = 0;

	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
	        sizeof wide / sizeof wide[0]) == 0)
		return (0);
	attributes = GetFileAttributesW(wide);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		return (0);
	if ((sid = ipc_user_sid_string(&self)) == NULL)
		return (0);
	LocalFree(sid);
	if (self != NULL &&
	    GetNamedSecurityInfoW(wide, SE_FILE_OBJECT,
	        OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) ==
	        ERROR_SUCCESS)
		ok = EqualSid(owner, self);
	if (sd != NULL)
		LocalFree(sd);
	free(self);
	return (ok);
}

int
win32_ipc_connect(const char *path)
{
	SOCKET	s;
	char	nonce[AUTH_TOKEN_LEN * 2 + 1], line[AUTH_TOKEN_LEN * 2 + 2];

	win32_wsa_init();
	s = pipe_discover_and_connect(path, nonce, sizeof nonce);
	if (s == INVALID_SOCKET)
		return (-1);
	snprintf(line, sizeof line, "%s\n", nonce);
	if (send_all(s, line, strlen(line)) != 0) {
		closesocket(s);
		errno = ECONNREFUSED;
		return (-1);
	}
	return ((int)s);
}

void
win32_generate_tty_token(char *buf, size_t bufsize)
{
	unsigned char random_bytes[AUTH_TOKEN_LEN];
	int i;

	if (bufsize < AUTH_TOKEN_LEN * 2 + 1) {
		buf[0] = '\0';
		return;
	}

	if (!ipc_random(random_bytes, sizeof random_bytes)) {
		buf[0] = '\0';
		return;
	}

	for (i = 0; i < AUTH_TOKEN_LEN; i++)
		snprintf(buf + i * 2, 3, "%02x", random_bytes[i]);
}

int
win32_ipc_verify_auth(int fd, const char *label, char *tty_token_out,
    size_t tty_token_size)
{
	char buf[256];
	int n, i, is_tty = 0;
	char ch = 0;
	ULONGLONG deadline = GetTickCount64() + 500;
	char *nonce_part;
	PSID sid = NULL;
	int free_slot;

	(void)label;

	{
		u_long zero = 0;
		ioctlsocket((SOCKET)fd, FIONBIO, &zero);
	}

	for (i = 0; i < (int)(sizeof buf - 1); i++) {
		fd_set ready;
		struct timeval remaining;
		ULONGLONG now = GetTickCount64();
		if (now >= deadline)
			return (-1);
		remaining.tv_sec = 0;
		remaining.tv_usec = (long)((deadline - now) * 1000);
		FD_ZERO(&ready);
		FD_SET((SOCKET)fd, &ready);
		if (select(0, &ready, NULL, NULL, &remaining) <= 0)
			return (-1);
		n = recv((SOCKET)fd, &ch, 1, 0);
		if (n != 1)
			break;
		if (ch == '\n')
			break;
		buf[i] = ch;
	}
	buf[i] = '\0';
	if (ch != '\n')
		return (-1);

	if (strncmp(buf, "TTY:", 4) == 0) {
		char *colon = strchr(buf + 4, ':');
		if (colon == NULL)
			return (-1);
		*colon = '\0';
		if (tty_token_out != NULL && tty_token_size > 0) {
			strncpy(tty_token_out, buf + 4, tty_token_size - 1);
			tty_token_out[tty_token_size - 1] = '\0';
		}
		nonce_part = colon + 1;
		is_tty = 1;
	} else {
		nonce_part = buf;
	}

	if (strlen(nonce_part) != AUTH_TOKEN_LEN * 2 ||
	    consume_pending_nonce(nonce_part, &sid) != 0)
		return (-1);

	free_slot = -1;
	for (i = 0; i < MAX_PEERS; i++) {
		if (peers[i].sid != NULL && peers[i].fd == fd) {
			free(peers[i].sid);
			peers[i].sid = NULL;
		}
		if (peers[i].sid == NULL && free_slot == -1)
			free_slot = i;
	}
	if (free_slot != -1) {
		peers[free_slot].fd = fd;
		peers[free_slot].sid = sid;
	} else
		free(sid);

	return (is_tty ? 1 : 0);
}

int
win32_ipc_connect_tty(const char *path, const char *tty_token)
{
	SOCKET	 s;
	char	 nonce[AUTH_TOKEN_LEN * 2 + 1];
	char	*line;
	size_t	 len;

	if (win32_remote_native_active())
		return (win32_remote_native_tty(tty_token));

	win32_wsa_init();
	s = pipe_discover_and_connect(path, nonce, sizeof nonce);
	if (s == INVALID_SOCKET)
		return (-1);
	len = 4 + strlen(tty_token) + 1 + strlen(nonce) + 1;
	if ((line = malloc(len + 1)) == NULL) {
		closesocket(s);
		return (-1);
	}
	snprintf(line, len + 1, "TTY:%s:%s\n", tty_token, nonce);
	if (send_all(s, line, len) != 0) {
		free(line);
		closesocket(s);
		return (-1);
	}
	free(line);
	return ((int)s);
}

void
win32_ipc_cleanup(const char *path)
{
	BY_HANDLE_FILE_INFORMATION	 id;

	(void)path;

	pipe_thread_running = 0;
	if (pipe_shutdown_event != NULL)
		SetEvent(pipe_shutdown_event);

	if (pipe_thread_handle != NULL) {
		if (discovery_pipe != INVALID_HANDLE_VALUE)
			CancelIoEx(discovery_pipe, NULL);
		while (WaitForSingleObject(pipe_thread_handle, 10) ==
		    WAIT_TIMEOUT) {
			CancelSynchronousIo(pipe_thread_handle);
			if (discovery_pipe != INVALID_HANDLE_VALUE)
				CancelIoEx(discovery_pipe, NULL);
		}
		CloseHandle(pipe_thread_handle);
		pipe_thread_handle = NULL;
	}

	if (discovery_pipe != INVALID_HANDLE_VALUE) {
		CloseHandle(discovery_pipe);
		discovery_pipe = INVALID_HANDLE_VALUE;
	}

	if (pipe_shutdown_event != NULL) {
		CloseHandle(pipe_shutdown_event);
		pipe_shutdown_event = NULL;
	}

	if (server_socket_id_valid &&
	    ipc_file_id(server_socket_path, &id) &&
	    id.dwVolumeSerialNumber == server_socket_id.dwVolumeSerialNumber &&
	    id.nFileIndexHigh == server_socket_id.nFileIndexHigh &&
	    id.nFileIndexLow == server_socket_id.nFileIndexLow)
		unlink(server_socket_path);
	server_socket_id_valid = 0;
}

void *
win32_ipc_peer_sid(int fd)
{
	int i;

	for (i = 0; i < MAX_PEERS; i++) {
		if (peers[i].sid != NULL && peers[i].fd == fd)
			return (peers[i].sid);
	}
	return (NULL);
}

void
win32_ipc_set_users(const uid_t *uids, u_int n)
{
	char			*sddl = NULL, *sid_str, *tmp;
	PSECURITY_DESCRIPTOR	 sd;
	PACL			 dacl;
	BOOL			 present, defaulted;
	PSID			 sid;
	size_t			 len;
	u_int			 i;

	for (i = 0; i < n; i++) {
		if ((sid = win32_uid_sid(uids[i])) == NULL ||
		    !ConvertSidToStringSidA(sid, &sid_str))
			continue;
		len = (sddl == NULL ? 0 : strlen(sddl)) + strlen(sid_str) + 16;
		if ((tmp = malloc(len)) != NULL) {
			snprintf(tmp, len, "%s(A;;GRGW;;;%s)",
			    sddl == NULL ? "" : sddl, sid_str);
			free(sddl);
			sddl = tmp;
		}
		LocalFree(sid_str);
	}
	free(pipe_users_sddl);
	pipe_users_sddl = sddl;

	if (server_socket_path[0] != '\0')
		ipc_secure_socket(server_socket_path);
	if (discovery_pipe == INVALID_HANDLE_VALUE ||
	    (sd = ipc_security()) == NULL)
		return;
	if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) &&
	    present)
		SetSecurityInfo(discovery_pipe, SE_KERNEL_OBJECT,
		    DACL_SECURITY_INFORMATION |
		        PROTECTED_DACL_SECURITY_INFORMATION,
		    NULL, NULL, dacl, NULL);
	LocalFree(sd);
}
