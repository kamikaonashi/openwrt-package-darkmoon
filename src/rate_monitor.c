#include "rate_monitor.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int read_counter(rate_monitor_t *rm, int i, uint64_t *out)
{
    char buf[32];

    if (rm->fd[i] < 0) {
        rm->fd[i] = open(rm->path[i], O_RDONLY | O_CLOEXEC);
        if (rm->fd[i] < 0)
            return -1;
    }
    ssize_t n = pread(rm->fd[i], buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(rm->fd[i]);
        rm->fd[i] = -1;
        return -1;
    }
    buf[n] = '\0';
    *out = strtoull(buf, NULL, 10);
    return 0;
}

static int is_ifb_or_veth(const char *ifname)
{
    return !strncmp(ifname, "ifb", 3) || !strncmp(ifname, "veth", 4);
}

void rate_monitor_init(rate_monitor_t *rm, const char *dl_if, const char *ul_if)
{
    memset(rm, 0, sizeof(*rm));
    rm->fd[0] = rm->fd[1] = -1;

    snprintf(rm->path[0], sizeof(rm->path[0]),
             "/sys/class/net/%s/statistics/tx_bytes", dl_if);
    snprintf(rm->path[1], sizeof(rm->path[1]),
             "/sys/class/net/%s/statistics/%s_bytes", ul_if,
             is_ifb_or_veth(ul_if) ? "rx" : "tx");

    for (int i = 0; i < 2; i++)
        if (read_counter(rm, i, &rm->prev[i]) < 0)
            rm->prev[i] = 0;
    rm->t_prev_us = mono_us();
}

void rate_monitor_close(rate_monitor_t *rm)
{
    for (int i = 0; i < 2; i++) {
        if (rm->fd[i] >= 0)
            close(rm->fd[i]);
        rm->fd[i] = -1;
    }
}

int64_t rate_monitor_update(rate_monitor_t *rm, uint32_t *dl_kbps, uint32_t *ul_kbps)
{
    int64_t now = mono_us();
    int64_t elapsed = now - rm->t_prev_us;
    uint32_t *out[2] = { dl_kbps, ul_kbps };

    if (elapsed <= 0)
        elapsed = 1;

    for (int i = 0; i < 2; i++) {
        uint64_t cur;
        if (read_counter(rm, i, &cur) < 0)
            cur = rm->prev[i];
        uint64_t d = (cur >= rm->prev[i]) ? cur - rm->prev[i] : 0;
        uint64_t kbps = d * 8000u / (uint64_t)elapsed;
        *out[i] = kbps > UINT32_MAX ? UINT32_MAX : (uint32_t)kbps;
        rm->prev[i] = cur;
    }
    rm->t_prev_us = now;
    return elapsed;
}
