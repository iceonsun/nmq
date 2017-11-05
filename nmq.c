//
// Created by Robert Cai on 10/21/17.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "dlist.h"
#include "enc.h"
#include "nmq.h"
#include "util.h"

#define ACK_TOLERANCE_DEF 2
#define TIMEOUT_TOLERANCE_DEF 2

const IUINT8 CMD_INVALID = 0;
const IUINT8 CMD_DATA = 1;
const IUINT8 CMD_ACK = 2;
const IUINT8 CMD_WND_REQ = 3;    // ask peer wnd
const IUINT8 CMD_WND_ANS = 4;   // tell my wnd to peer. split req and ans to avoid repeatedly sending cmd_wnd_X

static IINT32 sndq2buf(NMQ *q);
static IINT32 rcvbuf2q(NMQ *q);
static IINT8 input_segment(NMQ *q, segment *s);
static void nmq_flush_snd_buf(NMQ *q);
static void nmq_flush(NMQ *q);

// acks
static void set_ack_ts(NMQ *q, IUINT32 sn, IUINT32 ts_send);
static void process_ack(NMQ *q, IUINT32 sn, IUINT32 ts_send);
static void input_acks(NMQ *q, const char *buf, const IUINT32 old_una);
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

// flow control
static void flow_control_dup_acks(flow_control_s *s, IUINT32 n_loss, IUINT32 max_lost_sn);
static void flow_control_timeout(flow_control_s *f, IUINT32 n_timeout);
static void flow_control_recv_acks(flow_control_s *f, const IUINT32 new_snd_una, const IUINT32 old_una, IUINT32 nacks);
static void flow_control_fast_recovery_new_ack(IUINT32 snd_una, flow_control_s *f);
static void flow_control_normal_ack(flow_control_s *f, const IUINT32 nacks);

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

// enc
// encode certain segment fields to buf. return new address that can encode
char *encode_seg_and_data(segment *s, char *buf);

// utils
static inline IUINT32 modsn(IUINT32 sn, IUINT32 moder);

// flow_control
static inline void init_flow_control(NMQ *q, flow_control_s *f);

// global variables
static nmq_malloc_fn gs_malloc_fn = malloc;
static nmq_free_fn gs_free_fn = free;


static void nmq_flush_snd_buf(NMQ *q) {
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
            seg_timeout(q, s);
            n_timeout++;
        } else if (s->dupacks >=
                   resent) {  // n+1 dup acks. segment will be deleted if it's acked. so here use operator >=
            need_send = 1;  // will send if receive repeat acks. (fast retransmission). tcp/ip illustrated vol1. ch21.7
            seg_reach_fastack(q, s);
            n_loss++; // increment number of segments lost
            s->dupacks = 0;
        }

        if (need_send) {
//            fprintf(stderr, "sn: %u, ", s->sn);
            fprintf(stderr, "peer: %ld, sending seg: %p, rcv_wnd: %u, cwnd: %u, sn: %u, len: %u, nsnd_nxt: %d, snd_una: %d nsnd_que: %d, rto: %u, sendts: %u, resendts:%u, curr: %u, n_timeout: %d, n_loss: %d\n", (long)q->arg,  s, rcv_wnd, q->fc.cwnd, s->sn, s->len, q->snd_nxt, q->snd_una, q->nsnd_que, s->rto, s->sendts, s->resendts, current, n_timeout, n_loss);
//            fprintf(stderr, "peer: %ld, sending seg: %p, wnd_unsed: %u, cwnd: %u, sn: %u, len: %u, nsnd_nxt: %d, snd_una: %d nsnd_que: %d, rto: %u, sendts: %u, resendts:%u, curr: %u, n_timeout: %d, n_loss: %d\n", (long)q->arg,  s, rcv_wnd_unused, q->flow_ctrl.cwnd, s->sn, s->len, q->snd_nxt, q->snd_una, q->nsnd_que, s->rto, s->sendts, s->resendts, current, n_timeout, n_loss);
            max_lost_sn = s->sn;    // snd_buf is order of sn
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

            p = encode_seg_and_data(s, p);

            if (s->n_sent > q->MAX_PKT_TRY) {
                // todo: add implementation
                q->state = NMQ_STATE_SEND_FAILURE;
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


//    if (n_loss) {
//        flow_control_dup_acks(&q->flow_ctrl, n_loss, max_lost_sn);
//    }
//    if (n_timeout) {
//        fprintf(stderr, "peer: %ld, ", (long)q->arg);
//        flow_control_timeout(&q->flow_ctrl, n_timeout);
//    }
}


static void nmq_flush(NMQ *q) {
    IUINT32 current = q->current;
    window_probe_req2peer_if_need(q);

    acks2peer(q);

    sndq2buf(q);

    nmq_flush_snd_buf(q);

    window_probe_build_req_if_need(q);
}

static IINT8 input_segment(NMQ *q, segment *s) {
    if (s->conv != q->conv) {
        return NMQ_ERR_CONV_DIFF;
    }

    fprintf(stderr, "peer %ld: input segment ,seg: %p, receive sn: %u,  una: %u\n",  (
            long) q->arg, s, s->sn, s->una);

    if ((s->sn < q->rcv_nxt) || (s->sn >= (q->rcv_nxt + q->MAX_RCV_BUF_NUM))) {  // out of range
        return NMQ_ERR_INVALID_SN;
    }

    if (q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)]) {
        // important! ack seg may get lost.
        // if we receive this seg again, we believe ack seg is lost.
        // we set ack again to resend ack during next flush once ack is lost.
        set_ack_ts(q, s->sn, q->current);
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

    set_ack_ts(q, s->sn, q->current);

    dlist_add_after(prev, &s->head);
    q->nrcv_buf++;
    q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)] = &s->head;

    return 0;
}

