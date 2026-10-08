/* fujitcp_host.h -- what fujitcp_host.c adds to the staged fujitcp.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (matches the staged fujitcp.h it extends; copyright-holders Thomas Cherryhomes)
 */

#ifndef FUJITCP_HOST_H
#define FUJITCP_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Wake a transaction blocked on the socket in another thread; it fails with
 * FB_ENOLINK and the socket is closed. Safe from any thread, and a no-op
 * when there is no socket. */
void fujitcp_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* FUJITCP_HOST_H */
