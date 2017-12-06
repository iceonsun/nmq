//
// Created by Robert Cai on 10/21/17.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "dlist.h"
#include "enc.h"
#include "nmq.h"

#define ACK_TOLERANCE_DEF 2
#define TIMEOUT_TOLERANCE_DEF 2
#define INTERVAL_MAX_MS 10000
#define INTERVAL_MIN_MS 10

const IUINT8 CMD_INVALID = 0;
const IUINT8 CMD_DATA = 1;
const IUINT8 CMD_ACK = 2;
const IUINT8 CMD_WND_REQ = 3;    // ask peer wnd
const IUINT8 CMD_WND_ANS = 4;   // tell my wnd to peer. split req and ans to avoid repeatedly sending cmd_wnd_X
const IUINT8 CMD_FIN = 5;

static IINT32 sndq2buf(NMQ *q);

static IINT32 rcvbuf2q(NMQ *q);

static IINT32 do_recv(NMQ *q, char *buf, const int buf_size);

static IINT32 do_send(NMQ *q, const char *data, const int len);

static inline void append_fin(NMQ *q);

static inline void fin_ops(NMQ *q, char cmd);

static IINT8 input_segment(NMQ *q, segment *s);

static void flush_snd_buf(NMQ *q);

static void flush(NMQ *q);

static void send_failed(NMQ *q, IUINT32 sn);


static int fill_snd_buf(NMQ *q);

// acks
static void set_ack_ts(NMQ *q, IUINT32 sn, IUINT32 ts_send);

static void process_ack(NMQ *q, IUINT32 sn, IUINT32 ts_send);

static void input_acks(NMQ *q, const char *p, IUINT32 len, const IUINT32 old_una);

static void count_repeat_acks(NMQ *q, IUINT32 maxack);

static void acks2peer(NMQ *q);

// una
static void process_una(NMQ *q, IUINT32 una);

static inline void update_una(NMQ *q);

// window probe {
static inline void do_window_probe_or_answer(NMQ *q, IUINT16 cmd);

static inline void window_probe_build_req_if_need(NMQ *q);

static inline void window_probe_req2peer_if_need(NMQ *q);

static void window_probe_ans2peer(NMQ *q);

// fc
void fc_init(NMQ *q, fc_s *fc);

void fc_pkt_loss(fc_s *fc, IUINT32 max_lost_sn, IUINT32 n_loss, IUINT32 n_timeout);

void fc_input_acks(fc_s *fc, const IUINT32 una, IUINT32 nacks);

void fc_normal_acks(fc_s *fc, IUINT32 nacks);

// rtt & rto
static inline void update_rtt(NMQ *q, IUINT32 sn, IUINT32 sendts);

static void rtt_estimator(NMQ *q, rto_helper_s *rter, IUINT32 rtt);

static inline void update_rto(NMQ *q);

// wnd
static inline IUINT32 get_snd_cwnd(NMQ *q);

static inline IINT32 avaible_rcv_wnd(NMQ *q);

static inline void update_rmt_wnd(NMQ *q, IUINT32 rmt_wnd);

// nmq utils
static inline IINT8 dup_ack_limit(NMQ *q);

static inline void init_stat(nmq_stat_t *stat);

// return size of next packet in send_queue.
static IUINT32 next_packet_size(dlist *list);

static inline void seg_first_sent(NMQ *q, segment *s);

static inline void seg_timeout(NMQ *q, segment *s);

static inline void seg_reach_fastack(NMQ *q, segment *s);

// mem ops {
//static inline segment *nmq_new_segment(IUINT32 data_size);
//static inline void delete_segment(segment *seg);
static inline void *nmq_malloc(size_t size);

static inline void *nmq_free(void *addr);

static inline void allocate_mem(NMQ *q);

static inline int is_recv_done(NMQ *q);

static inline void check_send_done(NMQ *q);

static inline int is_self_closed(NMQ *q);

//static inline void check_done(NMQ *q);

// enc
// encode certain segment fields to buf. return new address that can encode
char *encode_seg_and_data(segment *s, char *buf);

// utils
static inline IUINT32 modsn(IUINT32 sn, IUINT32 moder);

// global variables
static nmq_malloc_fn gs_malloc_fn = malloc;
static nmq_free_fn gs_free_fn = free;


