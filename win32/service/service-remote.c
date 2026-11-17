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

#include <ws2tcpip.h>
#include <ntsecapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux-remote.h"
#include "win32-tls.h"
#include "service-internal.h"

#define PARAMETERS_KEY	L"SYSTEM\\CurrentControlSet\\Services\\wtmux-service" \
			L"\\Parameters"
#define LOGIN_GRACE_MS	120000
#define MAX_CONNS	64
#define MAX_PER_ADDR	8
#define MAX_ATTEMPTS	3

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0)
#endif

struct config {
	int		enabled;
	char		address[128];
	DWORD		port;
	int		windows;
	int		keys;
	int		no_ntlm;
	DWORD		grace;
};

struct conn {
	SOCKET		s;
	char		addr[INET6_ADDRSTRLEN];
};

static CRITICAL_SECTION		 remote_lock;
static int			 remote_lock_init;
static struct config		 config;
static PCCERT_CONTEXT		 host_cert;
static SOCKET			 listener = INVALID_SOCKET;
static HANDLE			 listener_thread;
static struct tmux_remote_limit	 limits;
static int			 conns_total;
static struct {
	char	addr[INET6_ADDRSTRLEN];
	int	n;
}				 conns_addr[MAX_CONNS];

static void
lock_init(void)
{
	if (!remote_lock_init) {
		InitializeCriticalSection(&remote_lock);
		remote_lock_init = 1;
	}
}

static void
config_read(struct config *c)
{
	HKEY	key;
	DWORD	n, v, type;
	wchar_t	w[128];

	memset(c, 0, sizeof *c);
	c->port = TMUX_REMOTE_PORT;
	c->windows = c->keys = 1;
	c->grace = LOGIN_GRACE_MS;
	snprintf(c->address, sizeof c->address, "127.0.0.1");
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY, 0, KEY_READ,
	    &key) != ERROR_SUCCESS)
		return;
	n = sizeof v;
	if (RegQueryValueExW(key, L"Listen", NULL, &type, (BYTE *)&v,
	    &n) == ERROR_SUCCESS && type == REG_DWORD)
		c->enabled = (v != 0);
	n = sizeof v;
	if (RegQueryValueExW(key, L"Port", NULL, &type, (BYTE *)&v,
	    &n) == ERROR_SUCCESS && type == REG_DWORD && v > 0 && v < 65536)
		c->port = v;
	n = sizeof v;
	if (RegQueryValueExW(key, L"NoNtlm", NULL, &type, (BYTE *)&v,
	    &n) == ERROR_SUCCESS && type == REG_DWORD)
		c->no_ntlm = (v != 0);
	n = sizeof v;
	if (RegQueryValueExW(key, L"LoginGraceSeconds", NULL, &type,
	    (BYTE *)&v, &n) == ERROR_SUCCESS && type == REG_DWORD && v > 0 &&
	    v <= 600)
		c->grace = v * 1000;
	n = sizeof w - sizeof w[0];
	if (RegQueryValueExW(key, L"Address", NULL, &type, (BYTE *)w,
	    &n) == ERROR_SUCCESS && type == REG_SZ) {
		w[n / sizeof w[0]] = L'\0';
		WideCharToMultiByte(CP_UTF8, 0, w, -1, c->address,
		    sizeof c->address, NULL, NULL);
	}
	n = sizeof w - sizeof w[0];
	if (RegQueryValueExW(key, L"Auth", NULL, &type, (BYTE *)w,
	    &n) == ERROR_SUCCESS && type == REG_SZ) {
		w[n / sizeof w[0]] = L'\0';
		c->windows = (wcscmp(w, L"keys") != 0);
		c->keys = (wcscmp(w, L"windows") != 0);
	}
	RegCloseKey(key);
}

static int
is_loopback(const char *address)
{
	return (strcmp(address, "127.0.0.1") == 0 ||
	    strcmp(address, "::1") == 0 || strcmp(address, "localhost") == 0);
}

static int
read_line(struct win32_tls *t, char *line, size_t size)
{
	unsigned char	*buf;
	size_t		 n;
	int		 channel, rc = -1;

	buf = xcalloc(1, TMUX_REMOTE_FRAME_MAX + 1);
	if (win32_tls_frame_read(t, &channel, buf, &n) == 0 &&
	    channel == TMUX_REMOTE_CONTROL && n < size) {
		memcpy(line, buf, n + 1);
		rc = 0;
	}
	free(buf);
	return (rc);
}

static const unsigned char *
binding_data(SecPkgContext_Bindings *b, size_t *n)
{
	SEC_CHANNEL_BINDINGS	*cb = b->Bindings;

	*n = cb->cbApplicationDataLength;
	return ((const unsigned char *)cb + cb->dwApplicationDataOffset);
}

static void
logon_reason(char *why, size_t whylen, SECURITY_STATUS st)
{
	if (st == SEC_E_LOGON_DENIED)
		snprintf(why, whylen, "logon refused: wrong user name or "
		    "password, or the account may not log on from the "
		    "network");
	else
		snprintf(why, whylen, "logon refused (0x%08lx)",
		    (unsigned long)st);
}

