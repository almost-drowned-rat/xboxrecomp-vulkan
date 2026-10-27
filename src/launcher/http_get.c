/* http_get.c -- see http_get.h. */

#include "http_get.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *xr_http_class_name(int c)
{
    switch (c) {
    case XR_HTTP_OK: return "ok";
    case XR_HTTP_DNS: return "DNS";
    case XR_HTTP_CONNECT: return "connect";
    case XR_HTTP_TLS: return "TLS";
    case XR_HTTP_STATUS: return "HTTP status";
    case XR_HTTP_FILE: return "file";
    case XR_HTTP_UNSUPPORTED: return "unsupported";
    default: return "other";
    }
}

#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4   /* Windows 8.1+; older ones and some Wine versions refuse it */
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

#define READ_CHUNK (256u << 10)

typedef struct { DWORD secure_flags; } Ctx;

/* the certificate problem behind ERROR_WINHTTP_SECURE_FAILURE */
static void CALLBACK status_cb(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len)
{
    (void)h;
    if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && ctx && info && len >= sizeof(DWORD))
        ((Ctx *)ctx)->secure_flags = *(DWORD *)info;
}

static int classify(DWORD e)
{
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return XR_HTTP_DNS;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_TIMEOUT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return XR_HTTP_CONNECT;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return XR_HTTP_TLS;
    default:
        return XR_HTTP_OTHER;
    }
}

static const char *tls_reason(DWORD f)
{
    if (f & WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA)
        return "the certificate's issuer is not trusted - is the certificate store there?";
    if (f & WINHTTP_CALLBACK_STATUS_FLAG_CERT_DATE_INVALID)
        return "the certificate is out of date - is the computer's clock right?";
    if (f & WINHTTP_CALLBACK_STATUS_FLAG_CERT_CN_INVALID)
        return "the certificate is for another name (a proxy or filter in between?)";
    if (f & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REVOKED)
        return "the certificate was revoked";
    if (f & WINHTTP_CALLBACK_STATUS_FLAG_CERT_REV_FAILED)
        return "the revocation check failed";
    if (f & WINHTTP_CALLBACK_STATUS_FLAG_SECURITY_CHANNEL_ERROR)
        return "no common TLS version / cipher";
    return "the TLS handshake failed";
}

static void describe(int c, DWORD e, DWORD tls, const char *host, const char *stage, char *msg, size_t n)
{
    switch (c) {
    case XR_HTTP_DNS:
        snprintf(msg, n, "the name %s could not be resolved (DNS): no internet connection, or the DNS server does "
                         "not answer - WinHTTP error %lu", host, (unsigned long)e);
        break;
    case XR_HTTP_CONNECT:
        snprintf(msg, n, "no connection to %s (%s): offline, a firewall or a proxy in the way - WinHTTP error %lu",
                 host, e == ERROR_WINHTTP_TIMEOUT ? "timed out" : e == ERROR_WINHTTP_CONNECTION_ERROR
                 ? "the connection dropped" : "refused or unreachable", (unsigned long)e);
        break;
    case XR_HTTP_TLS:
        snprintf(msg, n, "the secure connection to %s failed (TLS): %s (Wine / Proton: the host's ca-certificates "
                         "and 64-bit GnuTLS are needed) - WinHTTP error %lu, flags 0x%lx",
                 host, tls_reason(tls), (unsigned long)e, (unsigned long)tls);
        break;
    default:
        snprintf(msg, n, "the download from %s failed (%s) - WinHTTP error %lu", host, stage, (unsigned long)e);
        break;
    }
}

static int widen(const char *s, WCHAR *w, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0;
}

