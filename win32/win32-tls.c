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

#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux-remote.h"

#define HOST_KEY_NAME	L"wtmux-service-host"
#define HOST_CERT_NAME	L"wtmux-service host key"
#define USER_KEY_NAME	L"tmux-remote"

static void
tls_error(char *err, size_t errlen, const char *what, long code)
{
	if (err != NULL)
		snprintf(err, errlen, "%s (0x%08lx)", what, (unsigned long)code);
}

static int
send_all(SOCKET s, const void *data, size_t n)
{
	const char	*p = data;
	int		 r;

	while (n > 0) {
		r = send(s, p, n > 65536 ? 65536 : (int)n, 0);
		if (r <= 0)
			return (-1);
		p += r;
		n -= (size_t)r;
	}
	return (0);
}

static int
recv_more(struct win32_tls *t)
{
	unsigned char	*p;
	int		 r;

	if (t->insize - t->inlen < 16384) {
		if (t->insize > 4 * 65536)
			return (-1);
		p = realloc(t->in, t->insize + 32768);
		if (p == NULL)
			return (-1);
		t->in = p;
		t->insize += 32768;
	}
	r = recv(t->s, (char *)t->in + t->inlen, (int)(t->insize - t->inlen),
	    0);
	if (r < 0)
		return (-1);
	if (r == 0)
		return (0);
	t->inlen += (size_t)r;
	return (1);
}

static void
tls_init(struct win32_tls *t, SOCKET s, int server)
{
	memset(t, 0, sizeof *t);
	t->s = s;
	t->server = server;
	InitializeCriticalSection(&t->lock);
	InitializeCriticalSection(&t->wlock);
}

static SECURITY_STATUS
tls_acquire(struct win32_tls *t, PCCERT_CONTEXT cert)
{
	SCH_CREDENTIALS	 sc;
	TLS_PARAMETERS	 tp;
	SCHANNEL_CRED	 old;
	SECURITY_STATUS	 st;
	DWORD		 use, keep, flags;

	use = t->server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND;
	keep = t->server ? (SP_PROT_TLS1_2_SERVER|SP_PROT_TLS1_3_SERVER) :
	    (SP_PROT_TLS1_2_CLIENT|SP_PROT_TLS1_3_CLIENT);
	flags = SCH_USE_STRONG_CRYPTO;
	if (!t->server)
		flags |= SCH_CRED_MANUAL_CRED_VALIDATION|SCH_CRED_NO_DEFAULT_CREDS;

	memset(&sc, 0, sizeof sc);
	memset(&tp, 0, sizeof tp);
	sc.dwVersion = SCH_CREDENTIALS_VERSION;
	sc.dwFlags = flags;
	if (cert != NULL) {
		sc.cCreds = 1;
		sc.paCred = &cert;
	}
	tp.grbitDisabledProtocols = ~keep;
	sc.cTlsParameters = 1;
	sc.pTlsParameters = &tp;
	st = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, use, NULL, &sc,
	    NULL, NULL, &t->cred, NULL);
	if (st == SEC_E_OK) {
		t->have_cred = 1;
		return (st);
	}

	memset(&old, 0, sizeof old);
	old.dwVersion = SCHANNEL_CRED_VERSION;
	old.dwFlags = flags;
	old.grbitEnabledProtocols = t->server ? SP_PROT_TLS1_2_SERVER :
	    SP_PROT_TLS1_2_CLIENT;
	if (cert != NULL) {
		old.cCreds = 1;
		old.paCred = &cert;
	}
	st = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, use, NULL, &old,
	    NULL, NULL, &t->cred, NULL);
	if (st == SEC_E_OK)
		t->have_cred = 1;
	return (st);
}

static void
keep_extra(struct win32_tls *t, SecBuffer *b)
{
	if (b->BufferType == SECBUFFER_EXTRA && b->cbBuffer > 0) {
		memmove(t->in, t->in + (t->inlen - b->cbBuffer), b->cbBuffer);
		t->inlen = b->cbBuffer;
	} else
		t->inlen = 0;
}

