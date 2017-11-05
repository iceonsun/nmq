//
// Created by Robert Cai on 10/20/17.
//

#ifndef SOCKNM_MARQ_H
#define SOCKNM_MARQ_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "dlist.h"

#define NMQ_BUF_SIZE 1600
#define NMQ_MAX_BUF_NUM 1500
#define NMQ_MAX_QUE_NUM 1500
//#define NMQ_MAX_BUF_QUE_NUM_DEF 3

#define NMQ_STATE_SEND_FAILURE (-1)

#define NMQ_SSTHRESH_MIN 2
#define NMQ_SSTHRESH_DEF 200
#define NMQ_DUP_ACK_LIM_DEF 3
#define NMQ_RMT_WND_DEF NMQ_MAX_BUF_NUM
#define NMQ_FLUSH_INTERVAL_DEF 50
#define NMQ_PROBE_WAIT_DEF 500

#define NMQ_RTO_MIN 100
#define NMQ_RTO_DEF 200
#define NMQ_RTO_MAX 60000

#define NMQ_ERR_UNITIALIZED (-1)
#define NMQ_ERR_CONV_DIFF (-2)
#define NMQ_ERR_WRONG_CMD (-3)
#define NMQ_ERR_INVALID_SN (-4)
#define NMQ_ERR_DUPLICATE_SN (-5)
#define NMQ_ERR_RCV_QUE_INCONSISTANCE (-6)
#define NMQ_ERR_NO_DATA (-7)
#define NMQ_ERR_MSG_SIZE (-10)
#define NMQ_ERR_MSG_BROKEN (-11)
#define ERR_DATA_TOO_LONG (-12)
#define NMQ_ERR_RCV_BUF_NO_MEM (-13)
#define NMQ_ERR_SND_QUE_NO_MEM (-14)
#define NMQ_ERR_ACK_BUF_LEN (-20)

// 1500 minus ip, udp  and kcp header size.
#define NMQ_MSS_DEF 1458
#define NMQ_HEAD_SIZE 24
#define SEG_HEAD_SIZE NMQ_HEAD_SIZE

#define OFFSETOF(TYPE, MEMBER) \
    ((size_t)&(((TYPE *)0)->MEMBER))

#define ADDRESS_FOR(TYPE, MEMBER, mem_addr) \
    ((TYPE*)(((char *)(mem_addr)) - OFFSETOF(TYPE, MEMBER)))

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
    char *data;
} segment;


typedef struct fc_s {
    float ssth_alpha;
    float incr;     // float is always 32bit large whether in 32-bit or 64-bit machine
    IUINT32 cwnd;    // congestion window. maximum is MAX_SND_BUF_NUM. unit: MTU
    IUINT32 ssthresh;   // unit: MSS + SEG_HEAD_SIZE
    IUINT32 TROUBLE_TOLERANCE;
    IINT8 DUP_ACK_LIM;
    IUINT8 in_trouble;
    IUINT32 max_lost_sn;
    IUINT32 MSS;
} fc_s;

typedef struct flow_control_s {
    float incr;     // float is always 32bit large whether in 32-bit or 64-bit machine
    IUINT32 cwnd;    // congestion window. maximum is MAX_SND_BUF_NUM. unit: MTU
    IUINT32 ssthresh;   // unit: MSS + SEG_HEAD_SIZE
    IUINT8 NO_SLOW_START_WHEN_TIMEOUT;
    IINT8 DUP_ACK_LIM;
    IUINT8 ACK_TOLERANCE;
    IUINT32 TIMEOUT_TOLRANCE;
    IUINT8 in_fast_recovery;
    IUINT8 congestion_type;
    IUINT32 max_lost_sn;
    IUINT32 MSS;    // must be equal to q.MSS
} flow_control_s;

typedef struct rto_helper_s {
    IUINT32 srtt;
    IUINT32 mdev;
    IUINT32 mdev_max;
    IUINT32 rttvar;
    IUINT32 rtt_seq;
} rto_helper_s;

typedef void* (*nmq_malloc_fn)(size_t size);
typedef void (*nmq_free_fn)(void *ptr);