void nmq_update(NMQ *q, IUINT32 current) {
    if (!q->flushed) {
        q->flushed = 1;
    }

    q->current = current;
    IINT32 df = q->current - q->ts_flush_nxt;
    if (df > 10000 || df < -10000) {
        df = q->flush_interval;
    }

    if (df >= 0) {
        q->ts_flush_nxt = q->current + q->flush_interval;
        nmq_flush(q);
    }
}

IINT32 nmq_input(NMQ *q, const char *buf, const int buf_size) {
    const char *p = buf;
    IINT32 size = buf_size;
    const IUINT32 old_una = q->snd_una;

    while (size && p) {
        if (size < SEG_HEAD_SIZE) {
            fprintf(stderr, "peer: %lu, nmq_input, nsnd_que: %u, snd_nxt: %u, snd_una: %u, nrcv_que: %u, nrcv_buf: %u, buf_size: %d, size: %d\n", (long)q->arg, q->nsnd_que, q->snd_nxt, q->snd_una, q->nrcv_que, q->nrcv_buf, buf_size, size);
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
            && CMD_WND_ANS != cmd) {// drop this pkt if could not understand cmd
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

        update_rmt_wnd(q, wnd);

        if (CMD_ACK != cmd) {
            process_una(q, una);
        }

        size -= (len + SEG_HEAD_SIZE);

        if (CMD_DATA == cmd) {
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
                p += s->len;
            }
            IINT8 ret = input_segment(q, s);
            if (ret != 0) {
                delete_segment(s);
                fprintf(stderr, "nmq_input wrong: %d\n", ret);
//                return ret;  should not return. may appear duplicate segs. and they are valid pkt
            }
        } else if (CMD_ACK == cmd) {
            input_acks(q, buf, old_una);
            process_una(q, una);    //  s->sendts will be zero if processed before input_acks
//            sndq2buf(q);
        } else if (CMD_WND_REQ == cmd) {
            window_probe_ans2peer(q);
        } else if (CMD_WND_ANS == cmd) {
            fprintf(stderr, "cmd_wnd_ans\n");
        } else {
            fprintf(stderr, "unreachable branch!\n");
        }
    }

    rcvbuf2q(q);
    return 0;
}

IINT32 nmq_output(NMQ *q, const char *data, const int len) {
    assert(q->fn_output);

    if (len != 0) {
        return q->fn_output(data, len, q, q->arg);
    }
    return NMQ_ERR_NO_DATA;
}

