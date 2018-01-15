//
// Created on 10/21/17.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "dlist.h"
#include "enc.h"
#include "nmq.h"

#define INTERVAL_MAX_MS 10000
#define INTERVAL_MIN_MS 10

const uint8_t CMD_INVALID = 0;
const uint8_t CMD_DATA = 1;
const uint8_t CMD_ACK = 2;
const uint8_t CMD_WND_REQ = 3;    // ask peer wnd
const uint8_t CMD_WND_ANS = 4;   // tell my wnd to peer. split req and ans to avoid repeatedly sending cmd_wnd_X
const uint8_t CMD_FIN = 5;

static int32_t sndq2buf(NMQ *q);

static int32_t rcvbuf2q(NMQ *q);

static int32_t do_recv(NMQ *q, char *buf, const int buf_size);

static int32_t do_send(NMQ *q, const char *data, const int len);

static inline void append_fin(NMQ *q);

static inline void fin_ops(NMQ *q, char cmd);

static int8_t input_segment(NMQ *q, segment *s);

static void flush_snd_buf(NMQ *q);

static void flush(NMQ *q);

static void send_failed(NMQ *q, uint32_t sn);


// acks
static void set_ack_ts(NMQ *q, uint32_t sn, uint32_t ts_send);

static void process_ack(NMQ *q, uint32_t sn, uint32_t ts_send);

static void input_acks(NMQ *q, const char *p, uint32_t len, const uint32_t old_una);

static void count_repeat_acks(NMQ *q, uint32_t maxack);

static void acks2peer(NMQ *q);

// una
static void process_una(NMQ *q, uint32_t una);

static inline void update_una(NMQ *q);

// window probe {
static inline void do_window_probe_or_answer(NMQ *q, uint16_t cmd);

static inline void window_probe_build_req_if_need(NMQ *q);

static inline void window_probe_req2peer_if_need(NMQ *q);

static void window_probe_ans2peer(NMQ *q);

// fc
void fc_init(NMQ *q, fc_s *fc);

void fc_pkt_loss(fc_s *fc, uint32_t max_lost_sn, uint32_t n_loss, uint32_t n_timeout);

void fc_input_acks(fc_s *fc, const uint32_t una, uint32_t nacks);

void fc_normal_acks(fc_s *fc, uint32_t nacks);

// rtt & rto
static inline void update_rtt(NMQ *q, uint32_t sn, uint32_t sendts);

static void rtt_estimator(NMQ *q, rto_helper_s *rter, uint32_t rtt);

static inline void update_rto(NMQ *q);

// wnd
static inline uint32_t get_snd_cwnd(NMQ *q);

static inline int32_t avaible_rcv_wnd(NMQ *q);

static inline void update_rmt_wnd(NMQ *q, uint32_t rmt_wnd);


// nmq utils
static inline int8_t dup_ack_limit(NMQ *q);

static inline void init_stat(nmq_stat_t *stat);

static inline void init_steady_state(NMQ *q);

// return size of next packet in send_queue.
static uint32_t next_packet_size(dlist *list);

static inline void seg_first_sent(NMQ *q, segment *s);

static inline void seg_timeout(NMQ *q, segment *s);

static inline void seg_reach_fastack(NMQ *q, segment *s);

// mem ops {
static inline segment *new_segment(uint32_t data_size);

static inline void delete_segment(segment *seg);

static inline void *nmq_malloc(size_t size);

static inline void *nmq_free(void *addr);

static inline void allocate_mem(NMQ *q);

// segment pool
void init_segment_pool(segment_pool *pool, uint32_t MTU, uint8_t CAP);

segment *obtain_segment(segment_pool *pool);

void recycle_segment(segment_pool *pool, segment *s);

// progress
static inline int is_recv_done(NMQ *q);

static inline void check_send_done(NMQ *q);

static inline int is_self_closed(NMQ *q);

// enc
// encode certain segment fields to buf. return new address that can encode
char *encode_seg_and_data(segment *s, char *buf);

// utils
static inline uint32_t modsn(uint32_t sn, uint32_t moder);

// global variables
static nmq_malloc_fn gs_malloc_fn = malloc;
static nmq_free_fn gs_free_fn = free;