int xr_http_get_file(const char *url, const char *path, SDL_AtomicInt *bytes, char *msg, size_t n)
{
    static WCHAR wurl[2048], host[256], upath[2048], extra[1024], target[3072];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    Ctx ctx = { 0 };
    char hosta[256] = "?";
    SDL_IOStream *out = NULL;
    DWORD status = 0, sz = sizeof status, err = 0, proto;
    const char *stage = "start";
    void *buf = NULL;
    int rc = XR_HTTP_OTHER;

    if (bytes)
        SDL_SetAtomicInt(bytes, 0);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = sizeof host / sizeof host[0];
    uc.lpszUrlPath = upath;
    uc.dwUrlPathLength = sizeof upath / sizeof upath[0];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = sizeof extra / sizeof extra[0];
    if (!widen(url, wurl, sizeof wurl / sizeof wurl[0]) || !WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        snprintf(msg, n, "not a download address: %s", url);
        return XR_HTTP_OTHER;
    }
    _snwprintf(target, sizeof target / sizeof target[0] - 1, L"%ls%ls", upath, extra);
    target[sizeof target / sizeof target[0] - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, host, -1, hosta, sizeof hosta, NULL, NULL);

    /* the system's proxy settings (automatic first, then the WinHTTP default) */
    ses = WinHttpOpen(L"xboxrecomp-launcher/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        ses = WinHttpOpen(L"xboxrecomp-launcher/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        goto fail;
    /* TLS 1.2 + 1.3 where the system knows 1.3; refused flags only mean the default stays */
    proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof proto)) {
        proto = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &proto, sizeof proto);
    }
    WinHttpSetTimeouts(ses, 0, 30000, 30000, 60000);     /* resolve, connect, send, each receive */
    stage = "connect";
    if (!(con = WinHttpConnect(ses, host, uc.nPort, 0)))
        goto fail;
    stage = "request";
    req = WinHttpOpenRequest(con, L"GET", target, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        goto fail;
    WinHttpSetStatusCallback(req, status_cb, WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0);
    stage = "send";
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR)&ctx))
        goto fail;
    stage = "response";
    if (!WinHttpReceiveResponse(req, NULL))
        goto fail;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &sz, WINHTTP_NO_HEADER_INDEX))
        goto fail;
    if (status != 200) {
        snprintf(msg, n, "the server %s answered HTTP %lu%s", hosta, (unsigned long)status,
                 status == 404 ? " (not found)" : status >= 500 ? " (a server problem - try again later)" : "");
        rc = XR_HTTP_STATUS;
        goto done;
    }
    if (!(out = SDL_IOFromFile(path, "wb")) || !(buf = malloc(READ_CHUNK))) {
        snprintf(msg, n, "cannot write %s (%s)", path, out ? "out of memory" : SDL_GetError());
        rc = XR_HTTP_FILE;
        goto done;
    }
    stage = "read";
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, READ_CHUNK, &got))
            goto fail;
        if (!got)
            break;
        if (SDL_WriteIO(out, buf, got) != got) {
            snprintf(msg, n, "cannot write %s (%s) - is the disk full?", path, SDL_GetError());
            rc = XR_HTTP_FILE;
            goto done;
        }
        if (bytes)
            SDL_AddAtomicInt(bytes, (int)got);
    }
    if (!SDL_CloseIO(out)) {
        out = NULL;
        snprintf(msg, n, "cannot write %s (%s)", path, SDL_GetError());
        rc = XR_HTTP_FILE;
        goto done;
    }
    out = NULL;
    snprintf(msg, n, "downloaded from %s", hosta);
    rc = XR_HTTP_OK;
    goto done;

fail:
    err = GetLastError();
    rc = classify(err);
    describe(rc, err, ctx.secure_flags, hosta, stage, msg, n);
done:
    if (out)
        SDL_CloseIO(out);
    if (rc != XR_HTTP_OK)
        SDL_RemovePath(path);
    free(buf);
    if (req) {
        WinHttpSetStatusCallback(req, NULL, 0, 0);
        WinHttpCloseHandle(req);
    }
    if (con)
        WinHttpCloseHandle(con);
    if (ses)
        WinHttpCloseHandle(ses);
    return rc;
}

#else

int xr_http_get_file(const char *url, const char *path, SDL_AtomicInt *bytes, char *msg, size_t n)
{
    (void)path;
    if (bytes)
        SDL_SetAtomicInt(bytes, 0);
    snprintf(msg, n, "no built-in download on this platform - fetch %s by hand", url);
    return XR_HTTP_UNSUPPORTED;
}

#endif
