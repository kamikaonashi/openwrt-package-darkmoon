/*
 * darkmoon - cake-autorate in C for OpenWrt.
 * Algorithm by @lynxthecat: https://github.com/lynxthecat/cake-autorate
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "autorate.h"
#include "icmp.h"

#define FP 1000000LL

#define DBG(ar, ...) do { if ((ar)->cfg.debug) syslog(LOG_DEBUG, __VA_ARGS__); } while (0)

static autorate_t AR;

int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

const char *state_name(int s)
{
    switch (s) {
    case STATE_RUNNING: return "running";
    case STATE_IDLE:    return "idle";
    case STATE_STALL:   return "stall";
    default:            return "waiting";
    }
}

const char *load_name(int l)
{
    switch (l) {
    case LOAD_HIGH: return "high";
    case LOAD_LOW:  return "low";
    default:        return "idle";
    }
}

static const char *dir_name(int d) { return d == DL ? "download" : "upload"; }

static const char *ip_str(uint32_t be)
{
    static char buf[4][INET_ADDRSTRLEN];
    static int i;
    struct in_addr a = { .s_addr = be };
    i = (i + 1) & 3;
    return inet_ntop(AF_INET, &a, buf[i], sizeof(buf[i]));
}

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void set_error(autorate_t *ar, const char *fmt, ...)
{
    char buf[sizeof(ar->last_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(buf, ar->last_error) != 0) {
        snprintf(ar->last_error, sizeof(ar->last_error), "%s", buf);
        if (buf[0])
            syslog(LOG_WARNING, "%s", buf);
    }
}

static void set_state(autorate_t *ar, int s)
{
    if (ar->state == s)
        return;
    DBG(ar, "state %s -> %s", state_name(ar->state), state_name(s));
    ar->state = s;
    ar->t_state_us = now_us();
}

static int64_t ping_response_interval_us(const autorate_t *ar)
{
    return ar->cfg.reflector_ping_interval_us / ar->no_pingers;
}

/* add the serialisation delay of one full packet to the thresholds */
static void update_compensation(autorate_t *ar)
{
    const cake_config_t *c = &ar->cfg;
    int64_t comp[2];

    for (int d = 0; d < 2; d++) {
        comp[d] = ar->shaper[d] ? 1000LL * ar->max_wire_bits[d] / ar->shaper[d] : 0;
        ar->comp_delay_thr_us[d] = (d == DL ? c->dl_owd_delta_delay_thr_us
                                            : c->ul_owd_delta_delay_thr_us) + comp[d];
        ar->comp_up_thr_us[d]    = (d == DL ? c->dl_avg_owd_delta_max_adjust_up_thr_us
                                            : c->ul_avg_owd_delta_max_adjust_up_thr_us) + comp[d];
        ar->comp_down_thr_us[d]  = (d == DL ? c->dl_avg_owd_delta_max_adjust_down_thr_us
                                            : c->ul_avg_owd_delta_max_adjust_down_thr_us) + comp[d];
    }

    int64_t rtt = comp[DL] + comp[UL];
    ar->monitor_interval_us = c->monitor_achieved_rates_interval_us > 10 * rtt
                            ? c->monitor_achieved_rates_interval_us : 10 * rtt;
}

static void apply_shaper_rates(autorate_t *ar, int force)
{
    for (int d = 0; d < 2; d++) {
        uint32_t rate = ar->shaper[d], old = ar->applied[d];
        int ready = d == DL ? ar->dl_ready : ar->ul_ready;

        if (!ready || (rate == old && !force))
            continue;

        /* skip changes below 0.5% to save netlink calls (not in upstream) */
        if (!force && rate != ar->min_rate[d] && rate != ar->max_rate[d] &&
            rate != ar->base_rate[d]) {
            uint32_t diff = rate > old ? rate - old : old - rate;
            uint32_t thr = old / 200;
            if (thr < 1) thr = 1;
            if (diff < thr)
                continue;
        }

        const char *ifname = d == DL ? ar->cfg.dl_if : ar->cfg.ul_if;
        int r = tc_cake_set_bandwidth(ar->nl, ifname, ar->qdisc_kind[d], rate);
        if (r < 0) {
            set_error(ar, "%s: cannot change CAKE bandwidth on %s: %s",
                      dir_name(d), ifname, tc_nl_last_error(ar->nl));
            ar->need_resetup = 1;
            uloop_timeout_set(&ar->if_timer, 100);
            continue;
        }
        DBG(ar, "%s shaper %u -> %u kbit/s (achieved %u, load %u%%)",
            dir_name(d), old, rate, ar->achieved[d], ar->load_pct[d]);
        ar->applied[d] = rate;
        ar->n_rate_changes++;
    }
    update_compensation(ar);
}

static void set_min_shaper_rates(autorate_t *ar)
{
    ar->shaper[DL] = ar->min_rate[DL];
    ar->shaper[UL] = ar->min_rate[UL];
    apply_shaper_rates(ar, 0);
}

static uint32_t scale_rate(uint32_t rate, int64_t factor_fp)
{
    uint64_t r = (uint64_t)rate * (uint64_t)factor_fp / FP;
    if (r == rate && factor_fp > FP) r++;
    if (r == rate && factor_fp < FP && r > 0) r--;
    return r > UINT32_MAX ? UINT32_MAX : (uint32_t)r;
}