static void flush_snd_buf(NMQ *q) {
    int32_t rcv_wnd = avaible_rcv_wnd(q);
    int32_t resent = dup_ack_limit(q);

    uint32_t n_loss = 0;
    uint32_t max_lost_sn = 0;
    uint32_t n_timeout = 0;
    uint32_t current = q->current;

    char buf[NMQ_BUF_SIZE] = {0};//caution!!! must be larger than MSS, or invalid memory access will occur!! e.g BUFSIZ
    char *p = buf;

    // 1 mss can hold many complete segments
    dlnode *node = NULL, *nxt = NULL;
    uint32_t bytes_sent = 0;
    FOR_EACH(node, nxt, &q->snd_buf) {
        segment *s = ADDRESS_FOR(segment, head, node);
        uint8_t need_send = 0;

        if (0 == s->n_sent) { // first time, need send
            need_send = 1;
            seg_first_sent(q, s);
        } else if (current >= s->resendts) {   // timeout
            need_send = 1;
            max_lost_sn = s->sn;    // snd_buf is order of sn
            seg_timeout(q, s);
            n_timeout++;
            s->dupacks = 0; // reset dupacks
        } else if (s->dupacks >=
                   resent) {  // n+1 dup acks. segment will be deleted if it's acked. so here use operator >=
            need_send = 1;  // will send if receive repeat acks. (fast retransmission). tcp/ip illustrated vol1. ch21.7
            max_lost_sn = s->sn;    // snd_buf is order of sn
            seg_reach_fastack(q, s);    // reset to timeout to def
            n_loss++; // increment number of segments lost
            s->dupacks = 0;
        }

        if (need_send) {
            if (q->steady_on) {
                bytes_sent += (s->len + SEG_HEAD_SIZE);
                if (bytes_sent > q->BYTES_PER_FLUSH) {
                    break;
                }
            }
            s->n_sent++;
            s->wnd = rcv_wnd;
            s->una = q->rcv_nxt;    //  figure out diff among snd_una, una, rcv_nxt
            s->sendts = current;
            // note: s->cmd, s->conv, s->sn and s->frag must not be updated here. they are assigned values in #sndq2buf
            s->resendts = s->rto + current;

            if (s->len + (p - buf) >= q->MTU) {   // buf is not enough to hold another segment
                nmq_output(q, buf, p - buf);  // emit buf first
                p = buf;    // reset p to buf
            }
            q->stat.bytes_send_tot += s->len;

            p = encode_seg_and_data(s, p);

            if (s->n_sent > q->MAX_PKT_TRY) {
                send_failed(q, s->sn);
            }
        }
    }

    // flush remaining data
    if (p > buf) {
        nmq_output(q, buf, p - buf);
    }

    if (n_loss + n_timeout > 0) {
        fc_pkt_loss(&q->fc, max_lost_sn, n_loss, n_timeout);
    }
}


static void flush(NMQ *q) {

    window_probe_req2peer_if_need(q);

    acks2peer(q);

    sndq2buf(q);

    flush_snd_buf(q);

    rcvbuf2q(q);

    window_probe_build_req_if_need(q);

    check_send_done(q);
}

static void send_failed(NMQ *q, uint32_t sn) {
    if (q->state == NMQ_STATE_SEND_FAILURE) {
        if (q->failure_cb) {
            (q->failure_cb)(q, sn);
        }
    }
}

static int8_t input_segment(NMQ *q, segment *s) {
    if (s->conv != q->conv) {
        return NMQ_ERR_CONV_DIFF;
    }

    if ((s->sn < q->rcv_nxt) || (s->sn >= (q->rcv_nxt + q->MAX_RCV_BUF_NUM))) {  // out of range
        q->ack_failures++;
        if (s->sn < q->rcv_nxt) {   // send ack if sn < q.rcv_nxt
            set_ack_ts(q, s->sn, s->sendts);
        }
        return NMQ_ERR_INVALID_SN;
    }

    if (q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)]) {
        // important! ack seg may get lost.
        // if we receive this seg again, we believe ack seg is lost.
        // we set ack to resend ack during next flush once ack is lost.
        // in case peer sends all data (not closed), and server received them all. but server acks for last sn are lost.
        // if not set_ack_ts here, server will not respond to client.
        // this prevent that situation from happening.
        q->ack_failures++;
        set_ack_ts(q, s->sn, s->sendts);
        return NMQ_ERR_DUPLICATE_SN;
    }

    if (q->nrcv_buf >= q->MAX_RCV_BUF_NUM) {
        return NMQ_ERR_RCV_BUF_NO_MEM;
    }


    dlnode *prev;   // backward iteration. because we believe s is usually a new segment
    for (prev = q->rcv_buf.prev; prev != &q->rcv_buf; prev = prev->prev) {
        segment *seg = ADDRESS_FOR(segment, head, prev);
        if (seg->sn < s->sn) {
            break;
        }
    }

    set_ack_ts(q, s->sn, s->sendts);

    dlist_add_after(prev, &s->head);
    q->nrcv_buf++;
    q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)] = &s->head;

    return 0;
}

void nmq_update(NMQ *q, uint32_t current) {
    assert(q->inited == 1);

    int32_t df = current - q->current;  // df cannot < 0
    if (df > 10000 || df < 0) {
        df = q->flush_interval;
    }
    if (df >= q->flush_interval) {
        q->current = current;
        flush(q);
    }
}

// ignore it
void nmq_flush(NMQ *q, uint32_t current) {
    assert(q->inited == 1);
    q->current = current;
    flush(q);
}

void nmq_start(NMQ *q) {
    if (!q->inited) {
        q->inited = 1;
        init_steady_state(q);
        allocate_mem(q);
    }
}