static int
tls_handshake(struct win32_tls *t, const wchar_t *host, int initial,
    char *err, size_t errlen)
{
	SecBuffer	 inb[2], outb[1];
	SecBufferDesc	 ind, outd;
	SECURITY_STATUS	 st;
	DWORD		 flags, attrs;
	int		 first = initial, r;

	if (t->server && initial && t->inlen == 0 && recv_more(t) != 1) {
		tls_error(err, errlen, "connection closed in handshake", 0);
		return (-1);
	}
	for (;;) {
		inb[0].BufferType = SECBUFFER_TOKEN;
		inb[0].pvBuffer = t->in;
		inb[0].cbBuffer = (unsigned long)t->inlen;
		inb[1].BufferType = SECBUFFER_EMPTY;
		inb[1].pvBuffer = NULL;
		inb[1].cbBuffer = 0;
		ind.ulVersion = SECBUFFER_VERSION;
		ind.cBuffers = 2;
		ind.pBuffers = inb;
		outb[0].BufferType = SECBUFFER_TOKEN;
		outb[0].pvBuffer = NULL;
		outb[0].cbBuffer = 0;
		outd.ulVersion = SECBUFFER_VERSION;
		outd.cBuffers = 1;
		outd.pBuffers = outb;

		EnterCriticalSection(&t->lock);
		if (t->server) {
			flags = ASC_REQ_SEQUENCE_DETECT|ASC_REQ_REPLAY_DETECT|
			    ASC_REQ_CONFIDENTIALITY|ASC_REQ_ALLOCATE_MEMORY|
			    ASC_REQ_STREAM|ASC_REQ_EXTENDED_ERROR;
			st = AcceptSecurityContext(&t->cred,
			    first ? NULL : &t->ctx, &ind, flags, 0,
			    first ? &t->ctx : NULL, &outd, &attrs, NULL);
		} else {
			flags = ISC_REQ_SEQUENCE_DETECT|ISC_REQ_REPLAY_DETECT|
			    ISC_REQ_CONFIDENTIALITY|ISC_REQ_ALLOCATE_MEMORY|
			    ISC_REQ_STREAM|ISC_REQ_EXTENDED_ERROR|
			    ISC_REQ_MANUAL_CRED_VALIDATION;
			st = InitializeSecurityContextW(&t->cred,
			    first ? NULL : &t->ctx, (SEC_WCHAR *)host, flags,
			    0, 0, (first && !t->server) ? NULL : &ind, 0,
			    first ? &t->ctx : NULL, &outd, &attrs, NULL);
		}
		LeaveCriticalSection(&t->lock);
		if (st != SEC_E_INCOMPLETE_MESSAGE) {
			if (first)
				t->have_ctx = (st == SEC_E_OK ||
				    st == SEC_I_CONTINUE_NEEDED);
			first = 0;
		}
		if (outb[0].cbBuffer != 0 && outb[0].pvBuffer != NULL) {
			r = send_all(t->s, outb[0].pvBuffer, outb[0].cbBuffer);
			FreeContextBuffer(outb[0].pvBuffer);
			if (r != 0) {
				tls_error(err, errlen, "send failed in handshake",
				    WSAGetLastError());
				return (-1);
			}
		}
		if (st == SEC_E_INCOMPLETE_MESSAGE ||
		    (st == SEC_I_CONTINUE_NEEDED && inb[1].BufferType !=
		    SECBUFFER_EXTRA)) {
			if (st == SEC_I_CONTINUE_NEEDED)
				t->inlen = 0;
			if (recv_more(t) != 1) {
				tls_error(err, errlen,
				    "connection closed in handshake", 0);
				return (-1);
			}
			continue;
		}
		if (st == SEC_I_CONTINUE_NEEDED) {
			keep_extra(t, &inb[1]);
			continue;
		}
		if (st == SEC_I_INCOMPLETE_CREDENTIALS && !t->server)
			continue;
		if (st != SEC_E_OK) {
			tls_error(err, errlen, "TLS handshake failed", st);
			return (-1);
		}
		keep_extra(t, &inb[1]);
		return (0);
	}
}

