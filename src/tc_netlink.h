/*
 * Download, IFB mode:
 *   ip link add <ifb> type ifb && ip link set <ifb> up
 *   tc qdisc add dev <ifb> root cake ... ingress
 *   tc qdisc add dev <wan> handle ffff: ingress
 *   tc filter add dev <wan> parent ffff: matchall skip_hw action mirred egress redirect dev <ifb>
 * Download, direct mode: cake on the egress of an existing dl_if (e.g. br-lan).
 * Upload: tc qdisc add dev <wan> root cake ...
 */
#ifndef DARKMOON_TC_NETLINK_H
#define DARKMOON_TC_NETLINK_H

#include <stdint.h>

typedef struct {
    int32_t  overhead;      /* INT32_MIN = omit */
    uint32_t mpu;
    uint32_t atm;
    uint32_t nat;
    uint32_t wash;
    uint32_t ingress;
    uint32_t ack_filter;
    uint32_t diffserv;
    uint32_t flow_mode;
    uint32_t rtt_us;
    uint32_t memlimit;
    uint32_t split_gso;
    uint32_t try_cake_mq;
} cake_opts_t;

typedef struct tc_nl_ctx tc_nl_ctx_t;

tc_nl_ctx_t *tc_nl_open(void);
void         tc_nl_close(tc_nl_ctx_t *ctx);

const char  *tc_nl_last_error(tc_nl_ctx_t *ctx);

/* 1 = IFB, 0 = other device, -1 = does not exist */
int tc_link_is_ifb(tc_nl_ctx_t *ctx, const char *name);

int tc_dl_setup(tc_nl_ctx_t *ctx, const char *wan_if, const char *dl_if,
                int ifb_mode, uint32_t rate_kbps, const cake_opts_t *o,
                int *created_ifb, const char **kind);
void tc_dl_teardown(tc_nl_ctx_t *ctx, const char *wan_if, const char *dl_if,
                    int ifb_mode, int delete_ifb);

int  tc_ul_setup(tc_nl_ctx_t *ctx, const char *wan_if, uint32_t rate_kbps,
                 const cake_opts_t *o, const char **kind);
void tc_ul_teardown(tc_nl_ctx_t *ctx, const char *wan_if);

/* 1 = qdiscs intact, 0 = something was removed, -1 = unknown */
int tc_verify(tc_nl_ctx_t *ctx, const char *wan_if, const char *dl_if, int ifb_mode);

int tc_cake_set_bandwidth(tc_nl_ctx_t *ctx, const char *iface,
                          const char *kind, uint32_t rate_kbps);

/* RTNLGRP_LINK listener; drain returns 1 if anything was received */
int  tc_link_monitor_open(void);
int  tc_link_monitor_drain(int fd);

#endif
