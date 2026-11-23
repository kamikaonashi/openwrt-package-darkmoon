#ifndef DARKMOON_ICMP_H
#define DARKMOON_ICMP_H

#include <stdint.h>

#define MS_PER_DAY 86400000u

typedef struct {
    uint32_t src_be;
    uint8_t  type;
    uint16_t id;
    uint16_t seq;
    /* timestamp replies: ms since midnight UTC */
    uint32_t ts_originate;
    uint32_t ts_receive;
    uint32_t ts_transmit;
} icmp_reply_t;

int icmp_open(uint16_t id, const char *bind_if);

int icmp_send_echo(int fd, uint32_t dst_be, uint16_t id, uint16_t seq);
int icmp_send_timestamp(int fd, uint32_t dst_be, uint16_t id, uint16_t seq,
                        uint32_t originate_ms);

/* 1 = reply, 0 = nothing to read, -1 = ignored */
int icmp_recv(int fd, icmp_reply_t *r);

uint32_t icmp_ms_since_midnight(void);

int32_t icmp_ts_diff_ms(uint32_t later, uint32_t earlier);

#endif