static int
tls_finish(struct win32_tls *t, char *err, size_t errlen)
{
	SecPkgContext_CipherInfo	ci;
	SECURITY_STATUS			st;

	st = QueryContextAttributesW(&t->ctx, SECPKG_ATTR_STREAM_SIZES,
	    &t->sizes);
	if (st != SEC_E_OK) {
		tls_error(err, errlen, "no TLS record sizes", st);
		return (-1);
	}
	memset(&ci, 0, sizeof ci);
	st = QueryContextAttributesW(&t->ctx, SECPKG_ATTR_CIPHER_INFO, &ci);
	if (st != SEC_E_OK) {
		tls_error(err, errlen, "no TLS cipher information", st);
		return (-1);
	}
	if (ci.dwProtocol != SP_PROT_TLS1_2_CLIENT &&
	    ci.dwProtocol != SP_PROT_TLS1_2_SERVER &&
	    ci.dwProtocol != SP_PROT_TLS1_3_CLIENT &&
	    ci.dwProtocol != SP_PROT_TLS1_3_SERVER &&
	    ci.dwProtocol != 0x303 && ci.dwProtocol != 0x304) {
		tls_error(err, errlen, "TLS version not allowed", ci.dwProtocol);
		return (-1);
	}
	if ((wcsstr(ci.szCipherSuite, L"_GCM_") == NULL &&
	    wcsstr(ci.szCipherSuite, L"CHACHA20") == NULL) ||
	    (wcsstr(ci.szCipherSuite, L"ECDHE") == NULL &&
	    wcsncmp(ci.szCipherSuite, L"TLS_AES_", 8) != 0 &&
	    wcsncmp(ci.szCipherSuite, L"TLS_CHACHA20_", 13) != 0)) {
		if (err != NULL)
			snprintf(err, errlen, "cipher suite not allowed: %ls",
			    ci.szCipherSuite);
		return (-1);
	}
	return (0);
}

int
win32_tls_client(struct win32_tls *t, SOCKET s, const char *host, char *err,
    size_t errlen)
{
	wchar_t			whost[256];
	SECURITY_STATUS		st;

	tls_init(t, s, 0);
	if (MultiByteToWideChar(CP_UTF8, 0, host, -1, whost,
	    sizeof whost / sizeof whost[0]) == 0)
		wcscpy_s(whost, sizeof whost / sizeof whost[0], L"tmux");
	if ((st = tls_acquire(t, NULL)) != SEC_E_OK) {
		tls_error(err, errlen, "no TLS credentials", st);
		return (-1);
	}
	if (tls_handshake(t, whost, 1, err, errlen) != 0)
		return (-1);
	return (tls_finish(t, err, errlen));
}

int
win32_tls_server(struct win32_tls *t, SOCKET s, PCCERT_CONTEXT cert,
    char *err, size_t errlen)
{
	SECURITY_STATUS	st;

	tls_init(t, s, 1);
	if ((st = tls_acquire(t, cert)) != SEC_E_OK) {
		tls_error(err, errlen, "no TLS credentials", st);
		return (-1);
	}
	if (tls_handshake(t, NULL, 1, err, errlen) != 0)
		return (-1);
	return (tls_finish(t, err, errlen));
}