static void adjust_shaper_rate(autorate_t *ar, int d, int64_t t)
{
    const cake_config_t *c = &ar->cfg;
    int64_t avg = ar->avg_delta_us[d];
    int64_t delay_thr = ar->comp_delay_thr_us[d];
    int64_t up_thr = ar->comp_up_thr_us[d];
    int64_t down_thr = ar->comp_down_thr_us[d];
    int64_t f;

    if (ar->bufferbloat[d]) {
        if (t <= ar->t_last_bufferbloat_us[d] + c->bufferbloat_refractory_period_us)
            return;
        if (down_thr <= delay_thr)
            f = FP;
        else if (avg > delay_thr)
            f = clamp64(FP * (avg - delay_thr) / (down_thr - delay_thr), 0, FP);
        else
            f = 0;
        int64_t adj = c->shaper_rate_min_adjust_down_bufferbloat_fp -
                      f * (c->shaper_rate_min_adjust_down_bufferbloat_fp -
                           c->shaper_rate_max_adjust_down_bufferbloat_fp) / FP;
        ar->shaper[d] = scale_rate(ar->shaper[d], adj);
        ar->t_last_bufferbloat_us[d] = t;
        ar->t_last_decay_us[d] = t;
        ar->n_bufferbloat[d]++;
        DBG(ar, "%s bufferbloat: avg delta %lld us (thr %lld), factor %lld ppm",
            dir_name(d), (long long)avg, (long long)delay_thr, (long long)adj);
    } else if (ar->load_state[d] == LOAD_HIGH) {
        if (!ar->achieved_updated[d] ||
            t <= ar->t_last_bufferbloat_us[d] + c->bufferbloat_refractory_period_us)
            return;
        if (delay_thr <= up_thr)
            f = FP;
        else if (delay_thr - avg > 0)
            f = clamp64(FP * (delay_thr - avg) / (delay_thr - up_thr), 0, FP);
        else
            f = 0;
        int64_t adj = c->shaper_rate_min_adjust_up_load_high_fp +
                      f * (c->shaper_rate_max_adjust_up_load_high_fp -
                           c->shaper_rate_min_adjust_up_load_high_fp) / FP;
        ar->shaper[d] = scale_rate(ar->shaper[d], adj);
        ar->achieved_updated[d] = 0;
        ar->t_last_decay_us[d] = t;
    } else {
        if (t <= ar->t_last_decay_us[d] + c->decay_refractory_period_us)
            return;
        uint32_t base = ar->base_rate[d];
        if (ar->shaper[d] > base) {
            uint32_t r = scale_rate(ar->shaper[d], c->shaper_rate_adjust_down_load_low_fp);
            ar->shaper[d] = r > base ? r : base;
        } else if (ar->shaper[d] < base) {
            uint32_t r = scale_rate(ar->shaper[d], c->shaper_rate_adjust_up_load_low_fp);
            ar->shaper[d] = r < base ? r : base;
        }
        ar->t_last_decay_us[d] = t;
    }
}

static void pinger_assign(autorate_t *ar, int p, int pool_idx, int64_t t)
{
    pinger_t *pg = &ar->pingers[p];
    uint16_t gen = (uint16_t)(pg->gen + 1);
    retained_t *rt = &ar->retained[pool_idx];

    memset(pg, 0, sizeof(*pg));
    pg->gen = gen;
    pg->pool_idx = pool_idx;
    pg->addr_be = ar->cfg.reflectors[pool_idx];
    pg->last_response_us = t;
    if (ar->cfg.retain_reflector_stats && rt->valid) {
        pg->baseline_valid = 1;
        for (int d = 0; d < 2; d++) {
            pg->base_us[d] = rt->base_us[d];
            pg->ewma_us[d] = rt->ewma_us[d];
        }
    }
}

/* swap in the first spare, bad reflector goes to the back of the queue */
static void replace_reflector(autorate_t *ar, int p, int64_t t, const char *why)
{
    pinger_t *pg = &ar->pingers[p];
    int n = ar->no_pingers;

    if (ar->pool_n <= n) {
        DBG(ar, "reflector %s %s, but no spare reflectors", ip_str(pg->addr_be), why);
        memset(pg->offences, 0, sizeof(pg->offences));
        pg->sum_offences = 0;
        return;
    }

    int bad = ar->order[p], fresh = ar->order[n];
    retained_t *rt = &ar->retained[bad];
    rt->valid = ar->cfg.retain_reflector_stats && pg->baseline_valid;
    for (int d = 0; d < 2; d++) {
        rt->base_us[d] = pg->base_us[d];
        rt->ewma_us[d] = pg->ewma_us[d];
    }

    memmove(&ar->order[n], &ar->order[n + 1], (size_t)(ar->pool_n - n - 1) * sizeof(int));
    ar->order[ar->pool_n - 1] = bad;
    ar->order[p] = fresh;

    syslog(LOG_INFO, "reflector %s %s, replaced by %s",
           ip_str(pg->addr_be), why, ip_str(ar->cfg.reflectors[fresh]));
    pinger_assign(ar, p, fresh, t);
    ar->n_replacements++;
}

