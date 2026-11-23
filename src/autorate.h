#ifndef DARKMOON_AUTORATE_H
#define DARKMOON_AUTORATE_H

#include <stdint.h>
#include <libubox/uloop.h>

#include "config.h"
#include "rate_monitor.h"
#include "tc_netlink.h"

#define DARKMOON_VERSION "2.0.0"

#define DL 0
#define UL 1

enum load_state { LOAD_IDLE, LOAD_LOW, LOAD_HIGH };

enum main_state {
    STATE_WAITING,   /* no interface or qdisc setup failed */
    STATE_RUNNING,
    STATE_IDLE,      /* pingers asleep */
    STATE_STALL,
};

#define PING_RING 1024   /* indexed by seq, must be a power of two */

typedef struct {
    int64_t  t_sent_us;
    uint32_t dst_be;
    uint32_t originate_ms;
    uint16_t gen;
    int8_t   pinger;      /* -1 = free */
} ping_slot_t;

typedef struct {
    int      valid;
    int64_t  base_us[2];
    int64_t  ewma_us[2];
} retained_t;

typedef struct {
    int      pool_idx;
    uint32_t addr_be;
    uint16_t gen;                /* bumped on replacement */
    int      baseline_valid;
    int64_t  base_us[2];
    int64_t  ewma_us[2];
    int64_t  last_owd_us[2];
    int64_t  last_delta_us[2];
    int64_t  last_response_us;
    uint8_t  offences[MAX_OFFENCE_WINDOW];
    int      sum_offences;
    uint32_t sent, received;
} pinger_t;

typedef struct autorate {
    cake_config_t cfg;
    tc_nl_ctx_t  *nl;
    rate_monitor_t rm;

    int      state;
    int64_t  t_started_us;
    int64_t  t_state_us;
    char     last_error[192];

    int          dl_ifb_mode;
    int          created_ifb;
    int          dl_ready, ul_ready;
    unsigned int ul_ifindex, dl_ifindex;
    const char  *qdisc_kind[2];
    int          need_resetup;

    uint32_t min_rate[2], base_rate[2], max_rate[2];
    uint32_t shaper[2];
    uint32_t applied[2];
    uint32_t achieved[2];
    int      achieved_updated[2];
    uint32_t load_pct[2];
    int      load_state[2];
    int      bufferbloat[2];
    int64_t  t_last_bufferbloat_us[2];
    int64_t  t_last_decay_us[2];
    uint32_t max_wire_bits[2];
    int64_t  comp_delay_thr_us[2];
    int64_t  comp_up_thr_us[2];
    int64_t  comp_down_thr_us[2];
    int64_t  monitor_interval_us;

    uint8_t  delays[2][MAX_BB_WINDOW];
    int64_t  deltas_us[2][MAX_BB_WINDOW];
    int      delays_idx;
    int      sum_delays[2];
    int64_t  sum_deltas_us[2];
    int64_t  avg_delta_us[2];

    /* order[0..no_pingers-1] are active, the rest is the spare queue */
    int        pool_n;
    int        order[MAX_POOL];
    retained_t retained[MAX_POOL];
    int        no_pingers;
    pinger_t   pingers[MAX_PINGERS];
    int        offences_idx;

    int64_t  reflectors_last_us;
    int64_t  t_last_health_us;
    int64_t  t_last_replacement_us;
    int64_t  t_last_comparison_us;
    int64_t  t_last_start_pingers_us;
    int      global_timeout;

    int      sustained_idle;
    int64_t  t_sustained_idle_us;
    int64_t  t_last_idle_us;

    int      icmp_fd;
    int      pingers_active;
    uint16_t ping_id;
    uint16_t ping_seq;
    int      ping_rr;
    ping_slot_t ring[PING_RING];
    struct uloop_fd      icmp_ufd;
    struct uloop_timeout ping_timer;

    struct uloop_timeout startup_timer;
    struct uloop_timeout rate_timer;
    struct uloop_timeout health_timer;
    struct uloop_timeout if_timer;
    struct uloop_fd      link_ufd;

    uint32_t n_bufferbloat[2];
    uint32_t n_rate_changes;
    uint32_t n_replacements;
    uint32_t n_stalls;
    uint32_t n_setups;
} autorate_t;

int64_t now_us(void);
const char *state_name(int s);
const char *load_name(int l);

void ubus_init(autorate_t *ar);
void ubus_done(void);

#endif
