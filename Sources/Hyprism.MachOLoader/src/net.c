// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// BSD sockets with Darwin constants and layouts, plus kqueue on top of epoll.

#define _GNU_SOURCE
#include <errno.h>
#include <stdatomic.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "macho.h"

#define TRACE(...) do { if (getenv("MACHO_TRACE")) macho_log("net", __VA_ARGS__); } while (0)

static int fail(int e)
{
    errno = e;
    return -1;
}

// Logs the first unexpected socket call failures; would-block results are normal.
static ssize_t traced(const char *call, int fd, ssize_t rc)
{
    static _Atomic int logged;
    if (rc < 0 && errno != EAGAIN && errno != EINPROGRESS && errno != EINTR && atomic_fetch_add(&logged, 1) < 40) {
        int e = errno;
        macho_log("net", "%s(fd %d) failed: %s (%d)", call, fd, strerror(e), e);
        errno = e;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Addresses

#define APPLE_AF_INET6 30

static int family_from_apple(int f)
{
    switch (f) {
    case 0: return AF_UNSPEC;
    case 1: return AF_UNIX;
    case 2: return AF_INET;
    case APPLE_AF_INET6: return AF_INET6;
    default: return -1;
    }
}

static int family_to_apple(int f)
{
    switch (f) {
    case AF_UNSPEC: return 0;
    case AF_UNIX: return 1;
    case AF_INET: return 2;
    case AF_INET6: return APPLE_AF_INET6;
    default: return f;
    }
}

// Darwin sockaddr: uint8 len, uint8 family, then the family payload.
static int sockaddr_from_apple(const void *in, socklen_t in_len, struct sockaddr_storage *out, socklen_t *out_len)
{
    const uint8_t *a = in;
    if (!in || in_len < 2)
        return fail(EINVAL);
    int family = family_from_apple(a[1]);
    if (family < 0)
        return fail(EAFNOSUPPORT);
    memset(out, 0, sizeof *out);
    out->ss_family = (sa_family_t)family;
    size_t payload = in_len - 2;
    if (family == AF_UNIX) {
        struct sockaddr_un *u = (struct sockaddr_un *)out;
        if (payload >= sizeof u->sun_path)
            payload = sizeof u->sun_path - 1;
        memcpy(u->sun_path, a + 2, payload);
        *out_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strnlen(u->sun_path, payload) + 1);
        return 0;
    }
    if (payload > sizeof *out - 2)
        payload = sizeof *out - 2;
    memcpy((uint8_t *)out + 2, a + 2, payload);
    *out_len = family == AF_INET ? sizeof(struct sockaddr_in) : family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                                                   : (socklen_t)(payload + 2);
    return 0;
}

static void sockaddr_to_apple(const struct sockaddr *in, socklen_t in_len, void *out, socklen_t *out_len)
{
    if (!out || !out_len)
        return;
    uint8_t tmp[sizeof(struct sockaddr_storage)];
    memset(tmp, 0, sizeof tmp);
    socklen_t len = in_len;
    if (in->sa_family == AF_INET)
        len = sizeof(struct sockaddr_in);
    else if (in->sa_family == AF_INET6)
        len = sizeof(struct sockaddr_in6);
    if (len > sizeof tmp)
        len = sizeof tmp;
    memcpy(tmp + 2, (const uint8_t *)in + 2, len > 2 ? len - 2 : 0);
    tmp[0] = (uint8_t)(in->sa_family == AF_UNIX ? 106 : len);
    tmp[1] = (uint8_t)family_to_apple(in->sa_family);
    socklen_t copy = *out_len < len ? *out_len : len;
    memcpy(out, tmp, copy);
    *out_len = len;
}

// ---------------------------------------------------------------------------
// Socket calls

static int apple_socket(int domain, int type, int protocol)
{
    int family = family_from_apple(domain);
    if (family < 0)
        return fail(EAFNOSUPPORT);
    int fd = socket(family, type, protocol);
    TRACE("socket(%d, %d, %d) = %d", domain, type, protocol, fd);
    return fd;
}

static int apple_bind(int fd, const void *addr, socklen_t len)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    if (sockaddr_from_apple(addr, len, &ss, &sl) != 0)
        return -1;
    return bind(fd, (struct sockaddr *)&ss, sl);
}