static void process_response(autorate_t *ar, int p, int64_t dl_owd, int64_t ul_owd, int64_t t);

static void handle_reply(autorate_t *ar, const icmp_reply_t *r)
{
    const cake_config_t *c = &ar->cfg;
    int64_t t = now_us();
    int64_t owd[2];

    if (r->id != ar->ping_id)
        return;
    ping_slot_t *s = &ar->ring[r->seq & (PING_RING - 1)];
    if (s->pinger < 0 || s->pinger >= ar->no_pingers)
        return;
    pinger_t *pg = &ar->pingers[s->pinger];
    if (s->gen != pg->gen || s->dst_be != r->src_be || pg->addr_be != r->src_be)
        return;

    if (r->type == 0 && c->ping_type == PING_ECHO) {
        int64_t rtt = t - s->t_sent_us;
        if (rtt <= 0)
            return;
        owd[DL] = owd[UL] = rtt / 2;
    } else if (r->type == 14 && c->ping_type == PING_TIMESTAMP) {
        if (r->ts_originate != s->originate_ms)
            return;
        /* RFC 792: non-standard time */
        if (r->ts_receive >= MS_PER_DAY || r->ts_transmit >= MS_PER_DAY)
            return;
        uint32_t local = icmp_ms_since_midnight();
        owd[UL] = (int64_t)icmp_ts_diff_ms(r->ts_receive, s->originate_ms) * 1000;
        owd[DL] = (int64_t)icmp_ts_diff_ms(local, r->ts_transmit) * 1000;
    } else {
        return;
    }

    int p = s->pinger;
    s->pinger = -1;
    pg->received++;

    switch (ar->state) {
    case STATE_STALL:
        DBG(ar, "reflector response, connection stall ended");
        set_state(ar, STATE_RUNNING);
        /* fall through */
    case STATE_RUNNING:
        process_response(ar, p, owd[DL], owd[UL], t);
        break;
    default:
        break;
    }
}

static void icmp_cb(struct uloop_fd *u, unsigned int events)
{
    autorate_t *ar = container_of(u, autorate_t, icmp_ufd);
    icmp_reply_t r;
    (void)events;

    for (int i = 0; i < 64 && ar->pingers_active && ar->icmp_fd == u->fd; i++) {
        int k = icmp_recv(u->fd, &r);
        if (k == 0)
            break;
        if (k > 0)
            handle_reply(ar, &r);
    }
}

static int64_t t_next_ping_us;

static void ping_timer_cb(struct uloop_timeout *tm)
{
    autorate_t *ar = container_of(tm, autorate_t, ping_timer);
    const cake_config_t *c = &ar->cfg;

    if (!ar->pingers_active)
        return;

    int p = ar->ping_rr;
    ar->ping_rr = (p + 1) % ar->no_pingers;
    pinger_t *pg = &ar->pingers[p];
    uint16_t seq = ++ar->ping_seq;
    ping_slot_t *s = &ar->ring[seq & (PING_RING - 1)];
    int r;

    s->pinger = (int8_t)p;
    s->gen = pg->gen;
    s->dst_be = pg->addr_be;
    s->t_sent_us = now_us();

    if (c->ping_type == PING_TIMESTAMP) {
        s->originate_ms = icmp_ms_since_midnight();
        r = icmp_send_timestamp(ar->icmp_fd, pg->addr_be, ar->ping_id, seq, s->originate_ms);
    } else {
        r = icmp_send_echo(ar->icmp_fd, pg->addr_be, ar->ping_id, seq);
    }
    if (r == 0)
        pg->sent++;
    else
        DBG(ar, "ping %s: %s", ip_str(pg->addr_be), strerror(errno));

    int64_t now = now_us(), step = ping_response_interval_us(ar);
    t_next_ping_us += step;
    if (t_next_ping_us < now || t_next_ping_us > now + 2 * step)
        t_next_ping_us = now + step;
    uloop_timeout_set(tm, (int)((t_next_ping_us - now + 999) / 1000));
}

static int start_pingers(autorate_t *ar)
{
    if (ar->pingers_active)
        return 0;

    ar->icmp_fd = icmp_open(ar->ping_id, ar->cfg.bind_ping_to_ul_if ? ar->cfg.ul_if : NULL);
    if (ar->icmp_fd < 0) {
        set_error(ar, "cannot open ICMP socket: %s", strerror(errno));
        return -1;
    }
    ar->icmp_ufd.fd = ar->icmp_fd;
    ar->icmp_ufd.cb = icmp_cb;
    uloop_fd_add(&ar->icmp_ufd, ULOOP_READ);

    for (int i = 0; i < PING_RING; i++)
        ar->ring[i].pinger = -1;

    int64_t t = now_us();
    for (int p = 0; p < ar->no_pingers; p++)
        ar->pingers[p].last_response_us = t;
    ar->t_last_start_pingers_us = t;
    ar->pingers_active = 1;
    t_next_ping_us = t;
    ar->ping_timer.cb = ping_timer_cb;
    uloop_timeout_set(&ar->ping_timer, 1);
    return 0;
}