IINT32 nmq_send(NMQ *q, const char *data, const int len) {
    if (!data || len <= 0) {
        return 0;
    }

    IINT32 ndone = sndq2buf(q);

    const char *phead = data;
    IUINT32 tot = len / q->NMQ_MSS;
    tot = (len % q->NMQ_MSS) ? tot + 1 : tot;

    if (tot > 255) {    // frag num is stored with IUINT8
        return ERR_DATA_TOO_LONG;
    }

    if (tot + q->nsnd_que > q->MAX_SND_QUE_NUM) {
        fprintf(stderr, "peer: %lu, current len: %u, max: %u, data unit required: %u\n",(long)q->arg,  q->nsnd_que, q->MAX_SND_QUE_NUM, tot);
        return NMQ_ERR_SND_QUE_NO_MEM;
    }
//    fprintf(stderr, "peer: %lu, %d seg move from sndq2buf, nsnd_que: %u, snd_nxt: %u, snd_una: %u, cwnd: %u, snd_cwnd: %u, rmt_wnd: %u\n", (long)q->arg, ndone, q->nsnd_que, q->snd_nxt, q->snd_una, q->flow_ctrl.cwnd, get_snd_cwnd(q), q->rmt_wnd);
//  for (IUINT8 i = (IUINT8) (tot - 1); i >= 0; i--) {  caution!!! this is wrong because i > 0 is always true if i is IUINT8
    for (IUINT8 i = (IUINT8) tot; i > 0; i--) {     // frag order is n, n - 1 ... 0.
        const IUINT32 slen = (i > 1) ? q->NMQ_MSS : (len - (tot - 1) * q->NMQ_MSS);
        segment *s = nmq_new_segment(slen);
        s->conv = q->conv;
        s->cmd = CMD_DATA;
        s->n_sent = 0;    // important
//        s->sn = q->snd_nxt++; assigned in sndq2buf.
        s->frag = i - 1;
        s->len = slen;
        memcpy(s->data, phead, s->len);
        phead += s->len;
        // una, wnd will be assigned in flush_snd_buf.
        dlist_add_tail(&q->snd_que,
                       &s->head); // attention: add s->head to tail, not s. when can retrieve s from s->head
    }

    q->nsnd_que += tot;

    return (IINT32) (phead - data);
}