static int apple_connect(int fd, const void *addr, socklen_t len)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    if (sockaddr_from_apple(addr, len, &ss, &sl) != 0)
        return -1;
    return (int)traced("connect", fd, connect(fd, (struct sockaddr *)&ss, sl));
}

// connectx(fd, sa_endpoints_t *, assoc, flags, iov, iovcnt, len, connid)
struct apple_sa_endpoints {
    unsigned int sae_srcif;
    const void *sae_srcaddr;
    socklen_t sae_srcaddrlen;
    const void *sae_dstaddr;
    socklen_t sae_dstaddrlen;
};

static int apple_connectx(int fd, const struct apple_sa_endpoints *ep, unsigned assoc, unsigned flags,
                          const void *iov, unsigned iovcnt, size_t *len, void *connid)
{
    (void)assoc; (void)flags; (void)iov; (void)iovcnt; (void)connid;
    if (len)
        *len = 0;
    if (ep->sae_srcaddr && apple_bind(fd, ep->sae_srcaddr, ep->sae_srcaddrlen) != 0)
        return -1;
    return apple_connect(fd, ep->sae_dstaddr, ep->sae_dstaddrlen);
}

static int apple_disconnectx(int fd, unsigned assoc, unsigned connid)
{
    (void)assoc; (void)connid;
    return shutdown(fd, SHUT_RDWR);
}

static int apple_getsockname(int fd, void *addr, socklen_t *len)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int rc = getsockname(fd, (struct sockaddr *)&ss, &sl);
    if (rc == 0)
        sockaddr_to_apple((struct sockaddr *)&ss, sl, addr, len);
    return rc;
}

static int apple_getpeername(int fd, void *addr, socklen_t *len)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int rc = getpeername(fd, (struct sockaddr *)&ss, &sl);
    if (rc == 0)
        sockaddr_to_apple((struct sockaddr *)&ss, sl, addr, len);
    return rc;
}

static int msg_flags_from_apple(int f)
{
    int h = f & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);
    if (f & 0x8) h |= MSG_EOR;
    if (f & 0x40) h |= MSG_WAITALL;
    if (f & 0x80) h |= MSG_DONTWAIT;
    return h | MSG_NOSIGNAL; // Darwin programs rely on SO_NOSIGPIPE instead
}

static int msg_flags_to_apple(int h)
{
    int f = h & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);
    if (h & MSG_EOR) f |= 0x8;
    if (h & MSG_TRUNC) f |= 0x10;
    if (h & MSG_CTRUNC) f |= 0x20;
    return f;
}

static ssize_t apple_send(int fd, const void *buf, size_t len, int flags) { return traced("send", fd, send(fd, buf, len, msg_flags_from_apple(flags))); }
static ssize_t apple_recv(int fd, void *buf, size_t len, int flags) { return traced("recv", fd, recv(fd, buf, len, msg_flags_from_apple(flags) & ~MSG_NOSIGNAL)); }

struct apple_msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    int msg_iovlen;
    void *msg_control;
    socklen_t msg_controllen;
    int msg_flags;
};

// Darwin cmsghdr: u32 len, i32 level, i32 type, data aligned to 4.
#define APPLE_CMSG_ALIGN(n) (((n) + 3u) & ~3u)
#define APPLE_IP_PKTINFO 26
#define APPLE_IPV6_PKTINFO 46