// <0 for error.
// >= 0 for size inputed
int32_t nmq_input(NMQ *q, const char *buf, const int buf_size) {
    const char *p = buf;
    int32_t size = buf_size;
    const uint32_t old_una = q->snd_una;
    int32_t tot = 0;

    while (size && p) {
        if (size < SEG_HEAD_SIZE) {
            return NMQ_ERR_MSG_BROKEN;
        }

        uint32_t conv;
        p = decode_uint32(&conv, p);
        if (conv != q->conv) {  // just return. if conv does not match, the remaining data is consider invalid.
            return NMQ_ERR_CONV_DIFF;
        }

        uint8_t cmd;
        p = decode_uint8(&cmd, p);

        if (CMD_DATA != cmd && CMD_ACK != cmd && CMD_WND_REQ != cmd
            && CMD_WND_ANS != cmd && CMD_FIN != cmd) {// drop this pkt if could not understand cmd
            return NMQ_ERR_WRONG_CMD;
        }

        uint32_t sn, len = 0, ts_send, una;
        uint16_t wnd;
        uint8_t frag;
        p = decode_uint32(&sn, p);
        p = decode_uint8(&frag, p);
        p = decode_uint16(&wnd, p);
        p = decode_uint32(&una, p);
        p = decode_uint32(&ts_send, p);
        p = decode_uint32(&len, p);
        tot += len;

        update_rmt_wnd(q, wnd);

        if (CMD_ACK != cmd) {
            process_una(q, una);
        }

        size -= (len + SEG_HEAD_SIZE);

        if (CMD_DATA == cmd || CMD_FIN == cmd) {
            if (CMD_FIN == cmd) {
                q->peer_fin_sn = sn;
            }
            segment *s = obtain_segment(&q->pool);
            s->conv = conv;
            s->cmd = cmd;   // no use
            s->sn = sn;
            s->frag = frag;
            s->wnd = wnd;
            s->una = una;
            s->sendts = ts_send;
            s->len = len;

            if (s->len) {
                memcpy(s->data, p, s->len);
            }
            int8_t ret = input_segment(q, s);
            if (ret != 0) {
                recycle_segment(&q->pool, s);
//                return ret;  should not return. may appear duplicate segs. and they are valid pkt
            }
        } else if (CMD_ACK == cmd) {
            input_acks(q, p, len, old_una);
            process_una(q, una);    //  s->sendts will be zero if processed before input_acks
//            sndq2buf(q);
        } else if (CMD_WND_REQ == cmd) {
            window_probe_ans2peer(q);
        } else if (CMD_WND_ANS == cmd) {
        } else {
            assert(0);
        }
        if (len) {
            p += len;
        }
    }

    rcvbuf2q(q);

    return tot;
}

int32_t do_recv(NMQ *q, char *buf, const int buf_size) {
    // todo: how large should buf_size be for datgram pkt?
    // https://stackoverflow.com/questions/2862071/how-large-should-my-recv-buffer-be-when-calling-recv-in-the-socket-library
    int32_t rcvq_size = next_packet_size(&q->rcv_que);
    if (rcvq_size > buf_size) { // simply drop packet if no enough buf
        return NMQ_ERR_MSG_SIZE;
    }

    char *p = buf;
    dlnode *node = 0, *nxt = 0;
    FOR_EACH(node, nxt, &q->rcv_que) {
        segment *s = ADDRESS_FOR(segment, head, node);
        if (s->len) {
            memcpy(p, s->data, s->len);
        }
        p += s->len;
        const int8_t frag = s->frag;

        dlist_remove_node(node);
        recycle_segment(&q->pool, s);
        q->nrcv_que--;

        if (0 == frag) { // reach the end of next packet
            break;
        }
    }

    if (p - buf != rcvq_size) {
        return NMQ_ERR_RCV_QUE_INCONSISTANCE;
    }

    return p - buf;
}

int32_t do_send(NMQ *q, const char *data, const int len) {
    if (is_self_closed(q)) {
        return NMQ_ERR_SEND_ON_SHUTDOWNED;
    }

    const char *phead = data;
    uint32_t tot = len / q->MSS;
    tot = (len % q->MSS) ? tot + 1 : tot;

    if (tot > 255) {    // frag num is stored with uint8_t
        return ERR_DATA_TOO_LONG;
    }

//  for (uint8_t i = (uint8_t) (tot - 1); i >= 0; i--) {  caution!!! this is wrong because i > 0 is always true if i is uint8_t
    for (uint8_t i = (uint8_t) tot; i > 0; i--) {     // frag order is n, n - 1 ... 0.
        const uint32_t slen = (i > 1) ? q->MSS : (len - (tot - 1) * q->MSS);
        segment *s = obtain_segment(&q->pool);
        s->conv = q->conv;
        s->cmd = CMD_DATA;
        s->n_sent = 0;
//        s->sn = q->snd_nxt++; assigned in sndq2buf.
        s->frag = i - 1;
        s->len = slen;
        memcpy(s->data, phead, s->len);
        phead += s->len;
        // una, wnd will be assigned in flush_snd_buf.
        dlist_add_tail(&q->snd_que,
                       &s->head); // attention: add s->head to tail, not s. when can retrieve s from s->head
    }

    q->stat.bytes_send += len;

    q->nsnd_que += tot;

    return (int32_t) (phead - data);
}

int32_t nmq_output(NMQ *q, const char *data, const int len) {
    assert(q->output_cb != NULL);

    if (len > 0 || len == NMQ_SEND_EOF) {
        return q->output_cb(data, len, q, q->arg);
    }
    return NMQ_NO_DATA;
}

int32_t nmq_send(NMQ *q, const char *data, const int len) {
    if (!data || len <= 0) {
        return 0;
    }

    return do_send(q, data, len);
}

// caution. must pay attention to fields that are assigned. they should be equal to nmq_send
void append_fin(NMQ *q) {
    fin_ops(q, CMD_FIN);
}

void fin_ops(NMQ *q, char cmd) {
    segment *s = obtain_segment(&q->pool);
    s->conv = q->conv;
    s->cmd = cmd;
    s->n_sent = 0;
    s->frag = 0;
    s->len = 0;
    s->data[0] = 0;    // una is assigned in flush_snd_buf
    dlist_add_tail(&q->snd_que, &s->head);
    q->nsnd_que++;
}

