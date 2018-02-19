//
// Created by on 10/20/17.
//

#ifndef SOCKNM_MARQ_H
#define SOCKNM_MARQ_H

#if defined(_WIN32) || defined(WIN32)
#error "posix only now!"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

#include "dlist.h"
#include "util.h"


#define NMQ_BUF_SIZE 1600
#define NMQ_SND_BUF_NUM_DEF 150
#define NMQ_RCV_BUF_NUM_DEF 500

#define NMQ_STATE_SEND_FAILURE (-1)

#define NMQ_SSTHRESH_MIN 2
#define NMQ_SSTHRESH_DEF 200
#define NMQ_RMT_WND_DEF NMQ_RCV_BUF_NUM_DEF
#define NMQ_FLUSH_INTERVAL_DEF 50
#define NMQ_PROBE_WAIT_MS_DEF (500)

#define NMQ_TROUBLE_TOLERANCE_MIN 1
#define NMQ_TROUBLE_TOLERANCE_DEF 2
#define NMQ_TROUBLE_TOLERANCE_MAX 10
#define NMQ_DUP_ACK_LIM_DEF 3
#define NMQ_DUP_ACK_LIM_MIN 2
#define NMQ_DUP_ACK_LIM_MAX 5
#define NMQ_MAX_TRY 50

#define NMQ_CWND_INIT 100

#define NMQ_RTO_NODELAY 30
#define NMQ_RTO_MIN 100
#define NMQ_RTO_DEF 200
#define NMQ_RTO_MAX 60000


#define NMQ_NO_DATA (0)
#define NMQ_SEND_EOF (-1)
#define NMQ_RECV_EOF (-2)
#define NMQ_ERR_CONV_DIFF (-4)
#define NMQ_ERR_WRONG_CMD (-5)
#define NMQ_ERR_INVALID_SN (-6)
#define NMQ_ERR_DUPLICATE_SN (-7)
#define NMQ_ERR_RCV_QUE_INCONSISTANCE (-8)
#define NMQ_ERR_UNITIALIZED (-9)

#define NMQ_ERR_MSG_SIZE (-10)
#define NMQ_ERR_MSG_BROKEN (-11)
#define ERR_DATA_TOO_LONG (-12)
#define NMQ_ERR_RCV_BUF_NO_MEM (-13)
#define NMQ_ERR_SEND_ON_SHUTDOWNED (-15)
#define NMQ_ERR_ACK_BUF_LEN (-20)
#define NMQ_ERR_WRONG_INPUT (-21)
// 1500 minus ip, udp  and kcp header size.
#define NMQ_MTU_DEF 1400
#define NMQ_HEAD_SIZE 24
#define SEG_HEAD_SIZE NMQ_HEAD_SIZE

#define NMQ_FC_ALPHA_DEF 0.5

#define NMQ_SEGMENT_POOL_CAP_DEF 16

#ifndef OFFSETOF
#define OFFSETOF(TYPE, MEMBER) \
    ((size_t)&(((TYPE *)0)->MEMBER))
#endif

#ifndef ADDRESS_FOR
#define ADDRESS_FOR(TYPE, MEMBER, mem_addr) \
    ((TYPE*)(((char *)(mem_addr)) - OFFSETOF(TYPE, MEMBER)))
#endif

typedef struct segment_s {
    dlist head;
    uint32_t resendts;
    int32_t n_sent;
    int32_t rto;
    uint16_t dupacks;

    // data that will be sent to peer in the order they appear here. 24 bytes in total
    uint32_t conv;
    uint8_t cmd;            // send, ack, wnd_probe
    uint32_t sn;
    uint8_t frag;
    uint16_t wnd;     // tell peer self wnd size. (65535 * 1500 / (2 ^ 20) / 8 = 12.5MB
    uint32_t una;
    uint32_t sendts;     // to estimate rtt
    uint32_t len;
    char data[1];   // don't use pointer and must stay at last position
} segment;

typedef struct segment_pool_s {
    dlist seg_list;
    uint8_t left;
    uint8_t CAP;
    uint32_t MTU;
} segment_pool;

typedef struct fc_s {
    float ssth_alpha;
    float incr;     // float is always 32bit large whether in 32-bit or 64-bit machine
    uint32_t cwnd;    // congestion window. maximum is MAX_SND_BUF_NUM. unit: MTU
    uint32_t ssthresh;   // unit: MSS + SEG_HEAD_SIZE
    uint8_t TROUBLE_TOLERANCE;
    int8_t DUP_ACK_LIM;
    uint8_t in_trouble;
    uint32_t max_lost_sn;
    uint32_t MSS;
} fc_s;


typedef struct rto_helper_s {
    uint32_t srtt;
    uint32_t mdev;
    uint32_t mdev_max;
    uint32_t rttvar;
    uint32_t rtt_seq;
} rto_helper_s;