static HANDLE
auth_windows(struct win32_tls *t, char *pkg, size_t pkglen, char *why,
    size_t whylen)
{
	CredHandle			 cred;
	CtxtHandle			 ctx;
	SecPkgContext_Bindings		 b;
	SecPkgContext_NegotiationInfoW	 ni;
	SecBuffer			 inb[2], outb[1], mb[2];
	SecBufferDesc			 ind, outd, md;
	SECURITY_STATUS			 st;
	HANDLE				 token = NULL;
	const unsigned char		*bind;
	unsigned char			*in = NULL, *mic = NULL, *data = NULL;
	char				 line[32768], *b64;
	size_t				 inlen, miclen, blen;
	ULONG				 attrs;
	int				 first = 1, have_ctx = 0;

	snprintf(why, whylen, "logon failed");
	if (AcquireCredentialsHandleW(NULL, L"Negotiate", SECPKG_CRED_INBOUND,
	    NULL, NULL, NULL, NULL, &cred, NULL) != SEC_E_OK)
		return (NULL);
	if (win32_tls_binding(t, &b) != 0) {
		FreeCredentialsHandle(&cred);
		return (NULL);
	}
	for (;;) {
		if (read_line(t, line, sizeof line) != 0 ||
		    strncmp(line, "token ", 6) != 0 ||
		    (in = win32_tls_unbase64(line + 6, &inlen)) == NULL)
			goto out;
		inb[0].BufferType = SECBUFFER_TOKEN;
		inb[0].pvBuffer = in;
		inb[0].cbBuffer = (unsigned long)inlen;
		inb[1].BufferType = SECBUFFER_CHANNEL_BINDINGS;
		inb[1].pvBuffer = b.Bindings;
		inb[1].cbBuffer = b.BindingsLength;
		ind.ulVersion = SECBUFFER_VERSION;
		ind.cBuffers = 2;
		ind.pBuffers = inb;
		outb[0].BufferType = SECBUFFER_TOKEN;
		outb[0].pvBuffer = NULL;
		outb[0].cbBuffer = 0;
		outd.ulVersion = SECBUFFER_VERSION;
		outd.cBuffers = 1;
		outd.pBuffers = outb;
		st = AcceptSecurityContext(&cred, first ? NULL : &ctx, &ind,
		    ASC_REQ_INTEGRITY|ASC_REQ_CONNECTION|ASC_REQ_ALLOCATE_MEMORY,
		    SECURITY_NATIVE_DREP, &ctx, &outd, &attrs, NULL);
		free(in);
		in = NULL;
		if (st == SEC_I_COMPLETE_NEEDED ||
		    st == SEC_I_COMPLETE_AND_CONTINUE)
			CompleteAuthToken(&ctx, &outd);
		if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED &&
		    st != SEC_I_COMPLETE_NEEDED &&
		    st != SEC_I_COMPLETE_AND_CONTINUE) {
			if (outb[0].pvBuffer != NULL)
				FreeContextBuffer(outb[0].pvBuffer);
			logon_reason(why, whylen, st);
			goto out;
		}
		first = 0;
		have_ctx = 1;
		if (st == SEC_E_OK || st == SEC_I_COMPLETE_NEEDED) {
			if (outb[0].pvBuffer != NULL)
				FreeContextBuffer(outb[0].pvBuffer);
			break;
		}
		b64 = win32_tls_base64(outb[0].pvBuffer, outb[0].cbBuffer);
		FreeContextBuffer(outb[0].pvBuffer);
		if (b64 == NULL || win32_tls_line(t, "token %s", b64) != 0) {
			free(b64);
			goto out;
		}
		free(b64);
	}

	memset(&ni, 0, sizeof ni);
	if (QueryContextAttributesW(&ctx, SECPKG_ATTR_NEGOTIATION_INFO,
	    &ni) == SEC_E_OK && ni.PackageInfo != NULL) {
		WideCharToMultiByte(CP_UTF8, 0, ni.PackageInfo->Name, -1, pkg,
		    (int)pkglen, NULL, NULL);
		FreeContextBuffer(ni.PackageInfo);
	} else
		snprintf(pkg, pkglen, "Negotiate");
	if (config.no_ntlm && _stricmp(pkg, "NTLM") == 0) {
		snprintf(why, whylen, "NTLM is not allowed here");
		goto out;
	}

	if (read_line(t, line, sizeof line) != 0 ||
	    strncmp(line, "mic ", 4) != 0 ||
	    (mic = win32_tls_unbase64(line + 4, &miclen)) == NULL) {
		snprintf(why, whylen, "no channel signature");
		goto out;
	}
	bind = binding_data(&b, &blen);
	if ((data = malloc(17 + blen)) == NULL)
		goto out;
	memcpy(data, "tmux-remote-cb-v1", 17);
	memcpy(data + 17, bind, blen);
	mb[0].BufferType = SECBUFFER_DATA;
	mb[0].pvBuffer = data;
	mb[0].cbBuffer = (unsigned long)(17 + blen);
	mb[1].BufferType = SECBUFFER_TOKEN;
	mb[1].pvBuffer = mic;
	mb[1].cbBuffer = (unsigned long)miclen;
	md.ulVersion = SECBUFFER_VERSION;
	md.cBuffers = 2;
	md.pBuffers = mb;
	if (VerifySignature(&ctx, &md, 0, NULL) != SEC_E_OK) {
		snprintf(why, whylen, "the logon is not for this channel");
		goto out;
	}
	if (QuerySecurityContextToken(&ctx, &token) != SEC_E_OK)
		token = NULL;
out:
	free(in);
	free(mic);
	free(data);
	FreeContextBuffer(b.Bindings);
	if (have_ctx)
		DeleteSecurityContext(&ctx);
	FreeCredentialsHandle(&cred);
	return (token);
}