static void flush_snd_buf(NMQ *q) {
    IINT32 rcv_wnd = avaible_rcv_wnd(q);
    IINT32 resent = dup_ack_limit(q);

    IUINT32 n_loss = 0;
    IUINT32 max_lost_sn = 0;
    IUINT32 n_timeout = 0;
    IUINT32 current = q->current;

    char buf[NMQ_BUF_SIZE] = {0};//caution!!! must be larger than MSS, or invalid memory access will occur!! e.g BUFSIZ
    char *p = buf;

    // 1 mss can hold many complete segments
    dlnode *node = NULL, *nxt = NULL;
//    while (list_not_empty(&q->snd_buf)) { // caution!! this may cause error!
//    fprintf(stderr, "peer: %ld, snd_cwnd: %u, cwnd: %u, rmt_wnd: %u, rcv_wnd: %u, snd_nxt: %d, snd_una: %d nsnd_que: %d,  sending ",
//            (long)q->arg, get_snd_cwnd(q), q->fc.cwnd, q->rmt_wnd, rcv_wnd, q->snd_nxt, q->snd_una, q->nsnd_que);
    FOR_EACH(node, nxt, &q->snd_buf) {
        segment *s = ADDRESS_FOR(segment, head, node);
        IUINT8 need_send = 0;
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
            fprintf(stderr,
                    "peer: %ld, sending seg: %p, rcv_wnd: %u, cwnd: %u, sn: %u, len: %u, last sendts: %u, nsnd_nxt: %d, rcv_nxt: %u, snd_una: %d nsnd_que: %d, rto: %u, resendts:%u, curr: %u, n_timeout: %d, n_loss: %d\n",
                    (long) q->arg, s, rcv_wnd, q->fc.cwnd, s->sn, s->len, s->sendts, q->snd_nxt, q->rcv_nxt, q->snd_una,
                    q->nsnd_que, s->rto,
                    s->resendts, current, n_timeout, n_loss);
            s->n_sent++;
            s->wnd = rcv_wnd;
            s->una = q->rcv_nxt;    //  figure out diff among snd_una, una, rcv_nxt
            s->sendts = current;
            // note: s->cmd, s->conv, s->sn and s->frag must not be updated here. they are assigned values in #sndq2buf
            s->resendts = s->rto + current;

            if (s->len + (p - buf) >= q->NMQ_MSS) {   // buf is not enough to hold another segment
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
//    fprintf(stderr, "\n");

    // flush remaining data
    if (p > buf) {
        nmq_output(q, buf, p - buf);
    }

    if (n_loss + n_timeout > 0) {
        fc_pkt_loss(&q->fc, max_lost_sn, n_loss, n_timeout);
    }
}


static void flush(NMQ *q) {
    fill_snd_buf(q);

    window_probe_req2peer_if_need(q);

    acks2peer(q);

    sndq2buf(q);

    flush_snd_buf(q);

    rcvbuf2q(q);

    window_probe_build_req_if_need(q);

//    check_done(q);
    check_send_done(q);
//
//    is_recv_done(q);
}

int fill_snd_buf(NMQ *q) {
    if (is_self_closed(q)) {
        return 0;
    }

    if (!q->read_cb) {
        return 0;
    }

//    assert(q->read_cb != NULL);

    IUINT32 cwnd = get_snd_cwnd(q);
    IINT32 nNeed = cwnd - (q->snd_nxt - q->snd_una + q->nsnd_que);
    if (nNeed <= 0) {  // no need to
        fprintf(stderr, "nNeed %d, cwnd: %u(q.rmt_wnd: %u), snd_nxt: %u, snd_una: %u, nsnd_que: %u\n", nNeed, cwnd,
                q->rmt_wnd, q->snd_nxt, q->snd_una, q->nsnd_que);
        return 0;
    }
    char buf[q->NMQ_MSS];
    IINT32 nread = 0;
    IINT32 i = 0;
//    for (; i < nNeed; i++) {
    for (; i < q->MAX_SND_BUF_NUM; i++) {   // read q.MAX_SND_BUF_NUM other than nNeed;
        int err = 0;
        nread = q->read_cb(q, buf, q->NMQ_MSS, &err);
        if (nread <= 0) {
//            fprintf(stderr, "%s, read_cb return %d, errno: %d\n", __FUNCTION__, nread, err);
            if (0 == nread || !WDBLOCK(err)) {
                nmq_shutdown_send(q);
            }
            return nread;
        }
        do_send(q, buf, nread);
    }
    fprintf(stderr, "peer %ld, read %d lines.\n", q->arg, i);
    return i;
}

static void send_failed(NMQ *q, IUINT32 sn) {
    if (q->state == NMQ_STATE_SEND_FAILURE) {
        if (q->failure_cb) {
            (q->failure_cb)(q, sn);
        }
    }
}

static IINT8 input_segment(NMQ *q, segment *s) {
    if (s->conv != q->conv) {
        return NMQ_ERR_CONV_DIFF;
    }

//    fprintf(stderr, "peer %ld: input segment ,seg: %p, receive sn: %u,  s->una: %u, rmt_wnd: %u, nsnd_buf: %u\n",  (
//            long) q->arg, s, s->sn, s->una);
//
    fprintf(stderr,
            "peer %ld, input segment, s->sn: %u, cmd: %d, frag: %d, wnd: %u, sendts: %u, len: %u, s->una: %u, rmt_wnd: %u, snd_nxt: %u, nsnd_buf: %u, nsnd_que: %u\n",
            q->arg, s->sn, s->cmd, s->frag, s->wnd, s->sendts, s->len, s->una, q->rmt_wnd, q->snd_nxt,
            (q->snd_nxt - q->snd_una), q->nsnd_que);

    if ((s->sn < q->rcv_nxt) || (s->sn >= (q->rcv_nxt + q->MAX_RCV_BUF_NUM))) {  // out of range
        fprintf(stderr, "%s, s->sn: %u, s->una: %u ,q->rcv_nxt: %u\n", __FUNCTION__, s->sn, s->una, q->rcv_nxt);
        q->ack_failures++;
        return NMQ_ERR_INVALID_SN;
    }

    if (q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)]) {
        // important! ack seg may get lost.
        // if we receive this seg again, we believe ack seg is lost.
        // we set ack again to resend ack during next flush once ack is lost.
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

    fprintf(stderr,
            "peer: %ld, input seg: %p, wnd: %u, cwnd: %u, sn: %u, len: %u, sendts: %u nsnd_nxt: %d, snd_una: %d nsnd_que: %d, current: %u\n",
            (long) q->arg, s, s->wnd, q->fc.cwnd, s->sn, s->len, s->sendts, q->snd_nxt, q->snd_una, q->nsnd_que,
            (q->current) % 10000);

    set_ack_ts(q, s->sn, s->sendts);
//    set_ack_ts(q, s->sn, q->current); bug. here!!!

    dlist_add_after(prev, &s->head);
    q->nrcv_buf++;
    q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)] = &s->head;

    return 0;
}

