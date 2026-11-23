#ifndef DARKMOON_CONFIG_H
#define DARKMOON_CONFIG_H

#include <stdint.h>

#define MAX_POOL            128
#define MAX_PINGERS          32
#define MAX_BB_WINDOW        64
#define MAX_OFFENCE_WINDOW   64
#define MAX_IF_NAME          16

/* _us = microseconds, _fp = fixed point * 1000000 (no FPU needed) */

enum ping_type {
    PING_ECHO      = 0,
    PING_TIMESTAMP = 1,
};

typedef struct {
    char     instance[32];
    int      enabled;
    int      debug;

    char     dl_if[MAX_IF_NAME];
    char     ul_if[MAX_IF_NAME];
    int      adjust_dl_shaper_rate;
    int      adjust_ul_shaper_rate;

    uint32_t min_dl_shaper_rate_kbps, base_dl_shaper_rate_kbps, max_dl_shaper_rate_kbps;
    uint32_t min_ul_shaper_rate_kbps, base_ul_shaper_rate_kbps, max_ul_shaper_rate_kbps;
    uint32_t connection_active_thr_kbps;

    int      ping_type;
    int      no_pingers;
    int64_t  reflector_ping_interval_us;
    int      bind_ping_to_ul_if;
    int      randomize_reflectors;
    char     reflectors_file[256];
    int      no_reflectors;
    uint32_t reflectors[MAX_POOL];         /* network byte order */

    int64_t  dl_avg_owd_delta_max_adjust_up_thr_us;
    int64_t  ul_avg_owd_delta_max_adjust_up_thr_us;
    int64_t  dl_owd_delta_delay_thr_us;
    int64_t  ul_owd_delta_delay_thr_us;
    int64_t  dl_avg_owd_delta_max_adjust_down_thr_us;
    int64_t  ul_avg_owd_delta_max_adjust_down_thr_us;

    int64_t  alpha_baseline_increase_fp;
    int64_t  alpha_baseline_decrease_fp;
    int64_t  alpha_delta_ewma_fp;

    int64_t  shaper_rate_min_adjust_down_bufferbloat_fp;
    int64_t  shaper_rate_max_adjust_down_bufferbloat_fp;
    int64_t  shaper_rate_min_adjust_up_load_high_fp;
    int64_t  shaper_rate_max_adjust_up_load_high_fp;
    int64_t  shaper_rate_adjust_down_load_low_fp;
    int64_t  shaper_rate_adjust_up_load_low_fp;

    int      bufferbloat_detection_window;
    int      bufferbloat_detection_thr;
    int64_t  high_load_thr_fp;
    int64_t  bufferbloat_refractory_period_us;
    int64_t  decay_refractory_period_us;

    int64_t  reflector_health_check_interval_us;
    int64_t  reflector_response_deadline_us;
    int      reflector_misbehaving_detection_window;
    int      reflector_misbehaving_detection_thr;
    int64_t  reflector_replacement_interval_us;
    int64_t  reflector_comparison_interval_us;
    int64_t  reflector_sum_owd_baselines_delta_thr_us;
    int64_t  reflector_owd_delta_ewma_delta_thr_us;
    int      retain_reflector_stats;

    int      stall_detection_thr;
    uint32_t connection_stall_thr_kbps;
    int64_t  global_ping_response_timeout_us;
    int      enable_sleep_function;
    int64_t  sustained_idle_sleep_thr_us;
    int      min_shaper_rates_enforcement;

    int64_t  startup_wait_us;
    int64_t  monitor_achieved_rates_interval_us;
    int64_t  if_up_check_interval_us;

    int32_t  cake_overhead;        /* INT32_MIN: leave to the kernel */
    uint32_t cake_mpu;
    int      cake_atm;
    int      cake_nat;
    int      cake_wash;
    int      cake_ack_filter;
    int      cake_diffserv;
    int      cake_dl_flow_mode;
    int      cake_ul_flow_mode;
    uint32_t cake_rtt_us;
    uint32_t cake_memlimit_bytes;
    int      cake_split_gso;
    int      cake_mq;
} cake_config_t;

int config_load(const char *section, cake_config_t *cfg);

uint32_t config_random(void);

#endif
