#include "config.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <uci.h>

/* "0.3" with scale 1000000 -> 300000 */
static int64_t parse_fixed(const char *s, int64_t scale)
{
    int neg = 0;
    int64_t ip = 0, fp = 0, div = 1;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    while (*s >= '0' && *s <= '9' && ip < INT64_MAX / 100 / scale)
        ip = ip * 10 + (*s++ - '0');
    while (*s >= '0' && *s <= '9') s++;          /* overflow guard */

    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (div < 1000000000LL) {
                fp = fp * 10 + (*s - '0');
                div *= 10;
            }
            s++;
        }
    }

    int64_t r = ip * scale + fp * scale / div;
    return neg ? -r : r;
}

static const char *opt_get(struct uci_context *ctx, struct uci_section *s,
                           const char *name)
{
    struct uci_option *o = uci_lookup_option(ctx, s, name);
    if (o && o->type == UCI_TYPE_STRING && o->v.string[0])
        return o->v.string;
    return NULL;
}

#define GET_STR(field, name) do { const char *_v = opt_get(ctx, sec, name); \
    if (_v) snprintf(c->field, sizeof(c->field), "%s", _v); } while (0)
#define GET_INT(field, name) do { const char *_v = opt_get(ctx, sec, name); \
    if (_v) c->field = (int)strtol(_v, NULL, 10); } while (0)
#define GET_U32(field, name) do { const char *_v = opt_get(ctx, sec, name); \
    if (_v) c->field = (uint32_t)strtoul(_v, NULL, 10); } while (0)
#define GET_MS(field, name)  do { const char *_v = opt_get(ctx, sec, name); \
    if (_v) c->field = parse_fixed(_v, 1000LL); } while (0)
#define GET_S(field, name)   do { const char *_v = opt_get(ctx, sec, name); \
    if (_v) c->field = parse_fixed(_v, 1000000LL); } while (0)
#define GET_FP(field, name)  do { const char *_v = opt_get(ctx, sec, name); \
    if (_v) c->field = parse_fixed(_v, 1000000LL); } while (0)

struct name_val { const char *name; int val; };

static int lookup_enum(const char *v, const struct name_val *tbl, int def)
{
    if (!v) return def;
    if ((*v >= '0' && *v <= '9') || *v == '-')
        return (int)strtol(v, NULL, 10);
    for (; tbl->name; tbl++)
        if (!strcasecmp(v, tbl->name))
            return tbl->val;
    syslog(LOG_WARNING, "config: unknown value '%s', using default", v);
    return def;
}

static const struct name_val diffserv_tbl[] = {
    { "diffserv3", 0 }, { "diffserv4", 1 }, { "diffserv8", 2 },
    { "besteffort", 3 }, { "precedence", 4 }, { NULL, 0 }
};
static const struct name_val flow_tbl[] = {
    { "flowblind", 0 }, { "srchost", 1 }, { "dsthost", 2 }, { "hosts", 3 },
    { "flows", 4 }, { "dual-srchost", 5 }, { "dual-dsthost", 6 },
    { "triple-isolate", 7 }, { "triple", 7 }, { NULL, 0 }
};
static const struct name_val ack_tbl[] = {
    { "no-ack-filter", 0 }, { "off", 0 }, { "ack-filter", 1 },
    { "ack-filter-aggressive", 2 }, { "aggressive", 2 }, { NULL, 0 }
};
static const struct name_val atm_tbl[] = {
    { "noatm", 0 }, { "none", 0 }, { "atm", 1 }, { "ptm", 2 }, { NULL, 0 }
};

/* link-layer presets, values from tc-cake(8) */
struct link_preset { const char *name; int32_t overhead; uint32_t mpu; int atm; };
static const struct link_preset link_presets[] = {
    { "raw",             INT32_MIN, 0,  0 },
    { "ethernet",        38,        84, 0 },
    { "ethernet-vlan",   42,        84, 0 },
    { "docsis",          18,        64, 0 },
    { "pppoe-ptm",       30,        0,  2 },
    { "bridged-ptm",     22,        0,  2 },
    { "pppoe-llcsnap",   40,        0,  1 },
    { "pppoe-vcmux",     32,        0,  1 },
    { "pppoa-llc",       14,        0,  1 },
    { "pppoa-vcmux",     10,        0,  1 },
    { "bridged-llcsnap", 32,        0,  1 },
    { "bridged-vcmux",   24,        0,  1 },
    { "ipoa-llcsnap",    16,        0,  1 },
    { "ipoa-vcmux",      8,         0,  1 },
    { NULL, 0, 0, 0 }
};