static void stop_pingers(autorate_t *ar)
{
    if (!ar->pingers_active)
        return;
    uloop_timeout_cancel(&ar->ping_timer);
    uloop_fd_delete(&ar->icmp_ufd);
    close(ar->icmp_fd);
    ar->icmp_fd = -1;
    ar->pingers_active = 0;
}

static void reset_detection(autorate_t *ar)
{
    memset(ar->delays, 0, sizeof(ar->delays));
    memset(ar->deltas_us, 0, sizeof(ar->deltas_us));
    ar->delays_idx = 0;
    for (int d = 0; d < 2; d++) {
        ar->sum_delays[d] = 0;
        ar->sum_deltas_us[d] = 0;
        ar->avg_delta_us[d] = 0;
        ar->bufferbloat[d] = 0;
    }
}

static void enter_idle(autorate_t *ar)
{
    set_state(ar, STATE_IDLE);
    DBG(ar, "connection idle, pingers sleeping");
    if (ar->cfg.min_shaper_rates_enforcement)
        set_min_shaper_rates(ar);
    stop_pingers(ar);
    ar->sustained_idle = 0;
    ar->t_sustained_idle_us = 0;
}

static void process_response(autorate_t *ar, int p, int64_t dl_owd, int64_t ul_owd, int64_t t)
{
    const cake_config_t *c = &ar->cfg;
    pinger_t *pg = &ar->pingers[p];
    int64_t owd[2] = { dl_owd, ul_owd }, delta[2];

    if (!pg->baseline_valid) {
        /* upstream starts at 100 ms; the first sample also works with clock offsets */
        pg->base_us[DL] = owd[DL];
        pg->base_us[UL] = owd[UL];
        pg->baseline_valid = 1;
    } else {
        int64_t d0 = owd[DL] - pg->base_us[DL], d1 = owd[UL] - pg->base_us[UL];
        int64_t a0 = d0 < 0 ? -d0 : d0, a1 = d1 < 0 ? -d1 : d1;
        /* clock step on either side shifts dl/ul in opposite directions: reseed */
        if (c->ping_type == PING_TIMESTAMP &&
            ((a0 > 1000000 && a1 > 1000000 && (d0 > 0) != (d1 > 0)) || a0 + a1 > 3000000000LL)) {
            DBG(ar, "reflector %s clock step detected, resetting baseline", ip_str(pg->addr_be));
            pg->base_us[DL] = owd[DL];
            pg->base_us[UL] = owd[UL];
        } else {
            for (int d = 0; d < 2; d++) {
                int64_t a = owd[d] >= pg->base_us[d] ? c->alpha_baseline_increase_fp
                                                     : c->alpha_baseline_decrease_fp;
                pg->base_us[d] = (a * owd[d] + (FP - a) * pg->base_us[d]) / FP;
            }
        }
    }

    int64_t high_pct = c->high_load_thr_fp / 10000;
    for (int d = 0; d < 2; d++) {
        delta[d] = owd[d] - pg->base_us[d];
        pg->last_owd_us[d] = owd[d];
        pg->last_delta_us[d] = delta[d];
    }
    if (ar->load_pct[DL] < high_pct && ar->load_pct[UL] < high_pct) {
        for (int d = 0; d < 2; d++)
            pg->ewma_us[d] = (c->alpha_delta_ewma_fp * delta[d] +
                              (FP - c->alpha_delta_ewma_fp) * pg->ewma_us[d]) / FP;
    }

    pg->last_response_us = t;
    ar->reflectors_last_us = t;
    ar->global_timeout = 0;

    int i = ar->delays_idx, w = c->bufferbloat_detection_window;
    for (int d = 0; d < 2; d++) {
        ar->sum_delays[d] -= ar->delays[d][i];
        ar->delays[d][i] = delta[d] > ar->comp_delay_thr_us[d];
        ar->sum_delays[d] += ar->delays[d][i];

        ar->sum_deltas_us[d] -= ar->deltas_us[d][i];
        ar->deltas_us[d][i] = delta[d];
        ar->sum_deltas_us[d] += delta[d];

        ar->avg_delta_us[d] = ar->sum_deltas_us[d] / w;
        ar->bufferbloat[d] = ar->sum_delays[d] >= c->bufferbloat_detection_thr;
    }
    if (++ar->delays_idx == w)
        ar->delays_idx = 0;

    for (int d = 0; d < 2; d++) {
        adjust_shaper_rate(ar, d, t);
        ar->shaper[d] = (uint32_t)clamp64(ar->shaper[d], ar->min_rate[d], ar->max_rate[d]);
    }
    apply_shaper_rates(ar, 0);

    if (c->enable_sleep_function) {
        if (ar->load_state[DL] == LOAD_IDLE && ar->load_state[UL] == LOAD_IDLE) {
            if (ar->sustained_idle) {
                ar->t_sustained_idle_us += t - ar->t_last_idle_us;
                ar->t_last_idle_us = t;
            } else {
                ar->sustained_idle = 1;
                ar->t_last_idle_us = t;
            }
            if (ar->t_sustained_idle_us > c->sustained_idle_sleep_thr_us)
                enter_idle(ar);
        } else {
            ar->sustained_idle = 0;
            ar->t_sustained_idle_us = 0;
        }
    }
}