static ssize_t apple_recvmsg(int fd, struct apple_msghdr *m, int flags)
{
    struct sockaddr_storage ss;
    uint8_t control[256];
    struct msghdr h = {
        .msg_name = m->msg_name ? &ss : NULL,
        .msg_namelen = sizeof ss,
        .msg_iov = m->msg_iov,
        .msg_iovlen = (size_t)m->msg_iovlen,
        .msg_control = m->msg_controllen ? control : NULL,
        .msg_controllen = m->msg_controllen ? sizeof control : 0,
    };
    ssize_t n = traced("recvmsg", fd, recvmsg(fd, &h, msg_flags_from_apple(flags) & ~MSG_NOSIGNAL));
    if (n < 0)
        return n;
    if (m->msg_name)
        sockaddr_to_apple((struct sockaddr *)&ss, h.msg_namelen, m->msg_name, &m->msg_namelen);
    socklen_t used = 0;
    for (struct cmsghdr *c = h.msg_control ? CMSG_FIRSTHDR(&h) : NULL; c; c = CMSG_NXTHDR(&h, c)) {
        int type = c->cmsg_type;
        if (c->cmsg_level == IPPROTO_IP && type == IP_PKTINFO) {
            // Linux in_pktinfo {ifindex, spec_dst, addr}; Darwin {ifindex, spec_dst, addr} shares the layout.
            type = APPLE_IP_PKTINFO;
        } else if (c->cmsg_level == IPPROTO_IPV6 && type == IPV6_PKTINFO) {
            type = APPLE_IPV6_PKTINFO;
        } else {
            continue;
        }
        size_t data_len = c->cmsg_len - CMSG_LEN(0);
        size_t need = APPLE_CMSG_ALIGN(12) + data_len;
        if (used + need > m->msg_controllen)
            break;
        uint8_t *out = (uint8_t *)m->msg_control + used;
        uint32_t hdr[3] = { (uint32_t)(APPLE_CMSG_ALIGN(12) + data_len), (uint32_t)c->cmsg_level, (uint32_t)type };
        memcpy(out, hdr, sizeof hdr);
        memcpy(out + APPLE_CMSG_ALIGN(12), CMSG_DATA(c), data_len);
        used += (socklen_t)APPLE_CMSG_ALIGN(need);
    }
    m->msg_controllen = used;
    m->msg_flags = msg_flags_to_apple(h.msg_flags);
    return n;
}

static ssize_t apple_sendmsg(int fd, const struct apple_msghdr *m, int flags)
{
    struct sockaddr_storage ss;
    socklen_t sl = 0;
    if (m->msg_name && sockaddr_from_apple(m->msg_name, m->msg_namelen, &ss, &sl) != 0)
        return -1;
    struct msghdr h = {
        .msg_name = m->msg_name ? &ss : NULL,
        .msg_namelen = sl,
        .msg_iov = m->msg_iov,
        .msg_iovlen = (size_t)m->msg_iovlen,
    };
    return traced("sendmsg", fd, sendmsg(fd, &h, msg_flags_from_apple(flags)));
}

// ---------------------------------------------------------------------------
// Socket options

struct option_map { int apple_level, apple_name, level, name; };