static const char *default_reflectors[] = {
    "1.1.1.1", "1.0.0.1",
    "8.8.8.8", "8.8.4.4",
    "9.9.9.9", "9.9.9.10", "9.9.9.11",
    "94.140.14.15", "94.140.14.140", "94.140.14.141",
    "94.140.15.15", "94.140.15.16",
    "64.6.65.6",
    "156.154.70.1", "156.154.70.2", "156.154.70.3", "156.154.70.4",
    "156.154.70.5", "156.154.71.1", "156.154.71.2", "156.154.71.3",
    "156.154.71.4", "156.154.71.5",
    "208.67.220.2", "208.67.220.123", "208.67.220.220",
    "208.67.222.2", "208.67.222.123",
    "185.228.168.9", "185.228.168.10",
    NULL
};

static void set_defaults(cake_config_t *c)
{
    memset(c, 0, sizeof(*c));

    snprintf(c->dl_if, sizeof(c->dl_if), "ifb-wan");
    snprintf(c->ul_if, sizeof(c->ul_if), "wan");
    c->adjust_dl_shaper_rate = 1;
    c->adjust_ul_shaper_rate = 1;

    c->min_dl_shaper_rate_kbps  = 5000;
    c->base_dl_shaper_rate_kbps = 20000;
    c->max_dl_shaper_rate_kbps  = 80000;
    c->min_ul_shaper_rate_kbps  = 5000;
    c->base_ul_shaper_rate_kbps = 20000;
    c->max_ul_shaper_rate_kbps  = 35000;
    c->connection_active_thr_kbps = 2000;

    c->ping_type = PING_ECHO;
    c->no_pingers = 6;
    c->reflector_ping_interval_us = 300000;
    c->randomize_reflectors = 1;

    c->dl_avg_owd_delta_max_adjust_up_thr_us   = 10000;
    c->ul_avg_owd_delta_max_adjust_up_thr_us   = 10000;
    c->dl_owd_delta_delay_thr_us               = 30000;
    c->ul_owd_delta_delay_thr_us               = 30000;
    c->dl_avg_owd_delta_max_adjust_down_thr_us = 60000;
    c->ul_avg_owd_delta_max_adjust_down_thr_us = 60000;

    c->alpha_baseline_increase_fp = 1000;      /* 0.001 */
    c->alpha_baseline_decrease_fp = 900000;    /* 0.9   */
    c->alpha_delta_ewma_fp        = 95000;     /* 0.095 */

    c->shaper_rate_min_adjust_down_bufferbloat_fp = 990000;
    c->shaper_rate_max_adjust_down_bufferbloat_fp = 750000;
    c->shaper_rate_min_adjust_up_load_high_fp     = 1000000;
    c->shaper_rate_max_adjust_up_load_high_fp     = 1040000;
    c->shaper_rate_adjust_down_load_low_fp        = 990000;
    c->shaper_rate_adjust_up_load_low_fp          = 1010000;

    c->bufferbloat_detection_window = 6;
    c->bufferbloat_detection_thr    = 3;
    c->high_load_thr_fp             = 750000;
    c->bufferbloat_refractory_period_us = 300000;
    c->decay_refractory_period_us       = 1000000;

    c->reflector_health_check_interval_us      = 1000000;
    c->reflector_response_deadline_us          = 1000000;
    c->reflector_misbehaving_detection_window  = 60;
    c->reflector_misbehaving_detection_thr     = 3;
    c->reflector_replacement_interval_us       = 3600LL * 1000000LL;
    c->reflector_comparison_interval_us        = 60LL * 1000000LL;
    c->reflector_sum_owd_baselines_delta_thr_us = 20000;
    c->reflector_owd_delta_ewma_delta_thr_us    = 10000;
    c->retain_reflector_stats = 1;

    c->stall_detection_thr             = 5;
    c->connection_stall_thr_kbps       = 10;
    c->global_ping_response_timeout_us = 10000000;
    c->enable_sleep_function           = 1;
    c->sustained_idle_sleep_thr_us     = 60000000;
    c->min_shaper_rates_enforcement    = 0;

    c->startup_wait_us                    = 0;
    c->monitor_achieved_rates_interval_us = 200000;
    c->if_up_check_interval_us            = 10000000;

    c->cake_overhead     = 38;      /* "ethernet" preset */
    c->cake_mpu          = 84;
    c->cake_atm          = 0;
    c->cake_nat          = 1;
    c->cake_wash         = 1;
    c->cake_ack_filter   = 0;
    c->cake_diffserv     = 1;       /* diffserv4    */
    c->cake_dl_flow_mode = 6;       /* dual-dsthost */
    c->cake_ul_flow_mode = 5;       /* dual-srchost */
    c->cake_split_gso    = 1;
}