static void handle_global_ping_response_timeout(autorate_t *ar, int64_t t)
{
    const cake_config_t *c = &ar->cfg;

    if (t - ar->reflectors_last_us < c->global_ping_response_timeout_us)
        return;

    if (!ar->global_timeout) {
        ar->global_timeout = 1;
        if (c->min_shaper_rates_enforcement)
            set_min_shaper_rates(ar);
        syslog(LOG_WARNING, "no reflector response for %lld s%s",
               (long long)(c->global_ping_response_timeout_us / 1000000),
               c->ping_type == PING_TIMESTAMP
                   ? " (do your reflectors answer ICMP timestamp requests?)" : "");
    }
    if (t - ar->t_last_start_pingers_us >= c->global_ping_response_timeout_us) {
        DBG(ar, "restarting pingers");
        stop_pingers(ar);
        start_pingers(ar);
    }
}

static int64_t t_next_rate_us;

static void rate_timer_cb(struct uloop_timeout *tm)
{
    autorate_t *ar = container_of(tm, autorate_t, rate_timer);
    const cake_config_t *c = &ar->cfg;

    rate_monitor_update(&ar->rm, &ar->achieved[DL], &ar->achieved[UL]);

    int64_t high_pct = c->high_load_thr_fp / 10000;
    for (int d = 0; d < 2; d++) {
        ar->achieved_updated[d] = 1;
        ar->load_pct[d] = ar->shaper[d]
            ? (uint32_t)((uint64_t)ar->achieved[d] * 100 / ar->shaper[d]) : 0;
        if (ar->load_pct[d] > high_pct)
            ar->load_state[d] = LOAD_HIGH;
        else if (ar->achieved[d] > c->connection_active_thr_kbps)
            ar->load_state[d] = LOAD_LOW;
        else
            ar->load_state[d] = LOAD_IDLE;
    }

    int64_t t = now_us();
    int traffic = ar->achieved[DL] > c->connection_stall_thr_kbps &&
                  ar->achieved[UL] > c->connection_stall_thr_kbps;

    switch (ar->state) {
    case STATE_RUNNING:
        if (t - ar->reflectors_last_us > c->stall_detection_thr * ping_response_interval_us(ar)) {
            handle_global_ping_response_timeout(ar, t);
            if (!traffic && ar->state == STATE_RUNNING) {
                DBG(ar, "no reflector response and no traffic: connection stall");
                set_state(ar, STATE_STALL);
                ar->n_stalls++;
            }
        }
        break;
    case STATE_IDLE:
        if (ar->achieved[DL] > c->connection_active_thr_kbps ||
            ar->achieved[UL] > c->connection_active_thr_kbps) {
            DBG(ar, "traffic resumed (%u/%u kbit/s), waking pingers",
                ar->achieved[DL], ar->achieved[UL]);
            set_state(ar, STATE_RUNNING);
            start_pingers(ar);
            ar->t_sustained_idle_us = 0;
            ar->sustained_idle = 0;
            ar->reflectors_last_us = t + 2 * c->reflector_ping_interval_us;
            ar->t_last_health_us = ar->reflectors_last_us;
        }
        break;
    case STATE_STALL:
        if (traffic) {
            DBG(ar, "traffic above stall threshold, connection stall ended");
            set_state(ar, STATE_RUNNING);
        }
        handle_global_ping_response_timeout(ar, t);
        break;
    default:
        return;
    }

    t = now_us();
    t_next_rate_us += ar->monitor_interval_us;
    if (t_next_rate_us < t || t_next_rate_us > t + 2 * ar->monitor_interval_us)
        t_next_rate_us = t + ar->monitor_interval_us;
    uloop_timeout_set(tm, (int)((t_next_rate_us - t + 999) / 1000));
}

static void health_check(autorate_t *ar, int64_t t)
{
    const cake_config_t *c = &ar->cfg;
    int n = ar->no_pingers;

    if (t > ar->t_last_replacement_us + c->reflector_replacement_interval_us) {
        ar->t_last_replacement_us = t;
        replace_reflector(ar, (int)(config_random() % (uint32_t)n), t, "rotated (periodic)");
        return;
    }

    if (t > ar->t_last_comparison_us + c->reflector_comparison_interval_us) {
        ar->t_last_comparison_us = t;

        int all_valid = 1;
        for (int p = 0; p < n; p++)
            all_valid &= ar->pingers[p].baseline_valid;

        if (all_valid && n > 1) {
            int64_t min_sum = INT64_MAX, min_ewma[2] = { INT64_MAX, INT64_MAX };
            for (int p = 0; p < n; p++) {
                pinger_t *pg = &ar->pingers[p];
                int64_t sum = pg->base_us[DL] + pg->base_us[UL];
                if (sum < min_sum) min_sum = sum;
                for (int d = 0; d < 2; d++)
                    if (pg->ewma_us[d] < min_ewma[d]) min_ewma[d] = pg->ewma_us[d];
            }
            for (int p = 0; p < n; p++) {
                pinger_t *pg = &ar->pingers[p];
                int64_t sum = pg->base_us[DL] + pg->base_us[UL];
                if (sum - min_sum > c->reflector_sum_owd_baselines_delta_thr_us) {
                    replace_reflector(ar, p, t, "has a high baseline");
                    return;
                }
                for (int d = 0; d < 2; d++) {
                    if (pg->ewma_us[d] - min_ewma[d] > c->reflector_owd_delta_ewma_delta_thr_us) {
                        replace_reflector(ar, p, t, d == DL ? "is jittery (download)"
                                                            : "is jittery (upload)");
                        return;
                    }
                }
            }
        }
    }

    /* at most one replacement per check */
    int idx = ar->offences_idx, replaced = 0;
    for (int p = 0; p < n; p++) {
        pinger_t *pg = &ar->pingers[p];
        uint8_t off = (t - pg->last_response_us) > c->reflector_response_deadline_us;
        pg->sum_offences += off - pg->offences[idx];
        pg->offences[idx] = off;
        if (pg->sum_offences >= c->reflector_misbehaving_detection_thr && !replaced) {
            replace_reflector(ar, p, t, "is not responding");
            replaced = 1;
        }
    }
    if (++ar->offences_idx >= c->reflector_misbehaving_detection_window)
        ar->offences_idx = 0;
}