// < 0 for error.
int32_t nmq_recv(NMQ *q, char *buf, const int buf_size) {
    if (!q->inited) {
        return NMQ_ERR_UNITIALIZED;
    }

    if (is_recv_done(q)) {
        return NMQ_RECV_EOF;
    }

    rcvbuf2q(q);    // in case there is no seg in que

    if (!list_not_empty(&q->rcv_que)) {
        return NMQ_NO_DATA;
//        return NMQ_ERR_NO_DATA;
    }

    return do_recv(q, buf, buf_size);
}

// nmq private functions
static int32_t rcvbuf2q(NMQ *q) {
    int32_t tot = 0;
    dlnode *node = 0, *nxt = 0;
    FOR_EACH(node, nxt, &q->rcv_buf) {
        segment *s = ADDRESS_FOR(segment, head, node);
        if (s->sn == q->rcv_nxt) {
            q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)] = NULL;
            segment *seg = ADDRESS_FOR(segment, head, node);
            q->nrcv_que++;
            q->nrcv_buf--;
            q->rcv_nxt++;
            dlist_remove_node(node);
            dlist_add_tail(&q->rcv_que, node);
            tot += seg->len;
        } else {
            break;
        }
    }
    return tot;
}

static int32_t sndq2buf(NMQ *q) {
    int32_t tot = 0;

    // local send buf/que len should not be a bottleneck.
    uint32_t cwnd = get_snd_cwnd(q);

    // flow control. num(pending segments in snd_buf) <= cwnd
    dlnode *node = 0, *nxt = 0;

    FOR_EACH(node, nxt, &q->snd_que) {
        if (q->snd_nxt >= (q->snd_una + cwnd)) {    // cannot send new data
            break;
        }
        segment *s = (ADDRESS_FOR(segment, head, node));
        s->sn = q->snd_nxt++;   // snd_nxt is increment here other than nmq_flush_snd_buf. because it is used to compute cwnd
        dlist_remove_node(node);
        dlist_add_tail(&q->snd_buf, node);
        q->snd_sn_to_node[modsn(s->sn, q->MAX_SND_BUF_NUM)] = node;
        q->nsnd_que--;
        tot++;
    }
    return tot;
}


// acks {
static void acks2peer(NMQ *q) {
    if (!q->ackcount) {
        if (q->ack_failures) {
            window_probe_ans2peer(q);   // build a window probe answer
        }
        q->ack_failures = 0;
        return;
    }

    const int UNIT = 2 * sizeof(uint32_t);
    const uint32_t max_nack = q->MSS / UNIT;
    const uint32_t tot = UNIT * max_nack;
    const uint32_t ACKCNT = q->ackcount;
    for (int i = 0; i < ACKCNT; i++) {
        uint32_t sn;
        decode_uint32(&sn, (const char *) (i * 2 + q->acklist));
    }

    segment *s = obtain_segment(&q->pool);

    s->sn = 0;
    s->conv = q->conv;
    s->cmd = CMD_ACK;
    s->una = q->rcv_nxt;
    s->len = 0;
    s->wnd = avaible_rcv_wnd(q);
    s->frag = 0;
    s->sendts = 0;

    char *p = (char *) q->acklist;
    char buf[NMQ_BUF_SIZE] = {0};

    uint32_t ackcnt = ACKCNT;
    while (ackcnt > 0) {
        if (ackcnt > max_nack) {
            s->len = max_nack * UNIT;
            memcpy(s->data, p, s->len);
            ackcnt -= max_nack;
        } else {
            s->len = ackcnt * UNIT;
            memcpy(s->data, p, s->len);
            ackcnt = 0;
        }
        p += s->len;
        char *ptr = encode_seg_and_data(s, buf);
        nmq_output(q, buf, ptr - buf);
    }

    recycle_segment(&q->pool, s);
    q->ackcount = 0;    // important! must reset q.ackcount
    q->ack_failures = 0;
}

// ack(sn) sent by peer, telling local that peer itself received segment(sn). The segment(sn) is sent by local.
// this op will remove acked segs
static void process_ack(NMQ *q, uint32_t sn, uint32_t ts_send) {
    uint32_t sn_mod = modsn(sn, q->MAX_SND_BUF_NUM);
    if (sn < q->snd_una || sn >= q->snd_nxt || !q->snd_sn_to_node[sn_mod]) { // sn 0 is valid!!!
        return;
    }
    if (ts_send) {  //  during processing una. just delete segments and do nothing
        update_rtt(q, sn, ts_send);
    }

    dlnode *node = q->snd_sn_to_node[sn_mod];
    dlist_remove_node(node);

    q->snd_sn_to_node[sn_mod] = NULL;
    segment *s = ADDRESS_FOR(segment, head, node);
    recycle_segment(&q->pool, s);
}

static void count_repeat_acks(NMQ *q, uint32_t maxack) {
    dlnode *node = 0, *nxt = 0;
    FOR_EACH(node, nxt, &q->snd_buf) {
        segment *s = ADDRESS_FOR(segment, head, node);
        if (s->sn < maxack) {
            s->dupacks++;
        } else {
            break;
        }
    }
}