static const struct option_map k_options[] = {
    { 0xffff, 0x0001, SOL_SOCKET, SO_DEBUG }, { 0xffff, 0x0002, SOL_SOCKET, SO_ACCEPTCONN },
    { 0xffff, 0x0004, SOL_SOCKET, SO_REUSEADDR }, { 0xffff, 0x0008, SOL_SOCKET, SO_KEEPALIVE },
    { 0xffff, 0x0010, SOL_SOCKET, SO_DONTROUTE }, { 0xffff, 0x0020, SOL_SOCKET, SO_BROADCAST },
    { 0xffff, 0x0080, SOL_SOCKET, SO_LINGER }, { 0xffff, 0x1080, SOL_SOCKET, SO_LINGER },
    { 0xffff, 0x0100, SOL_SOCKET, SO_OOBINLINE }, { 0xffff, 0x0200, SOL_SOCKET, SO_REUSEPORT },
    { 0xffff, 0x0400, SOL_SOCKET, SO_TIMESTAMP }, { 0xffff, 0x1001, SOL_SOCKET, SO_SNDBUF },
    { 0xffff, 0x1002, SOL_SOCKET, SO_RCVBUF }, { 0xffff, 0x1003, SOL_SOCKET, SO_SNDLOWAT },
    { 0xffff, 0x1004, SOL_SOCKET, SO_RCVLOWAT }, { 0xffff, 0x1005, SOL_SOCKET, SO_SNDTIMEO },
    { 0xffff, 0x1006, SOL_SOCKET, SO_RCVTIMEO }, { 0xffff, 0x1007, SOL_SOCKET, SO_ERROR },
    { 0xffff, 0x1008, SOL_SOCKET, SO_TYPE },
    { IPPROTO_IP, 2, IPPROTO_IP, IP_HDRINCL }, { IPPROTO_IP, 3, IPPROTO_IP, IP_TOS },
    { IPPROTO_IP, 4, IPPROTO_IP, IP_TTL }, { IPPROTO_IP, 9, IPPROTO_IP, IP_MULTICAST_IF },
    { IPPROTO_IP, 10, IPPROTO_IP, IP_MULTICAST_TTL }, { IPPROTO_IP, 11, IPPROTO_IP, IP_MULTICAST_LOOP },
    { IPPROTO_IP, 12, IPPROTO_IP, IP_ADD_MEMBERSHIP }, { IPPROTO_IP, 13, IPPROTO_IP, IP_DROP_MEMBERSHIP },
    { IPPROTO_IP, 26, IPPROTO_IP, IP_PKTINFO }, { IPPROTO_IP, 27, IPPROTO_IP, IP_RECVTOS },
    { IPPROTO_IPV6, 4, IPPROTO_IPV6, IPV6_UNICAST_HOPS }, { IPPROTO_IPV6, 9, IPPROTO_IPV6, IPV6_MULTICAST_IF },
    { IPPROTO_IPV6, 10, IPPROTO_IPV6, IPV6_MULTICAST_HOPS }, { IPPROTO_IPV6, 11, IPPROTO_IPV6, IPV6_MULTICAST_LOOP },
    { IPPROTO_IPV6, 12, IPPROTO_IPV6, IPV6_JOIN_GROUP }, { IPPROTO_IPV6, 13, IPPROTO_IPV6, IPV6_LEAVE_GROUP },
    { IPPROTO_IPV6, 27, IPPROTO_IPV6, IPV6_V6ONLY }, { IPPROTO_IPV6, 61, IPPROTO_IPV6, IPV6_RECVPKTINFO },
    { IPPROTO_IPV6, 46, IPPROTO_IPV6, IPV6_PKTINFO }, { IPPROTO_IPV6, 62, IPPROTO_IPV6, IPV6_DONTFRAG },
    { IPPROTO_IPV6, 36, IPPROTO_IPV6, IPV6_TCLASS },
    { IPPROTO_TCP, 0x01, IPPROTO_TCP, TCP_NODELAY }, { IPPROTO_TCP, 0x02, IPPROTO_TCP, TCP_MAXSEG },
    { IPPROTO_TCP, 0x10, IPPROTO_TCP, TCP_KEEPIDLE }, { IPPROTO_TCP, 0x101, IPPROTO_TCP, TCP_KEEPINTVL },
    { IPPROTO_TCP, 0x102, IPPROTO_TCP, TCP_KEEPCNT }, { IPPROTO_TCP, 0x105, IPPROTO_TCP, TCP_FASTOPEN },
};

static const struct option_map *find_option(int level, int name)
{
    for (size_t i = 0; i < SHIM_COUNT(k_options); i++)
        if (k_options[i].apple_level == level && k_options[i].apple_name == name)
            return &k_options[i];
    return NULL;
}

#define APPLE_SO_NOSIGPIPE 0x1022
#define APPLE_SO_NREAD 0x1020
#define APPLE_IP_DONTFRAG 28

static int apple_setsockopt(int fd, int level, int name, const void *value, socklen_t len)
{
    if (level == 0xffff && name == APPLE_SO_NOSIGPIPE)
        return 0; // every send already uses MSG_NOSIGNAL
    if (level == IPPROTO_IP && name == APPLE_IP_DONTFRAG) {
        int on = value && len >= 4 && *(const int *)value;
        int mode = on ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        return setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &mode, sizeof mode);
    }
    const struct option_map *o = find_option(level, name);
    if (!o) {
        TRACE("setsockopt unsupported level %#x name %#x", level, name);
        return fail(ENOPROTOOPT);
    }
    if (o->level == SOL_SOCKET && (o->name == SO_SNDTIMEO || o->name == SO_RCVTIMEO) && len >= 12) {
        // Darwin timeval is {long, int}; widen the microseconds.
        struct timeval tv = { *(const long *)value, *(const int *)((const uint8_t *)value + 8) };
        return setsockopt(fd, o->level, o->name, &tv, sizeof tv);
    }
    return setsockopt(fd, o->level, o->name, value, len);
}

