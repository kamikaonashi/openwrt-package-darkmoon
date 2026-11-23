/* CAKE/IFB setup over NETLINK_ROUTE, see iproute2 tc/q_cake.c and tc/f_matchall.c */
#include "tc_netlink.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/pkt_cls.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <linux/tc_act/tc_mirred.h>

#ifndef NETLINK_CAP_ACK
#define NETLINK_CAP_ACK 10
#endif
#ifndef NETLINK_EXT_ACK
#define NETLINK_EXT_ACK 11
#endif
#ifndef NLM_F_CAPPED
#define NLM_F_CAPPED 0x100
#endif
#ifndef NLM_F_ACK_TLVS
#define NLM_F_ACK_TLVS 0x200
#endif
#ifndef NLMSGERR_ATTR_MSG
#define NLMSGERR_ATTR_MSG 1
#endif
#ifndef TC_H_INGRESS
#define TC_H_INGRESS 0xFFFFFFF1U
#endif
#ifndef TCA_MATCHALL_ACT
#define TCA_MATCHALL_ACT   2
#endif
#ifndef TCA_MATCHALL_FLAGS
#define TCA_MATCHALL_FLAGS 3
#endif
#ifndef TCA_CLS_FLAGS_SKIP_HW
#define TCA_CLS_FLAGS_SKIP_HW (1 << 0)
#endif

/* TCA_CAKE_* from linux/pkt_sched.h */
#define CAKE_BASE_RATE64   2
#define CAKE_DIFFSERV_MODE 3
#define CAKE_ATM           4
#define CAKE_FLOW_MODE     5
#define CAKE_OVERHEAD      6
#define CAKE_RTT           7
#define CAKE_MEMORY        10
#define CAKE_NAT           11
#define CAKE_WASH          13
#define CAKE_MPU           14
#define CAKE_INGRESS       15
#define CAKE_ACK_FILTER    16
#define CAKE_SPLIT_GSO     17

#define INGRESS_HANDLE 0xFFFF0000U
#define BUF_SIZE  2048
#define RECV_SIZE 8192

struct tc_nl_ctx {
    int      fd;
    uint32_t seq;
    char     err[160];
};

struct msg {
    char buf[BUF_SIZE] __attribute__((aligned(NLMSG_ALIGNTO)));
    int  len;
};

static void *msg_init(struct msg *m, uint16_t type, uint16_t flags, size_t hdrlen)
{
    memset(m, 0, sizeof(*m));
    struct nlmsghdr *n = (struct nlmsghdr *)m->buf;
    n->nlmsg_type = type;
    n->nlmsg_flags = (uint16_t)(NLM_F_REQUEST | NLM_F_ACK | flags);
    m->len = (int)(NLMSG_HDRLEN + NLMSG_ALIGN(hdrlen));
    return m->buf + NLMSG_HDRLEN;
}

static void put_attr(struct msg *m, uint16_t type, const void *data, size_t dlen)
{
    struct nlattr *a = (struct nlattr *)(m->buf + m->len);
    size_t total = NLA_ALIGN(NLA_HDRLEN + dlen);
    if ((size_t)m->len + total > sizeof(m->buf))
        return;
    a->nla_type = type;
    a->nla_len = (uint16_t)(NLA_HDRLEN + dlen);
    if (dlen)
        memcpy((char *)a + NLA_HDRLEN, data, dlen);
    m->len += (int)total;
}

static void put_u32(struct msg *m, uint16_t t, uint32_t v) { put_attr(m, t, &v, 4); }
static void put_s32(struct msg *m, uint16_t t, int32_t v)  { put_attr(m, t, &v, 4); }
static void put_u64(struct msg *m, uint16_t t, uint64_t v) { put_attr(m, t, &v, 8); }
static void put_str(struct msg *m, uint16_t t, const char *s) { put_attr(m, t, s, strlen(s) + 1); }

static int nest_start(struct msg *m, uint16_t type)
{
    int off = m->len;
    put_attr(m, type | NLA_F_NESTED, NULL, 0);
    return off;
}