// must before input_segments
static void input_acks(NMQ *q, const char *p, uint32_t len, const uint32_t old_una) {
    uint32_t maxack = 0;
    const int UNIT = 2 * sizeof(uint32_t);
    uint32_t nacks = len / UNIT;

    while (len >= UNIT) {
        len -= UNIT;
        uint32_t ack_sn, ack_ts_send;
        p = decode_uint32(&ack_sn, p);
        p = decode_uint32(&ack_ts_send, p);
        maxack = (uint32_t) MAX(maxack, ack_sn);
        process_ack(q, ack_sn, ack_ts_send);    // will delete segments
    }

    // todo: multiple segments send(or receive?) at same time will not regard as dup acks.this cause retransmit very soon.
    count_repeat_acks(q, maxack);

    update_una(q);
    update_rto(q);
    fc_input_acks(&q->fc, q->snd_una, nacks);
}

static void set_ack_ts(NMQ *q, uint32_t sn, uint32_t ts_send) {
    if (q->ackcount == q->ackmaxnum) {
        const size_t UNIT = 2 * sizeof(uint32_t);
        uint32_t newnum = q->ackmaxnum << 1;
        void *newbuf = nmq_malloc(newnum * UNIT);

        memcpy(newbuf, q->acklist, q->ackcount * UNIT);
        nmq_free(q->acklist);

        q->acklist = (uint32_t *) newbuf;
        q->ackmaxnum = newnum;
    }

    char *p1 = (char *) (q->acklist + q->ackcount * 2);
    char *p2 = (char *) (q->acklist + q->ackcount * 2 + 1);

    encode_uint32(sn, p1);
    encode_uint32(ts_send, p2);

    q->ackcount++;
}
// } acks


// una {
static void process_una(NMQ *q, uint32_t una) {
    dlnode *node = 0, *nxt = 0;
    FOR_EACH(node, nxt, &q->snd_buf) {
        segment *s = ADDRESS_FOR(segment, head, node);
        if (s->sn < una) {
            process_ack(q, s->sn, 0);
        } else {
            break;
        }
    }

    update_una(q);
}

static inline void update_una(NMQ *q) {
    // update una
    if (list_not_empty(&q->snd_buf)) {
        q->snd_una = ADDRESS_FOR(segment, head, q->snd_buf.next)->sn;
    } else {
        q->snd_una = q->snd_nxt;
    }
}
// } una


// window probe {
static inline void window_probe_req2peer_if_need(NMQ *q) {
    if (q->probe_pending && q->current >= q->ts_probe_wait) {
        do_window_probe_or_answer(q, CMD_WND_REQ);
    }
}

static inline void window_probe_ans2peer(NMQ *q) {
    do_window_probe_or_answer(q, CMD_WND_ANS);
}

static inline void window_probe_build_req_if_need(NMQ *q) {
    if (!q->rmt_wnd && !q->probe_pending) {
        q->probe_pending = 1;
        q->ts_probe_wait = q->current + NMQ_PROBE_WAIT_MS_DEF;
    }
}

static void do_window_probe_or_answer(NMQ *q, uint16_t cmd) {
    segment seg = {0};
    seg.conv = q->conv;
    seg.len = 0;
    seg.cmd = cmd;
    // should not be snd_una! una is unidirection notification.
    // it only contains information about next sn that this client epxects receive.
    // its peer's responsibility to notify this client that what peer expects to receive.
    seg.una = q->rcv_nxt;
    seg.wnd =
            q->MAX_RCV_BUF_NUM - q->nrcv_buf;// this may be a problem!  this limits maximum window size is about 12.5MB!
    char buf[SEG_HEAD_SIZE] = {0};
    char *p = encode_seg_and_data(&seg, buf);
    nmq_output(q, buf, p - buf);
    q->probe_pending = 0;   // if receiver receive probe req, this behaviour clear its own pending probe req if any.
    q->ts_probe_wait = 0;
}
// } window probe

// fc {
void fc_init(NMQ *q, fc_s *fc) {
    memset(fc, 0, sizeof(fc_s));
    fc->TROUBLE_TOLERANCE = NMQ_TROUBLE_TOLERANCE_DEF;
    fc->DUP_ACK_LIM = NMQ_DUP_ACK_LIM_DEF;
    fc->MSS = q->MSS;
    fc->ssth_alpha = NMQ_FC_ALPHA_DEF;
    fc->cwnd = q->MAX_SND_BUF_NUM;
    fc->incr = fc->cwnd * fc->MSS;
    fc->ssthresh = NMQ_SSTHRESH_DEF;
    fc->in_trouble = 0;
    fc->max_lost_sn = 0;
}

void fc_pkt_loss(fc_s *fc, uint32_t max_lost_sn, uint32_t n_loss, uint32_t n_timeout) {
    if (!fc->in_trouble && (n_loss + n_timeout) >= fc->TROUBLE_TOLERANCE) {
        fc->in_trouble = 1;
        fc->max_lost_sn = max_lost_sn;
        fc->ssthresh = MAX(NMQ_SSTHRESH_MIN, fc->cwnd * fc->ssth_alpha);
        fc->cwnd = fc->ssthresh;  //todo: test the ratio. e.g. change to 0.5
        if (n_loss) {
            fc->cwnd += fc->DUP_ACK_LIM;
        }
        fc->incr = fc->cwnd * fc->MSS;
    }
}

void fc_input_acks(fc_s *fc, const uint32_t una, uint32_t nacks) {
    if (fc->in_trouble) {
        if (una > fc->max_lost_sn) {    // new data arrives
            fc->cwnd = fc->ssthresh + 1;
            fc->in_trouble = 0;
            fc->max_lost_sn = 0;
        } else {
            fc->cwnd += nacks;
        }
        fc->incr = fc->cwnd * fc->MSS;
    } else {
        fc_normal_acks(fc, nacks);
    }

}