static int apple_getsockopt(int fd, int level, int name, void *value, socklen_t *len)
{
    if (level == 0xffff && name == APPLE_SO_NOSIGPIPE) {
        if (value && *len >= 4)
            *(int *)value = 1;
        *len = 4;
        return 0;
    }
    if (level == 0xffff && name == APPLE_SO_NREAD) {
        int n = 0;
        ioctl(fd, FIONREAD, &n);
        if (value && *len >= 4)
            *(int *)value = n;
        *len = 4;
        return 0;
    }
    const struct option_map *o = find_option(level, name);
    if (!o) {
        TRACE("getsockopt unsupported level %#x name %#x", level, name);
        return fail(ENOPROTOOPT);
    }
    int rc = getsockopt(fd, o->level, o->name, value, len);
    if (rc == 0 && o->level == SOL_SOCKET && o->name == SO_ERROR && value)
        *(int *)value = errno_to_apple(*(int *)value);
    if (rc == 0 && o->level == SOL_SOCKET && o->name == SO_TYPE && value)
        ; // SOCK_* values match
    return rc;
}

// ---------------------------------------------------------------------------
// Name resolution

struct apple_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct apple_addrinfo *ai_next;
};

static int ai_flags_from_apple(int f)
{
    int h = f & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
    if (f & 0x1000) h |= AI_NUMERICSERV;
    if (f & 0x0400) h |= AI_ADDRCONFIG;
    if (f & 0x0800) h |= AI_V4MAPPED;
    if (f & 0x0100) h |= AI_ALL;
    return h;
}

static int apple_getaddrinfo(const char *node, const char *service, const struct apple_addrinfo *hints,
                             struct apple_addrinfo **result)
{
    struct addrinfo h, *res = NULL;
    memset(&h, 0, sizeof h);
    if (hints) {
        h.ai_flags = ai_flags_from_apple(hints->ai_flags);
        h.ai_family = family_from_apple(hints->ai_family);
        h.ai_socktype = hints->ai_socktype;
        h.ai_protocol = hints->ai_protocol;
        if (h.ai_family < 0)
            return EAI_FAMILY;
    }
    int rc = getaddrinfo(node, service, hints ? &h : NULL, &res);
    TRACE("getaddrinfo(%s, %s) = %d", node ? node : "", service ? service : "", rc);
    if (rc != 0)
        return rc;
    struct apple_addrinfo *head = NULL, **tail = &head;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        struct apple_addrinfo *a = calloc(1, sizeof *a + sizeof(struct sockaddr_storage));
        a->ai_flags = hints ? hints->ai_flags : 0;
        a->ai_family = family_to_apple(r->ai_family);
        a->ai_socktype = r->ai_socktype;
        a->ai_protocol = r->ai_protocol;
        a->ai_addr = a + 1;
        a->ai_addrlen = sizeof(struct sockaddr_storage);
        sockaddr_to_apple(r->ai_addr, r->ai_addrlen, a->ai_addr, &a->ai_addrlen);
        a->ai_canonname = r->ai_canonname ? strdup(r->ai_canonname) : NULL;
        *tail = a;
        tail = &a->ai_next;
    }
    freeaddrinfo(res);
    *result = head;
    return 0;
}

static void apple_freeaddrinfo(struct apple_addrinfo *a)
{
    while (a) {
        struct apple_addrinfo *next = a->ai_next;
        free(a->ai_canonname);
        free(a);
        a = next;
    }
}

static int apple_getnameinfo(const void *addr, socklen_t len, char *host, socklen_t hostlen, char *serv,
                             socklen_t servlen, int flags)
{
    struct sockaddr_storage ss;
    socklen_t sl;
    if (sockaddr_from_apple(addr, len, &ss, &sl) != 0)
        return EAI_FAMILY;
    return getnameinfo((struct sockaddr *)&ss, sl, host, hostlen, serv, servlen, flags);
}