typedef struct nmq_s {
    IUINT32 conv;
    void *arg;

    IUINT32 ts_flush_nxt;
    IUINT32 current;
    IINT8 flushed;
    IUINT16 flush_interval;

    IUINT32 rmt_wnd;

    IUINT32 snd_una;    // sent unacknowledged for this client
    IUINT32 snd_nxt;
    dlist snd_buf;
    dlist snd_que;
    dlnode **snd_sn_to_node;       // the size is MAX_SND_BUF_NUM

    IUINT32 rcv_nxt;
    dlist rcv_buf;    // use array to stroe it
    dlnode **rcv_sn_to_node; // sn: &seg.head    // the size is MAX_RCV_BUF_NUM
    dlist rcv_que;

    IUINT32 nrcv_que;    // number of packet in receive queue now
    IUINT32 nrcv_buf;    // number of packets in receive buf now
    IUINT32 nsnd_que;
//    IUINT32 nsnd_buf; // nsnd_buf = snd_nxt - snd_una
    // estimate bandwidth. set this number to a number larger than bandwidth. unit: MSS + SEG_HEAD_SIZE
    IUINT32 MAX_SND_BUF_NUM;    // use limited or unlimited que?? current is limited que
    IUINT32 MAX_SND_QUE_NUM;
    IUINT32 MAX_RCV_BUF_NUM;
    IUINT32 MAX_RCV_QUE_NUM;

    IUINT32 ackmaxnum;
    IUINT32 *acklist;  // acklist[i] is sn, acklist[i]+1 is ts_send. size is 2 * MAX_RCV_BUF_NUM
    IUINT32 ackcount;

//    IUINT32 rto;    // retransmission timeout.
//    IUINT32 rtt;    // used to calculate rto

    IUINT8 flow_ctrl_on;
//    flow_control_s flow_ctrl;
    fc_s fc;
//    IUINT32 ssthresh;   // unit: MSS + SEG_HEAD_SIZE
//    float incr;     // float is always 32bit large whether in 32-bit or 64-bit machine
//    IUINT32 cwnd;    // congestion window. maximum is MAX_SND_BUF_NUM. unit: MTU
//    IUINT32 rmt_wnd;
//    IUINT8 NO_SLOW_START_WHEN_TIMEOUT;
//    IINT8 FAST_ACK;
//    IUINT8 ACK_TOLERANCE;
//    IUINT8 in_fast_recovery;
//    IUINT32 max_lost_sn;

    IUINT32 rto;

    IUINT8 nodelay;

    IUINT32 ts_probe_wait;
    IUINT8 probe_pending;

    rto_helper_s rto_helper;

    IINT32 MAX_PKT_TRY; // maximum times to send packet. or failure
    IINT8 state;

    IUINT32 NMQ_MSS;    // not including head size. MSS + SEG_HEAD_SIZE + OTHER_PROTOCOL_HEAD_SIZE = MTU
//    IUINT32 MTU;    // MTU = MSS + SEG_HEAD_SIZE, current: 24B. this MTU is different from MTU for tcp/ip suite.

//    nmq_output_fn fn_output;
    IINT32 (*fn_output)(const char *data, const int len, struct nmq_s *nmq, void *arg);
} NMQ;

typedef IINT32 (*nmq_output_fn)(const char *data, const int len, struct nmq_s *nmq, void *arg);

void nmq_update(NMQ *q, IUINT32 current);
// upper <-> nmq
IINT32 nmq_send(NMQ *q, const char *data, const int len);

// return 0 if success.
// > 0 for specifc reason.
// < 0 if buf is too small and -retval is size that buf should be.
IINT32 nmq_recv(NMQ *q, char *buf, const int buf_size);
IINT32 nmq_output(NMQ *q, const char *data, const int len);
IINT32 nmq_input(NMQ *q, const char *buf, const int buf_size);

NMQ *nmq_new(IUINT32 conv, void *arg);
void nmq_destroy(NMQ *q);
IUINT32 nmq_get_conv(const char *buf);
void nmq_set_output_func(NMQ *q, nmq_output_fn fn);
segment *nmq_new_segment(IUINT32 data_size);
void delete_segment(segment *seg);

#ifdef __cplusplus
}
#endif

#endif //SOCKNM_MARQ_H