static int
strict_path(const wchar_t *path, PSID user)
{
	PSECURITY_DESCRIPTOR	 sd;
	PSID			 owner;
	PACL			 dacl;
	ACE_HEADER		*ace;
	ACCESS_ALLOWED_ACE	*allow;
	PSID			 who;
	DWORD			 i, mask;
	int			 ok = 1;

	mask = FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|
	    FILE_DELETE_CHILD|DELETE|WRITE_DAC|WRITE_OWNER|GENERIC_WRITE|
	    GENERIC_ALL;
	if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT,
	    OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION, &owner, NULL,
	    &dacl, NULL, &sd) != ERROR_SUCCESS)
		return (0);
	if (!EqualSid(owner, user) && !is_trusted_sid(owner))
		ok = 0;
	if (dacl == NULL)
		ok = 0;
	for (i = 0; ok && i < dacl->AceCount; i++) {
		if (!GetAce(dacl, i, (void **)&ace) ||
		    ace->AceType != ACCESS_ALLOWED_ACE_TYPE ||
		    (ace->AceFlags & INHERIT_ONLY_ACE))
			continue;
		allow = (ACCESS_ALLOWED_ACE *)ace;
		who = &allow->SidStart;
		if ((allow->Mask & mask) != 0 && !EqualSid(who, user) &&
		    !is_trusted_sid(who))
			ok = 0;
	}
	LocalFree(sd);
	return (ok);
}

static int
profile_dir(PSID sid, wchar_t *out, size_t outlen)
{
	wchar_t	*s, key[512], raw[MAX_PATH];
	DWORD	 n = sizeof raw - sizeof raw[0], type;
	HKEY	 h;
	LONG	 r;

	if (!ConvertSidToStringSidW(sid, &s))
		return (-1);
	_snwprintf_s(key, 512, _TRUNCATE, L"SOFTWARE\\Microsoft\\Windows NT\\"
	    L"CurrentVersion\\ProfileList\\%s", s);
	LocalFree(s);
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) !=
	    ERROR_SUCCESS)
		return (-1);
	r = RegQueryValueExW(h, L"ProfileImagePath", NULL, &type, (BYTE *)raw,
	    &n);
	RegCloseKey(h);
	if (r != ERROR_SUCCESS)
		return (-1);
	raw[n / sizeof raw[0]] = L'\0';
	if (ExpandEnvironmentStringsW(raw, out, (DWORD)outlen) == 0)
		return (-1);
	return (0);
}

static HANDLE
s4u_logon(const wchar_t *name, const wchar_t *domain, char *why,
    size_t whylen)
{
	LSA_STRING		 process, origin, package;
	HANDLE			 lsa, token = NULL;
	LSA_OPERATIONAL_MODE	 mode;
	ULONG			 pkg, plen;
	TOKEN_SOURCE		 source;
	QUOTA_LIMITS		 quotas;
	LUID			 luid;
	NTSTATUS		 st, sub;
	void			*profile = NULL;
	wchar_t			 computer[MAX_COMPUTERNAME_LENGTH + 1];
	DWORD			 cn = MAX_COMPUTERNAME_LENGTH + 1;
	size_t			 nlen = wcslen(name) * sizeof (wchar_t);
	size_t			 dlen = wcslen(domain) * sizeof (wchar_t);
	size_t			 size;
	BYTE			*buf;
	int			 local;

	process.Buffer = "wtmux-service";
	process.Length = process.MaximumLength = 12;
	if (LsaRegisterLogonProcess(&process, &lsa, &mode) != STATUS_SUCCESS) {
		snprintf(why, whylen, "cannot register with LSA");
		return (NULL);
	}
	GetComputerNameW(computer, &cn);
	local = (_wcsicmp(domain, computer) == 0);
	package.Buffer = local ? MSV1_0_PACKAGE_NAME : MICROSOFT_KERBEROS_NAME_A;
	package.Length = package.MaximumLength =
	    (USHORT)strlen(package.Buffer);
	if (LsaLookupAuthenticationPackage(lsa, &package, &pkg) !=
	    STATUS_SUCCESS) {
		LsaDeregisterLogonProcess(lsa);
		snprintf(why, whylen, "no authentication package");
		return (NULL);
	}

	if (local) {
		MSV1_0_S4U_LOGON	*m;

		size = sizeof *m + nlen + dlen;
		buf = xcalloc(1, size);
		m = (MSV1_0_S4U_LOGON *)buf;
		m->MessageType = MsV1_0S4ULogon;
		m->UserPrincipalName.Buffer = (PWSTR)(buf + sizeof *m);
		m->UserPrincipalName.Length =
		    m->UserPrincipalName.MaximumLength = (USHORT)nlen;
		memcpy(m->UserPrincipalName.Buffer, name, nlen);
		m->DomainName.Buffer = (PWSTR)(buf + sizeof *m + nlen);
		m->DomainName.Length = m->DomainName.MaximumLength =
		    (USHORT)dlen;
		memcpy(m->DomainName.Buffer, domain, dlen);
	} else {
		KERB_S4U_LOGON	*k;

		size = sizeof *k + nlen + dlen;
		buf = xcalloc(1, size);
		k = (KERB_S4U_LOGON *)buf;
		k->MessageType = KerbS4ULogon;
		k->ClientUpn.Buffer = (PWSTR)(buf + sizeof *k);
		k->ClientUpn.Length = k->ClientUpn.MaximumLength =
		    (USHORT)nlen;
		memcpy(k->ClientUpn.Buffer, name, nlen);
		k->ClientRealm.Buffer = (PWSTR)(buf + sizeof *k + nlen);
		k->ClientRealm.Length = k->ClientRealm.MaximumLength =
		    (USHORT)dlen;
		memcpy(k->ClientRealm.Buffer, domain, dlen);
	}
	memcpy(source.SourceName, "tmuxsvc", 8);
	AllocateLocallyUniqueId(&source.SourceIdentifier);
	origin.Buffer = "wtmux-service";
	origin.Length = origin.MaximumLength = 12;
	st = LsaLogonUser(lsa, &origin, Network, pkg, buf, (ULONG)size, NULL,
	    &source, &profile, &plen, &luid, &token, &quotas, &sub);
	free(buf);
	if (profile != NULL)
		LsaFreeReturnBuffer(profile);
	LsaDeregisterLogonProcess(lsa);
	if (st != STATUS_SUCCESS) {
		snprintf(why, whylen, "logon refused (0x%08lx)",
		    (unsigned long)LsaNtStatusToWinError(st));
		return (NULL);
	}
	return (token);
}