typedef void *(*nmq_malloc_fn)(size_t size);

typedef void (*nmq_free_fn)(void *ptr);

typedef struct nmq_stat_t {
    uint32_t nrtt;
    uint64_t nrtt_tot;
    uint32_t bytes_send;
    uint32_t bytes_send_tot;
} nmq_stat_t;

typedef struct nmq_s {
    uint32_t conv;
    void *arg;

    uint32_t current;
    int8_t inited;
    uint16_t flush_interval;

    uint32_t rmt_wnd;

    uint32_t snd_una;    // sent unacknowledged for this client
    uint32_t snd_nxt;
    dlist snd_buf;
    dlist snd_que;
    dlnode **snd_sn_to_node;       // the size is MAX_SND_BUF_NUM

    segment_pool pool;

    uint32_t rcv_nxt;
    dlist rcv_buf;    // use array to stroe it
    dlnode **rcv_sn_to_node; // sn: &seg.head    // the size is MAX_RCV_BUF_NUM
    dlist rcv_que;

    uint32_t nrcv_que;    // number of packet in receive queue now
    uint32_t nrcv_buf;    // number of packets in receive buf now
    uint32_t nsnd_que;
    // estimate bandwidth. set this number to a number larger than bandwidth. unit: MSS + SEG_HEAD_SIZE
    uint32_t MAX_SND_BUF_NUM;    // use limited or unlimited que?? current is limited que
    uint32_t MAX_RCV_BUF_NUM;

    uint32_t ackmaxnum;
    uint32_t *acklist;  // acklist[i] is sn, acklist[i]+1 is ts_send. size is 2 * MAX_RCV_BUF_NUM
    uint32_t ackcount;
    uint32_t ack_failures;

    uint8_t fc_on;
    fc_s fc;

    uint32_t rto;
    nmq_stat_t stat;

    uint8_t nodelay;

    uint32_t ts_probe_wait;
    uint8_t probe_pending;

    rto_helper_s rto_helper;

    int32_t MAX_PKT_TRY; // maximum times to send packet. or failure
    int8_t state;

    uint32_t MSS;    // not including head size. MSS + SEG_HEAD_SIZE + OTHER_PROTOCOL_HEAD_SIZE = MTU
    uint32_t MTU;

    int32_t (*output_cb)(const char *data, const int len, struct nmq_s *nmq, void *arg);

    void (*failure_cb)(struct nmq_s *nmq, uint32_t cause_sn);

    uint32_t peer_fin_sn;
    char fin_sn;


    uint8_t steady_on;   // no blast send. default on.
    uint32_t BYTES_PER_FLUSH;
} NMQ;

typedef int32_t (*nmq_output_cb)(const char *data, const int len, struct nmq_s *nmq, void *arg);

typedef void (*nmq_failure_cb)(struct nmq_s *nmq, uint32_t cause_sn);

void nmq_update(NMQ *q, uint32_t current);

// we regard
void nmq_flush(NMQ *q, uint32_t current);

// upper <-> nmq
int32_t nmq_send(NMQ *q, const char *data, const int len);

void nmq_shutdown_send(NMQ *q);

// > 0 for specifc reason.
// < 0 if buf is too small and -retval is size that buf should be.
int32_t nmq_recv(NMQ *q, char *buf, const int buf_size);

int32_t nmq_output(NMQ *q, const char *data, const int len);

int32_t nmq_input(NMQ *q, const char *buf, const int buf_size);

NMQ *nmq_new(uint32_t conv, void *arg);

void nmq_destroy(NMQ *q);

uint32_t nmq_get_conv(const char *buf);

void nmq_set_output_cb(NMQ *q, nmq_output_cb cb);

void nmq_set_wnd_size(NMQ *nmq, uint32_t sndwnd, uint32_t rcvwnd);

void nmq_set_fc_on(NMQ *q, uint8_t on);


void nmq_start(NMQ *q); // first memeory allocation
void nmq_set_ssthresh(NMQ *q, uint32_t ssthresh);

void nmq_set_trouble_tolerance(NMQ *q, uint8_t n_tolerance);

void nmq_set_dup_acks_limit(NMQ *q, uint8_t lim);

// MSS <= MTU - SEG_HEAD_SIZE - sum(OTHER_PROTOCOL_HEAD_SIZE)
void nmq_set_nmq_mtu(NMQ *q, uint32_t MTU);

void nmq_set_max_attempt(NMQ *q, uint32_t max_try, nmq_failure_cb cb);

void nmq_set_interval(NMQ *q, uint32_t interval);

void nmq_set_fc_alpha(NMQ *q, float alpha);

void nmq_set_segment_pool_cap(NMQ *q, uint8_t CAP);

void nmq_set_steady(NMQ *q, uint8_t steady_on);

#ifdef __cplusplus
}
#endif

#endif //SOCKNM_MARQ_H
