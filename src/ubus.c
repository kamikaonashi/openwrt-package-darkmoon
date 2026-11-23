/* ubus call darkmoon.<instance> status */
#include <arpa/inet.h>
#include <libubus.h>
#include <libubox/blobmsg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "autorate.h"

static struct ubus_auto_conn conn;
static struct blob_buf b;
static autorate_t *g_ar;
static char obj_name[64];

static void add_dir(const char *name, autorate_t *ar, int d)
{
    const cake_config_t *c = &ar->cfg;
    void *t = blobmsg_open_table(&b, name);

    blobmsg_add_string(&b, "interface", d == DL ? c->dl_if : c->ul_if);
    blobmsg_add_string(&b, "qdisc", ar->qdisc_kind[d]);
    blobmsg_add_u8(&b, "adjust", d == DL ? c->adjust_dl_shaper_rate : c->adjust_ul_shaper_rate);
    blobmsg_add_u32(&b, "shaper_kbps", ar->shaper[d]);
    blobmsg_add_u32(&b, "applied_kbps", ar->applied[d]);
    blobmsg_add_u32(&b, "achieved_kbps", ar->achieved[d]);
    blobmsg_add_u32(&b, "min_kbps", ar->min_rate[d]);
    blobmsg_add_u32(&b, "base_kbps", ar->base_rate[d]);
    blobmsg_add_u32(&b, "max_kbps", ar->max_rate[d]);
    blobmsg_add_u32(&b, "load_percent", ar->load_pct[d]);
    blobmsg_add_string(&b, "load", load_name(ar->load_state[d]));
    blobmsg_add_u8(&b, "bufferbloat", ar->bufferbloat[d]);
    blobmsg_add_u32(&b, "delayed_samples", (uint32_t)ar->sum_delays[d]);
    blobmsg_add_u64(&b, "avg_owd_delta_us", (uint64_t)ar->avg_delta_us[d]);
    blobmsg_add_u64(&b, "delay_thr_us", (uint64_t)ar->comp_delay_thr_us[d]);
    blobmsg_add_u64(&b, "max_adjust_up_thr_us", (uint64_t)ar->comp_up_thr_us[d]);
    blobmsg_add_u64(&b, "max_adjust_down_thr_us", (uint64_t)ar->comp_down_thr_us[d]);
    blobmsg_add_u32(&b, "bufferbloat_events", ar->n_bufferbloat[d]);
    blobmsg_close_table(&b, t);
}

static int status_cb(struct ubus_context *ctx, struct ubus_object *obj,
                     struct ubus_request_data *req, const char *method,
                     struct blob_attr *msg)
{
    autorate_t *ar = g_ar;
    const cake_config_t *c = &ar->cfg;
    int64_t t = now_us();
    char ip[INET_ADDRSTRLEN];
    (void)obj; (void)method; (void)msg;

    blob_buf_init(&b, 0);
    blobmsg_add_string(&b, "instance", c->instance);
    blobmsg_add_string(&b, "version", DARKMOON_VERSION);
    blobmsg_add_string(&b, "state", state_name(ar->state));
    blobmsg_add_u32(&b, "state_age_s", (uint32_t)((t - ar->t_state_us) / 1000000));
    blobmsg_add_u32(&b, "uptime_s", (uint32_t)((t - ar->t_started_us) / 1000000));
    blobmsg_add_string(&b, "error", ar->last_error);
    blobmsg_add_string(&b, "ping_type", c->ping_type == PING_TIMESTAMP ? "timestamp" : "echo");
    blobmsg_add_u8(&b, "pingers_active", ar->pingers_active);
    blobmsg_add_u8(&b, "dl_ifb_mode", ar->dl_ifb_mode);
    blobmsg_add_u32(&b, "rate_changes", ar->n_rate_changes);
    blobmsg_add_u32(&b, "reflector_replacements", ar->n_replacements);
    blobmsg_add_u32(&b, "stalls", ar->n_stalls);
    blobmsg_add_u32(&b, "setups", ar->n_setups);
    blobmsg_add_u32(&b, "last_response_ms",
                    ar->reflectors_last_us && t > ar->reflectors_last_us
                        ? (uint32_t)((t - ar->reflectors_last_us) / 1000) : 0);

    add_dir("download", ar, DL);
    add_dir("upload", ar, UL);

    void *arr = blobmsg_open_array(&b, "reflectors");
    for (int p = 0; p < ar->no_pingers; p++) {
        const pinger_t *pg = &ar->pingers[p];
        struct in_addr a = { .s_addr = pg->addr_be };
        void *r = blobmsg_open_table(&b, NULL);
        inet_ntop(AF_INET, &a, ip, sizeof(ip));
        blobmsg_add_string(&b, "address", ip);
        blobmsg_add_u8(&b, "valid", pg->baseline_valid);
        blobmsg_add_u32(&b, "sent", pg->sent);
        blobmsg_add_u32(&b, "received", pg->received);
        blobmsg_add_u32(&b, "offences", (uint32_t)pg->sum_offences);
        blobmsg_add_u32(&b, "last_response_ms",
                        t > pg->last_response_us ? (uint32_t)((t - pg->last_response_us) / 1000) : 0);
        blobmsg_add_u64(&b, "dl_baseline_us", (uint64_t)pg->base_us[DL]);
        blobmsg_add_u64(&b, "ul_baseline_us", (uint64_t)pg->base_us[UL]);
        blobmsg_add_u64(&b, "dl_delta_us", (uint64_t)pg->last_delta_us[DL]);
        blobmsg_add_u64(&b, "ul_delta_us", (uint64_t)pg->last_delta_us[UL]);
        blobmsg_add_u64(&b, "dl_delta_ewma_us", (uint64_t)pg->ewma_us[DL]);
        blobmsg_add_u64(&b, "ul_delta_ewma_us", (uint64_t)pg->ewma_us[UL]);
        blobmsg_close_table(&b, r);
    }
    blobmsg_close_array(&b, arr);
    blobmsg_add_u32(&b, "spare_reflectors",
                    (uint32_t)(ar->pool_n > ar->no_pingers ? ar->pool_n - ar->no_pingers : 0));

    ubus_send_reply(ctx, req, b.head);
    return 0;
}

static const struct ubus_method methods[] = {
    UBUS_METHOD_NOARG("status", status_cb),
};

static struct ubus_object_type obj_type =
    UBUS_OBJECT_TYPE("darkmoon", methods);

static struct ubus_object obj = {
    .name = obj_name,
    .type = &obj_type,
    .methods = methods,
    .n_methods = ARRAY_SIZE(methods),
};

static void connect_cb(struct ubus_context *ctx)
{
    int r = ubus_add_object(ctx, &obj);
    if (r)
        syslog(LOG_WARNING, "ubus: cannot register %s: %s", obj_name, ubus_strerror(r));
}

void ubus_init(autorate_t *ar)
{
    g_ar = ar;
    snprintf(obj_name, sizeof(obj_name), "darkmoon.%s", ar->cfg.instance);
    conn.cb = connect_cb;
    ubus_auto_connect(&conn);
}

void ubus_done(void)
{
    ubus_auto_shutdown(&conn);
    blob_buf_free(&b);
}