static void nest_end(struct msg *m, int off)
{
    struct nlattr *a = (struct nlattr *)(m->buf + off);
    a->nla_len = (uint16_t)(m->len - off);
}

typedef void (*reply_cb_t)(const struct nlmsghdr *n, void *arg);

static void set_err(tc_nl_ctx_t *ctx, int err, const char *ext)
{
    if (ext && ext[0])
        snprintf(ctx->err, sizeof(ctx->err), "%s (%s)", ext, strerror(err));
    else
        snprintf(ctx->err, sizeof(ctx->err), "%s", strerror(err));
}

static void parse_extack(tc_nl_ctx_t *ctx, const struct nlmsghdr *n, int err)
{
    const struct nlmsgerr *e = NLMSG_DATA(n);
    const char *ext = NULL;

    if (n->nlmsg_flags & NLM_F_ACK_TLVS) {
        size_t off = sizeof(*e);
        if (!(n->nlmsg_flags & NLM_F_CAPPED))
            off += e->msg.nlmsg_len - NLMSG_HDRLEN;
        const char *p = (const char *)e + off;
        const char *end = (const char *)n + n->nlmsg_len;
        while (p + NLA_HDRLEN <= end) {
            const struct nlattr *a = (const struct nlattr *)p;
            if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end)
                break;
            if ((a->nla_type & NLA_TYPE_MASK) == NLMSGERR_ATTR_MSG)
                ext = p + NLA_HDRLEN;
            p += NLA_ALIGN(a->nla_len);
        }
    }
    set_err(ctx, err, ext);
}

/* returns 0 or -errno */
static int nl_talk(tc_nl_ctx_t *ctx, struct msg *m, reply_cb_t cb, void *arg)
{
    struct nlmsghdr *req = (struct nlmsghdr *)m->buf;
    req->nlmsg_len = (uint32_t)m->len;
    req->nlmsg_seq = ++ctx->seq;

    struct sockaddr_nl dst = { .nl_family = AF_NETLINK };
    if (sendto(ctx->fd, m->buf, (size_t)m->len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        int e = errno;
        set_err(ctx, e, "netlink send failed");
        return -e;
    }

    static char rbuf[RECV_SIZE] __attribute__((aligned(NLMSG_ALIGNTO)));
    for (;;) {
        ssize_t r = recv(ctx->fd, rbuf, sizeof(rbuf), 0);
        if (r < 0) {
            int e = errno;
            if (e == EINTR)
                continue;
            set_err(ctx, e, "netlink receive failed");
            return -e;
        }
        for (struct nlmsghdr *n = (struct nlmsghdr *)rbuf; NLMSG_OK(n, (size_t)r);
             n = NLMSG_NEXT(n, r)) {
            if (n->nlmsg_seq != req->nlmsg_seq)
                continue;
            if (n->nlmsg_type == NLMSG_DONE)
                return 0;
            if (n->nlmsg_type == NLMSG_ERROR) {
                const struct nlmsgerr *e = NLMSG_DATA(n);
                if (e->error == 0)
                    return 0;
                parse_extack(ctx, n, -e->error);
                return e->error;
            }
            if (cb)
                cb(n, arg);
        }
    }
}

tc_nl_ctx_t *tc_nl_open(void)
{
    tc_nl_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (ctx->fd < 0) {
        syslog(LOG_ERR, "netlink: socket(): %m");
        free(ctx);
        return NULL;
    }

    int one = 1;
    setsockopt(ctx->fd, SOL_NETLINK, NETLINK_EXT_ACK, &one, sizeof(one));
    setsockopt(ctx->fd, SOL_NETLINK, NETLINK_CAP_ACK, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (bind(ctx->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        syslog(LOG_ERR, "netlink: bind(): %m");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }
    return ctx;
}

void tc_nl_close(tc_nl_ctx_t *ctx)
{
    if (!ctx)
        return;
    close(ctx->fd);
    free(ctx);
}

const char *tc_nl_last_error(tc_nl_ctx_t *ctx)
{
    return ctx ? ctx->err : "";
}

struct kind_arg { char kind[32]; int found; };

static void link_kind_cb(const struct nlmsghdr *n, void *arg)
{
    struct kind_arg *ka = arg;
    if (n->nlmsg_type != RTM_NEWLINK)
        return;
    ka->found = 1;

    const struct ifinfomsg *ifi = NLMSG_DATA(n);
    int len = (int)n->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ifi));
    const struct rtattr *rta = IFLA_RTA(ifi);
    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
        if (rta->rta_type != IFLA_LINKINFO)
            continue;
        int ilen = (int)RTA_PAYLOAD(rta);
        const struct rtattr *in = RTA_DATA(rta);
        for (; RTA_OK(in, ilen); in = RTA_NEXT(in, ilen)) {
            if (in->rta_type == IFLA_INFO_KIND)
                snprintf(ka->kind, sizeof(ka->kind), "%s", (const char *)RTA_DATA(in));
        }
    }
}