// ---------------------------------------------------------------------------
// Interfaces

struct apple_ifaddrs {
    struct apple_ifaddrs *next;
    char *name;
    unsigned int flags;
    void *addr;
    void *netmask;
    void *dstaddr;
    void *data;
};

static unsigned iff_to_apple(unsigned f)
{
    unsigned a = f & (IFF_UP | IFF_BROADCAST | IFF_DEBUG | IFF_LOOPBACK | IFF_POINTOPOINT | IFF_RUNNING |
                      IFF_NOARP | IFF_PROMISC | IFF_ALLMULTI);
    if (f & IFF_MULTICAST)
        a |= 0x8000;
    return a;
}

static void *copy_addr(const struct sockaddr *sa)
{
    if (!sa)
        return NULL;
    uint8_t *out = calloc(1, sizeof(struct sockaddr_storage));
    socklen_t len = sizeof(struct sockaddr_storage);
    sockaddr_to_apple(sa, len, out, &len);
    return out;
}

static int apple_getifaddrs(struct apple_ifaddrs **result)
{
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0)
        return -1;
    struct apple_ifaddrs *head = NULL, **tail = &head;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_addr->sa_family != AF_INET && i->ifa_addr->sa_family != AF_INET6))
            continue; // Darwin AF_LINK entries have no Linux equivalent here
        struct apple_ifaddrs *a = calloc(1, sizeof *a);
        a->name = strdup(i->ifa_name);
        a->flags = iff_to_apple(i->ifa_flags);
        a->addr = copy_addr(i->ifa_addr);
        a->netmask = copy_addr(i->ifa_netmask);
        a->dstaddr = copy_addr(i->ifa_broadaddr);
        *tail = a;
        tail = &a->next;
    }
    freeifaddrs(list);
    *result = head;
    return 0;
}

static void apple_freeifaddrs(struct apple_ifaddrs *a)
{
    while (a) {
        struct apple_ifaddrs *next = a->next;
        free(a->name);
        free(a->addr);
        free(a->netmask);
        free(a->dstaddr);
        free(a);
        a = next;
    }
}

// ---------------------------------------------------------------------------
// poll: Darwin POLLWRNORM is 0x4 (== POLLOUT) and POLLWRBAND 0x100.

static int apple_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    for (nfds_t i = 0; i < n; i++) {
        short e = fds[i].events & 0xFF;
        if (fds[i].events & 0x100)
            e |= POLLWRBAND;
        fds[i].events = e;
    }
    int rc = poll(fds, n, timeout);
    for (nfds_t i = 0; i < n; i++) {
        short r = fds[i].revents & 0xFF;
        if (fds[i].revents & POLLWRBAND)
            r |= 0x100;
        if (fds[i].revents & POLLWRNORM)
            r |= POLLOUT;
        fds[i].revents = r;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// kqueue on epoll. Read and write filters for one descriptor share one epoll
// registration; EV_CLEAR maps to edge-triggered, EV_ONESHOT to EPOLLONESHOT.

#define EVFILT_READ (-1)
#define EVFILT_WRITE (-2)
#define EVFILT_USER (-10)
#define EV_ADD 0x0001
#define EV_DELETE 0x0002
#define EV_ENABLE 0x0004
#define EV_DISABLE 0x0008
#define EV_ONESHOT 0x0010
#define EV_CLEAR 0x0020
#define EV_RECEIPT 0x0040
#define EV_ERROR 0x4000
#define EV_EOF 0x8000
#define NOTE_TRIGGER 0x01000000

struct apple_kevent {
    uintptr_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    intptr_t data;
    void *udata;
};

struct kq_watch {
    int fd;
    int read, write;
    uint16_t read_flags, write_flags;
    void *read_udata, *write_udata;
};

struct kq {
    int epfd;
    int user_fd; // eventfd for EVFILT_USER triggers
    pthread_mutex_t lock;
    struct kq_watch *watches;
    size_t count, capacity;
    struct apple_kevent user;
    int user_registered;
};

#define MAX_KQ 64
static struct kq *g_kqueues[1024];
static pthread_mutex_t g_kq_lock = PTHREAD_MUTEX_INITIALIZER;

static struct kq *kq_for(int fd) { return fd >= 0 && fd < 1024 ? g_kqueues[fd] : NULL; }

static int apple_kqueue(void)
{
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0 || epfd >= 1024)
        return epfd < 0 ? -1 : (close(epfd), fail(EMFILE));
    struct kq *q = calloc(1, sizeof *q);
    q->epfd = epfd;
    q->user_fd = -1;
    pthread_mutex_init(&q->lock, NULL);
    pthread_mutex_lock(&g_kq_lock);
    g_kqueues[epfd] = q;
    pthread_mutex_unlock(&g_kq_lock);
    TRACE("kqueue() = %d", epfd);
    return epfd;
}