int
win32_tls_read(struct win32_tls *t, void *buf, size_t n)
{
	SecBuffer	 b[4], *data, *extra;
	SecBufferDesc	 d;
	SECURITY_STATUS	 st;
	unsigned char	*p;
	size_t		 have;
	int		 i, r;

	while (t->plainoff == t->plainlen) {
		if (t->eof)
			return (0);
		t->plainoff = t->plainlen = 0;
		if (t->inlen > 0) {
			b[0].BufferType = SECBUFFER_DATA;
			b[0].pvBuffer = t->in;
			b[0].cbBuffer = (unsigned long)t->inlen;
			for (i = 1; i < 4; i++) {
				b[i].BufferType = SECBUFFER_EMPTY;
				b[i].pvBuffer = NULL;
				b[i].cbBuffer = 0;
			}
			d.ulVersion = SECBUFFER_VERSION;
			d.cBuffers = 4;
			d.pBuffers = b;
			EnterCriticalSection(&t->lock);
			st = DecryptMessage(&t->ctx, &d, 0, NULL);
			LeaveCriticalSection(&t->lock);
			if (st == SEC_I_CONTEXT_EXPIRED) {
				t->eof = 1;
				return (0);
			}
			if (st != SEC_E_INCOMPLETE_MESSAGE) {
				if (st != SEC_E_OK && st != SEC_I_RENEGOTIATE)
					return (-1);
				data = extra = NULL;
				for (i = 1; i < 4; i++) {
					if (b[i].BufferType == SECBUFFER_DATA)
						data = &b[i];
					if (b[i].BufferType == SECBUFFER_EXTRA)
						extra = &b[i];
				}
				if (data != NULL && data->cbBuffer > 0) {
					p = realloc(t->plain, data->cbBuffer);
					if (p == NULL)
						return (-1);
					t->plain = p;
					memcpy(t->plain, data->pvBuffer,
					    data->cbBuffer);
					t->plainlen = data->cbBuffer;
				}
				if (extra != NULL && extra->cbBuffer > 0) {
					memmove(t->in, t->in + (t->inlen -
					    extra->cbBuffer), extra->cbBuffer);
					t->inlen = extra->cbBuffer;
				} else
					t->inlen = 0;
				if (st == SEC_I_RENEGOTIATE &&
				    tls_handshake(t, L"tmux", 0, NULL, 0) != 0)
					return (-1);
				continue;
			}
		}
		if ((r = recv_more(t)) != 1)
			return (r == 0 ? 0 : -1);
	}
	have = t->plainlen - t->plainoff;
	if (n > have)
		n = have;
	memcpy(buf, t->plain + t->plainoff, n);
	t->plainoff += n;
	return ((int)n);
}

int
win32_tls_read_full(struct win32_tls *t, void *buf, size_t n)
{
	unsigned char	*p = buf;
	int		 r;

	while (n > 0) {
		if ((r = win32_tls_read(t, p, n)) <= 0)
			return (-1);
		p += r;
		n -= (size_t)r;
	}
	return (0);
}

int
win32_tls_write(struct win32_tls *t, const void *data, size_t n)
{
	SecBuffer	 b[4];
	SecBufferDesc	 d;
	SECURITY_STATUS	 st;
	const char	*p = data;
	unsigned char	*buf;
	size_t		 chunk, total;
	int		 rc = 0;

	total = t->sizes.cbHeader + t->sizes.cbMaximumMessage +
	    t->sizes.cbTrailer;
	if ((buf = malloc(total)) == NULL)
		return (-1);
	EnterCriticalSection(&t->wlock);
	while (n > 0 && rc == 0) {
		chunk = n > t->sizes.cbMaximumMessage ?
		    t->sizes.cbMaximumMessage : n;
		memcpy(buf + t->sizes.cbHeader, p, chunk);
		b[0].BufferType = SECBUFFER_STREAM_HEADER;
		b[0].pvBuffer = buf;
		b[0].cbBuffer = t->sizes.cbHeader;
		b[1].BufferType = SECBUFFER_DATA;
		b[1].pvBuffer = buf + t->sizes.cbHeader;
		b[1].cbBuffer = (unsigned long)chunk;
		b[2].BufferType = SECBUFFER_STREAM_TRAILER;
		b[2].pvBuffer = buf + t->sizes.cbHeader + chunk;
		b[2].cbBuffer = t->sizes.cbTrailer;
		b[3].BufferType = SECBUFFER_EMPTY;
		b[3].pvBuffer = NULL;
		b[3].cbBuffer = 0;
		d.ulVersion = SECBUFFER_VERSION;
		d.cBuffers = 4;
		d.pBuffers = b;
		EnterCriticalSection(&t->lock);
		st = EncryptMessage(&t->ctx, 0, &d, 0);
		LeaveCriticalSection(&t->lock);
		if (st != SEC_E_OK ||
		    send_all(t->s, buf, b[0].cbBuffer + b[1].cbBuffer +
		    b[2].cbBuffer) != 0)
			rc = -1;
		p += chunk;
		n -= chunk;
	}
	LeaveCriticalSection(&t->wlock);
	free(buf);
	return (rc);
}

