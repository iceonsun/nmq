//
// Created by on 10/20/17.
//

#ifndef SOCKNM_MARQ_H
#define SOCKNM_MARQ_H

#if defined(_WIN32) || defined(WIN32)
#error posix only now!
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
    IUINT32 resendts;
    IINT32 n_sent;
    IINT32 rto;
    IUINT16 dupacks;

    // data that will be sent to peer in the order they appear here. 24 bytes in total
    IUINT32 conv;
    IUINT8 cmd;            // send, ack, wnd_probe
    IUINT32 sn;
    IUINT8 frag;
    IUINT16 wnd;     // tell peer self wnd size. (65535 * 1500 / (2 ^ 20) / 8 = 12.5MB
    IUINT32 una;
    IUINT32 sendts;     // to estimate rtt
    IUINT32 len;
    char data[1];   // don't use pointer and must stay at last position
} segment;

typedef struct segment_pool_s {
    dlist seg_list;
    IUINT8 left;
    IUINT8 CAP;
    IUINT32 MTU;
} segment_pool;

typedef struct fc_s {
    float ssth_alpha;
    float incr;     // float is always 32bit large whether in 32-bit or 64-bit machine
    IUINT32 cwnd;    // congestion window. maximum is MAX_SND_BUF_NUM. unit: MTU
    IUINT32 ssthresh;   // unit: MSS + SEG_HEAD_SIZE
    IUINT8 TROUBLE_TOLERANCE;
    IINT8 DUP_ACK_LIM;
    IUINT8 in_trouble;
    IUINT32 max_lost_sn;
    IUINT32 MSS;
} fc_s;


typedef struct rto_helper_s {
    IUINT32 srtt;
    IUINT32 mdev;
    IUINT32 mdev_max;
    IUINT32 rttvar;
    IUINT32 rtt_seq;
} rto_helper_s;

typedef void *(*nmq_malloc_fn)(size_t size);

typedef void (*nmq_free_fn)(void *ptr);

typedef struct nmq_stat_t {
    IUINT32 nrtt;
    IUINT64 nrtt_tot;
    IUINT32 bytes_send;
    IUINT32 bytes_send_tot;
} nmq_stat_t;

typedef struct nmq_s {
    IUINT32 conv;
    void *arg;

    IUINT32 current;
    IINT8 inited;
    IUINT16 flush_interval;

    IUINT32 rmt_wnd;

    IUINT32 snd_una;    // sent unacknowledged for this client
    IUINT32 snd_nxt;
    dlist snd_buf;
    dlist snd_que;
    dlnode **snd_sn_to_node;       // the size is MAX_SND_BUF_NUM

    segment_pool pool;

    IUINT32 rcv_nxt;
    dlist rcv_buf;    // use array to stroe it
    dlnode **rcv_sn_to_node; // sn: &seg.head    // the size is MAX_RCV_BUF_NUM
    dlist rcv_que;

    IUINT32 nrcv_que;    // number of packet in receive queue now
    IUINT32 nrcv_buf;    // number of packets in receive buf now
    IUINT32 nsnd_que;
    // estimate bandwidth. set this number to a number larger than bandwidth. unit: MSS + SEG_HEAD_SIZE
    IUINT32 MAX_SND_BUF_NUM;    // use limited or unlimited que?? current is limited que
    IUINT32 MAX_RCV_BUF_NUM;

    IUINT32 ackmaxnum;
    IUINT32 *acklist;  // acklist[i] is sn, acklist[i]+1 is ts_send. size is 2 * MAX_RCV_BUF_NUM
    IUINT32 ackcount;
    IUINT32 ack_failures;

    IUINT8 fc_on;
    fc_s fc;

    IUINT32 rto;
    nmq_stat_t stat;

    IUINT8 nodelay;

    IUINT32 ts_probe_wait;
    IUINT8 probe_pending;

    rto_helper_s rto_helper;

    IINT32 MAX_PKT_TRY; // maximum times to send packet. or failure
    IINT8 state;

    IUINT32 MSS;    // not including head size. MSS + SEG_HEAD_SIZE + OTHER_PROTOCOL_HEAD_SIZE = MTU
    IUINT32 MTU;

    IINT32 (*output_cb)(const char *data, const int len, struct nmq_s *nmq, void *arg);

    void (*failure_cb)(struct nmq_s *nmq, IUINT32 cause_sn);

    IUINT32 peer_fin_sn;
    char fin_sn;


    IUINT8 steady_on;   // no blast send. default on.
    IUINT32 BYTES_PER_FLUSH;
} NMQ;

typedef IINT32 (*nmq_output_cb)(const char *data, const int len, struct nmq_s *nmq, void *arg);

typedef void (*nmq_failure_cb)(struct nmq_s *nmq, IUINT32 cause_sn);

void nmq_update(NMQ *q, IUINT32 current);

// we regard
void nmq_flush(NMQ *q, IUINT32 current);

// upper <-> nmq
IINT32 nmq_send(NMQ *q, const char *data, const int len);

void nmq_shutdown_send(NMQ *q);

// > 0 for specifc reason.
// < 0 if buf is too small and -retval is size that buf should be.
IINT32 nmq_recv(NMQ *q, char *buf, const int buf_size);

IINT32 nmq_output(NMQ *q, const char *data, const int len);

IINT32 nmq_input(NMQ *q, const char *buf, const int buf_size);

NMQ *nmq_new(IUINT32 conv, void *arg);

void nmq_destroy(NMQ *q);

IUINT32 nmq_get_conv(const char *buf);

void nmq_set_output_cb(NMQ *q, nmq_output_cb cb);

void nmq_set_wnd_size(NMQ *nmq, IUINT32 sndwnd, IUINT32 rcvwnd);

void nmq_set_fc_on(NMQ *q, IUINT8 on);


void nmq_start(NMQ *q); // first memeory allocation
void nmq_set_ssthresh(NMQ *q, IUINT32 ssthresh);

void nmq_set_trouble_tolerance(NMQ *q, IUINT8 n_tolerance);

void nmq_set_dup_acks_limit(NMQ *q, IUINT8 lim);

// MSS <= MTU - SEG_HEAD_SIZE - sum(OTHER_PROTOCOL_HEAD_SIZE)
void nmq_set_nmq_mtu(NMQ *q, IUINT32 MTU);

void nmq_set_max_attempt(NMQ *q, IUINT32 max_try, nmq_failure_cb cb);

void nmq_set_interval(NMQ *q, IUINT32 interval);

void nmq_set_fc_alpha(NMQ *q, float alpha);

void nmq_set_segment_pool_cap(NMQ *q, IUINT8 CAP);

void nmq_set_steady(NMQ *q, IUINT8 steady_on);

#ifdef __cplusplus
}
#endif

#endif //SOCKNM_MARQ_H