static HANDLE
auth_key(struct win32_tls *t, const char *user, const char *public,
    char *why, size_t whylen)
{
	SecPkgContext_Bindings	 b;
	const unsigned char	*bind;
	unsigned char		 challenge[32], *msg, *sig;
	BYTE			 sid[SECURITY_MAX_SID_SIZE];
	wchar_t			*wuser, domain[256], name[256], dir[MAX_PATH];
	wchar_t			 path[MAX_PATH];
	DWORD			 sidlen = sizeof sid, dlen = 256, nlen = 256;
	DWORD			 got, attrs;
	SID_NAME_USE		 use;
	HANDLE			 f, token = NULL;
	char			*keys = NULL, *b64, line[1024];
	size_t			 mlen, siglen, blen;
	LARGE_INTEGER		 size;

	snprintf(why, whylen, "key not accepted");
	wuser = to_wide(user);
	if (!LookupAccountNameW(NULL, wuser, sid, &sidlen, domain, &dlen,
	    &use) || use != SidTypeUser ||
	    (dlen = 256, !LookupAccountSidW(NULL, sid, name, &nlen, domain,
	    &dlen, &use))) {
		free(wuser);
		goto fail;
	}
	free(wuser);
	if (profile_dir(sid, dir, MAX_PATH) != 0)
		goto fail;
	_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\.tmux\\authorized_keys",
	    dir);
	attrs = GetFileAttributesW(path);
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		snprintf(why, whylen, "no %ls", path);
		goto fail;
	}
	if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) {
		snprintf(why, whylen, "%ls is not a plain file", path);
		goto fail;
	}
	_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\.tmux", dir);
	if (!strict_path(path, sid)) {
		snprintf(why, whylen, "%ls may be changed by others", path);
		goto fail;
	}
	_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\.tmux\\authorized_keys",
	    dir);
	if (!strict_path(path, sid)) {
		snprintf(why, whylen, "%ls may be changed by others", path);
		goto fail;
	}
	f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
	    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (f == INVALID_HANDLE_VALUE)
		goto fail;
	if (!GetFileSizeEx(f, &size) || size.QuadPart > 65536) {
		CloseHandle(f);
		goto fail;
	}
	keys = xcalloc(1, (size_t)size.QuadPart + 1);
	if (!ReadFile(f, keys, (DWORD)size.QuadPart, &got, NULL))
		got = 0;
	keys[got] = '\0';
	CloseHandle(f);
	if (!tmux_remote_key_authorized(keys, public))
		goto fail;

	if (BCryptGenRandom(NULL, challenge, sizeof challenge,
	    BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		goto fail;
	b64 = win32_tls_base64(challenge, sizeof challenge);
	if (b64 == NULL || win32_tls_line(t, "challenge %s", b64) != 0) {
		free(b64);
		goto fail;
	}
	free(b64);
	if (read_line(t, line, sizeof line) != 0 ||
	    strncmp(line, "sign ", 5) != 0 ||
	    (sig = win32_tls_unbase64(line + 5, &siglen)) == NULL)
		goto fail;
	if (win32_tls_binding(t, &b) != 0) {
		free(sig);
		goto fail;
	}
	bind = binding_data(&b, &blen);
	msg = tmux_remote_key_message(challenge, sizeof challenge, bind, blen,
	    user, &mlen);
	FreeContextBuffer(b.Bindings);
	if (msg == NULL || win32_tls_key_verify(public, msg, mlen, sig,
	    (DWORD)siglen) != 0) {
		free(msg);
		free(sig);
		snprintf(why, whylen, "bad signature");
		goto fail;
	}
	free(msg);
	free(sig);
	token = s4u_logon(name, domain, why, whylen);
fail:
	free(keys);
	return (token);
}

static int
conns_add(const char *addr, int delta)
{
	int	i, free_slot = -1, ok = 1;

	EnterCriticalSection(&remote_lock);
	for (i = 0; i < MAX_CONNS; i++) {
		if (conns_addr[i].n != 0 &&
		    strcmp(conns_addr[i].addr, addr) == 0)
			break;
		if (conns_addr[i].n == 0 && free_slot == -1)
			free_slot = i;
	}
	if (delta > 0) {
		if (conns_total >= MAX_CONNS ||
		    (i < MAX_CONNS && conns_addr[i].n >= MAX_PER_ADDR) ||
		    (i == MAX_CONNS && free_slot == -1))
			ok = 0;
		else {
			if (i == MAX_CONNS) {
				i = free_slot;
				snprintf(conns_addr[i].addr,
				    sizeof conns_addr[i].addr, "%s", addr);
			}
			conns_addr[i].n++;
			conns_total++;
		}
	} else if (i < MAX_CONNS) {
		conns_addr[i].n--;
		conns_total--;
	}
	LeaveCriticalSection(&remote_lock);
	return (ok);
}

static HANDLE
spawn_helper(HANDLE logon, PSID sid, HANDLE *to, HANDLE *from,
    HANDLE *profile, char *why, size_t whylen)
{
	STARTUPINFOEXW			 si;
	PROCESS_INFORMATION		 pi;
	SECURITY_ATTRIBUTES		 sa;
	LPPROC_THREAD_ATTRIBUTE_LIST	 attrs = NULL;
	PROFILEINFOW			 prof;
	struct wts_entry		*list;
	HANDLE				 token = NULL, in_r = NULL, in_w = NULL;
	HANDLE				 out_r = NULL, out_w = NULL, nul = NULL;
	HANDLE				 inherit[3], proc = NULL;
	const wchar_t			*relay = L"TMUX_WIN32_RELAY=1";
	wchar_t				 cmd[MAX_PATH + 8], home[MAX_PATH];
	wchar_t				 name[256], *env = NULL, *env2 = NULL;
	wchar_t				*p, *desktop;
	DWORD				 n, i, pick = (DWORD)-1;
	size_t				 len;
	SIZE_T				 size = 0;

	*profile = NULL;
	list = wts_list(&n);
	for (i = 0; i < n; i++) {
		if (!EqualSid(list[i].sid, sid))
			continue;
		if (pick == (DWORD)-1 || list[i].active)
			pick = list[i].id;
	}
	wts_free(list, n);
	if (pick != (DWORD)-1 && WTSQueryUserToken(pick, &token))
		desktop = L"winsta0\\default";
	else {
		token = NULL;
		if (!DuplicateTokenEx(logon, MAXIMUM_ALLOWED, NULL,
		    SecurityImpersonation, TokenPrimary, &token)) {
			snprintf(why, whylen, "no token (%lu)", GetLastError());
			return (NULL);
		}
		sid_name(sid, name, 256);
		if ((p = wcschr(name, L'\\')) != NULL)
			memmove(name, p + 1, (wcslen(p + 1) + 1) *
			    sizeof (wchar_t));
		memset(&prof, 0, sizeof prof);
		prof.dwSize = sizeof prof;
		prof.dwFlags = PI_NOUI;
		prof.lpUserName = name;
		if (LoadUserProfileW(logon, &prof))
			*profile = prof.hProfile;
		desktop = L"";
		relay = L"TMUX_WIN32_RELAY=0";
	}

	if (CreateEnvironmentBlock((void **)&env, token, FALSE)) {
		for (p = env; *p != L'\0'; p += wcslen(p) + 1)
			;
		env2 = xcalloc((size_t)(p - env) + 32, sizeof *env2);
		len = 0;
		for (p = env; *p != L'\0'; p += wcslen(p) + 1) {
			if (_wcsnicmp(p, L"TMUX_WIN32_RELAY=", 17) == 0)
				continue;
			memcpy(env2 + len, p, (wcslen(p) + 1) * sizeof *env2);
			len += wcslen(p) + 1;
		}
		wcscpy_s(env2 + len, 32, relay);
		env2[len + wcslen(relay) + 1] = L'\0';
		DestroyEnvironmentBlock(env);
	} else {
		snprintf(why, whylen, "no environment");
		goto fail;
	}

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	if (!CreatePipe(&in_r, &in_w, &sa, 0) ||
	    !CreatePipe(&out_r, &out_w, &sa, 0)) {
		snprintf(why, whylen, "no pipes");
		goto fail;
	}
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
	nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
	    OPEN_EXISTING, 0, NULL);
	inherit[0] = in_r;
	inherit[1] = out_w;
	inherit[2] = nul;
	InitializeProcThreadAttributeList(NULL, 1, 0, &size);
	attrs = xcalloc(1, size);
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size) ||
	    !UpdateProcThreadAttribute(attrs, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
	    (nul != INVALID_HANDLE_VALUE ? 3 : 2) * sizeof inherit[0], NULL,
	    NULL)) {
		snprintf(why, whylen, "no attributes");
		goto fail;
	}
	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.lpDesktop = desktop;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = nul;
	si.lpAttributeList = attrs;
	_snwprintf_s(cmd, MAX_PATH + 8, _TRUNCATE, L"\"%s\"", tmux_path);
	n = MAX_PATH;
	if (!GetUserProfileDirectoryW(token, home, &n))
		home[0] = L'\0';
	if (!CreateProcessAsUserW(token, tmux_path, cmd, NULL, NULL, TRUE,
	    CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|
	    EXTENDED_STARTUPINFO_PRESENT, env2, home[0] != L'\0' ? home : NULL,
	    &si.StartupInfo, &pi)) {
		snprintf(why, whylen, "cannot start the helper (%lu)",
		    GetLastError());
		goto fail;
	}
	CloseHandle(pi.hThread);
	proc = pi.hProcess;
	*to = in_w;
	*from = out_r;
	in_w = out_r = NULL;