void
win32_tls_free(struct win32_tls *t)
{
	SecBuffer	 b;
	SecBufferDesc	 d;
	DWORD		 type = SCHANNEL_SHUTDOWN, attrs;

	if (t->have_ctx && t->s != INVALID_SOCKET) {
		b.BufferType = SECBUFFER_TOKEN;
		b.pvBuffer = &type;
		b.cbBuffer = sizeof type;
		d.ulVersion = SECBUFFER_VERSION;
		d.cBuffers = 1;
		d.pBuffers = &b;
		EnterCriticalSection(&t->lock);
		if (ApplyControlToken(&t->ctx, &d) == SEC_E_OK) {
			b.BufferType = SECBUFFER_TOKEN;
			b.pvBuffer = NULL;
			b.cbBuffer = 0;
			if (t->server)
				AcceptSecurityContext(&t->cred, &t->ctx, NULL,
				    ASC_REQ_ALLOCATE_MEMORY|ASC_REQ_STREAM, 0,
				    NULL, &d, &attrs, NULL);
			else
				InitializeSecurityContextW(&t->cred, &t->ctx,
				    NULL, ISC_REQ_ALLOCATE_MEMORY|
				    ISC_REQ_STREAM, 0, 0, NULL, 0, NULL, &d,
				    &attrs, NULL);
			if (b.pvBuffer != NULL) {
				send_all(t->s, b.pvBuffer, b.cbBuffer);
				FreeContextBuffer(b.pvBuffer);
			}
		}
		LeaveCriticalSection(&t->lock);
	}
	if (t->s != INVALID_SOCKET) {
		shutdown(t->s, SD_BOTH);
		closesocket(t->s);
		t->s = INVALID_SOCKET;
	}
	if (t->have_ctx)
		DeleteSecurityContext(&t->ctx);
	if (t->have_cred)
		FreeCredentialsHandle(&t->cred);
	t->have_ctx = t->have_cred = 0;
	free(t->in);
	free(t->plain);
	t->in = t->plain = NULL;
	DeleteCriticalSection(&t->lock);
	DeleteCriticalSection(&t->wlock);
}

int
win32_tls_binding(struct win32_tls *t, SecPkgContext_Bindings *b)
{
	memset(b, 0, sizeof *b);
	if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_ENDPOINT_BINDINGS,
	    b) != SEC_E_OK || b->Bindings == NULL)
		return (-1);
	return (0);
}

int
win32_tls_peer_fingerprint(struct win32_tls *t, char *out, size_t outlen)
{
	PCCERT_CONTEXT	cert = NULL;
	int		rc;

	if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
	    &cert) != SEC_E_OK || cert == NULL)
		return (-1);
	rc = win32_tls_cert_fingerprint(cert, out, outlen);
	CertFreeCertificateContext(cert);
	return (rc);
}

int
win32_tls_frame_write(struct win32_tls *t, int channel, const void *data,
    size_t n)
{
	unsigned char	*buf;
	int		 rc;

	if (n > TMUX_REMOTE_FRAME_MAX)
		return (-1);
	if ((buf = malloc(n + TMUX_REMOTE_FRAME_HEADER)) == NULL)
		return (-1);
	tmux_remote_frame_header(buf, channel, n);
	memcpy(buf + TMUX_REMOTE_FRAME_HEADER, data, n);
	rc = win32_tls_write(t, buf, n + TMUX_REMOTE_FRAME_HEADER);
	free(buf);
	return (rc);
}

int
win32_tls_frame_read(struct win32_tls *t, int *channel, unsigned char *buf,
    size_t *n)
{
	unsigned char	h[TMUX_REMOTE_FRAME_HEADER];

	if (win32_tls_read_full(t, h, sizeof h) != 0 ||
	    tmux_remote_frame_parse(h, channel, n) != 0)
		return (-1);
	if (*n != 0 && win32_tls_read_full(t, buf, *n) != 0)
		return (-1);
	buf[*n] = '\0';
	return (0);
}