int tc_link_is_ifb(tc_nl_ctx_t *ctx, const char *name)
{
    struct msg m;
    struct ifinfomsg *ifi = msg_init(&m, RTM_GETLINK, 0, sizeof(*ifi));
    ifi->ifi_family = AF_UNSPEC;
    put_str(&m, IFLA_IFNAME, name);

    struct kind_arg ka = { .found = 0 };
    if (nl_talk(ctx, &m, link_kind_cb, &ka) < 0 || !ka.found)
        return -1;
    return strcmp(ka.kind, "ifb") == 0;
}

static int link_create_ifb(tc_nl_ctx_t *ctx, const char *name)
{
    struct msg m;
    struct ifinfomsg *ifi = msg_init(&m, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL, sizeof(*ifi));
    ifi->ifi_family = AF_UNSPEC;
    put_str(&m, IFLA_IFNAME, name);
    int li = nest_start(&m, IFLA_LINKINFO);
    put_str(&m, IFLA_INFO_KIND, "ifb");
    nest_end(&m, li);
    return nl_talk(ctx, &m, NULL, NULL);
}

static int link_set_up(tc_nl_ctx_t *ctx, unsigned int ifindex)
{
    struct msg m;
    struct ifinfomsg *ifi = msg_init(&m, RTM_NEWLINK, 0, sizeof(*ifi));
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = (int)ifindex;
    ifi->ifi_flags = IFF_UP;
    ifi->ifi_change = IFF_UP;
    return nl_talk(ctx, &m, NULL, NULL);
}

static int link_delete(tc_nl_ctx_t *ctx, const char *name)
{
    unsigned int idx = if_nametoindex(name);
    if (!idx)
        return 0;
    struct msg m;
    struct ifinfomsg *ifi = msg_init(&m, RTM_DELLINK, 0, sizeof(*ifi));
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = (int)idx;
    int r = nl_talk(ctx, &m, NULL, NULL);
    return (r == -ENODEV) ? 0 : r;
}

static struct tcmsg *tc_init(struct msg *m, uint16_t type, uint16_t flags,
                             unsigned int ifindex, uint32_t parent, uint32_t handle)
{
    struct tcmsg *t = msg_init(m, type, flags, sizeof(*t));
    t->tcm_family = AF_UNSPEC;
    t->tcm_ifindex = (int)ifindex;
    t->tcm_parent = parent;
    t->tcm_handle = handle;
    return t;
}

static int qdisc_del(tc_nl_ctx_t *ctx, unsigned int ifindex, uint32_t parent, uint32_t handle)
{
    struct msg m;
    tc_init(&m, RTM_DELQDISC, 0, ifindex, parent, handle);
    int r = nl_talk(ctx, &m, NULL, NULL);
    /* EINVAL: default qdisc with handle 0 */
    return (r == -ENOENT || r == -EINVAL) ? 0 : r;
}

static uint64_t kbps_to_Bps(uint32_t kbps) { return (uint64_t)kbps * 125u; }