// return 0 if success.
// > 0 for specifc reason.
// < 0 if buf is too small and -retval is size that buf should be.
IINT32 nmq_recv(NMQ *q, char *buf, const int buf_size) {
    if (!q->flushed) {
        return NMQ_ERR_UNITIALIZED;
    }

    rcvbuf2q(q);    // in case there is no seg in que

    if (!list_not_empty(&q->rcv_que)) {
        return NMQ_ERR_NO_DATA;
    }

    // todo: how large should buf_size be for datgram pkt?
    // https://stackoverflow.com/questions/2862071/how-large-should-my-recv-buffer-be-when-calling-recv-in-the-socket-library
    IINT32 rcvq_size = next_packet_size(&q->rcv_que);
    if (rcvq_size > buf_size) { // simply drop packet if no enough buf
        return -rcvq_size;
    }

    char *p = buf;
    dlnode *node, *nxt;
    FOR_EACH(node, nxt, &q->rcv_que) {
        segment *s = ADDRESS_FOR(segment, head, node);
        memcpy(p, s->data, s->len);
        p += s->len;
        const IINT8 frag = s->frag;

        dlist_remove_node(node);
        delete_segment(s);
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

static IINT32 rcvbuf2q(NMQ *q) {
    IINT32 tot = 0;
    dlnode *node, *nxt;
//    for (dlnode *node = q->rcv_buf.next, *nxt = node; node != &q->rcv_buf; node = nxt) {
    FOR_EACH(node, nxt, &q->rcv_buf) {
//        nxt = nxt->next;
        segment *s = ADDRESS_FOR(segment, head, node);
        if ((s->sn == q->rcv_nxt) && (q->nrcv_que < q->MAX_RCV_QUE_NUM)) {
            q->rcv_sn_to_node[modsn(s->sn, q->MAX_RCV_BUF_NUM)] = NULL;
            segment *seg = ADDRESS_FOR(segment, head, node);
            q->nrcv_que++;
            q->nrcv_buf--;
            q->rcv_nxt++;
            dlist_remove_node(node);
            dlist_add_tail(&q->rcv_que, node);
            fprintf(stderr, "peer: %ld, rcvbuf2q, seg: %p, sn: %u, rcv_nxt: %u\n", (long)q->arg, s, s->sn, q->rcv_nxt);
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
//    while (list_not_empty(&q->snd_que)
//           && (q->snd_nxt < (q->snd_una + cwnd))) {
//        dlnode *node = q->snd_que.next;
        if (q->snd_nxt >= (q->snd_una + cwnd)) {    // cannot send new data
            break;
        }
        segment *s = (ADDRESS_FOR(segment, head, node));
        s->sn = q->snd_nxt++;   // snd_nxt is increment here other than nmq_flush_snd_buf. because it is used to compute cwnd
        dlist_remove_node(node);
        dlist_add_tail(&q->snd_buf, node);
        q->snd_sn_to_node[modsn(s->sn, q->MAX_SND_BUF_NUM)] = node;
        q->nsnd_que--;
        fprintf(stderr, "peer: %lu, sndq2buf, sn: %u, snd_una: %u, snd_nxt: %u, rcv_nxt: %u, nrcv_buf: %u, nsnd_que: %u, cwnd: %u, snd_wnd: %u\n", (long)q->arg, s->sn, q->snd_una, q->snd_nxt, q->rcv_nxt, q->nrcv_buf, q->nsnd_que, q->fc.cwnd, get_snd_cwnd(q));
//        fprintf(stderr, "peer: %lu, sndq2buf, sn: %u, snd_una: %u, snd_nxt: %u, rcv_nxt: %u, nrcv_buf: %u, nsnd_que: %u, cwnd: %u, snd_wnd: %u\n", (long)q->arg, s->sn, q->snd_una, q->snd_nxt, q->rcv_nxt, q->nrcv_buf, q->nsnd_que, q->flow_ctrl.cwnd, get_snd_cwnd(q));
//        q->nsnd_buf++;
        tot++;
    }
    return tot;
}


// acks {
static void acks2peer(NMQ *q) {
    if (!q->ackcount) {
        return;
    }

    const int UNIT = 2 * sizeof(IUINT32);
    const IUINT32 max_nack = q->NMQ_MSS / UNIT;
    const IUINT32 tot = UNIT * max_nack;
    fprintf(stderr, "peer: %lu, acks2peer, ackcount: %d\n", (
    long)q->arg, q->ackcount);
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

    delete_segment(s);
    q->ackcount = 0;    // important! must reset q.ackcount
}

// ack(sn) sent by peer, telling local that peer itself received segment(sn). The segment(sn) is sent by local.
// this op will remove acked segs
static void process_ack(NMQ *q, IUINT32 sn, IUINT32 ts_send) {
    IUINT32 sn_mod = modsn(sn, q->MAX_SND_BUF_NUM);
//    if (!sn || sn < q->snd_una || sn >= q->snd_nxt || !q->snd_sn_to_node[sn_mod]) { // sn 0 is invalid
    if (sn < q->snd_una || sn >= q->snd_nxt || !q->snd_sn_to_node[sn_mod]) { // sn 0 is valid!!!
        fprintf(stderr, "peer: %ld, invalid ack sn: %u, ts_send: %u\n", (long)q->arg, sn, ts_send);
        return;
    }

    fprintf(stderr, "peer: %ld, ack received: sn: %u, ts_send: %u\n", (long)q->arg, sn, ts_send);

    update_rtt(q, sn, ts_send);

    dlnode *node = q->snd_sn_to_node[sn_mod];
    dlist_remove_node(node);

    q->snd_sn_to_node[sn_mod] = NULL;
    segment *s = ADDRESS_FOR(segment, head, node);
    delete_segment(s);
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
static void input_acks(NMQ *q, const char *buf, const IUINT32 old_una) {
    IUINT32 conv;
    const char *p = decode_uint32(&conv, buf);
    if (conv != q->conv) {
        fprintf(stderr, "conv(%u) not match q.conv(%u)\n", conv, q->conv);
        return; // NMQ_ERR_CONV_DIFF;
    }

    IUINT8 cmd = CMD_INVALID;            // send, ack, wnd_probe
    p = decode_uint8(&cmd, p);
    if (cmd != CMD_ACK) {
        fprintf(stderr, "should be ack cmd. but it's %u\n", cmd);
        return; // NMQ_ERR_WRONG_CMD;
    }

    IUINT32 sn;
    IUINT8 frag;
    IUINT16 wnd;     // tell peer self wnd size
    IUINT32 una;
    IUINT32 ts_send;     // to estimate rtt
    IUINT32 len = 0;
    p = decode_uint32(&sn, p);
    p = decode_uint8(&frag, p);
    p = decode_uint16(&wnd, p);
    p = decode_uint32(&una, p);
    p = decode_uint32(&ts_send, p);
    p = decode_uint32(&len, p);
    IUINT32 maxack = 0;
    IUINT32 nacks = len / (2 * sizeof(IUINT32));

    while (len >= 2) {
        len -= (2 * sizeof(IUINT32));
        IUINT32 ack_sn, ack_ts_send;
        p = decode_uint32(&ack_sn, p);
        p = decode_uint32(&ack_ts_send, p);
        maxack = (IUINT32) MAX(maxack, ack_sn);
        process_ack(q, ack_sn, ack_ts_send);    // will delete segments
    }

    count_repeat_acks(q, maxack);

    update_una(q);

    fprintf(stderr, "peer: %ld, nsnd_buf: %u, old_una: %u, una: %u\n", (
            long)q->arg, (q->snd_nxt - q->snd_una), old_una, q->snd_una);

    update_rto(q);
//    flow_control_recv_acks(&q->flow_ctrl, q->snd_una, old_una, nacks);
    fc_input_acks(&q->fc, q->snd_una, nacks);
}

static void set_ack_ts(NMQ *q, IUINT32 sn, IUINT32 ts_send) {
    if (q->ackcount == q->ackmaxnum) {
        const size_t UNIT = 2 * sizeof(IUINT32);
        IUINT32 newnum = q->ackmaxnum << 1;
        void *newbuf = nmq_malloc(newnum * UNIT);

        memcpy(newbuf, q->acklist, q->ackcount * UNIT);
        nmq_free(q->acklist);

        q->acklist = (IUINT32*)newbuf;
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
        if (s->sn < una){
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
        q->probe_pending = 1;
        q->ts_probe_wait = q->current + NMQ_PROBE_WAIT_DEF;
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


// flow control {
static void flow_control_recv_acks(flow_control_s *f, const IUINT32 new_snd_una, const IUINT32 old_una, IUINT32 nacks) {
    if (f->in_fast_recovery) {
        if (old_una < new_snd_una) { // new ack arrives
            flow_control_fast_recovery_new_ack(new_snd_una, f);
        } else {    // simply increment cwnd to let new seg to be sent
            f->cwnd += nacks;   // tcp/ip illustrated. ch21.7
//            f->cwnd++;
            f->incr = f->cwnd * f->MSS;
        }
    } else {
        flow_control_normal_ack(f, nacks);
    }
}

static void flow_control_normal_ack(flow_control_s *f, const IUINT32 nacks) {
    IINT32 ack2[2] = {0};   // cannot be IUINT32. prevent integer overflow!!!
    if (nacks > f->ssthresh - f->cwnd) {    // first slow start. then congestion avoidance
        ack2[0] = f->ssthresh - f->cwnd + 1;
        ack2[1] = nacks - ack2[0] - 1;  // if type is IUINT32, this may cause overflow
    } else {
        ack2[0] = nacks;
        ack2[1] = 0;
    }

    fprintf(stderr, "%s, before, f->cwnd: %u, nacks: %u, ssthreash: %u\n", __FUNCTION__, f->cwnd, nacks, f->ssthresh);


    f->cwnd += ack2[0]; // slow start. increase exponentially // todo f->incr has to update
    f->incr = f->cwnd * f->MSS;
    while (ack2[1] > 0) {
        f->incr += f->MSS * f->MSS / f->incr + f->MSS / 8;  // congestion avoidance. increase linearly
        ack2[1]--;
    }
    if ((f->cwnd + 1) * f->MSS <= f->incr) {
//        f->cwnd += (IUINT32) f->incr / f->MSS;
        f->cwnd += (IUINT32) (f->incr - f->cwnd * f->MSS) / f->MSS;
    }
    fprintf(stderr, "%s, before, f->cwnd: %u, incr: %f,  nacks: %u, ssthreash: %u\n", __FUNCTION__, f->cwnd, f->incr, nacks, f->ssthresh);
}

static void flow_control_fast_recovery_new_ack(IUINT32 snd_una, flow_control_s *f) {
    if (snd_una > f->max_lost_sn) {  // new ack is bigger than last f.max_lost_sn
        f->cwnd = f->ssthresh;
        f->cwnd++;
        f->incr = f->cwnd * f->MSS;
        f->in_fast_recovery = 0;    // exit fast recovery because new acks arrived
        f->max_lost_sn = 0;
    }
}

static void flow_control_dup_acks(flow_control_s *f, IUINT32 n_loss, IUINT32 max_lost_sn) {
    if (!f->in_fast_recovery) {
        if (n_loss >= f->ACK_TOLERANCE) {    // we believe some segs are really lost due to net congestion
            f->max_lost_sn = max_lost_sn;
            f->in_fast_recovery = 1;
            f->ssthresh = f->cwnd >> 1;
            if (f->ssthresh < NMQ_SSTHRESH_MIN) {
                f->ssthresh = NMQ_SSTHRESH_MIN;
            }
            f->cwnd = f->ssthresh + f->DUP_ACK_LIM;
            f->incr = f->cwnd * f->MSS;
        }
    }  // else ignore repeat acks if in fast recovery
}

static void flow_control_timeout(flow_control_s *f, IUINT32 n_timeout) {
    if (!f->in_fast_recovery) {
        //  repeating ack is main flow control measure if net speed is high
        if (n_timeout >= f->TIMEOUT_TOLRANCE) {
            f->ssthresh = f->cwnd >> 1;
            if (f->ssthresh < NMQ_SSTHRESH_MIN) {
                f->ssthresh = NMQ_SSTHRESH_MIN;
            }

            if (f->NO_SLOW_START_WHEN_TIMEOUT) {
                f->cwnd >>= 1;  // skip slow start when timeout.
                f->cwnd = f->cwnd < 1 ? 1 : f->cwnd;
            } else {
                f->cwnd = 1;    // do slow start
            }
            fprintf(stderr, "n_timeout: %u, cwnd: %u, ssthresh: %u\n", n_timeout, f->cwnd, f->ssthresh);
            f->incr = f->cwnd * f->MSS;
        }
    }
}
// } flow control

// fc {
void fc_init(NMQ *q, fc_s *fc) {
    memset(fc, 0, sizeof(fc_s));
    fc->TROUBLE_TOLERANCE = 2;  // todo
    fc->DUP_ACK_LIM = NMQ_DUP_ACK_LIM_DEF;  // todo
    fc->MSS = q->NMQ_MSS;
    fc->ssth_alpha = 0.5;   // todo
    fc->cwnd = 10;
    fc->incr = fc->cwnd * fc->MSS;
    fc->ssthresh = NMQ_SSTHRESH_DEF;
    fc->in_trouble = 0;
    fc->max_lost_sn = 0;
}
void fc_pkt_loss(fc_s *fc, IUINT32 max_lost_sn, IUINT32 n_loss, IUINT32 n_timeout) {
    if (!fc->in_trouble && (n_loss + n_timeout) >= fc->TROUBLE_TOLERANCE) {
        fprintf(stderr, "%s, before, n_loss: %u, n_timeout: %u, max_lost_sn: %u, ssthresh: %u, cwnd: %u\n", __FUNCTION__, n_loss, n_timeout, max_lost_sn, fc->ssthresh, fc->cwnd);
        fc->in_trouble = 1;
        fc->max_lost_sn = max_lost_sn;
        fc->ssthresh = MAX(NMQ_SSTHRESH_MIN, fc->cwnd * fc->ssth_alpha);   // todo, change 0.5 for test
        fc->cwnd = fc->ssthresh;
        if (n_loss) {
            fc->cwnd += fc->DUP_ACK_LIM;
        }
        fc->incr = fc->cwnd * fc->MSS;
        fprintf(stderr, "%s, after, n_loss: %u, n_timeout: %u, max_lost_sn: %u, ssthresh: %u, cwnd: %u\n", __FUNCTION__, n_loss, n_timeout, max_lost_sn, fc->ssthresh, fc->cwnd);

    }
}

void fc_input_acks(fc_s *fc, const IUINT32 una, IUINT32 nacks) {
    fprintf(stderr, "%s, before, f->cwnd: %u, nacks: %u, ssthreash: %u, una: %u, max_lost_sn: %u\n", __FUNCTION__, fc->cwnd, nacks, fc->ssthresh, una, fc->max_lost_sn);

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

    fprintf(stderr, "%s, after, f->cwnd: %u, nacks: %u, ssthreash: %u, una: %u, max_lost_sn: %u\n", __FUNCTION__, fc->cwnd, nacks, fc->ssthresh, una, fc->max_lost_sn);
}

void fc_normal_acks(fc_s *fc, IUINT32 nacks) {
    fprintf(stderr, "%s, before, f->cwnd: %u, nacks: %u, ssthreash: %u, nacks: %u\n", __FUNCTION__, fc->cwnd, nacks, fc->ssthresh, nacks);
    if (fc->cwnd <= fc->ssthresh) {  // slow start
        fc->cwnd = MIN(fc->cwnd + nacks, fc->ssthresh + 1);
        fc->incr = fc->cwnd * fc->MSS;
    } else {    // congestion avoidance
        fc->incr += fc->MSS * fc->MSS / fc->incr + (fc->MSS >> 3);
        if ((fc->cwnd + 1) * fc->MSS <= fc->incr) {
            fc->cwnd += 1;
        }
    }
    fprintf(stderr, "%s, before, f->cwnd: %u, nacks: %u, ssthreash: %u\n", __FUNCTION__, fc->cwnd, nacks, fc->ssthresh);
}
// } fc

// rtt & rto {
static inline void update_rtt(NMQ *q, IUINT32 sn, IUINT32 sendts) {
//    segment *s = ADDRESS_FOR(segment, head, q->snd_sn_to_node[sn]); bug!!
    segment *s = ADDRESS_FOR(segment, head, q->snd_sn_to_node[modsn(sn, q->MAX_SND_BUF_NUM)]);
    if (s->sendts == sendts) {
        IUINT32 rtt = q->current - sendts;
        rtt_estimator(q, &q->rto_helper, rtt);
    }
}

static void rtt_estimator(NMQ *q, rto_helper_s *rter, IUINT32 rtt) {
    // copied from linux kernel. chinese explaination: http://www.pagefault.info/?p=430
    IINT64 m = rtt;
    if (m <= 0) {
        m = 1;
    }
    if (!rter->srtt) {
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
    IUINT32  rto1 = q->rto;
    q->rto = MAX(NMQ_RTO_MIN, MIN(NMQ_RTO_MAX, rter->srtt >> 3 + rter->rttvar));
    fprintf(stderr, "peer %ld, update rto, first old rto: %u, new rto: %u, rter->srrt >> 3: %u, rter->rttvar: %u\n", (long)q->arg, rto1, q->rto, rter->srtt >> 3, rter->rttvar);
}
// } rtt & rto


// wnd ops {
static inline IUINT32 get_snd_cwnd(NMQ *q) {
//    IUINT32 cwnd = MIN(q->MAX_SND_BUF_NUM - q->snd_una, q->rmt_wnd); bug!!
//    IUINT32 cwnd = MIN(q->MAX_SND_BUF_NUM - (q->snd_nxt - q->snd_una), q->rmt_wnd); bug!!
    IUINT32 cwnd = MIN(q->MAX_SND_BUF_NUM, q->rmt_wnd); // attention to how get_snd_cwnd is used
    if (q->flow_ctrl_on) {
//        return MIN(cwnd, q->flow_ctrl.cwnd);
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
    if (q->flow_ctrl_on) {
//        return q->flow_ctrl.DUP_ACK_LIM;
        return q->fc.DUP_ACK_LIM;
    }
    return NMQ_DUP_ACK_LIM_DEF;   // standard
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
}

static inline void seg_timeout(NMQ *q, segment *s) {
    if (q->nodelay) {
        s->rto += (s->rto >> 1);
    } else {
        s->rto <<= 1;   // double the time
    }

    if (s->rto > NMQ_RTO_MAX) {
        s->rto = NMQ_RTO_MAX;
    }
}

static inline void seg_reach_fastack(NMQ *q, segment *s) {
    s->rto = NMQ_RTO_DEF;   // will resend the fragment. so reset timeout to defval.
}

IUINT32 nmq_get_conv(const char *buf) {
    IUINT32 conv;
    decode_uint32(&conv, buf);
    return conv;
}

void nmq_set_output_func(NMQ *q, nmq_output_fn fn) {
    q->fn_output = fn;
}
//} nmq utils


// memory ops. {
NMQ *nmq_new(IUINT32 conv, void *arg) {
    NMQ *q = (NMQ *) nmq_malloc(sizeof(NMQ));
//    memset(q, 0, sizeof(NMQ));

    q->conv = conv;
    q->arg = arg;
    q->flushed = 0;
    q->flush_interval = NMQ_FLUSH_INTERVAL_DEF;

    q->MAX_SND_BUF_NUM = NMQ_MAX_BUF_NUM;   // must be initialized before rcv_sn_to_node
    q->MAX_RCV_BUF_NUM = NMQ_MAX_BUF_NUM;
    q->MAX_SND_QUE_NUM = NMQ_MAX_QUE_NUM;
    q->MAX_RCV_QUE_NUM = NMQ_MAX_QUE_NUM;
    q->NMQ_MSS = NMQ_MSS_DEF;


    q->rmt_wnd = NMQ_RMT_WND_DEF;

    q->snd_una = 0;
    q->snd_nxt = 0;
    // q.snd_buf
    // q.snd_que
    q->snd_sn_to_node = (dlist**)nmq_malloc(sizeof(dlnode*) * q->MAX_SND_BUF_NUM);
    memset(q->snd_sn_to_node, 0, sizeof(dlnode*) * q->MAX_SND_BUF_NUM);

    q->rcv_nxt = 0;
//    q->rcv_buf
    q->rcv_sn_to_node = (dlist**)nmq_malloc(sizeof(dlnode*) * q->MAX_SND_BUF_NUM);
    memset(q->rcv_sn_to_node, 0, sizeof(dlnode*) * q->MAX_RCV_BUF_NUM);
//    q->rcv_que

    dlist* lists[] = {&q->snd_buf, &q->snd_que, &q->rcv_buf, &q->rcv_que, NULL};
    for (int i = 0; lists[i]; i++) {
        dlist_init(lists[i]);
    }

    q->nrcv_que = 0;
    q->nrcv_buf = 0;
    q->nsnd_que = 0;

    q->ackmaxnum = 128;
    q->acklist = (IUINT32*)nmq_malloc(q->ackmaxnum * sizeof(IUINT32) * 2);
    q->ackcount = 0;

    q->rto = NMQ_RTO_DEF;
    q->nodelay = 0; // todo:
    q->ts_probe_wait = NMQ_PROBE_WAIT_DEF;
    q->probe_pending = 0;

    memset(&q->rto_helper, 0, sizeof(q->rto_helper));

    q->MAX_PKT_TRY = 50;
    q->state = 0;

    q->flow_ctrl_on = 1;    // todo: verify cwnd later. it's not working right now
    fc_init(q, &q->fc);
//    init_flow_control(q, &q->flow_ctrl);
//    q->flow_ctrl.MSS = q->NMQ_MSS;

    return q;
}

void nmq_destroy(NMQ *q) {
    nmq_free(q->snd_sn_to_node);
    nmq_free(q->rcv_sn_to_node);;
    nmq_free(q->acklist);
    dlist* lists[] = {&q->snd_buf, &q->snd_que, &q->rcv_buf, &q->rcv_que, NULL};
    for (int i = 0; lists[i]; i++) {
        dlnode *node, *nxt;
        fprintf(stderr, "delete list %p\n", lists[i]);
        FOR_EACH(node, nxt, lists[i]) {
            segment *s = ADDRESS_FOR(segment, head, node);
            dlist_remove_node(node);
            delete_segment(s);
        }
    }
    nmq_free(q);
}

//static inline segment *nmq_new_segment(IUINT32 data_size) {
segment *nmq_new_segment(IUINT32 data_size) {
    segment *s = (segment*) nmq_malloc(sizeof(segment));
    memset(s, 0, sizeof(segment));  // the order must no be wrong.

    dlist_init(&s->head);
    if (data_size) {
        s->data = (char *) nmq_malloc(data_size); // must after memset
        memset(s->data, 0, data_size);
    }

//    fprintf(stderr, "new segment: %p\n", s);
    return s;
}

void delete_segment(segment *seg) {
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
static inline IUINT32 modsn(IUINT32 sn, IUINT32 moder) {
    return (sn + moder) % moder;
}

// } util


// flow control {
static inline void init_flow_control(NMQ *q, flow_control_s *f) {
//    todo. f.DUP_ACK_LIM
    f->MSS = q->NMQ_MSS;
    f->cwnd = q->MAX_SND_BUF_NUM;
    f->incr = f->cwnd * f->MSS;
    f->ssthresh = NMQ_SSTHRESH_DEF;
    f->NO_SLOW_START_WHEN_TIMEOUT = 0;
    f->DUP_ACK_LIM = NMQ_DUP_ACK_LIM_DEF;
    f->ACK_TOLERANCE = ACK_TOLERANCE_DEF;
    f->TIMEOUT_TOLRANCE = TIMEOUT_TOLERANCE_DEF;
}
// } flow control
