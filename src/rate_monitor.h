#ifndef DARKMOON_RATE_MONITOR_H
#define DARKMOON_RATE_MONITOR_H

#include <stdint.h>

typedef struct {
    char     path[2][96];
    int      fd[2];
    uint64_t prev[2];
    int64_t  t_prev_us;
} rate_monitor_t;

void    rate_monitor_init(rate_monitor_t *rm, const char *dl_if, const char *ul_if);
void    rate_monitor_close(rate_monitor_t *rm);

int64_t rate_monitor_update(rate_monitor_t *rm, uint32_t *dl_kbps, uint32_t *ul_kbps);

#endif
