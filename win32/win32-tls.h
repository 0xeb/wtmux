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

#ifndef WIN32_TLS_H
#define WIN32_TLS_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>

struct win32_tls {
	SOCKET				 s;
	CredHandle			 cred;
	CtxtHandle			 ctx;
	int				 have_cred;
	int				 have_ctx;
	int				 server;
	SecPkgContext_StreamSizes	 sizes;
	unsigned char			*in;
	size_t				 inlen;
	size_t				 insize;
	unsigned char			*plain;
	size_t				 plainlen;
	size_t				 plainoff;
	CRITICAL_SECTION		 lock;
	CRITICAL_SECTION		 wlock;
	int				 eof;
};

int	 win32_tls_client(struct win32_tls *, SOCKET, const char *, char *,
	     size_t);
int	 win32_tls_server(struct win32_tls *, SOCKET, PCCERT_CONTEXT, char *,
	     size_t);
int	 win32_tls_read(struct win32_tls *, void *, size_t);
int	 win32_tls_read_full(struct win32_tls *, void *, size_t);
int	 win32_tls_write(struct win32_tls *, const void *, size_t);
void	 win32_tls_free(struct win32_tls *);
int	 win32_tls_binding(struct win32_tls *, SecPkgContext_Bindings *);
int	 win32_tls_peer_fingerprint(struct win32_tls *, char *, size_t);

int	 win32_tls_frame_write(struct win32_tls *, int, const void *, size_t);
int	 win32_tls_frame_read(struct win32_tls *, int *, unsigned char *,
	     size_t *);
int	 win32_tls_line(struct win32_tls *, const char *, ...);

PCCERT_CONTEXT win32_tls_host_cert(int, char *, size_t);
int	 win32_tls_host_cert_delete(void);
int	 win32_tls_cert_fingerprint(PCCERT_CONTEXT, char *, size_t);

int	 win32_tls_user_key(int, NCRYPT_KEY_HANDLE *, char *, size_t);
char	*win32_tls_user_key_public(NCRYPT_KEY_HANDLE);
int	 win32_tls_user_key_sign(NCRYPT_KEY_HANDLE, const void *, size_t,
	     unsigned char *, DWORD *);
int	 win32_tls_key_verify(const char *, const void *, size_t,
	     const unsigned char *, DWORD);

char	*win32_tls_base64(const void *, size_t);
unsigned char *win32_tls_unbase64(const char *, size_t *);

#endif
