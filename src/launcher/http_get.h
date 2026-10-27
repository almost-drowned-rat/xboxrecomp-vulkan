/* http_get.h -- one HTTPS download into a file, in-process (the launcher's compiler fetch).
 *
 * Windows: WinHTTP (the system's proxy settings, its TLS and certificate store; redirects followed). No external
 * program (no curl.exe / tar.exe / powershell - Wine and Proton do not have them) and no "is the network up"
 * check before (Wine answers those wrongly): the download is simply tried and the real error comes back, sorted
 * into a class with a message for the player (DNS, connect, TLS, HTTP status). Other platforms: not built in
 * (XR_HTTP_UNSUPPORTED). */
#ifndef XR_HTTP_GET_H
#define XR_HTTP_GET_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XR_HTTP_OK = 0,
    XR_HTTP_DNS,             /* the host name could not be resolved */
    XR_HTTP_CONNECT,         /* no connection / timeout / connection dropped */
    XR_HTTP_TLS,             /* the secure connection failed (certificate, protocol) */
    XR_HTTP_STATUS,          /* the server answered, but not 200 */
    XR_HTTP_FILE,            /* the target file could not be written */
    XR_HTTP_OTHER,
    XR_HTTP_UNSUPPORTED
};

/* GETs url into path (replaced; removed again on any error). bytes (optional) counts the bytes written so far
 * for a progress bar. msg gets a one-line description of the outcome. Returns an XR_HTTP_* class. */
int xr_http_get_file(const char *url, const char *path, SDL_AtomicInt *bytes, char *msg, size_t n);

/* a short name of the class ("DNS", "connect", ...) */
const char *xr_http_class_name(int c);

#ifdef __cplusplus
}
#endif

#endif