int
win32_tls_line(struct win32_tls *t, const char *fmt, ...)
{
	char	line[8192];
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof line)
		return (-1);
	return (win32_tls_frame_write(t, TMUX_REMOTE_CONTROL, line,
	    (size_t)n));
}

char *
win32_tls_base64(const void *data, size_t n)
{
	DWORD	 len = 0;
	char	*s;

	if (!CryptBinaryToStringA(data, (DWORD)n,
	    CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF, NULL, &len))
		return (NULL);
	if ((s = malloc(len + 1)) == NULL)
		return (NULL);
	if (!CryptBinaryToStringA(data, (DWORD)n,
	    CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF, s, &len)) {
		free(s);
		return (NULL);
	}
	s[len] = '\0';
	return (s);
}

unsigned char *
win32_tls_unbase64(const char *s, size_t *n)
{
	DWORD		 len = 0;
	unsigned char	*p;

	if (!CryptStringToBinaryA(s, 0, CRYPT_STRING_BASE64, NULL, &len, NULL,
	    NULL))
		return (NULL);
	if ((p = malloc(len + 1)) == NULL)
		return (NULL);
	if (!CryptStringToBinaryA(s, 0, CRYPT_STRING_BASE64, p, &len, NULL,
	    NULL)) {
		free(p);
		return (NULL);
	}
	*n = len;
	return (p);
}

static int
sha256(const void *data, size_t n, unsigned char *out)
{
	return (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)data,
	    (ULONG)n, out, 32) == 0 ? 0 : -1);
}

int
win32_tls_cert_fingerprint(PCCERT_CONTEXT cert, char *out, size_t outlen)
{
	BYTE		*der = NULL;
	DWORD		 derlen = 0;
	unsigned char	 hash[32];
	char		*b64, *eq;

	if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO,
	    &cert->pCertInfo->SubjectPublicKeyInfo, CRYPT_ENCODE_ALLOC_FLAG,
	    NULL, &der, &derlen))
		return (-1);
	if (sha256(der, derlen, hash) != 0) {
		LocalFree(der);
		return (-1);
	}
	LocalFree(der);
	if ((b64 = win32_tls_base64(hash, sizeof hash)) == NULL)
		return (-1);
	if ((eq = strchr(b64, '=')) != NULL)
		*eq = '\0';
	snprintf(out, outlen, "SHA256:%s", b64);
	free(b64);
	return (0);
}

static HCERTSTORE
host_store(void)
{
	return (CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
	    CERT_SYSTEM_STORE_LOCAL_MACHINE, L"MY"));
}

static PCCERT_CONTEXT
host_cert_find(HCERTSTORE store)
{
	PCCERT_CONTEXT	 c = NULL;
	wchar_t		 name[64];
	DWORD		 len;

	while ((c = CertEnumCertificatesInStore(store, c)) != NULL) {
		len = sizeof name;
		if (CertGetCertificateContextProperty(c,
		    CERT_FRIENDLY_NAME_PROP_ID, name, &len) &&
		    wcscmp(name, HOST_CERT_NAME) == 0)
			return (c);
	}
	return (NULL);
}