fail:
	if (nul != NULL && nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	if (attrs != NULL) {
		DeleteProcThreadAttributeList(attrs);
		free(attrs);
	}
	if (in_r != NULL)
		CloseHandle(in_r);
	if (out_w != NULL)
		CloseHandle(out_w);
	if (in_w != NULL)
		CloseHandle(in_w);
	if (out_r != NULL)
		CloseHandle(out_r);
	free(env2);
	if (token != NULL)
		CloseHandle(token);
	return (proc);
}

struct pump {
	struct win32_tls	*tls;
	HANDLE			 from;
};

static DWORD WINAPI
pump_out(LPVOID arg)
{
	struct pump	*p = arg;
	char		 buf[16384];
	DWORD		 n;

	while (ReadFile(p->from, buf, sizeof buf, &n, NULL) && n != 0) {
		if (win32_tls_write(p->tls, buf, n) != 0)
			break;
	}
	shutdown(p->tls->s, SD_BOTH);
	return (0);
}

static VOID CALLBACK
grace_expired(PVOID arg, BOOLEAN fired)
{
	(void)fired;
	shutdown((SOCKET)(UINT_PTR)arg, SD_BOTH);
}

static void
grace_stop(HANDLE *timer)
{
	if (*timer != NULL) {
		DeleteTimerQueueTimer(NULL, *timer, INVALID_HANDLE_VALUE);
		*timer = NULL;
	}
}