static uint32_t rng_state;

static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void rng_seed(void)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, &rng_state, sizeof(rng_state)) != sizeof(rng_state))
        rng_state = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    if (fd >= 0) close(fd);
    if (!rng_state) rng_state = 0x9e3779b9u;
}

uint32_t config_random(void) { return rng_next(); }

static int add_reflector(cake_config_t *c, const char *s, int *seen, int quiet)
{
    struct in_addr a;
    char buf[64];
    size_t n = strcspn(s, " \t\r\n#,;");

    if (n == 0 || n >= sizeof(buf))
        return -1;
    memcpy(buf, s, n);
    buf[n] = '\0';

    if (inet_pton(AF_INET, buf, &a) != 1) {
        if (!quiet)
            syslog(LOG_WARNING, "config: ignoring reflector '%s' (IPv4 address required)", buf);
        return -1;
    }
    for (int i = 0; i < c->no_reflectors; i++)
        if (c->reflectors[i] == a.s_addr)
            return 0;

    (*seen)++;
    if (c->no_reflectors < MAX_POOL) {
        c->reflectors[c->no_reflectors++] = a.s_addr;
    } else {
        /* reservoir sampling: random subset of large lists */
        uint32_t j = rng_next() % (uint32_t)*seen;
        if (j < MAX_POOL)
            c->reflectors[j] = a.s_addr;
    }
    return 1;
}

static void shuffle_reflectors(cake_config_t *c)
{
    for (int i = c->no_reflectors - 1; i > 0; i--) {
        int j = (int)(rng_next() % (uint32_t)(i + 1));
        uint32_t t = c->reflectors[i];
        c->reflectors[i] = c->reflectors[j];
        c->reflectors[j] = t;
    }
}

static void load_reflectors_file(cake_config_t *c)
{
    FILE *fp = fopen(c->reflectors_file, "re");
    char line[256];
    int seen = 0, before = c->no_reflectors, bad = 0;

    if (!fp) {
        syslog(LOG_WARNING, "config: cannot open reflectors_file '%s', using reflector list",
               c->reflectors_file);
        return;
    }

    c->no_reflectors = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        if (add_reflector(c, p, &seen, 1) < 0)
            bad++;
    }
    fclose(fp);
    if (bad)
        syslog(LOG_INFO, "config: %s: skipped %d lines without an IPv4 address",
               c->reflectors_file, bad);

    if (c->no_reflectors == 0) {
        syslog(LOG_WARNING, "config: reflectors_file '%s' has no usable IPv4 entries",
               c->reflectors_file);
        c->no_reflectors = before;
    } else {
        syslog(LOG_INFO, "config: loaded %d reflectors from '%s' (%d candidates)",
               c->no_reflectors, c->reflectors_file, seen);
    }
}

#define CLAMP(v, lo, hi) do { if ((v) < (lo)) (v) = (lo); if ((v) > (hi)) (v) = (hi); } while (0)