static struct kq_watch *find_watch(struct kq *q, int fd, int create)
{
    for (size_t i = 0; i < q->count; i++)
        if (q->watches[i].fd == fd)
            return &q->watches[i];
    if (!create)
        return NULL;
    if (q->count == q->capacity) {
        q->capacity = q->capacity ? q->capacity * 2 : 64;
        q->watches = realloc(q->watches, q->capacity * sizeof *q->watches);
    }
    struct kq_watch *w = &q->watches[q->count++];
    memset(w, 0, sizeof *w);
    w->fd = fd;
    return w;
}

static int sync_watch(struct kq *q, struct kq_watch *w, int existed)
{
    uint32_t events = 0;
    if (w->read)
        events |= EPOLLIN | EPOLLRDHUP;
    if (w->write)
        events |= EPOLLOUT;
    if ((w->read && (w->read_flags & EV_CLEAR)) || (w->write && (w->write_flags & EV_CLEAR)))
        events |= EPOLLET;
    if ((w->read && (w->read_flags & EV_ONESHOT)) || (w->write && (w->write_flags & EV_ONESHOT)))
        events |= EPOLLONESHOT;
    struct epoll_event ev = { .events = events, .data.fd = w->fd };
    if (!w->read && !w->write) {
        epoll_ctl(q->epfd, EPOLL_CTL_DEL, w->fd, NULL);
        *w = q->watches[--q->count];
        return 0;
    }
    int rc = epoll_ctl(q->epfd, existed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, w->fd, &ev);
    if (rc != 0 && errno == ENOENT)
        rc = epoll_ctl(q->epfd, EPOLL_CTL_ADD, w->fd, &ev);
    if (rc != 0 && errno == EEXIST)
        rc = epoll_ctl(q->epfd, EPOLL_CTL_MOD, w->fd, &ev);
    return rc;
}

static int apply_change(struct kq *q, const struct apple_kevent *c)
{
    if (c->filter == EVFILT_USER) {
        if (q->user_fd < 0) {
            q->user_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = -2 };
            epoll_ctl(q->epfd, EPOLL_CTL_ADD, q->user_fd, &ev);
        }
        if (c->flags & EV_ADD) {
            q->user = *c;
            q->user_registered = 1;
        }
        if (c->flags & EV_DELETE)
            q->user_registered = 0;
        if (c->fflags & NOTE_TRIGGER) {
            uint64_t one = 1;
            write(q->user_fd, &one, sizeof one);
        }
        return 0;
    }
    if (c->filter != EVFILT_READ && c->filter != EVFILT_WRITE) {
        TRACE("kevent unsupported filter %d", c->filter);
        return ENOTSUP;
    }
    int fd = (int)c->ident;
    struct kq_watch *w = find_watch(q, fd, (c->flags & EV_ADD) != 0);
    if (!w)
        return ENOENT;
    int existed = w->read || w->write;
    int *on = c->filter == EVFILT_READ ? &w->read : &w->write;
    uint16_t *flags = c->filter == EVFILT_READ ? &w->read_flags : &w->write_flags;
    void **udata = c->filter == EVFILT_READ ? &w->read_udata : &w->write_udata;
    if (c->flags & EV_ADD) {
        *on = 1;
        *flags = c->flags;
        *udata = c->udata;
    }
    if (c->flags & EV_DISABLE)
        *on = 0;
    if (c->flags & EV_ENABLE)
        *on = 1;
    if (c->flags & EV_DELETE)
        *on = 0;
    return sync_watch(q, w, existed) == 0 ? 0 : errno;
}