static DWORD WINAPI
conn_thread(LPVOID arg)
{
	struct conn		*c = arg;
	struct win32_tls	 tls;
	struct pump		 pump;
	PCCERT_CONTEXT		 cert;
	HANDLE			 token = NULL, helper = NULL, to = NULL;
	HANDLE			 from = NULL, profile = NULL, out = NULL;
	HANDLE			 timer = NULL;
	PSID			 sid = NULL;
	wchar_t			 wname[512];
	char			 line[2048], user[512] = "", key[600], why[512];
	char			 err[256], pkg[64] = "", method[64] = "";
	char			*account = NULL, buf[16384];
	DWORD			 timeout, w;
	ULONGLONG		 wait;
	int			 have_tls = 0, attempts, n;

	EnterCriticalSection(&remote_lock);
	timeout = config.grace;
	wait = tmux_remote_limit_wait(&limits, c->addr, GetTickCount64());
	cert = host_cert != NULL ? CertDuplicateCertificateContext(host_cert) :
	    NULL;
	LeaveCriticalSection(&remote_lock);
	if (wait != 0 || cert == NULL) {
		if (wait != 0)
			service_log(EVENTLOG_WARNING_TYPE, "remote: %s refused "
			    "for %llu s after failed logons", c->addr,
			    (wait + 999) / 1000);
		closesocket(c->s);
		goto done;
	}
	setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
	    sizeof timeout);
	setsockopt(c->s, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
	    sizeof timeout);
	if (!CreateTimerQueueTimer(&timer, NULL, grace_expired,
	    (PVOID)(UINT_PTR)c->s, timeout, 0, WT_EXECUTEONLYONCE))
		timer = NULL;
	if (win32_tls_server(&tls, c->s, cert, err, sizeof err) != 0) {
		grace_stop(&timer);
		CertFreeCertificateContext(cert);
		service_log(EVENTLOG_WARNING_TYPE, "remote: %s: %s", c->addr,
		    err);
		win32_tls_free(&tls);
		goto done;
	}
	CertFreeCertificateContext(cert);
	have_tls = 1;

	if (read_line(&tls, line, sizeof line) != 0 ||
	    strncmp(line, "hello 1 user=", 13) != 0)
		goto done;
	snprintf(user, sizeof user, "%s", line + 13);
	snprintf(key, sizeof key, "user:%s", user);
	CharLowerBuffA(key, (DWORD)strlen(key));
	EnterCriticalSection(&remote_lock);
	wait = tmux_remote_limit_wait(&limits, key, GetTickCount64());
	LeaveCriticalSection(&remote_lock);
	if (wait != 0) {
		win32_tls_line(&tls, "denied too many failed logons; wait %llu "
		    "seconds", (wait + 999) / 1000);
		service_log(EVENTLOG_WARNING_TYPE, "remote: %s refused for %s "
		    "after failed logons", c->addr, user);
		goto done;
	}
	win32_tls_line(&tls, "methods%s%s", config.keys ? " key" : "",
	    config.windows ? " windows" : "");

	for (attempts = 0; token == NULL && attempts < MAX_ATTEMPTS;
	    attempts++) {
		if (read_line(&tls, line, sizeof line) != 0)
			break;
		*why = '\0';
		if (strncmp(line, "key ", 4) == 0 && config.keys) {
			snprintf(method, sizeof method, "key");
			token = auth_key(&tls, user, line + 4, why, sizeof why);
		} else if (strcmp(line, "negotiate") == 0 && config.windows) {
			token = auth_windows(&tls, pkg, sizeof pkg, why,
			    sizeof why);
			snprintf(method, sizeof method, *pkg != '\0' ?
			    "windows (%s)" : "windows", pkg);
		} else {
			win32_tls_line(&tls, "denied method not allowed");
			break;
		}
		if (token == NULL) {
			EnterCriticalSection(&remote_lock);
			tmux_remote_limit_fail(&limits, c->addr,
			    GetTickCount64());
			tmux_remote_limit_fail(&limits, key, GetTickCount64());
			LeaveCriticalSection(&remote_lock);
			service_log(EVENTLOG_WARNING_TYPE, "remote: failed logon "
			    "for %s from %s by %s: %s", user, c->addr, method,
			    why);
			win32_tls_line(&tls, "denied %s",
			    strcmp(method, "key") == 0 ? "key not accepted" :
			    why);
		}
	}
	if (token == NULL)
		goto done;
	EnterCriticalSection(&remote_lock);
	tmux_remote_limit_ok(&limits, c->addr);
	tmux_remote_limit_ok(&limits, key);
	LeaveCriticalSection(&remote_lock);

	if ((sid = token_user(token)) == NULL)
		goto done;
	sid_name(sid, wname, 512);
	account = to_utf8(wname);
	helper = spawn_helper(token, sid, &to, &from, &profile, why,
	    sizeof why);
	if (helper == NULL) {
		service_log(EVENTLOG_ERROR_TYPE, "remote: %s logged on from %s "
		    "by %s, but %s", account, c->addr, method, why);
		win32_tls_line(&tls, "denied %s", why);
		goto done;
	}
	service_log(EVENTLOG_INFORMATION_TYPE, "remote: %s logged on from %s "
	    "by %s", account, c->addr, method);
	if (win32_tls_line(&tls, "ok %s", account) != 0)
		goto done;

	grace_stop(&timer);
	timeout = 0;
	setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout,
	    sizeof timeout);
	setsockopt(c->s, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
	    sizeof timeout);
	pump.tls = &tls;
	pump.from = from;
	out = CreateThread(NULL, 0, pump_out, &pump, 0, NULL);
	while ((n = win32_tls_read(&tls, buf, sizeof buf)) > 0) {
		if (!WriteFile(to, buf, (DWORD)n, &w, NULL) || w != (DWORD)n)
			break;
	}
	CloseHandle(to);
	to = NULL;
	if (WaitForSingleObject(helper, 5000) != WAIT_OBJECT_0)
		TerminateProcess(helper, 1);
	if (out != NULL) {
		shutdown(tls.s, SD_BOTH);
		WaitForSingleObject(out, INFINITE);
		CloseHandle(out);
	}
	service_log(EVENTLOG_INFORMATION_TYPE, "remote: %s from %s "
	    "disconnected", account, c->addr);