static void health_timer_cb(struct uloop_timeout *tm)
{
    autorate_t *ar = container_of(tm, autorate_t, health_timer);
    int64_t t = now_us();

    if (ar->state == STATE_RUNNING && t > ar->t_last_health_us) {
        health_check(ar, t);
        ar->t_last_health_us = t;
    }
    uloop_timeout_set(tm, (int)(ar->cfg.reflector_health_check_interval_us / 1000));
}

static uint32_t read_mtu(const char *ifname)
{
    char path[64], buf[16];
    snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", ifname);
    FILE *f = fopen(path, "re");
    uint32_t mtu = 1500;
    if (f) {
        if (fgets(buf, sizeof(buf), f))
            mtu = (uint32_t)strtoul(buf, NULL, 10);
        fclose(f);
    }
    return mtu ? mtu : 1500;
}

static void compute_max_wire_bits(autorate_t *ar)
{
    const cake_config_t *c = &ar->cfg;
    int32_t overhead = c->cake_overhead == INT32_MIN ? 14 : c->cake_overhead;

    for (int d = 0; d < 2; d++) {
        uint32_t bits = 8 * (read_mtu(d == DL ? c->dl_if : c->ul_if) + (uint32_t)overhead);
        if (c->cake_atm == 1)
            bits = 424 * ((bits + 376) / 384);
        ar->max_wire_bits[d] = bits;
    }
}

static void make_opts(const autorate_t *ar, int d, cake_opts_t *o)
{
    const cake_config_t *c = &ar->cfg;
    memset(o, 0, sizeof(*o));
    o->overhead  = c->cake_overhead;
    o->mpu       = c->cake_mpu;
    o->atm       = (uint32_t)c->cake_atm;
    o->nat       = (uint32_t)c->cake_nat;
    o->diffserv  = (uint32_t)c->cake_diffserv;
    o->rtt_us    = c->cake_rtt_us;
    o->memlimit  = c->cake_memlimit_bytes;
    o->split_gso = (uint32_t)c->cake_split_gso;
    if (d == DL) {
        o->flow_mode = (uint32_t)c->cake_dl_flow_mode;
        o->ingress   = ar->dl_ifb_mode;
    } else {
        o->flow_mode   = (uint32_t)c->cake_ul_flow_mode;
        o->wash        = (uint32_t)c->cake_wash;
        o->ack_filter  = (uint32_t)c->cake_ack_filter;
        o->try_cake_mq = (uint32_t)c->cake_mq;
    }
}

static void qdisc_teardown(autorate_t *ar)
{
    const cake_config_t *c = &ar->cfg;
    if (ar->dl_ready || ar->created_ifb)
        tc_dl_teardown(ar->nl, c->ul_if, c->dl_if, ar->dl_ifb_mode, ar->created_ifb);
    if (ar->ul_ready)
        tc_ul_teardown(ar->nl, c->ul_if);
    if (ar->dl_ready || ar->ul_ready)
        syslog(LOG_INFO, "CAKE qdiscs removed");
    ar->dl_ready = ar->ul_ready = 0;
    ar->created_ifb = 0;
}

static int qdisc_setup(autorate_t *ar)
{
    const cake_config_t *c = &ar->cfg;
    cake_opts_t o;
    int r;

    if (!if_nametoindex(c->ul_if)) {
        set_error(ar, "waiting for interface %s", c->ul_if);
        return -1;
    }

    ar->dl_ifb_mode = tc_link_is_ifb(ar->nl, c->dl_if) != 0;

    make_opts(ar, UL, &o);
    r = tc_ul_setup(ar->nl, c->ul_if, ar->shaper[UL], &o, &ar->qdisc_kind[UL]);
    if (r < 0) {
        set_error(ar, "upload: %s", tc_nl_last_error(ar->nl));
        return -1;
    }
    ar->ul_ready = 1;

    make_opts(ar, DL, &o);
    r = tc_dl_setup(ar->nl, c->ul_if, c->dl_if, ar->dl_ifb_mode, ar->shaper[DL], &o,
                    &ar->created_ifb, &ar->qdisc_kind[DL]);
    if (r < 0) {
        set_error(ar, "download: %s", tc_nl_last_error(ar->nl));
        ar->dl_ready = 1;
        qdisc_teardown(ar);
        return -1;
    }
    ar->dl_ready = 1;

    ar->ul_ifindex = if_nametoindex(c->ul_if);
    ar->dl_ifindex = if_nametoindex(c->dl_if);
    ar->applied[DL] = ar->shaper[DL];
    ar->applied[UL] = ar->shaper[UL];
    ar->n_setups++;
    compute_max_wire_bits(ar);
    update_compensation(ar);
    set_error(ar, "%s", "");
    return 0;
}

