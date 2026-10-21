/* http.h - fetch a URL to a file. As little HTTP as that needs.
 *
 * This exists for one job: public hub lists are files on the web, usually
 * bzip2-compressed XML, and a client that cannot fetch one has no way to
 * find a hub except being told an address.
 *
 * Deliberately small, and deliberately not general:
 *
 *   - GET only. No POST, no authentication, no cookies, no keep-alive.
 *   - HTTP/1.0, so the server closes the connection and there is no
 *     chunked encoding to decode. Servers that insist on 1.1 still answer
 *     a 1.0 request correctly; that is what 1.0 is for.
 *   - No TLS. An https:// URL fails with a message saying so rather than
 *     pretending. Same position as the hub connection: encryption is the
 *     relay's job until BearSSL lands (TODO.md).
 *   - Straight to a file. Nothing is buffered in memory, because a hublist
 *     is a megabyte and this machine has sixteen.
 *
 * Everything from the network is treated as hostile: the status line, the
 * headers and the body are all bounded, and a redirect is followed only a
 * couple of times and only to another http:// URL.
 */
#ifndef DOSYPP_HTTP_H
#define DOSYPP_HTTP_H

#include <stddef.h>

/* Bytes we will take for a body. A hublist is a few hundred KB; anything
   far bigger is either not a hublist or not one this machine wants. */
#define HTTP_MAX_BODY (4UL * 1024UL * 1024UL)

/* How many redirects to follow. Hublist URLs move, and the ones that move
   usually move once. */
#define HTTP_MAX_REDIRECTS 3

/* Fetch `url` into `path`. Returns the number of bytes written, or -1 with
   the reason available from http_error().
 *
 * `progress` is called as bytes arrive, with the running total and the
 * content length when the server gave one (0 when it did not). It may be
 * NULL. Returning non-zero from it cancels the fetch -- which is how a
 * client with one thread stays answerable while a slow download runs. */
typedef int (*http_progress)(void *user, unsigned long got,
                             unsigned long total);

long http_get(const char *url, const char *path,
              http_progress progress, void *user);

/* The last failure, as a sentence. Never NULL. */
const char *http_error(void);

/* Split a URL into its parts. Exposed because it is the part with the
   edge cases, and it is worth testing on its own. Returns 0 on success.
   `path_out` always starts with '/'. */
int http_split_url(const char *url, char *host_out, size_t host_sz,
                   int *port_out, char *path_out, size_t path_sz);

#endif /* DOSYPP_HTTP_H */