done:
	grace_stop(&timer);
	if (have_tls)
		win32_tls_free(&tls);
	if (helper != NULL)
		CloseHandle(helper);
	if (to != NULL)
		CloseHandle(to);
	if (from != NULL)
		CloseHandle(from);
	if (profile != NULL && token != NULL)
		UnloadUserProfile(token, profile);
	if (token != NULL)
		CloseHandle(token);
	free(sid);
	free(account);
	conns_add(c->addr, -1);
	free(c);
	return (0);
}

static DWORD WINAPI
listen_thread(LPVOID arg)
{
	SOCKET			 l = (SOCKET)(UINT_PTR)arg, s;
	struct sockaddr_storage	 ss;
	struct conn		*c;
	HANDLE			 t;
	int			 len;

	for (;;) {
		len = sizeof ss;
		s = accept(l, (struct sockaddr *)&ss, &len);
		if (s == INVALID_SOCKET)
			break;
		c = xcalloc(1, sizeof *c);
		c->s = s;
		getnameinfo((struct sockaddr *)&ss, len, c->addr,
		    sizeof c->addr, NULL, 0, NI_NUMERICHOST);
		if (!conns_add(c->addr, 1)) {
			service_log(EVENTLOG_WARNING_TYPE, "remote: %s refused: "
			    "too many connections", c->addr);
			closesocket(s);
			free(c);
			continue;
		}
		t = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
		if (t == NULL) {
			conns_add(c->addr, -1);
			closesocket(s);
			free(c);
		} else
			CloseHandle(t);
	}
	return (0);
}

void
remote_stop(void)
{
	lock_init();
	EnterCriticalSection(&remote_lock);
	if (listener != INVALID_SOCKET) {
		closesocket(listener);
		listener = INVALID_SOCKET;
	}
	LeaveCriticalSection(&remote_lock);
	if (listener_thread != NULL) {
		WaitForSingleObject(listener_thread, 5000);
		CloseHandle(listener_thread);
		listener_thread = NULL;
	}
}

void
remote_reload(char *status, size_t statuslen)
{
	struct addrinfo	 hints, *res = NULL;
	WSADATA		 wsa;
	SOCKET		 l;
	char		 port[16], err[256], fp[128] = "";
	PCCERT_CONTEXT	 cert;
	int		 on = 1;

	lock_init();
	WSAStartup(MAKEWORD(2, 2), &wsa);
	remote_stop();
	config_read(&config);
	if (!config.enabled) {
		snprintf(status, statuslen, "not listening");
		return;
	}
	if ((cert = win32_tls_host_cert(1, err, sizeof err)) == NULL) {
		snprintf(status, statuslen, "not listening: %s", err);
		service_log(EVENTLOG_ERROR_TYPE, "remote: %s", status);
		return;
	}
	win32_tls_cert_fingerprint(cert, fp, sizeof fp);
	EnterCriticalSection(&remote_lock);
	if (host_cert != NULL)
		CertFreeCertificateContext(host_cert);
	host_cert = cert;
	LeaveCriticalSection(&remote_lock);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE|AI_NUMERICHOST;
	snprintf(port, sizeof port, "%lu", config.port);
	if (getaddrinfo(config.address, port, &hints, &res) != 0) {
		snprintf(status, statuslen, "not listening: bad address %s",
		    config.address);
		service_log(EVENTLOG_ERROR_TYPE, "remote: %s", status);
		return;
	}
	l = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&on,
	    sizeof on);
	if (l == INVALID_SOCKET ||
	    bind(l, res->ai_addr, (int)res->ai_addrlen) != 0 ||
	    listen(l, 16) != 0) {
		snprintf(status, statuslen, "not listening: cannot listen on "
		    "%s:%s (%d)", config.address, port, WSAGetLastError());
		service_log(EVENTLOG_ERROR_TYPE, "remote: %s", status);
		if (l != INVALID_SOCKET)
			closesocket(l);
		freeaddrinfo(res);
		return;
	}
	freeaddrinfo(res);
	EnterCriticalSection(&remote_lock);
	listener = l;
	LeaveCriticalSection(&remote_lock);
	listener_thread = CreateThread(NULL, 0, listen_thread,
	    (LPVOID)(UINT_PTR)l, 0, NULL);
	snprintf(status, statuslen, "listening on %s port %s (logons: %s%s%s);"
	    " host key %s", config.address, port,
	    config.keys ? "keys" : "", config.keys && config.windows ? ", " :
	    "", config.windows ? (config.no_ntlm ? "Windows, no NTLM" :
	    "Windows") : "", fp);
	service_log(EVENTLOG_INFORMATION_TYPE, "remote: %s", status);
}