void nmq_update(NMQ *q, IUINT32 current) {
    assert(q->inited == 1);

    IINT32 df = current - q->current;  // df cannot < 0
    if (df > 10000 || df < 0) {
        df = q->flush_interval;
    }
    if (df >= q->flush_interval) {
        q->current = current;
        flush(q);
    }
}

// ignore it
void nmq_flush(NMQ *q, IUINT32 current) {
    assert(q->inited == 1);
    q->current = current;
    flush(q);
}

void nmq_start(NMQ *q) {
    if (!q->inited) {
        q->inited = 1;
        allocate_mem(q);
    }
}

// <0 for error.
// >= 0 for size inputed
IINT32 nmq_input(NMQ *q, const char *buf, const int buf_size) {
    const char *p = buf;
    IINT32 size = buf_size;
    const IUINT32 old_una = q->snd_una;
    IINT32 tot = 0;

    while (size && p) {
        if (size < SEG_HEAD_SIZE) {
            fprintf(stderr,
                    "peer: %lu, nmq_input, nsnd_que: %u, snd_nxt: %u, snd_una: %u, nrcv_que: %u, nrcv_buf: %u, buf_size: %d, size: %d\n",
                    (long) q->arg, q->nsnd_que, q->snd_nxt, q->snd_una, q->nrcv_que, q->nrcv_buf, buf_size, size);
            return NMQ_ERR_MSG_BROKEN;
        }

        IUINT32 conv;
        p = decode_uint32(&conv, p);
        if (conv != q->conv) {  // just return. if conv does not match, the remaining data is consider invalid.
            return NMQ_ERR_CONV_DIFF;
        }

        IUINT8 cmd;
        p = decode_uint8(&cmd, p);

        if (CMD_DATA != cmd && CMD_ACK != cmd && CMD_WND_REQ != cmd
            && CMD_WND_ANS != cmd && CMD_FIN != cmd) {// drop this pkt if could not understand cmd
            fprintf(stderr, "wrong cmd: %d\n", cmd);
            return NMQ_ERR_WRONG_CMD;
        }

        IUINT32 sn, len = 0, ts_send, una;
        IUINT16 wnd;
        IUINT8 frag;
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
                fprintf(stderr, "peer eof. sn: %u\n", sn);
                q->peer_fin_sn = sn;
            }
            segment *s = nmq_new_segment(len);
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
//                p += s->len;
            }
            IINT8 ret = input_segment(q, s);
            if (ret != 0) {
                nmq_delete_segment(s);
                fprintf(stderr, "nmq_input wrong: %d\n", ret);
//                return ret;  should not return. may appear duplicate segs. and they are valid pkt
            }
        } else if (CMD_ACK == cmd) {
            input_acks(q, p, len, old_una);
            process_una(q, una);    //  s->sendts will be zero if processed before input_acks
//            sndq2buf(q);
        } else if (CMD_WND_REQ == cmd) {
            fprintf(stderr, "cmd_wnd_req, wnd: %u, una: %u, rmt_wnd: %u, snd_nxt: %u, nsnd_buf: %u, nsnd_que: %u\n",
                    wnd, una, q->rmt_wnd, q->snd_nxt, (q->snd_nxt - q->snd_una), q->nsnd_que);
            window_probe_ans2peer(q);
        } else if (CMD_WND_ANS == cmd) {
            fprintf(stderr, "cmd_wnd_ans, wnd: %u, una: %u, rmt_wnd: %u, snd_nxt: %u, nsnd_buf: %u, nsnd_que: %u\n",
                    wnd, una, q->rmt_wnd, q->snd_nxt, (q->snd_nxt - q->snd_una), q->nsnd_que);
        } else {
            fprintf(stderr, "unreachable branch!\n");
            assert(0);
        }
        if (len) {
            p += len;
        }
    }

    rcvbuf2q(q);
//    if (rcvbuf2q(q)) {
//        flush_rcv_q(q);
//    }

    fill_snd_buf(q);
    return tot;
}

//void flush_rcv_q(NMQ *q) {
//    if (q->recv_cb && list_not_empty(&q->rcv_que)) {
//        char buf[q->NMQ_MSS] = {0};
//        int nrcv = 0;
//        do {
//            nrcv = do_recv(q, buf, q->NMQ_MSS);
//            if (nrcv > 0 || nrcv != NMQ_ERR_NO_DATA) {  // dont report err if empty
//                q->recv_cb(q, buf, nrcv);
//            }
//        } while (nrcv > 0 && list_not_empty(&q->rcv_que));
//    }
//}