void fc_normal_acks(fc_s *fc, uint32_t nacks) {
    if (fc->cwnd <= fc->ssthresh) {  // slow start
        fc->cwnd = MIN(fc->cwnd + nacks, fc->ssthresh + 1);
        fc->incr = fc->cwnd * fc->MSS;
    } else {    // congestion avoidance
        fc->incr += fc->MSS * fc->MSS / fc->incr + (fc->MSS >> 3);
        if ((fc->cwnd + 1) * fc->MSS <= fc->incr) {
            fc->cwnd += 1;
        }
    }
}
// } fc

// rtt & rto {
static inline void update_rtt(NMQ *q, uint32_t sn, uint32_t sendts) {
    segment *s = ADDRESS_FOR(segment, head, q->snd_sn_to_node[modsn(sn, q->MAX_SND_BUF_NUM)]);
    if (s->sendts == sendts) {
        uint32_t rtt = q->current - sendts;
        q->stat.nrtt++;
        q->stat.nrtt_tot += rtt;
        rtt_estimator(q, &q->rto_helper, rtt);
    }
}

static void rtt_estimator(NMQ *q, rto_helper_s *rter, uint32_t rtt) {
    // copied from linux kernel. chinese explaination: http://www.pagefault.info/?p=430
    int64_t m = rtt;
    if (m <= 0) {
        m = 1;
    }
    if (rter->srtt) {
        m -= (rter->srtt >> 3);
        rter->srtt += m;
        if (m < 0) {
            m = -m;
            m -= (rter->mdev >> 2);
            if (m > 0) {
                m >>= 3;
            }
        } else {
            m -= (rter->mdev >> 2);
        }
        rter->mdev += m;
        if (rter->mdev > rter->mdev_max) {
            rter->mdev_max = rter->mdev;    // dev_max is valid in a rtt.
            if (rter->mdev_max > rter->rttvar) {    // rttvar is valid in whole session
                rter->rttvar = rter->mdev_max;  // why? rttvar is smoothed mdev_max
            }
        }
        if (rter->rtt_seq < q->snd_una) {   // next rtt
            if (rter->mdev_max < rter->rttvar) {
                rter->rttvar -= (rter->rttvar - rter->mdev_max) >> 2;
            }
            rter->rtt_seq = q->snd_nxt;
            rter->mdev_max = NMQ_RTO_MIN;
        }
    } else {
        rter->srtt = m << 3;
        rter->mdev = m << 1;
        rter->mdev_max = MAX(rter->mdev, NMQ_RTO_MIN);
        rter->rttvar = rter->mdev_max;
        rter->rtt_seq = q->snd_nxt;
    }
}

static inline void update_rto(NMQ *q) {
    rto_helper_s *rter = &q->rto_helper;
    uint32_t rto1 = q->rto;
    q->rto = MAX(NMQ_RTO_MIN, MIN(NMQ_RTO_MAX, (rter->srtt >> 3) + rter->rttvar));
}
// } rtt & rto


// wnd ops {
static inline uint32_t get_snd_cwnd(NMQ *q) {
    uint32_t cwnd = MIN(q->MAX_SND_BUF_NUM, q->rmt_wnd); // attention to how get_snd_cwnd is used
    if (q->fc_on) {
        return MIN(cwnd, q->fc.cwnd);
    }
    return cwnd;
}

static inline int32_t avaible_rcv_wnd(NMQ *q) {
    return q->MAX_RCV_BUF_NUM - q->nrcv_buf;
}

static inline void update_rmt_wnd(NMQ *q, uint32_t rmt_wnd) {
    q->rmt_wnd = rmt_wnd;
}
// } wnd ops


// nmq utils {
static inline int8_t dup_ack_limit(NMQ *q) {
    return q->fc.DUP_ACK_LIM;
}

void init_stat(nmq_stat_t *stat) {
    stat->nrtt = 0;
    stat->nrtt_tot = 0;
    stat->bytes_send = 0;
    stat->bytes_send_tot = 0;
}

void init_steady_state(NMQ *q) {
    if (q->steady_on) {
        q->BYTES_PER_FLUSH = (uint32_t) ((q->MAX_SND_BUF_NUM * q->flush_interval / 1000.0) * q->MTU);
    } else {
        q->BYTES_PER_FLUSH = 0;
    }
}

// return size of next packet in send_queue.
static uint32_t next_packet_size(dlist *list) {
    dlnode *it = 0, *nxt = 0;
    uint32_t len = 0;
    FOR_EACH(it, nxt, list) {
        segment *s = ADDRESS_FOR(segment, head, it);
        len += s->len;
        if (0 == s->frag) { // reach the end of packet. if there only frag n - 1, n - 2 ... 2, 1. no frag 0
            break;
        }
    }
    return len;
}

static inline void seg_first_sent(NMQ *q, segment *s) {
    s->rto = q->rto;
//    s->resendts = s->rto + current;
}

static inline void seg_timeout(NMQ *q, segment *s) {
    if (q->nodelay) {
        s->rto += (q->rto >> 1);
    } else {
        s->rto += q->rto;
    }

    if (s->rto > NMQ_RTO_MAX) {
        s->rto = NMQ_RTO_MAX;
    }
}

static inline void seg_reach_fastack(NMQ *q, segment *s) {
    s->rto = NMQ_RTO_DEF;   // will resend the fragment. so reset timeout to defval.
}