void
remote_forget(void)
{
	RegDeleteTreeW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY);
	win32_tls_host_cert_delete();
}

int
remote_fingerprint(char *out, size_t outlen)
{
	PCCERT_CONTEXT	cert;
	int		rc;

	if ((cert = win32_tls_host_cert(0, NULL, 0)) == NULL)
		return (-1);
	rc = win32_tls_cert_fingerprint(cert, out, outlen);
	CertFreeCertificateContext(cert);
	return (rc);
}

int
cmd_listen(int argc, wchar_t **argv)
{
	HKEY		 key;
	DWORD		 one = 1, zero = 0, port = TMUX_REMOTE_PORT;
	const wchar_t	*address = L"127.0.0.1", *auth = L"both";
	char		*req[1] = { "reload" }, a[128];
	int		 i, no_ntlm = 0, off = 0;

	for (i = 2; i < argc; i++) {
		if (wcscmp(argv[i], L"off") == 0 && argc == 3)
			off = 1;
		else if (wcscmp(argv[i], L"-a") == 0 && i + 1 < argc)
			address = argv[++i];
		else if (wcscmp(argv[i], L"-p") == 0 && i + 1 < argc) {
			port = wcstoul(argv[++i], NULL, 10);
			if (port == 0 || port > 65535)
				fatal("bad port");
		} else if (wcscmp(argv[i], L"-auth") == 0 && i + 1 < argc) {
			auth = argv[++i];
			if (wcscmp(auth, L"keys") != 0 &&
			    wcscmp(auth, L"windows") != 0 &&
			    wcscmp(auth, L"both") != 0)
				fatal("-auth is keys, windows or both");
		} else if (wcscmp(argv[i], L"-no-ntlm") == 0)
			no_ntlm = 1;
		else
			fatal("usage: wtmux-service listen [-a address] "
			    "[-p port] [-auth keys|windows|both] [-no-ntlm] | "
			    "listen off");
	}
	if (!process_is_elevated())
		fatal("listen needs an elevated (administrator) prompt");
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, PARAMETERS_KEY, 0, NULL, 0,
	    KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
		fatal("cannot write the configuration (is the service "
		    "installed?)");
	RegSetValueExW(key, L"Listen", 0, REG_DWORD,
	    (const BYTE *)(off ? &zero : &one), sizeof one);
	if (!off) {
		RegSetValueExW(key, L"Address", 0, REG_SZ, (const BYTE *)address,
		    (DWORD)((wcslen(address) + 1) * sizeof (wchar_t)));
		RegSetValueExW(key, L"Port", 0, REG_DWORD, (const BYTE *)&port,
		    sizeof port);
		RegSetValueExW(key, L"Auth", 0, REG_SZ, (const BYTE *)auth,
		    (DWORD)((wcslen(auth) + 1) * sizeof (wchar_t)));
		RegSetValueExW(key, L"NoNtlm", 0, REG_DWORD,
		    (const BYTE *)(no_ntlm ? &one : &zero), sizeof one);
	}
	RegCloseKey(key);
	i = client_request(1, req);
	if (!off) {
		WideCharToMultiByte(CP_UTF8, 0, address, -1, a, sizeof a, NULL,
		    NULL);
		if (!is_loopback(a))
			printf("Other machines can connect only through the "
			    "firewall, for example (elevated):\n  netsh "
			    "advfirewall firewall add rule name=wtmux-service "
			    "dir=in action=allow protocol=TCP localport=%lu\n",
			    port);
	}
	return (i);
}

int
cmd_keygen(int create)
{
	NCRYPT_KEY_HANDLE	 key;
	char			 err[256], *pub, name[256] = "", host[256] = "";
	DWORD			 n = sizeof name, hn = sizeof host;

	if (win32_tls_user_key(create, &key, err, sizeof err) != 0)
		fatal(err);
	if ((pub = win32_tls_user_key_public(key)) == NULL)
		fatal("cannot read the public key");
	NCryptFreeObject(key);
	GetUserNameA(name, &n);
	GetComputerNameA(host, &hn);
	printf("%s %s %s@%s\n", TMUX_REMOTE_KEY_TYPE, pub, name, host);
	if (create)
		fprintf(stderr, "Add that line to %%USERPROFILE%%\\.tmux\\"
		    "authorized_keys of the account on the host.\n");
	free(pub);
	return (0);
}