IINT32 do_recv(NMQ *q, char *buf, const int buf_size) {
    // todo: how large should buf_size be for datgram pkt?
    // https://stackoverflow.com/questions/2862071/how-large-should-my-recv-buffer-be-when-calling-recv-in-the-socket-library
    IINT32 rcvq_size = next_packet_size(&q->rcv_que);
    if (rcvq_size > buf_size) { // simply drop packet if no enough buf
        fprintf(stderr, "buf too small. size: %d. require: %d\n", buf_size, rcvq_size);
        return NMQ_ERR_MSG_SIZE;
    }

    char *p = buf;
    dlnode *node, *nxt;
    FOR_EACH(node, nxt, &q->rcv_que) {
        segment *s = ADDRESS_FOR(segment, head, node);
        if (s->len) {
            memcpy(p, s->data, s->len);
        }
        p += s->len;
        const IINT8 frag = s->frag;

        dlist_remove_node(node);
        nmq_delete_segment(s);
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

IINT32 do_send(NMQ *q, const char *data, const int len) {
    if (is_self_closed(q)) {
        fprintf(stderr, "send already shutdown.\n");
        return NMQ_ERR_SEND_ON_SHUTDOWNED;
    }

    const char *phead = data;
    IUINT32 tot = len / q->NMQ_MSS;
    tot = (len % q->NMQ_MSS) ? tot + 1 : tot;

    if (tot > 255) {    // frag num is stored with IUINT8
        return ERR_DATA_TOO_LONG;
    }

//  for (IUINT8 i = (IUINT8) (tot - 1); i >= 0; i--) {  caution!!! this is wrong because i > 0 is always true if i is IUINT8
    for (IUINT8 i = (IUINT8) tot; i > 0; i--) {     // frag order is n, n - 1 ... 0.
        const IUINT32 slen = (i > 1) ? q->NMQ_MSS : (len - (tot - 1) * q->NMQ_MSS);
        segment *s = nmq_new_segment(slen);
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

    return (IINT32) (phead - data);
}

IINT32 nmq_output(NMQ *q, const char *data, const int len) {
    assert(q->output_cb != NULL);

    if (len > 0 || len == NMQ_SEND_EOF) {
        return q->output_cb(data, len, q, q->arg);
    }
    return NMQ_NO_DATA;
}

IINT32 nmq_send(NMQ *q, const char *data, const int len) {
    if (!data || len <= 0) {
        return 0;
    }

    if (q->read_cb) {
        fprintf(stderr, "input ways collide. read_cb is set.\n");
        return NMQ_ERR_WRONG_INPUT;
    }

    return do_send(q, data, len);
}

// caution. must pay attention to fields that are assigned. they should be equal to nmq_send
void append_fin(NMQ *q) {
    fin_ops(q, CMD_FIN);
    fprintf(stderr, "append fin.\n");
}

void fin_ops(NMQ *q, char cmd) {
    segment *s = nmq_new_segment(0);
    s->conv = q->conv;
    s->cmd = cmd;
    s->n_sent = 0;
    s->frag = 0;
    s->len = 0;
    s->data = 0;    // una is assigned in flush_snd_buf
    dlist_add_tail(&q->snd_que, &s->head);
    q->nsnd_que++;
}

// < 0 for error.
IINT32 nmq_recv(NMQ *q, char *buf, const int buf_size) {
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
static IINT32 rcvbuf2q(NMQ *q) {
    IINT32 tot = 0;
    dlnode *node, *nxt;
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
//            fprintf(stderr, "peer: %ld, rcvbuf2q, seg: %p, sn: %u, rcv_nxt: %u\n", (long) q->arg, s, s->sn, q->rcv_nxt);
//            set_ack_ts(q, s->sn, 0); it's called in input_segment
            tot += seg->len;
        } else {
            break;
        }
    }
    return tot;
}

static IINT32 sndq2buf(NMQ *q) {
    IINT32 tot = 0;

    // local send buf/que len should not be a bottleneck.
    IUINT32 cwnd = get_snd_cwnd(q);

    // flow control. num(pending segments in snd_buf) <= cwnd
    dlnode *node, *nxt;

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
//        fprintf(stderr,
//                "peer: %lu, sndq2buf, sn: %u, snd_una: %u, snd_nxt: %u, rcv_nxt: %u, nrcv_buf: %u, nsnd_que: %u, cwnd: %u, snd_wnd: %u\n",
//                (long) q->arg, s->sn, q->snd_una, q->snd_nxt, q->rcv_nxt, q->nrcv_buf, q->nsnd_que, q->fc.cwnd,
//                get_snd_cwnd(q));
//        q->nsnd_buf++;
        tot++;
    }
    return tot;
}


// acks {
static void acks2peer(NMQ *q) {
    if (!q->ackcount) {
        if (q->ack_failures) {
            fprintf(stderr, "ackcount is zero. build window probe answer!!!\n");
            window_probe_ans2peer(q);   // build a window probe answer
        }
        q->ack_failures = 0;
        return;
    }

    const int UNIT = 2 * sizeof(IUINT32);
    const IUINT32 max_nack = q->NMQ_MSS / UNIT;
    const IUINT32 tot = UNIT * max_nack;
    fprintf(stderr, "peer: %lu, rcv_nxt: %u, una: %u, snd_nxt: %u, acks2peer, ackcount: %d, current: %u\n", (
            long) q->arg, q->rcv_nxt, q->snd_una, q->snd_nxt, q->ackcount, (q->current) % 10000);
    for (int i = 0; i < q->ackcount; i++) {
        IUINT32 sn;
        decode_uint32(&sn, (const char *) (i * 2 + q->acklist));
        fprintf(stderr, "sn: %u, ", sn);
    }
    fprintf(stderr, "\n");

    segment *s = nmq_new_segment(tot);    // allocate maximum size todo: declare on stack

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

    IUINT32 ackcnt = q->ackcount;
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

    nmq_delete_segment(s);
    q->ackcount = 0;    // important! must reset q.ackcount
    q->ack_failures = 0;
}

// ack(sn) sent by peer, telling local that peer itself received segment(sn). The segment(sn) is sent by local.
// this op will remove acked segs
static void process_ack(NMQ *q, IUINT32 sn, IUINT32 ts_send) {
    IUINT32 sn_mod = modsn(sn, q->MAX_SND_BUF_NUM);
//    if (!sn || sn < q->snd_una || sn >= q->snd_nxt || !q->snd_sn_to_node[sn_mod]) { // sn 0 is invalid
    if (sn < q->snd_una || sn >= q->snd_nxt || !q->snd_sn_to_node[sn_mod]) { // sn 0 is valid!!!
        fprintf(stderr, "peer: %ld, invalid ack sn: %u, ts_send: %u\n", (long) q->arg, sn, ts_send);
        return;
    }
    if (ts_send) {  //  during processing una. just delete segments and do nothing
        update_rtt(q, sn, ts_send);
    }

    dlnode *node = q->snd_sn_to_node[sn_mod];
    dlist_remove_node(node);

    q->snd_sn_to_node[sn_mod] = NULL;
    segment *s = ADDRESS_FOR(segment, head, node);
    nmq_delete_segment(s);
//    q->nsnd_buf--;
}

static void count_repeat_acks(NMQ *q, IUINT32 maxack) {
    dlnode *node, *nxt;
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
static void input_acks(NMQ *q, const char *p, IUINT32 len, const IUINT32 old_una) {
    IUINT32 maxack = 0;
    const int UNIT = 2 * sizeof(IUINT32);
    IUINT32 nacks = len / UNIT;

    fprintf(stderr, "%s, len: %d, ", __FUNCTION__, len);
    while (len >= UNIT) {
        len -= UNIT;
        IUINT32 ack_sn, ack_ts_send;
        p = decode_uint32(&ack_sn, p);
        p = decode_uint32(&ack_ts_send, p);
        maxack = (IUINT32) MAX(maxack, ack_sn);
        process_ack(q, ack_sn, ack_ts_send);    // will delete segments
        fprintf(stderr, "sn: %u, ", ack_sn);
    }
    fprintf(stderr, "\n");
    // todo: multiple segments send(or receive?) at same time will not regard as dup acks.
    // todo: cause this cause retransmit very soon.

    count_repeat_acks(q, maxack);

    update_una(q);
    update_rto(q);
    fc_input_acks(&q->fc, q->snd_una, nacks);
}

static void set_ack_ts(NMQ *q, IUINT32 sn, IUINT32 ts_send) {
    if (q->ackcount == q->ackmaxnum) {
        const size_t UNIT = 2 * sizeof(IUINT32);
        IUINT32 newnum = q->ackmaxnum << 1;
        void *newbuf = nmq_malloc(newnum * UNIT);

        memcpy(newbuf, q->acklist, q->ackcount * UNIT);
        nmq_free(q->acklist);

        q->acklist = (IUINT32 *) newbuf;
        q->ackmaxnum = newnum;
    }

    char *p1 = (char *) (q->acklist + q->ackcount * 2);
    char *p2 = (char *) (q->acklist + q->ackcount * 2 + 1);

//    if (is_little_endian()) {
    encode_uint32(sn, p1);
    encode_uint32(ts_send, p2);
//    } else {
//        big_endian_to_little(sn, p1);
//        big_endian_to_little(ts_send, p2);
//    }

    q->ackcount++;
}
// } acks


// una {
static void process_una(NMQ *q, IUINT32 una) {
//    fprintf(stderr, "peer: %ld, receive una: %u\n", (
//    long)q->arg, una);
    dlnode *node, *nxt;
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
//    fprintf(stderr, "peer: %ld, before update_una, nsnd_que: %u, snd_nxt: %u, snd_una: %u, nrcv_buf: %u, nrcv_que: %u\n", (long) q->arg,  q->nsnd_que, q->snd_nxt, q->snd_una, q->nrcv_buf, q->nrcv_que);

    if (list_not_empty(&q->snd_buf)) {
        q->snd_una = ADDRESS_FOR(segment, head, q->snd_buf.next)->sn;
    } else {
        q->snd_una = q->snd_nxt;
    }
//    fprintf(stderr, "peer: %ld, after update_una, nsnd_uque: %u, snd_nxt: %u, snd_una: %u, nrcv_buf: %u, nrcv_que: %u\n", (long) q->arg, q->nsnd_que, q->snd_nxt, q->snd_una, q->nrcv_buf, q->nrcv_que);

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
        fprintf(stderr, "rmt_wnd: %u, probe_pending: %d\n", q->rmt_wnd, q->probe_pending);
        q->probe_pending = 1;
        q->ts_probe_wait = q->current + NMQ_PROBE_WAIT_MS_DEF;
    }
}

static void do_window_probe_or_answer(NMQ *q, IUINT16 cmd) {
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
    fc->MSS = q->NMQ_MSS;
    fc->ssth_alpha = 0.5;   // todo
    fc->cwnd = NMQ_CWND_INIT;
    fc->incr = fc->cwnd * fc->MSS;
    fc->ssthresh = NMQ_SSTHRESH_DEF;
    fc->in_trouble = 0;
    fc->max_lost_sn = 0;
}

void fc_pkt_loss(fc_s *fc, IUINT32 max_lost_sn, IUINT32 n_loss, IUINT32 n_timeout) {
    if (!fc->in_trouble && (n_loss + n_timeout) >= fc->TROUBLE_TOLERANCE) {
        fprintf(stderr, "%s, before, n_loss: %u, n_timeout: %u, max_lost_sn: %u, ssthresh: %u, cwnd: %u\n",
                __FUNCTION__, n_loss, n_timeout, max_lost_sn, fc->ssthresh, fc->cwnd);
        fc->in_trouble = 1;
        fc->max_lost_sn = max_lost_sn;
        fc->ssthresh = MAX(NMQ_SSTHRESH_MIN, fc->cwnd * fc->ssth_alpha);   // todo, change 0.5 for test
        fc->cwnd = fc->ssthresh;  //todo: test
//        fc->cwnd  = fc->cwnd * 3 / 4;
        if (n_loss) {
            fc->cwnd += fc->DUP_ACK_LIM;
        }
        fc->incr = fc->cwnd * fc->MSS;
        fprintf(stderr, "%s, after, n_loss: %u, n_timeout: %u, max_lost_sn: %u, ssthresh: %u, cwnd: %u\n", __FUNCTION__,
                n_loss, n_timeout, max_lost_sn, fc->ssthresh, fc->cwnd);

    }
}

void fc_input_acks(fc_s *fc, const IUINT32 una, IUINT32 nacks) {
    fprintf(stderr, "%s, before, f->cwnd: %u, nacks: %u, ssthreash: %u, una: %u, max_lost_sn: %u\n", __FUNCTION__,
            fc->cwnd, nacks, fc->ssthresh, una, fc->max_lost_sn);

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

    fprintf(stderr, "%s, after, f->cwnd: %u, nacks: %u, ssthreash: %u, una: %u, max_lost_sn: %u\n", __FUNCTION__,
            fc->cwnd, nacks, fc->ssthresh, una, fc->max_lost_sn);
}

void fc_normal_acks(fc_s *fc, IUINT32 nacks) {
    fprintf(stderr, "%s, before, f->cwnd: %u, nacks: %u, ssthreash: %u, nacks: %u\n", __FUNCTION__, fc->cwnd, nacks,
            fc->ssthresh, nacks);
    if (fc->cwnd <= fc->ssthresh) {  // slow start
        fc->cwnd = MIN(fc->cwnd + nacks, fc->ssthresh + 1);
        fc->incr = fc->cwnd * fc->MSS;
    } else {    // congestion avoidance
        fc->incr += fc->MSS * fc->MSS / fc->incr + (fc->MSS >> 3);
        if ((fc->cwnd + 1) * fc->MSS <= fc->incr) {
            fc->cwnd += 1;
        }
    }
    fprintf(stderr, "%s, after, f->cwnd: %u, nacks: %u, ssthreash: %u\n", __FUNCTION__, fc->cwnd, nacks, fc->ssthresh);
}
// } fc

// rtt & rto {
static inline void update_rtt(NMQ *q, IUINT32 sn, IUINT32 sendts) {
//    segment *s = ADDRESS_FOR(segment, head, q->snd_sn_to_node[sn]); bug!!
    segment *s = ADDRESS_FOR(segment, head, q->snd_sn_to_node[modsn(sn, q->MAX_SND_BUF_NUM)]);
    if (s->sendts == sendts) {
        IUINT32 rtt = q->current - sendts;
        q->stat.nrtt++;
        q->stat.nrtt_tot += rtt;
        rtt_estimator(q, &q->rto_helper, rtt);
    }
}

static void rtt_estimator(NMQ *q, rto_helper_s *rter, IUINT32 rtt) {
    // copied from linux kernel. chinese explaination: http://www.pagefault.info/?p=430
    IINT64 m = rtt;
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
    IUINT32 rto1 = q->rto;
    q->rto = MAX(NMQ_RTO_MIN, MIN(NMQ_RTO_MAX, (rter->srtt >> 3) + rter->rttvar));
    fprintf(stderr, "peer %ld, update rto, first old rto: %u, new rto: %u, rter->srrt >> 3: %u, rter->rttvar: %u\n",
            (long) q->arg, rto1, q->rto, rter->srtt >> 3, rter->rttvar);
}
// } rtt & rto


// wnd ops {
static inline IUINT32 get_snd_cwnd(NMQ *q) {
    IUINT32 cwnd = MIN(q->MAX_SND_BUF_NUM, q->rmt_wnd); // attention to how get_snd_cwnd is used
    if (q->fc_on) {    // todo
        return MIN(cwnd, q->fc.cwnd);
    }
    return cwnd;
}

static inline IINT32 avaible_rcv_wnd(NMQ *q) {
    return q->MAX_RCV_BUF_NUM - q->nrcv_buf;
}

static inline void update_rmt_wnd(NMQ *q, IUINT32 rmt_wnd) {
    q->rmt_wnd = rmt_wnd;
}
// } wnd ops


// nmq utils {
static inline IINT8 dup_ack_limit(NMQ *q) {
    if (q->fc_on) {
        return q->fc.DUP_ACK_LIM;
    }
    return NMQ_DUP_ACK_LIM_DEF;   // standard
}


void init_stat(nmq_stat_t *stat) {
    stat->nrtt = 0;
    stat->nrtt_tot = 0;
    stat->bytes_send = 0;
    stat->bytes_send_tot = 0;
}

// return size of next packet in send_queue.
static IUINT32 next_packet_size(dlist *list) {
    dlnode *it, *nxt;
    IUINT32 len = 0;
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

// todo: seg rto and q->rto has no relathionship right now!!!!!
// timeout should be rto + rtt!!!
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

void nmq_set_wnd_size(NMQ *nmq, IUINT32 sndwnd, IUINT32 rcvwnd) {
    nmq->MAX_SND_BUF_NUM = sndwnd;
    nmq->MAX_RCV_BUF_NUM = rcvwnd;
}

//void nmq_set_recv_cb(NMQ *q, nmq_recv_cb cb) {
//    if (!q->flushed) {
//        q->recv_cb = cb;
//    }
//}
//} nmq utils


void nmq_shutdown_send(NMQ *q) {
    fprintf(stderr, "%s\n", __FUNCTION__);
    if (!q->fin_sn) {
        q->fin_sn = 1;
        append_fin(q);
    } else {
        fprintf(stderr, "already shutdown!!\n");
    }
}

void nmq_set_read_cb(NMQ *q, nmq_read_cb cb) {
    q->read_cb = cb;
}

// memory ops. {
NMQ *nmq_new(IUINT32 conv, void *arg) {
    NMQ *q = (NMQ *) nmq_malloc(sizeof(NMQ));
    memset(q, 0, sizeof(NMQ));

    q->conv = conv;
    q->arg = arg;
    q->inited = 0;
    q->flush_interval = NMQ_FLUSH_INTERVAL_DEF;
    q->type = NMQ_TYPE_DGRAM;

    q->MAX_SND_BUF_NUM = NMQ_BUF_NUM_DEF;   // must be initialized before rcv_sn_to_node
    q->MAX_RCV_BUF_NUM = NMQ_BUF_NUM_DEF;
//    q->MAX_SND_QUE_NUM = NMQ_QUE_NUM_DEF;
//    q->MAX_RCV_QUE_NUM = NMQ_QUE_NUM_DEF;
    q->NMQ_MSS = NMQ_MSS_DEF;

    q->rmt_wnd = NMQ_RMT_WND_DEF;

    q->snd_una = 0;
    q->snd_nxt = 0;

    q->rcv_nxt = 0;

    dlist *lists[] = {&q->snd_buf, &q->snd_que, &q->rcv_buf, &q->rcv_que, NULL};
    for (int i = 0; lists[i]; i++) {
        dlist_init(lists[i]);
    }

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

    q->fc_on = 0;    // todo: change to off
    fc_init(q, &q->fc);

    q->output_cb = NULL;
    q->failure_cb = NULL;
//    q->recv_cb = NULL;

    q->peer_fin_sn = 0;
    q->fin_sn = 0;
//    q->send_done_cb = NULL;

    return q;
}

static inline void allocate_mem(NMQ *q) {
    q->snd_sn_to_node = (dlist **) nmq_malloc(sizeof(dlnode *) * q->MAX_SND_BUF_NUM);
    memset(q->snd_sn_to_node, 0, sizeof(dlnode *) * q->MAX_SND_BUF_NUM);

    q->rcv_sn_to_node = (dlist **) nmq_malloc(sizeof(dlnode *) * q->MAX_RCV_BUF_NUM);
    memset(q->rcv_sn_to_node, 0, sizeof(dlnode *) * q->MAX_RCV_BUF_NUM);

    q->acklist = (IUINT32 *) nmq_malloc(q->ackmaxnum * sizeof(IUINT32) * 2);
    memset(q->acklist, 0, q->ackmaxnum * sizeof(IUINT32) * 2);
}

// either send_done or recv_done exists, it means self or peer closed.
void check_send_done(NMQ *q) {
    if (is_self_closed(q) && !list_not_empty(&q->snd_que) && (!list_not_empty(&q->snd_buf))) {
        fprintf(stderr, "send completed\n");
        nmq_output(q, NULL, NMQ_SEND_EOF);
    }
}

//void check_done(NMQ *q) {
//    // if true, it means peer is closed.
//    // since we already finished our task, we no longer need to send to peer or recv from peer.
//    // if peer want us send data it, it should never close itself.
//    if (check_send_done(q) || is_recv_done(q)) {
//        nmq_output(q, NULL, NMQ_EOF);
//    }
//}

int is_recv_done(NMQ *q) {
    if (q->peer_fin_sn > 0) {
        if ((q->rcv_nxt > q->peer_fin_sn) && (!list_not_empty(&q->rcv_buf)) && (!list_not_empty(&q->rcv_que))) {
            fprintf(stderr, "recv completed\n");
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
    fprintf(stderr,
            "%s, snd_una: %u, rcv_nxt: %u, snd_nxt: %u, avg rtt: %lf. bytes_send: %d, bytes_output_tot: %d, ratio: %lf\n",
            __FUNCTION__, q->snd_una, q->rcv_nxt,
            q->snd_nxt, q->stat.nrtt != 0 ? (q->stat.nrtt_tot * 1.0) / (q->stat.nrtt) : -1, q->stat.bytes_send,
            q->stat.bytes_send_tot, q->stat.bytes_send == 0 ? -1.0 : (q->stat.bytes_send_tot * 1.0 / q->stat.bytes_send));
    dlist *lists[] = {&q->snd_buf, &q->snd_que, &q->rcv_buf, &q->rcv_que, NULL};
    for (int i = 0; lists[i]; i++) {
        dlnode *node, *nxt;
        fprintf(stderr, "delete list %p\n", lists[i]);
        FOR_EACH(node, nxt, lists[i]) {
            segment *s = ADDRESS_FOR(segment, head, node);
            dlist_remove_node(node);
            nmq_delete_segment(s);
        }
    }
    nmq_free(q);
}

//static inline segment *nmq_new_segment(IUINT32 data_size) {
segment *nmq_new_segment(IUINT32 data_size) {
    segment *s = (segment *) nmq_malloc(sizeof(segment));
    memset(s, 0, sizeof(segment));  // the order must no be wrong.

    dlist_init(&s->head);
    if (data_size) {
        s->data = (char *) nmq_malloc(data_size); // must after memset
        memset(s->data, 0, data_size);
    }

//    fprintf(stderr, "new segment: %p\n", s);
    return s;
}

void nmq_delete_segment(segment *seg) {
//static inline void delete_segment(segment *seg) {
    if (seg) {
//        if (seg->sn == 0) {
//            fprintf(stderr, "delete sn = 0\n");
//        }
//        fprintf(stderr, "deleting segment: %p, sn: %u\n", seg, seg->sn);
        nmq_free(seg->data);
        nmq_free(seg);
    }
}

static inline void *nmq_malloc(size_t size) {
    void *p = gs_malloc_fn(size);
    if (NULL == p) {
        //todo
        fprintf(stderr, "no enough memeory. abort!");
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
IUINT32 nmq_get_conv(const char *buf) {
    if (buf) {
        IUINT32 conv;
        decode_uint32(&conv, buf);
        return conv;
    }
    return 0;
}

static inline IUINT32 modsn(IUINT32 sn, IUINT32 moder) {
    return (sn + moder) % moder;
}

// } util

// nmq helper functions
void nmq_set_ssthresh(NMQ *q, IUINT32 ssthresh) {
    if (!q->inited) {
        if (ssthresh > NMQ_SSTHRESH_MIN) {
            q->fc.ssthresh = ssthresh;
        }
    }
}

void nmq_set_init_cwnd(NMQ *q, IUINT32 cwnd) {
    if (!q->inited && cwnd) {
        q->fc.cwnd = cwnd;
    }
}

void nmq_set_trouble_tolerance(NMQ *q, IUINT8 n_tolerance) {
    if (!q->inited) {
        if (n_tolerance >= NMQ_TROUBLE_TOLERANCE_MIN && n_tolerance <= NMQ_TROUBLE_TOLERANCE_MAX) {
            q->fc.TROUBLE_TOLERANCE = n_tolerance;
        }
    }
}

void nmq_set_dup_acks_limit(NMQ *q, IUINT8 lim) {
    if (!q->inited) {
        if (lim >= NMQ_DUP_ACK_LIM_MIN && lim <= NMQ_DUP_ACK_LIM_MAX) {
            q->fc.DUP_ACK_LIM = lim;
        }
    }
}

// MSS <= MTU - SEG_HEAD_SIZE - sum(OTHER_PROTOCOL_HEAD_SIZE)
void nmq_set_mss(NMQ *q, IUINT32 MSS) {
    if (!q->inited) {
        q->NMQ_MSS = MSS;
        q->fc.MSS = MSS;
    }
}

void nmq_set_max_attempt(NMQ *q, IUINT32 max_try, nmq_failure_cb cb) {
    if (!q->inited) {
        q->MAX_PKT_TRY = max_try > 0 ? max_try : NMQ_MAX_TRY;
        q->failure_cb = cb;
    }
}

//void nmq_set_max_que_len(NMQ *q, IUINT32 que_len) {
//    if (!q->inited) {
//        q->MAX_SND_QUE_NUM = que_len;
//        q->MAX_RCV_QUE_NUM = que_len;
//    }
//}


void nmq_set_interval(NMQ *q, IUINT32 interval) {
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