#include "icmp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/icmp.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef ICMP_TIMESTAMP
#define ICMP_TIMESTAMP      13
#define ICMP_TIMESTAMPREPLY 14
#endif

#define ECHO_MAGIC 0x444d4f4fu   /* "DMOO" */

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t cksum;
    uint16_t id;
    uint16_t seq;
};

struct icmp_ts {
    struct icmp_hdr h;
    uint32_t originate;
    uint32_t receive;
    uint32_t transmit;
};

struct icmp_echo {
    struct icmp_hdr h;
    uint32_t magic;
    uint32_t pad[3];
};

static uint16_t csum16(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return htons((uint16_t)~sum);
}

int icmp_open(uint16_t id, const char *bind_if)
{
    int fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (fd < 0) {
        syslog(LOG_ERR, "icmp: socket(): %m");
        return -1;
    }

    struct icmp_filter filt;
    filt.data = ~((1u << ICMP_ECHOREPLY) | (1u << ICMP_TIMESTAMPREPLY));
    if (setsockopt(fd, SOL_RAW, ICMP_FILTER, &filt, sizeof(filt)) < 0)
        syslog(LOG_DEBUG, "icmp: ICMP_FILTER: %m");

    /* only wake up for our own echo/timestamp replies */
    struct sock_filter code[] = {
        BPF_STMT(BPF_LDX | BPF_B | BPF_MSH, 0),                  /* X = 4*(ip[0]&0xf) */
        BPF_STMT(BPF_LD | BPF_B | BPF_IND, 0),                   /* A = icmp.type     */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP_ECHOREPLY, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ICMP_TIMESTAMPREPLY, 0, 3),
        BPF_STMT(BPF_LD | BPF_H | BPF_IND, 4),                   /* A = icmp.id       */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, id, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0xffff),
        BPF_STMT(BPF_RET | BPF_K, 0),
    };
    struct sock_fprog prog = { .len = sizeof(code) / sizeof(code[0]), .filter = code };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)) < 0)
        syslog(LOG_DEBUG, "icmp: SO_ATTACH_FILTER: %m");

    if (bind_if && bind_if[0]) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bind_if,
                       (socklen_t)strlen(bind_if) + 1) < 0)
            syslog(LOG_WARNING, "icmp: SO_BINDTODEVICE(%s): %m", bind_if);
    }

    char junk[256];
    while (recv(fd, junk, sizeof(junk), 0) > 0)
        ;

    return fd;
}

static int send_pkt(int fd, uint32_t dst_be, void *pkt, size_t len)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = dst_be;

    if (sendto(fd, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -1;
    return 0;
}

int icmp_send_echo(int fd, uint32_t dst_be, uint16_t id, uint16_t seq)
{
    struct icmp_echo p;
    memset(&p, 0, sizeof(p));
    p.h.type = 8;  /* ICMP_ECHO */
    p.h.id = htons(id);
    p.h.seq = htons(seq);
    p.magic = htonl(ECHO_MAGIC);
    p.h.cksum = csum16(&p, sizeof(p));
    return send_pkt(fd, dst_be, &p, sizeof(p));
}

int icmp_send_timestamp(int fd, uint32_t dst_be, uint16_t id, uint16_t seq,
                        uint32_t originate_ms)
{
    struct icmp_ts p;
    memset(&p, 0, sizeof(p));
    p.h.type = ICMP_TIMESTAMP;
    p.h.id = htons(id);
    p.h.seq = htons(seq);
    p.originate = htonl(originate_ms);
    p.h.cksum = csum16(&p, sizeof(p));
    return send_pkt(fd, dst_be, &p, sizeof(p));
}

int icmp_recv(int fd, icmp_reply_t *r)
{
    uint8_t buf[512];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;

    if (n < 20)
        return -1;
    size_t ihl = (size_t)(buf[0] & 0x0f) * 4;
    if (ihl < 20 || (size_t)n < ihl + sizeof(struct icmp_hdr))
        return -1;

    const struct icmp_hdr *h = (const struct icmp_hdr *)(buf + ihl);
    size_t plen = (size_t)n - ihl;

    memset(r, 0, sizeof(*r));
    r->src_be = src.sin_addr.s_addr;
    r->type = h->type;
    r->id = ntohs(h->id);
    r->seq = ntohs(h->seq);

    if (h->type == ICMP_ECHOREPLY) {
        const struct icmp_echo *e = (const struct icmp_echo *)h;
        if (plen < sizeof(*e) || e->magic != htonl(ECHO_MAGIC))
            return -1;
        return 1;
    }
    if (h->type == ICMP_TIMESTAMPREPLY) {
        const struct icmp_ts *t = (const struct icmp_ts *)h;
        if (plen < sizeof(*t))
            return -1;
        r->ts_originate = ntohl(t->originate);
        r->ts_receive   = ntohl(t->receive);
        r->ts_transmit  = ntohl(t->transmit);
        return 1;
    }
    return -1;
}

uint32_t icmp_ms_since_midnight(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return (uint32_t)(ms % MS_PER_DAY);
}

int32_t icmp_ts_diff_ms(uint32_t later, uint32_t earlier)
{
    int64_t d = (int64_t)later - (int64_t)earlier;
    if (d > (int64_t)MS_PER_DAY / 2)  d -= MS_PER_DAY;
    if (d < -(int64_t)MS_PER_DAY / 2) d += MS_PER_DAY;
    return (int32_t)d;
}