static int cake_add(tc_nl_ctx_t *ctx, unsigned int ifindex, const char *kind,
                    uint32_t rate_kbps, const cake_opts_t *o)
{
    struct msg m;
    tc_init(&m, RTM_NEWQDISC, NLM_F_CREATE | NLM_F_EXCL, ifindex, TC_H_ROOT, 0);
    put_str(&m, TCA_KIND, kind);

    int opts = nest_start(&m, TCA_OPTIONS);
    put_u64(&m, CAKE_BASE_RATE64, kbps_to_Bps(rate_kbps));
    put_u32(&m, CAKE_DIFFSERV_MODE, o->diffserv);
    put_u32(&m, CAKE_ATM, o->atm);
    put_u32(&m, CAKE_FLOW_MODE, o->flow_mode);
    if (o->overhead != INT32_MIN)
        put_s32(&m, CAKE_OVERHEAD, o->overhead);
    if (o->mpu)
        put_u32(&m, CAKE_MPU, o->mpu);
    if (o->rtt_us)
        put_u32(&m, CAKE_RTT, o->rtt_us);
    if (o->memlimit)
        put_u32(&m, CAKE_MEMORY, o->memlimit);
    put_u32(&m, CAKE_NAT, o->nat);
    put_u32(&m, CAKE_WASH, o->wash);
    put_u32(&m, CAKE_INGRESS, o->ingress);
    put_u32(&m, CAKE_ACK_FILTER, o->ack_filter);
    put_u32(&m, CAKE_SPLIT_GSO, o->split_gso);
    nest_end(&m, opts);

    return nl_talk(ctx, &m, NULL, NULL);
}

static int cake_install(tc_nl_ctx_t *ctx, unsigned int ifindex, const char *ifname,
                        uint32_t rate_kbps, const cake_opts_t *o, const char **kind)
{
    int r;

    qdisc_del(ctx, ifindex, TC_H_ROOT, 0);

    if (o->try_cake_mq) {
        r = cake_add(ctx, ifindex, "cake_mq", rate_kbps, o);
        if (r == 0) {
            *kind = "cake_mq";
            return 0;
        }
        syslog(LOG_NOTICE, "%s: cake_mq unavailable (%s), using cake", ifname, ctx->err);
    }

    r = cake_add(ctx, ifindex, "cake", rate_kbps, o);
    if (r == -EEXIST) {
        qdisc_del(ctx, ifindex, TC_H_ROOT, 0);
        r = cake_add(ctx, ifindex, "cake", rate_kbps, o);
    }
    if (r == -ENOENT && strstr(ctx->err, "nknown"))
        snprintf(ctx->err, sizeof(ctx->err),
                 "CAKE qdisc not available - install kmod-sched-cake");
    if (r < 0) {
        syslog(LOG_ERR, "%s: cannot attach CAKE: %s", ifname, ctx->err);
        return r;
    }
    *kind = "cake";
    return 0;
}

int tc_cake_set_bandwidth(tc_nl_ctx_t *ctx, const char *iface, const char *kind,
                          uint32_t rate_kbps)
{
    unsigned int idx = if_nametoindex(iface);
    if (!idx) {
        set_err(ctx, ENODEV, iface);
        return -ENODEV;
    }

    struct msg m;
    tc_init(&m, RTM_NEWQDISC, 0, idx, TC_H_ROOT, 0);
    put_str(&m, TCA_KIND, kind ? kind : "cake");
    int opts = nest_start(&m, TCA_OPTIONS);
    put_u64(&m, CAKE_BASE_RATE64, kbps_to_Bps(rate_kbps));
    nest_end(&m, opts);
    return nl_talk(ctx, &m, NULL, NULL);
}

static int ingress_add(tc_nl_ctx_t *ctx, unsigned int ifindex)
{
    struct msg m;
    tc_init(&m, RTM_NEWQDISC, NLM_F_CREATE | NLM_F_EXCL, ifindex, TC_H_INGRESS, INGRESS_HANDLE);
    put_str(&m, TCA_KIND, "ingress");
    return nl_talk(ctx, &m, NULL, NULL);
}

