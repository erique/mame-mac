// cc -O2 -Wall -o mame-tap-helper mame_tap_helper.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        die("helper: missing socket fd");

    int sockfd = atoi(argv[1]);

    int tunfd = open("/dev/net/tun", O_RDWR);
    if (tunfd < 0)
        die("open(/dev/net/tun)");

    struct ifreq ifr = {0};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    snprintf(ifr.ifr_name, IFNAMSIZ, "tap-mess-%d-0", getuid());

    if (ioctl(tunfd, TUNSETIFF, &ifr) < 0)
        die("TUNSETIFF");

    /* send fd + ifname */
    struct msghdr msg = {0};
    struct iovec iov;

    iov.iov_base = ifr.ifr_name;
    iov.iov_len  = IFNAMSIZ;
    msg.msg_iov  = &iov;
    msg.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &tunfd, sizeof(int));

    if (sendmsg(sockfd, &msg, 0) < 0)
        die("sendmsg");

    return 0;
}