static int validate(cake_config_t *c)
{
    int ok = 1;

    if (!c->ul_if[0] || !c->dl_if[0]) {
        syslog(LOG_ERR, "config: dl_if and ul_if must be set");
        ok = 0;
    }
    if (!strcmp(c->ul_if, c->dl_if)) {
        syslog(LOG_ERR, "config: dl_if and ul_if must differ");
        ok = 0;
    }

#define CHECK_RATES(d) \
    if (c->min_##d##_shaper_rate_kbps < 64) c->min_##d##_shaper_rate_kbps = 64; \
    if (c->min_##d##_shaper_rate_kbps > c->max_##d##_shaper_rate_kbps) { \
        syslog(LOG_ERR, "config: min_" #d "_shaper_rate_kbps (%u) > max (%u)", \
               c->min_##d##_shaper_rate_kbps, c->max_##d##_shaper_rate_kbps); \
        ok = 0; \
    } else { \
        CLAMP(c->base_##d##_shaper_rate_kbps, c->min_##d##_shaper_rate_kbps, \
              c->max_##d##_shaper_rate_kbps); \
    }
    CHECK_RATES(dl)
    CHECK_RATES(ul)
#undef CHECK_RATES

    if (c->no_reflectors < 1) {
        syslog(LOG_ERR, "config: no usable reflectors");
        ok = 0;
    }
    CLAMP(c->no_pingers, 1, MAX_PINGERS);
    if (c->no_pingers > c->no_reflectors && c->no_reflectors > 0) {
        syslog(LOG_WARNING, "config: no_pingers %d > reflectors %d, reducing",
               c->no_pingers, c->no_reflectors);
        c->no_pingers = c->no_reflectors;
    }
    if (c->ping_type != PING_ECHO && c->ping_type != PING_TIMESTAMP)
        c->ping_type = PING_ECHO;

    CLAMP(c->reflector_ping_interval_us, 10000, 10000000);
    CLAMP(c->bufferbloat_detection_window, 1, MAX_BB_WINDOW);
    CLAMP(c->bufferbloat_detection_thr, 1, c->bufferbloat_detection_window);
    CLAMP(c->reflector_misbehaving_detection_window, 1, MAX_OFFENCE_WINDOW);
    CLAMP(c->reflector_misbehaving_detection_thr, 1, c->reflector_misbehaving_detection_window);
    CLAMP(c->stall_detection_thr, 1, 1000);

    CLAMP(c->alpha_baseline_increase_fp, 1, 1000000);
    CLAMP(c->alpha_baseline_decrease_fp, 1, 1000000);
    CLAMP(c->alpha_delta_ewma_fp, 1, 1000000);
    CLAMP(c->high_load_thr_fp, 10000, 1000000);

    CLAMP(c->shaper_rate_min_adjust_down_bufferbloat_fp, 100000, 1000000);
    CLAMP(c->shaper_rate_max_adjust_down_bufferbloat_fp, 100000, 1000000);
    CLAMP(c->shaper_rate_min_adjust_up_load_high_fp, 1000000, 2000000);
    CLAMP(c->shaper_rate_max_adjust_up_load_high_fp, 1000000, 2000000);
    CLAMP(c->shaper_rate_adjust_down_load_low_fp, 100000, 1000000);
    CLAMP(c->shaper_rate_adjust_up_load_low_fp, 1000000, 2000000);

    CLAMP(c->monitor_achieved_rates_interval_us, 50000, 10000000);
    CLAMP(c->reflector_health_check_interval_us, 100000, 3600000000LL);
    CLAMP(c->if_up_check_interval_us, 1000000, 3600000000LL);
    CLAMP(c->startup_wait_us, 0, 3600000000LL);
    if (c->reflector_replacement_interval_us < 60000000)
        c->reflector_replacement_interval_us = 60000000;
    if (c->reflector_comparison_interval_us < 1000000)
        c->reflector_comparison_interval_us = 1000000;

    CLAMP(c->cake_diffserv, 0, 4);
    CLAMP(c->cake_dl_flow_mode, 0, 7);
    CLAMP(c->cake_ul_flow_mode, 0, 7);
    CLAMP(c->cake_atm, 0, 2);
    CLAMP(c->cake_ack_filter, 0, 2);

    return ok ? 0 : -1;
}

int config_load(const char *section, cake_config_t *c)
{
    struct uci_context *ctx;
    struct uci_package *pkg = NULL;
    struct uci_section *sec;
    const char *v;
    int seen = 0;

    set_defaults(c);
    rng_seed();
    snprintf(c->instance, sizeof(c->instance), "%s", section);

    ctx = uci_alloc_context();
    if (!ctx)
        return -1;
    if (uci_load(ctx, "darkmoon", &pkg) != UCI_OK || !pkg) {
        syslog(LOG_ERR, "config: cannot load /etc/config/darkmoon");
        uci_free_context(ctx);
        return -1;
    }
    sec = uci_lookup_section(ctx, pkg, section);
    if (!sec) {
        syslog(LOG_ERR, "config: section '%s' not found", section);
        uci_unload(ctx, pkg);
        uci_free_context(ctx);
        return -1;
    }

    GET_INT(enabled, "enabled");
    GET_INT(debug,   "debug");

    GET_STR(dl_if, "dl_if");
    GET_STR(ul_if, "ul_if");
    GET_INT(adjust_dl_shaper_rate, "adjust_dl_shaper_rate");
    GET_INT(adjust_ul_shaper_rate, "adjust_ul_shaper_rate");

    GET_U32(min_dl_shaper_rate_kbps,  "min_dl_shaper_rate_kbps");
    GET_U32(base_dl_shaper_rate_kbps, "base_dl_shaper_rate_kbps");
    GET_U32(max_dl_shaper_rate_kbps,  "max_dl_shaper_rate_kbps");
    GET_U32(min_ul_shaper_rate_kbps,  "min_ul_shaper_rate_kbps");
    GET_U32(base_ul_shaper_rate_kbps, "base_ul_shaper_rate_kbps");
    GET_U32(max_ul_shaper_rate_kbps,  "max_ul_shaper_rate_kbps");
    GET_U32(connection_active_thr_kbps, "connection_active_thr_kbps");

    v = opt_get(ctx, sec, "ping_type");
    if (v) c->ping_type = (!strcmp(v, "timestamp") || !strcmp(v, "1")) ? PING_TIMESTAMP : PING_ECHO;
    GET_INT(no_pingers, "no_pingers");
    GET_S  (reflector_ping_interval_us, "reflector_ping_interval_s");
    GET_INT(bind_ping_to_ul_if, "bind_ping_to_ul_if");
    GET_INT(randomize_reflectors, "randomize_reflectors");
    GET_STR(reflectors_file, "reflectors_file");

    {
        struct uci_option *o = uci_lookup_option(ctx, sec, "reflectors");
        if (o && o->type == UCI_TYPE_LIST) {
            struct uci_element *e;
            uci_foreach_element(&o->v.list, e)
                add_reflector(c, e->name, &seen, 0);
        } else if (o && o->type == UCI_TYPE_STRING) {
            char *dup = strdup(o->v.string), *tok, *save = NULL;
            for (tok = strtok_r(dup, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
                add_reflector(c, tok, &seen, 0);
            free(dup);
        }
    }

    GET_MS(dl_avg_owd_delta_max_adjust_up_thr_us,   "dl_avg_owd_delta_max_adjust_up_thr_ms");
    GET_MS(ul_avg_owd_delta_max_adjust_up_thr_us,   "ul_avg_owd_delta_max_adjust_up_thr_ms");
    GET_MS(dl_owd_delta_delay_thr_us,               "dl_owd_delta_delay_thr_ms");
    GET_MS(ul_owd_delta_delay_thr_us,               "ul_owd_delta_delay_thr_ms");
    GET_MS(dl_avg_owd_delta_max_adjust_down_thr_us, "dl_avg_owd_delta_max_adjust_down_thr_ms");
    GET_MS(ul_avg_owd_delta_max_adjust_down_thr_us, "ul_avg_owd_delta_max_adjust_down_thr_ms");

    GET_FP(alpha_baseline_increase_fp, "alpha_baseline_increase");
    GET_FP(alpha_baseline_decrease_fp, "alpha_baseline_decrease");
    GET_FP(alpha_delta_ewma_fp,        "alpha_delta_ewma");

    GET_FP(shaper_rate_min_adjust_down_bufferbloat_fp, "shaper_rate_min_adjust_down_bufferbloat");
    GET_FP(shaper_rate_max_adjust_down_bufferbloat_fp, "shaper_rate_max_adjust_down_bufferbloat");
    GET_FP(shaper_rate_min_adjust_up_load_high_fp,     "shaper_rate_min_adjust_up_load_high");
    GET_FP(shaper_rate_max_adjust_up_load_high_fp,     "shaper_rate_max_adjust_up_load_high");
    GET_FP(shaper_rate_adjust_down_load_low_fp,        "shaper_rate_adjust_down_load_low");
    GET_FP(shaper_rate_adjust_up_load_low_fp,          "shaper_rate_adjust_up_load_low");

    GET_INT(bufferbloat_detection_window, "bufferbloat_detection_window");
    GET_INT(bufferbloat_detection_thr,    "bufferbloat_detection_thr");
    GET_FP (high_load_thr_fp,             "high_load_thr");
    GET_MS (bufferbloat_refractory_period_us, "bufferbloat_refractory_period_ms");
    GET_MS (decay_refractory_period_us,       "decay_refractory_period_ms");

    GET_S  (reflector_health_check_interval_us,      "reflector_health_check_interval_s");
    GET_S  (reflector_response_deadline_us,          "reflector_response_deadline_s");
    GET_INT(reflector_misbehaving_detection_window,  "reflector_misbehaving_detection_window");
    GET_INT(reflector_misbehaving_detection_thr,     "reflector_misbehaving_detection_thr");
    GET_S  (reflector_replacement_interval_us,       "reflector_replacement_interval_s");
    GET_S  (reflector_comparison_interval_us,        "reflector_comparison_interval_s");
    GET_MS (reflector_sum_owd_baselines_delta_thr_us, "reflector_sum_owd_baselines_delta_thr_ms");
    GET_MS (reflector_owd_delta_ewma_delta_thr_us,    "reflector_owd_delta_ewma_delta_thr_ms");
    GET_INT(retain_reflector_stats,                  "retain_reflector_stats");

    GET_INT(stall_detection_thr,             "stall_detection_thr");
    GET_U32(connection_stall_thr_kbps,       "connection_stall_thr_kbps");
    GET_S  (global_ping_response_timeout_us, "global_ping_response_timeout_s");
    GET_INT(enable_sleep_function,           "enable_sleep_function");
    GET_S  (sustained_idle_sleep_thr_us,     "sustained_idle_sleep_thr_s");
    GET_INT(min_shaper_rates_enforcement,    "min_shaper_rates_enforcement");

    GET_S  (startup_wait_us,                    "startup_wait_s");
    GET_MS (monitor_achieved_rates_interval_us, "monitor_achieved_rates_interval_ms");
    GET_S  (if_up_check_interval_us,            "if_up_check_interval_s");

    v = opt_get(ctx, sec, "link_layer");
    if (!v && (opt_get(ctx, sec, "cake_overhead") || opt_get(ctx, sec, "cake_atm")))
        v = "custom";   /* darkmoon 1.x config */
    if (v && strcmp(v, "custom") != 0) {
        const struct link_preset *p;
        for (p = link_presets; p->name; p++)
            if (!strcmp(p->name, v)) break;
        if (p->name) {
            c->cake_overhead = p->overhead;
            c->cake_mpu      = p->mpu;
            c->cake_atm      = p->atm;
        } else {
            syslog(LOG_WARNING, "config: unknown link_layer '%s', using ethernet", v);
        }
    } else if (v) {
        c->cake_overhead = 0;
        c->cake_mpu      = 0;
        c->cake_atm      = 0;
        const char *o = opt_get(ctx, sec, "cake_overhead");
        if (o) c->cake_overhead = (int32_t)strtol(o, NULL, 10);
        GET_U32(cake_mpu, "cake_mpu");
        c->cake_atm = lookup_enum(opt_get(ctx, sec, "cake_atm"), atm_tbl, 0);
    }

    GET_INT(cake_nat,       "cake_nat");
    GET_INT(cake_wash,      "cake_wash");
    GET_INT(cake_split_gso, "cake_split_gso");
    GET_INT(cake_mq,        "cake_mq");
    c->cake_ack_filter = lookup_enum(opt_get(ctx, sec, "cake_ack_filter"), ack_tbl, c->cake_ack_filter);
    c->cake_diffserv   = lookup_enum(opt_get(ctx, sec, "cake_diffserv"), diffserv_tbl, c->cake_diffserv);

    {
        const char *shared = opt_get(ctx, sec, "cake_flow_mode");
        const char *dl = opt_get(ctx, sec, "cake_dl_flow_mode");
        const char *ul = opt_get(ctx, sec, "cake_ul_flow_mode");
        if (dl && !strcmp(dl, "-1")) dl = NULL;
        if (ul && !strcmp(ul, "-1")) ul = NULL;
        c->cake_dl_flow_mode = lookup_enum(dl ? dl : shared, flow_tbl, c->cake_dl_flow_mode);
        c->cake_ul_flow_mode = lookup_enum(ul ? ul : shared, flow_tbl, c->cake_ul_flow_mode);
    }

    v = opt_get(ctx, sec, "cake_rtt_ms");
    if (v) c->cake_rtt_us = (uint32_t)parse_fixed(v, 1000);
    v = opt_get(ctx, sec, "cake_memlimit_kb");
    if (v) c->cake_memlimit_bytes = (uint32_t)strtoul(v, NULL, 10) * 1024u;

    uci_unload(ctx, pkg);
    uci_free_context(ctx);

    if (c->no_reflectors == 0)
        for (int i = 0; default_reflectors[i]; i++)
            add_reflector(c, default_reflectors[i], &seen, 0);

    if (c->reflectors_file[0])
        load_reflectors_file(c);

    if (c->randomize_reflectors)
        shuffle_reflectors(c);

    return validate(c);
}