static int redirect_add(tc_nl_ctx_t *ctx, unsigned int src_idx, unsigned int dst_idx)
{
    struct msg m;
    struct tcmsg *t = tc_init(&m, RTM_NEWTFILTER, NLM_F_CREATE | NLM_F_EXCL,
                              src_idx, INGRESS_HANDLE, 0);
    t->tcm_info = TC_H_MAKE(1U << 16, (uint32_t)htons(ETH_P_ALL));

    put_str(&m, TCA_KIND, "matchall");
    int opts = nest_start(&m, TCA_OPTIONS);
    put_u32(&m, TCA_MATCHALL_FLAGS, TCA_CLS_FLAGS_SKIP_HW);
    int acts = nest_start(&m, TCA_MATCHALL_ACT);
    int a1 = nest_start(&m, 1);
    put_str(&m, TCA_ACT_KIND, "mirred");
    int aopts = nest_start(&m, TCA_ACT_OPTIONS);
    struct tc_mirred p;
    memset(&p, 0, sizeof(p));
    p.action = TC_ACT_STOLEN;
    p.eaction = TCA_EGRESS_REDIR;
    p.ifindex = dst_idx;
    put_attr(&m, TCA_MIRRED_PARMS, &p, sizeof(p));
    nest_end(&m, aopts);
    nest_end(&m, a1);
    nest_end(&m, acts);
    nest_end(&m, opts);

    return nl_talk(ctx, &m, NULL, NULL);
}

int tc_dl_setup(tc_nl_ctx_t *ctx, const char *wan_if, const char *dl_if,
                int ifb_mode, uint32_t rate_kbps, const cake_opts_t *o,
                int *created_ifb, const char **kind)
{
    unsigned int wan_idx = if_nametoindex(wan_if);
    unsigned int dl_idx;
    int r;

    *created_ifb = 0;
    if (!wan_idx) {
        set_err(ctx, ENODEV, wan_if);
        return -ENODEV;
    }

    if (!ifb_mode) {
        dl_idx = if_nametoindex(dl_if);
        if (!dl_idx) {
            set_err(ctx, ENODEV, dl_if);
            return -ENODEV;
        }
        r = cake_install(ctx, dl_idx, dl_if, rate_kbps, o, kind);
        if (r == 0)
            syslog(LOG_INFO, "download: %s on %s (egress) @ %u kbit/s", *kind, dl_if, rate_kbps);
        return r;
    }

    if (!if_nametoindex(dl_if)) {
        r = link_create_ifb(ctx, dl_if);
        if (r < 0 && r != -EEXIST) {
            if (r == -EOPNOTSUPP || r == -ENOENT)
                snprintf(ctx->err, sizeof(ctx->err),
                         "cannot create IFB device - install kmod-ifb");
            syslog(LOG_ERR, "download: create %s: %s", dl_if, ctx->err);
            return r;
        }
        *created_ifb = (r == 0);
    }
    dl_idx = if_nametoindex(dl_if);
    if (!dl_idx || (r = link_set_up(ctx, dl_idx)) < 0) {
        syslog(LOG_ERR, "download: bring up %s: %s", dl_if, ctx->err);
        return dl_idx ? r : -ENODEV;
    }

    cake_opts_t dl = *o;
    dl.ingress = 1;
    dl.try_cake_mq = 0;
    r = cake_install(ctx, dl_idx, dl_if, rate_kbps, &dl, kind);
    if (r < 0)
        return r;

    qdisc_del(ctx, wan_idx, TC_H_INGRESS, INGRESS_HANDLE);
    r = ingress_add(ctx, wan_idx);
    if (r < 0 && r != -EEXIST) {
        syslog(LOG_ERR, "download: ingress qdisc on %s: %s", wan_if, ctx->err);
        return r;
    }
    r = redirect_add(ctx, wan_idx, dl_idx);
    if (r < 0) {
        syslog(LOG_ERR, "download: redirect %s -> %s: %s", wan_if, dl_if, ctx->err);
        return r;
    }

    syslog(LOG_INFO, "download: %s ingress -> %s (%s) @ %u kbit/s", wan_if, dl_if, *kind, rate_kbps);
    return 0;
}