void nmq_set_output_cb(NMQ *q, nmq_output_cb cb) {
    if (!q->inited) {
        q->output_cb = cb;
    }
}

void nmq_set_wnd_size(NMQ *nmq, uint32_t sndwnd, uint32_t rcvwnd) {
    nmq->MAX_SND_BUF_NUM = sndwnd;
    nmq->MAX_RCV_BUF_NUM = rcvwnd;
    nmq->fc.cwnd = sndwnd;
}

void nmq_set_fc_on(NMQ *q, uint8_t on) {
    if (!q->inited) {
        q->fc_on = on;
    }
}

void nmq_set_steady(NMQ *q, uint8_t steady_on) {
    if (!q->inited) {
        q->steady_on = steady_on;
    }
}
//} nmq utils

void nmq_shutdown_send(NMQ *q) {
    if (!q->fin_sn) {
        q->fin_sn = 1;
        append_fin(q);
    }
}

// memory ops. {
NMQ *nmq_new(uint32_t conv, void *arg) {
    NMQ *q = (NMQ *) nmq_malloc(sizeof(NMQ));
    memset(q, 0, sizeof(NMQ));

    q->conv = conv;
    q->arg = arg;

    q->inited = 0;
    q->flush_interval = NMQ_FLUSH_INTERVAL_DEF;

    q->MAX_SND_BUF_NUM = NMQ_SND_BUF_NUM_DEF;   // must be initialized before rcv_sn_to_node
    q->MAX_RCV_BUF_NUM = NMQ_RCV_BUF_NUM_DEF;
    q->MTU = NMQ_MTU_DEF;
    q->MSS = q->MTU - SEG_HEAD_SIZE;

    q->rmt_wnd = NMQ_RMT_WND_DEF;

    q->snd_una = 0;
    q->snd_nxt = 0;

    q->rcv_nxt = 0;

    dlist *lists[] = {&q->snd_buf, &q->snd_que, &q->rcv_buf, &q->rcv_que, NULL};
    for (int i = 0; lists[i]; i++) {
        dlist_init(lists[i]);
    }

    nmq_set_segment_pool_cap(q, NMQ_SEGMENT_POOL_CAP_DEF);

    q->nrcv_que = 0;
    q->nrcv_buf = 0;
    q->nsnd_que = 0;

    q->ackmaxnum = 128;
    q->ackcount = 0;
    q->ack_failures = 0;

    q->rto = NMQ_RTO_DEF;
    init_stat(&q->stat);
    q->nodelay = 1; // todo:
    q->ts_probe_wait = NMQ_PROBE_WAIT_MS_DEF;
    q->probe_pending = 0;

    memset(&q->rto_helper, 0, sizeof(q->rto_helper));

    q->MAX_PKT_TRY = NMQ_MAX_TRY;
    q->state = 0;

    q->fc_on = 0;   // default is off
    fc_init(q, &q->fc);

    q->output_cb = NULL;
    q->failure_cb = NULL;

    q->peer_fin_sn = 0;
    q->fin_sn = 0;

    q->steady_on = 1;
    q->BYTES_PER_FLUSH = 0;     // initialized later
    return q;
}

static inline void allocate_mem(NMQ *q) {
    q->snd_sn_to_node = (dlist **) nmq_malloc(sizeof(dlnode *) * q->MAX_SND_BUF_NUM);
    memset(q->snd_sn_to_node, 0, sizeof(dlnode *) * q->MAX_SND_BUF_NUM);

    q->rcv_sn_to_node = (dlist **) nmq_malloc(sizeof(dlnode *) * q->MAX_RCV_BUF_NUM);
    memset(q->rcv_sn_to_node, 0, sizeof(dlnode *) * q->MAX_RCV_BUF_NUM);

    q->acklist = (uint32_t *) nmq_malloc(q->ackmaxnum * sizeof(uint32_t) * 2);
    memset(q->acklist, 0, q->ackmaxnum * sizeof(uint32_t) * 2);

    init_segment_pool(&q->pool, q->MSS, q->pool.CAP);
}

// either send_done or recv_done exists, it means self or peer closed.
void check_send_done(NMQ *q) {
    if (is_self_closed(q) && !list_not_empty(&q->snd_que) && (!list_not_empty(&q->snd_buf))) {
        nmq_output(q, NULL, NMQ_SEND_EOF);
    }
}

int is_recv_done(NMQ *q) {
    if (q->peer_fin_sn > 0) {
        if ((q->rcv_nxt > q->peer_fin_sn) && (!list_not_empty(&q->rcv_buf)) && (!list_not_empty(&q->rcv_que))) {
            return 1;
        }
    }
    return 0;
}

int is_self_closed(NMQ *q) {
    return q->fin_sn != 0;
}

void nmq_destroy(NMQ *q) {
    nmq_free(q->snd_sn_to_node);
    nmq_free(q->rcv_sn_to_node);;
    nmq_free(q->acklist);
    // remember to destroy pool.seg_list.
    dlist *lists[] = {&q->snd_buf, &q->snd_que, &q->rcv_buf, &q->rcv_que, &q->pool.seg_list, NULL};
    for (int i = 0; lists[i]; i++) {
        dlnode *node = 0, *nxt = 0;
        FOR_EACH(node, nxt, lists[i]) {
            segment *s = ADDRESS_FOR(segment, head, node);
            dlist_remove_node(node);
            delete_segment(s);
        }
    }
    q->pool.CAP = q->pool.left = 0;
    nmq_free(q);
}