PCCERT_CONTEXT
win32_tls_host_cert(int create, char *err, size_t errlen)
{
	HCERTSTORE		 store;
	PCCERT_CONTEXT		 cert, added = NULL;
	NCRYPT_PROV_HANDLE	 prov = 0;
	NCRYPT_KEY_HANDLE	 key = 0;
	CRYPT_KEY_PROV_INFO	 kpi;
	CRYPT_ALGORITHM_IDENTIFIER alg;
	CERT_NAME_BLOB		 name;
	CRYPT_DATA_BLOB		 fn;
	SYSTEMTIME		 end;
	BYTE			 namebuf[256];
	DWORD			 policy = 0, len = sizeof namebuf;
	SECURITY_STATUS		 st;

	if ((store = host_store()) == NULL) {
		tls_error(err, errlen, "cannot open the machine certificate "
		    "store", GetLastError());
		return (NULL);
	}
	if ((cert = host_cert_find(store)) != NULL || !create) {
		CertCloseStore(store, 0);
		if (cert == NULL)
			tls_error(err, errlen, "no host key", 0);
		return (cert);
	}

	if ((st = NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER,
	    0)) != ERROR_SUCCESS ||
	    (st = NCryptCreatePersistedKey(prov, &key,
	    BCRYPT_ECDSA_P256_ALGORITHM, HOST_KEY_NAME, 0,
	    NCRYPT_MACHINE_KEY_FLAG|NCRYPT_OVERWRITE_KEY_FLAG)) !=
	    ERROR_SUCCESS ||
	    (st = NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY,
	    (PBYTE)&policy, sizeof policy, NCRYPT_PERSIST_FLAG)) !=
	    ERROR_SUCCESS ||
	    (st = NCryptFinalizeKey(key, 0)) != ERROR_SUCCESS) {
		tls_error(err, errlen, "cannot create the host key", st);
		goto out;
	}
	if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=wtmux-service",
	    CERT_X500_NAME_STR, NULL, namebuf, &len, NULL)) {
		tls_error(err, errlen, "bad certificate name", GetLastError());
		goto out;
	}
	name.pbData = namebuf;
	name.cbData = len;
	memset(&kpi, 0, sizeof kpi);
	kpi.pwszContainerName = HOST_KEY_NAME;
	kpi.pwszProvName = MS_KEY_STORAGE_PROVIDER;
	kpi.dwFlags = NCRYPT_MACHINE_KEY_FLAG;
	memset(&alg, 0, sizeof alg);
	alg.pszObjId = szOID_ECDSA_SHA256;
	GetSystemTime(&end);
	end.wYear += 20;
	if (end.wMonth == 2 && end.wDay == 29)
		end.wDay = 28;
	cert = CertCreateSelfSignCertificate(key, &name, 0, &kpi, &alg, NULL,
	    &end, NULL);
	if (cert == NULL) {
		tls_error(err, errlen, "cannot create the host certificate",
		    GetLastError());
		goto out;
	}
	fn.pbData = (BYTE *)HOST_CERT_NAME;
	fn.cbData = sizeof HOST_CERT_NAME;
	CertSetCertificateContextProperty(cert, CERT_FRIENDLY_NAME_PROP_ID, 0,
	    &fn);
	if (!CertAddCertificateContextToStore(store, cert,
	    CERT_STORE_ADD_REPLACE_EXISTING, &added)) {
		tls_error(err, errlen, "cannot store the host certificate",
		    GetLastError());
		added = NULL;
	}
	CertFreeCertificateContext(cert);
out:
	if (key != 0)
		NCryptFreeObject(key);
	if (prov != 0)
		NCryptFreeObject(prov);
	CertCloseStore(store, 0);
	return (added);
}

int
win32_tls_host_cert_delete(void)
{
	HCERTSTORE		 store;
	PCCERT_CONTEXT		 cert;
	NCRYPT_PROV_HANDLE	 prov;
	NCRYPT_KEY_HANDLE	 key;

	if ((store = host_store()) != NULL) {
		while ((cert = host_cert_find(store)) != NULL)
			CertDeleteCertificateFromStore(cert);
		CertCloseStore(store, 0);
	}
	if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) ==
	    ERROR_SUCCESS) {
		if (NCryptOpenKey(prov, &key, HOST_KEY_NAME, 0,
		    NCRYPT_MACHINE_KEY_FLAG) == ERROR_SUCCESS)
			NCryptDeleteKey(key, 0);
		NCryptFreeObject(prov);
	}
	return (0);
}

