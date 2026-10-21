/* netinit.c - stack bringup for Watt-32 (DOS) and POSIX (host tests). */
#include "netinit.h"

#include <stdio.h>
#include <string.h>

static char g_err[256];

const char *net_lasterr(void)
{
    return g_err[0] ? g_err : "no error";
}

#ifdef DOSYPP_WATT32

#include <tcp.h>

int net_startup(void)
{
    int rc;

    /* Watt-32 calls exit() on these failures unless told to survive them,
       which would rip the screen out from under us once the UI exists. We
       want to report the problem on our own terms instead. */
    survive_eth = 1;
    survive_bootp = 1;
    survive_dhcp = 1;
    survive_rarp = 1;

    rc = sock_init();
    if (rc != 0) {
        snprintf(g_err, sizeof g_err,
                 "TCP/IP init failed: %s (code %d).\n"
                 "  * Is a packet driver loaded?  NE2000.COM for ISA cards and\n"
                 "    DOSBox-X, RTSPKT.COM for v86's RTL8029.\n"
                 "  * Is WATTCP.CFG present, or is a DHCP server reachable?",
                 sock_init_err(rc), rc);
        return -1;
    }

    /* With the survive_* flags set, sock_init() succeeds even when address
       configuration did not -- so an unset address is its own failure. */
    if (my_ip_addr == 0) {
        snprintf(g_err, sizeof g_err,
                 "No IP address.\n"
                 "  * DHCP got no reply, or WATTCP.CFG has no my_ip.\n"
                 "  * The packet driver loaded, so the card is being seen.");
        return -1;
    }

    return 0;
}

void net_shutdown(void)
{
    sock_exit();
}

void net_yield(void)
{
    tcp_tick(NULL);
}

unsigned long net_local_ip(void)
{
    return (unsigned long)my_ip_addr;
}

#else /* POSIX host build */

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int net_startup(void)
{
    g_err[0] = '\0';
    return 0;
}

void net_shutdown(void)
{
}

void net_yield(void)
{
    usleep(1000);
}

unsigned long net_local_ip(void)
{
    /* No packets are sent by connecting a UDP socket; it just makes the
       kernel pick the source address it would use for off-link traffic. */
    struct sockaddr_in to, me;
    socklen_t len = sizeof me;
    unsigned long ip = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return 0;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = inet_addr("192.0.2.1");

    if (connect(fd, (struct sockaddr *)&to, sizeof to) == 0 &&
        getsockname(fd, (struct sockaddr *)&me, &len) == 0)
        ip = ntohl(me.sin_addr.s_addr);

    close(fd);
    return ip;
}

#endif