static inline segment *new_segment(uint32_t data_size) {
    segment *s = (segment *) nmq_malloc(sizeof(segment) + data_size);
    memset(s, 0, sizeof(segment) + data_size);  // the order must no be wrong.
    dlist_init(&s->head);
    return s;
}

void delete_segment(segment *seg) {
    if (seg) {
        nmq_free(seg);
    }
}

static inline void *nmq_malloc(size_t size) {
    void *p = gs_malloc_fn(size);
    if (NULL == p) {
        abort();
        return NULL;
    }
    return p;
}

static inline void *nmq_free(void *addr) {
    if (addr) {
        gs_free_fn(addr);
    }
}
// } memory ops.


// encoding {
char *encode_seg_and_data(segment *s, char *buf) {
    if (!s || !buf) {
        return buf;
    }

    char *p = buf;
    // segment head size 24B
    p = encode_uint32(s->conv, p);
    p = encode_uint8(s->cmd, p);
    p = encode_uint32(s->sn, p);
    p = encode_uint8(s->frag, p);
    p = encode_uint16(s->wnd, p);
    p = encode_uint32(s->una, p);
    p = encode_uint32(s->sendts, p);
    p = encode_uint32(s->len, p);

    if (s->len > 0 && NULL != s->data) {
        memcpy(p, s->data, s->len);
        p += s->len;
    }
    return p;
}
// } encoding


// util {
uint32_t nmq_get_conv(const char *buf) {
    if (buf) {
        uint32_t conv;
        decode_uint32(&conv, buf);
        return conv;
    }
    return 0;
}

static inline uint32_t modsn(uint32_t sn, uint32_t moder) {
    return (sn + moder) % moder;
}

// } util

// nmq helper functions
void nmq_set_ssthresh(NMQ *q, uint32_t ssthresh) {
    if (!q->inited) {
        if (ssthresh > NMQ_SSTHRESH_MIN) {
            q->fc.ssthresh = ssthresh;
        }
    }
}

void nmq_set_trouble_tolerance(NMQ *q, uint8_t n_tolerance) {
    if (!q->inited) {
        if (n_tolerance >= NMQ_TROUBLE_TOLERANCE_MIN && n_tolerance <= NMQ_TROUBLE_TOLERANCE_MAX) {
            q->fc.TROUBLE_TOLERANCE = n_tolerance;
        }
    }
}

void nmq_set_dup_acks_limit(NMQ *q, uint8_t lim) {
    if (!q->inited) {
        if (lim >= NMQ_DUP_ACK_LIM_MIN && lim <= NMQ_DUP_ACK_LIM_MAX) {
            q->fc.DUP_ACK_LIM = lim;
        }
    }
}

// MSS <= MTU - SEG_HEAD_SIZE - sum(OTHER_PROTOCOL_HEAD_SIZE)
void nmq_set_nmq_mtu(NMQ *q, uint32_t MTU) {
    if (!q->inited) {
        q->MTU = MTU;
        q->MSS = MTU - SEG_HEAD_SIZE;
        q->fc.MSS = MTU - SEG_HEAD_SIZE;
    }
}

void nmq_set_max_attempt(NMQ *q, uint32_t max_try, nmq_failure_cb cb) {
    if (!q->inited) {
        q->MAX_PKT_TRY = max_try > 0 ? max_try : NMQ_MAX_TRY;
        q->failure_cb = cb;
    }
}

void nmq_set_interval(NMQ *q, uint32_t interval) {
    if (!q->inited) {
        if (interval > INTERVAL_MAX_MS) {
            q->flush_interval = INTERVAL_MAX_MS;
        } else if (interval < INTERVAL_MIN_MS) {
            q->flush_interval = INTERVAL_MIN_MS;
        } else {
            q->flush_interval = interval;
        }
    }
}

void nmq_set_fc_alpha(NMQ *q, float alpha) {
    if (!q->inited && alpha > 0.0f && alpha < 1.0f) {
        q->fc.ssth_alpha = alpha;
    }
}

// seg_pool {
void init_segment_pool(segment_pool *pool, uint32_t MTU, uint8_t CAP) {
    dlist_init(&pool->seg_list);
    pool->left = CAP;
    pool->CAP = CAP;
    pool->MTU = MTU;
    for (uint8_t i = 0; i < CAP; i++) {
        segment *s = new_segment(MTU);
        dlist_add_tail(&pool->seg_list, &s->head);
    }
}

// len should be just mtu
segment *obtain_segment(segment_pool *pool) {
    dlnode *node = 0, *nxt = 0;
    FOR_EACH(node, nxt, &pool->seg_list) {
        segment *s = ADDRESS_FOR(segment, head, node);
        dlist_remove_node(node);
        pool->left--;

        return s;
    }

    segment *s = new_segment(pool->MTU);
    return s;
}

void recycle_segment(segment_pool *pool, segment *s) {
    if (!s) {
        return;
    }
    dlist_remove_node(&s->head);    // ensure the node is removed from list
    if (pool->left >= pool->CAP) {  // simply delete it if pool full
        delete_segment(s);
        return;
    }
    memset(s, 0, sizeof(*s));
    dlist_add_tail(&pool->seg_list, &s->head);
    pool->left++;
}

void nmq_set_segment_pool_cap(NMQ *q, uint8_t CAP) {
    if (!q->inited) {
        q->pool.CAP = CAP;
    }
}

//} seg_pool