static void enter_running(autorate_t *ar)
{
    int64_t t = now_us();

    rate_monitor_init(&ar->rm, ar->cfg.dl_if, ar->cfg.ul_if);
    reset_detection(ar);

    for (int d = 0; d < 2; d++) {
        ar->t_last_bufferbloat_us[d] = t;
        ar->t_last_decay_us[d] = t;
        ar->load_state[d] = LOAD_IDLE;
        ar->load_pct[d] = 0;
        ar->achieved[d] = 0;
        ar->achieved_updated[d] = 0;
    }
    /* path may have changed (PPPoE reconnect etc.) */
    for (int p = 0; p < ar->no_pingers; p++)
        ar->pingers[p].baseline_valid = 0;
    for (int i = 0; i < ar->pool_n; i++)
        ar->retained[i].valid = 0;

    ar->reflectors_last_us = t;
    ar->t_last_health_us = t;
    ar->t_last_replacement_us = t;
    ar->t_last_comparison_us = t;
    ar->global_timeout = 0;
    ar->sustained_idle = 0;
    ar->t_sustained_idle_us = 0;

    set_state(ar, STATE_RUNNING);
    syslog(LOG_INFO, "running: download %s %u kbit/s, upload %s %u kbit/s, %s pings to %d of %d reflectors",
           ar->cfg.dl_if, ar->shaper[DL], ar->cfg.ul_if, ar->shaper[UL],
           ar->cfg.ping_type == PING_TIMESTAMP ? "ICMP timestamp" : "ICMP echo",
           ar->no_pingers, ar->pool_n);

    start_pingers(ar);

    t_next_rate_us = now_us();
    ar->rate_timer.cb = rate_timer_cb;
    uloop_timeout_set(&ar->rate_timer, (int)(ar->monitor_interval_us / 1000));
    ar->health_timer.cb = health_timer_cb;
    uloop_timeout_set(&ar->health_timer, (int)(ar->cfg.reflector_health_check_interval_us / 1000));
}

static void leave_running(autorate_t *ar)
{
    stop_pingers(ar);
    uloop_timeout_cancel(&ar->rate_timer);
    uloop_timeout_cancel(&ar->health_timer);
    rate_monitor_close(&ar->rm);
    qdisc_teardown(ar);
    ar->shaper[DL] = ar->base_rate[DL];
    ar->shaper[UL] = ar->base_rate[UL];
    for (int d = 0; d < 2; d++) {
        ar->achieved[d] = 0;
        ar->load_state[d] = LOAD_IDLE;
        ar->load_pct[d] = 0;
    }
    set_state(ar, STATE_WAITING);
}

static void try_setup(autorate_t *ar)
{
    if (qdisc_setup(ar) == 0)
        enter_running(ar);
}

static void if_timer_cb(struct uloop_timeout *tm)
{
    autorate_t *ar = container_of(tm, autorate_t, if_timer);
    const cake_config_t *c = &ar->cfg;

    if (ar->state == STATE_WAITING) {
        try_setup(ar);
    } else {
        unsigned int ul = if_nametoindex(c->ul_if), dl = if_nametoindex(c->dl_if);
        const char *why = NULL;
        if (ar->need_resetup)
            why = "qdisc update failed";
        else if (!ul)
            why = "upload interface disappeared";
        else if (ul != ar->ul_ifindex)
            why = "upload interface was recreated";
        else if (!dl || dl != ar->dl_ifindex)
            why = "download interface changed";
        else if (tc_verify(ar->nl, c->ul_if, c->dl_if, ar->dl_ifb_mode) == 0)
            why = tc_nl_last_error(ar->nl);
        if (why) {
            syslog(LOG_WARNING, "%s, re-initialising shaping", why);
            ar->need_resetup = 0;
            leave_running(ar);
            try_setup(ar);
        }
    }
    uloop_timeout_set(tm, (int)(c->if_up_check_interval_us / 1000));
}

static void link_event_cb(struct uloop_fd *u, unsigned int events)
{
    autorate_t *ar = container_of(u, autorate_t, link_ufd);
    (void)events;
    if (tc_link_monitor_drain(u->fd))
        uloop_timeout_set(&ar->if_timer, 300);
}

static void startup_cb(struct uloop_timeout *tm)
{
    autorate_t *ar = container_of(tm, autorate_t, startup_timer);
    try_setup(ar);
    ar->if_timer.cb = if_timer_cb;
    uloop_timeout_set(&ar->if_timer, (int)(ar->cfg.if_up_check_interval_us / 1000));
}