int
win32_tls_user_key(int create, NCRYPT_KEY_HANDLE *key, char *err,
    size_t errlen)
{
	static const wchar_t	*providers[] = { MS_PLATFORM_CRYPTO_PROVIDER,
				    MS_KEY_STORAGE_PROVIDER };
	NCRYPT_PROV_HANDLE	 prov;
	DWORD			 policy = 0;
	SECURITY_STATUS		 st = 0;
	int			 i;

	*key = 0;
	for (i = 0; i < 2; i++) {
		if (NCryptOpenStorageProvider(&prov, providers[i], 0) !=
		    ERROR_SUCCESS)
			continue;
		st = NCryptOpenKey(prov, key, USER_KEY_NAME, 0, 0);
		NCryptFreeObject(prov);
		if (st == ERROR_SUCCESS)
			return (0);
	}
	if (!create) {
		tls_error(err, errlen,
		    "no key (wtmux-service keygen makes one)", st);
		return (-1);
	}
	for (i = 0; i < 2; i++) {
		if (NCryptOpenStorageProvider(&prov, providers[i], 0) !=
		    ERROR_SUCCESS)
			continue;
		st = NCryptCreatePersistedKey(prov, key,
		    BCRYPT_ECDSA_P256_ALGORITHM, USER_KEY_NAME, 0, 0);
		if (st == ERROR_SUCCESS) {
			NCryptSetProperty(*key, NCRYPT_EXPORT_POLICY_PROPERTY,
			    (PBYTE)&policy, sizeof policy, NCRYPT_PERSIST_FLAG);
			st = NCryptFinalizeKey(*key, 0);
			if (st != ERROR_SUCCESS) {
				NCryptDeleteKey(*key, 0);
				*key = 0;
			}
		}
		NCryptFreeObject(prov);
		if (st == ERROR_SUCCESS)
			return (0);
	}
	if (st == NTE_PERM) {
		snprintf(err, errlen, "cannot create a key here (0x%08lx): "
		    "this logon cannot keep keys (for example an SSH key "
		    "logon); run it from a desktop session", (unsigned long)st);
	} else
		tls_error(err, errlen, "cannot create a key", st);
	return (-1);
}

char *
win32_tls_user_key_public(NCRYPT_KEY_HANDLE key)
{
	BYTE			 blob[256], point[65];
	DWORD			 len = 0;
	BCRYPT_ECCKEY_BLOB	*h = (BCRYPT_ECCKEY_BLOB *)blob;

	if (NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, NULL, blob,
	    sizeof blob, &len, 0) != ERROR_SUCCESS || h->cbKey != 32)
		return (NULL);
	point[0] = 0x04;
	memcpy(point + 1, blob + sizeof *h, 64);
	return (win32_tls_base64(point, sizeof point));
}

int
win32_tls_user_key_sign(NCRYPT_KEY_HANDLE key, const void *data, size_t n,
    unsigned char *sig, DWORD *siglen)
{
	unsigned char	hash[32];
	DWORD		size = *siglen;

	if (sha256(data, n, hash) != 0)
		return (-1);
	return (NCryptSignHash(key, NULL, hash, sizeof hash, sig, size,
	    siglen, 0) == ERROR_SUCCESS ? 0 : -1);
}

int
win32_tls_key_verify(const char *public, const void *data, size_t n,
    const unsigned char *sig, DWORD siglen)
{
	BCRYPT_KEY_HANDLE	 h;
	BYTE			 blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
	BCRYPT_ECCKEY_BLOB	*k = (BCRYPT_ECCKEY_BLOB *)blob;
	unsigned char		*point, hash[32];
	size_t			 len;
	NTSTATUS		 st;

	if ((point = win32_tls_unbase64(public, &len)) == NULL)
		return (-1);
	if (len != 65 || point[0] != 0x04 || siglen != 64) {
		free(point);
		return (-1);
	}
	k->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
	k->cbKey = 32;
	memcpy(blob + sizeof *k, point + 1, 64);
	free(point);
	if (BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, NULL,
	    BCRYPT_ECCPUBLIC_BLOB, &h, blob, sizeof blob, 0) != 0)
		return (-1);
	st = -1;
	if (sha256(data, n, hash) == 0)
		st = BCryptVerifySignature(h, NULL, hash, sizeof hash,
		    (PUCHAR)sig, siglen, 0);
	BCryptDestroyKey(h);
	return (st == 0 ? 0 : -1);
}
