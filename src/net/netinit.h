/* netinit.h - TCP/IP stack bringup. The only part of net/ that differs
 * meaningfully between DOS and the host build. */
#ifndef DOSYPP_NETINIT_H
#define DOSYPP_NETINIT_H

/* Bring up the stack. On DOS this locates the packet driver and runs DHCP or
 * reads WATTCP.CFG; on the host it does nothing. Returns 0 on success, -1 on
 * failure with a human-readable reason in net_lasterr(). */
int net_startup(void);

void net_shutdown(void);

const char *net_lasterr(void);

/* Give the stack a slice. Watt-32 needs regular servicing from the main loop;
 * on the host this is a short sleep so busy-waits don't spin a core. */
void net_yield(void);

/* Local IPv4 address in host byte order, 0 if unknown. Needed later for
 * active-mode CTM. */
unsigned long net_local_ip(void);

#endif /* DOSYPP_NETINIT_H */