static void usage(void)
{
    fprintf(stderr,
            "darkmoon " DARKMOON_VERSION " - adaptive CAKE bandwidth controller\n"
            "usage: darkmoon [-d] [-f] [instance]\n"
            "  -d  debug logging (same as option debug '1')\n"
            "  -f  run even if the instance is disabled in UCI\n"
            "status: ubus call darkmoon.<instance> status\n");
}

int main(int argc, char **argv)
{
    autorate_t *ar = &AR;
    const char *section = "primary";
    int force_debug = 0, force_enable = 0, opt;

    while ((opt = getopt(argc, argv, "dfh")) != -1) {
        switch (opt) {
        case 'd': force_debug = 1; break;
        case 'f': force_enable = 1; break;
        default:  usage(); return opt == 'h' ? 0 : 1;
        }
    }
    if (optind < argc)
        section = argv[optind];

    openlog("darkmoon", LOG_PID | (isatty(2) ? LOG_PERROR : 0), LOG_DAEMON);

    memset(ar, 0, sizeof(*ar));
    ar->icmp_fd = -1;
    ar->link_ufd.fd = -1;
    ar->rm.fd[0] = ar->rm.fd[1] = -1;
    ar->t_started_us = ar->t_state_us = now_us();

    if (config_load(section, &ar->cfg) < 0) {
        fprintf(stderr, "darkmoon: invalid configuration for instance '%s' (see logread)\n", section);
        return 1;
    }
    cake_config_t *c = &ar->cfg;
    if (force_debug)
        c->debug = 1;
    if (!c->enabled && !force_enable) {
        syslog(LOG_INFO, "instance '%s' is disabled", section);
        return 0;
    }
    setlogmask(LOG_UPTO(c->debug ? LOG_DEBUG : LOG_INFO));

    /* adjust_*=0 pins the rate at base, like upstream */
    uint32_t mn[2] = { c->min_dl_shaper_rate_kbps, c->min_ul_shaper_rate_kbps };
    uint32_t bs[2] = { c->base_dl_shaper_rate_kbps, c->base_ul_shaper_rate_kbps };
    uint32_t mx[2] = { c->max_dl_shaper_rate_kbps, c->max_ul_shaper_rate_kbps };
    int adj[2] = { c->adjust_dl_shaper_rate, c->adjust_ul_shaper_rate };
    for (int d = 0; d < 2; d++) {
        ar->base_rate[d] = bs[d];
        ar->min_rate[d] = adj[d] ? mn[d] : bs[d];
        ar->max_rate[d] = adj[d] ? mx[d] : bs[d];
        ar->shaper[d] = bs[d];
        ar->qdisc_kind[d] = "cake";
    }

    ar->pool_n = c->no_reflectors;
    ar->no_pingers = c->no_pingers;
    for (int i = 0; i < ar->pool_n; i++)
        ar->order[i] = i;
    for (int p = 0; p < ar->no_pingers; p++)
        pinger_assign(ar, p, ar->order[p], ar->t_started_us);

    if (c->debug && c->bufferbloat_refractory_period_us <
                    c->bufferbloat_detection_window * ping_response_interval_us(ar))
        syslog(LOG_DEBUG, "warning: bufferbloat_refractory_period is shorter than the time to "
                          "refill the detection window; consider increasing it");
    if (c->reflector_response_deadline_us < 2 * c->reflector_ping_interval_us)
        syslog(LOG_NOTICE, "reflector_response_deadline_s < 2 x reflector_ping_interval_s; "
                           "healthy reflectors may be replaced");

    ar->ping_id = (uint16_t)(getpid() & 0xffff);
    ar->state = STATE_WAITING;
    update_compensation(ar);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    uloop_init();

    ar->nl = tc_nl_open();
    if (!ar->nl) {
        syslog(LOG_ERR, "cannot open netlink socket");
        return 1;
    }

    int lfd = tc_link_monitor_open();
    if (lfd >= 0) {
        ar->link_ufd.fd = lfd;
        ar->link_ufd.cb = link_event_cb;
        uloop_fd_add(&ar->link_ufd, ULOOP_READ);
    }

    ubus_init(ar);

    syslog(LOG_INFO, "darkmoon %s starting instance '%s'", DARKMOON_VERSION, section);
    ar->startup_timer.cb = startup_cb;
    ar->if_timer.cb = if_timer_cb;
    if (c->startup_wait_us > 0)
        syslog(LOG_INFO, "waiting %lld s before startup", (long long)(c->startup_wait_us / 1000000));
    uloop_timeout_set(&ar->startup_timer, (int)(c->startup_wait_us / 1000));

    uloop_run();

    syslog(LOG_INFO, "stopping instance '%s'", section);
    stop_pingers(ar);
    uloop_timeout_cancel(&ar->rate_timer);
    uloop_timeout_cancel(&ar->health_timer);
    uloop_timeout_cancel(&ar->if_timer);
    uloop_timeout_cancel(&ar->startup_timer);
    rate_monitor_close(&ar->rm);
    qdisc_teardown(ar);
    ubus_done();
    if (ar->link_ufd.fd >= 0) {
        uloop_fd_delete(&ar->link_ufd);
        close(ar->link_ufd.fd);
    }
    uloop_done();
    tc_nl_close(ar->nl);
    closelog();
    return 0;
}