void tc_dl_teardown(tc_nl_ctx_t *ctx, const char *wan_if, const char *dl_if,
                    int ifb_mode, int delete_ifb)
{
    unsigned int idx;

    if (ifb_mode && (idx = if_nametoindex(wan_if)))
        qdisc_del(ctx, idx, TC_H_INGRESS, INGRESS_HANDLE);

    if (ifb_mode && delete_ifb) {
        link_delete(ctx, dl_if);
    } else if ((idx = if_nametoindex(dl_if))) {
        qdisc_del(ctx, idx, TC_H_ROOT, 0);
    }
}

struct qcheck {
    unsigned int ul_idx, dl_idx;
    int ul_cake, dl_cake, ingress;
};

static void qdisc_dump_cb(const struct nlmsghdr *n, void *arg)
{
    struct qcheck *q = arg;
    if (n->nlmsg_type != RTM_NEWQDISC)
        return;
    const struct tcmsg *t = NLMSG_DATA(n);
    int len = (int)n->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*t));
    const char *kind = NULL;
    for (const struct rtattr *a = (const struct rtattr *)((const char *)t + NLMSG_ALIGN(sizeof(*t)));
         RTA_OK(a, len); a = RTA_NEXT(a, len))
        if (a->rta_type == TCA_KIND)
            kind = RTA_DATA(a);
    if (!kind)
        return;
    unsigned int idx = (unsigned int)t->tcm_ifindex;
    int cake = !strcmp(kind, "cake") || !strcmp(kind, "cake_mq");
    if (t->tcm_parent == TC_H_ROOT && cake) {
        if (idx == q->ul_idx) q->ul_cake = 1;
        if (idx == q->dl_idx) q->dl_cake = 1;
    }
    if (t->tcm_parent == TC_H_INGRESS && idx == q->ul_idx && !strcmp(kind, "ingress"))
        q->ingress = 1;
}

int tc_verify(tc_nl_ctx_t *ctx, const char *wan_if, const char *dl_if, int ifb_mode)
{
    struct qcheck q = { .ul_idx = if_nametoindex(wan_if), .dl_idx = if_nametoindex(dl_if) };
    struct msg m;
    struct tcmsg *t = msg_init(&m, RTM_GETQDISC, NLM_F_DUMP, sizeof(*t));
    t->tcm_family = AF_UNSPEC;
    if (!q.ul_idx || !q.dl_idx || nl_talk(ctx, &m, qdisc_dump_cb, &q) < 0)
        return -1;
    if (!q.ul_cake) { snprintf(ctx->err, sizeof(ctx->err), "CAKE on %s was removed", wan_if); return 0; }
    if (!q.dl_cake) { snprintf(ctx->err, sizeof(ctx->err), "CAKE on %s was removed", dl_if); return 0; }
    if (ifb_mode && !q.ingress) { snprintf(ctx->err, sizeof(ctx->err), "ingress redirect on %s was removed", wan_if); return 0; }
    return 1;
}

int tc_ul_setup(tc_nl_ctx_t *ctx, const char *wan_if, uint32_t rate_kbps,
                const cake_opts_t *o, const char **kind)
{
    unsigned int idx = if_nametoindex(wan_if);
    if (!idx) {
        set_err(ctx, ENODEV, wan_if);
        return -ENODEV;
    }
    int r = cake_install(ctx, idx, wan_if, rate_kbps, o, kind);
    if (r == 0)
        syslog(LOG_INFO, "upload: %s on %s @ %u kbit/s", *kind, wan_if, rate_kbps);
    return r;
}

void tc_ul_teardown(tc_nl_ctx_t *ctx, const char *wan_if)
{
    unsigned int idx = if_nametoindex(wan_if);
    if (idx)
        qdisc_del(ctx, idx, TC_H_ROOT, 0);
}

int tc_link_monitor_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK };
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tc_link_monitor_drain(int fd)
{
    char buf[4096];
    int any = 0;
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r > 0) { any = 1; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == ENOBUFS) { any = 1; continue; }
        break;
    }
    return any;
}