static int apple_kevent(int kq, const struct apple_kevent *changes, int nchanges, struct apple_kevent *events,
                        int nevents, const struct timespec *timeout)
{
    struct kq *q = kq_for(kq);
    if (!q)
        return fail(EBADF);
    int out = 0;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < nchanges; i++) {
        int err = apply_change(q, &changes[i]);
        if ((err || (changes[i].flags & EV_RECEIPT)) && out < nevents) {
            events[out] = changes[i];
            events[out].flags = EV_ERROR;
            events[out].data = errno_to_apple(err);
            out++;
        }
    }
    pthread_mutex_unlock(&q->lock);
    if (out || nevents == 0)
        return out;

    int ms = timeout ? (int)(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000) : -1;
    struct epoll_event ready[64];
    int max = nevents < 32 ? nevents : 32; // each fd may produce two kevents
    int n = epoll_wait(q->epfd, ready, max, ms);
    if (n < 0)
        return -1;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < n && out < nevents; i++) {
        uint32_t e = ready[i].events;
        if (ready[i].data.fd == -2) {
            uint64_t v;
            read(q->user_fd, &v, sizeof v);
            if (q->user_registered)
                events[out++] = q->user;
            continue;
        }
        struct kq_watch *w = find_watch(q, ready[i].data.fd, 0);
        if (!w)
            continue;
        uint16_t eof = (e & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) ? EV_EOF : 0;
        if (w->read && (e & (EPOLLIN | EPOLLHUP | EPOLLRDHUP | EPOLLERR)) && out < nevents) {
            int avail = 0;
            ioctl(w->fd, FIONREAD, &avail);
            events[out++] = (struct apple_kevent){ (uintptr_t)w->fd, EVFILT_READ, (uint16_t)(w->read_flags | eof), 0,
                                                   avail, w->read_udata };
        }
        if (w->write && (e & (EPOLLOUT | EPOLLHUP | EPOLLERR)) && out < nevents)
            events[out++] = (struct apple_kevent){ (uintptr_t)w->fd, EVFILT_WRITE, (uint16_t)(w->write_flags | eof), 0,
                                                   0, w->write_udata };
    }
    pthread_mutex_unlock(&q->lock);
    return out;
}

// ---------------------------------------------------------------------------

const struct shim net_shims[] = {
    { "_socket", (void *)apple_socket },
    { "_bind", (void *)apple_bind },
    { "_connect$NOCANCEL", (void *)apple_connect },
    { "_connectx", (void *)apple_connectx },
    { "_disconnectx", (void *)apple_disconnectx },
    { "_getsockname", (void *)apple_getsockname },
    { "_getpeername", (void *)apple_getpeername },
    { "_send$NOCANCEL", (void *)apple_send },
    { "_recv$NOCANCEL", (void *)apple_recv },
    { "_sendmsg$NOCANCEL", (void *)apple_sendmsg },
    { "_recvmsg$NOCANCEL", (void *)apple_recvmsg },
    { "_setsockopt", (void *)apple_setsockopt },
    { "_getsockopt", (void *)apple_getsockopt },
    { "_shutdown", (void *)shutdown },
    { "_getaddrinfo", (void *)apple_getaddrinfo },
    { "_freeaddrinfo", (void *)apple_freeaddrinfo },
    { "_getnameinfo", (void *)apple_getnameinfo },
    { "_gai_strerror", (void *)gai_strerror },
    { "_getifaddrs", (void *)apple_getifaddrs },
    { "_freeifaddrs", (void *)apple_freeifaddrs },
    { "_if_nametoindex", (void *)if_nametoindex },
    { "_if_indextoname", (void *)if_indextoname },
    { "_poll$NOCANCEL", (void *)apple_poll },
    { "_kqueue", (void *)apple_kqueue },
    { "_kevent", (void *)apple_kevent },
};
const size_t net_shims_count = SHIM_COUNT(net_shims);
