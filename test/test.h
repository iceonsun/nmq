#ifndef __TEST_H__
#define __TEST_H__

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

#include "../nmq.h"

#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#include <windows.h>
#elif !defined(__unix)
#define __unix
#endif

#ifdef __unix

#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>

#endif

/* get system time */
static inline void itimeofday(long *sec, long *usec) {
#if defined(__unix)
    struct timeval time;
    gettimeofday(&time, NULL);
    if (sec) *sec = time.tv_sec;
    if (usec) *usec = time.tv_usec;
#else
    static long mode = 0, addsec = 0;
    BOOL retval;
    static IINT64 freq = 1;
    IINT64 qpc;
    if (mode == 0) {
        retval = QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
        freq = (freq == 0)? 1 : freq;
        retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
        addsec = (long)time(NULL);
        addsec = addsec - (long)((qpc / freq) & 0x7fffffff);
        mode = 1;
    }
    retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
    retval = retval * 2;
    if (sec) *sec = (long)(qpc / freq) + addsec;
    if (usec) *usec = (long)((qpc % freq) * 1000000 / freq);
#endif
}

/* get clock in millisecond 64 */
//static inline IINT64 iclock64(void)
//{
//	long s, u;
//	IINT64 value;
//	itimeofday(&s, &u);
//	value = ((IINT64)s) * 1000 + (u / 1000);
//	return value;
//}
//
//static inline IUINT32 iclock()
//{
//	return (IUINT32)(iclock64() & 0xfffffffful);
//}
//
/* sleep in millisecond */
static inline void isleep(unsigned long millisecond) {
#ifdef __unix    /* usleep( time * 1000 ); */
//	struct timespec ts;
//	ts.tv_sec = (time_t)(millisecond / 1000);
//	ts.tv_nsec = (long)((millisecond % 1000) * 1000000);
    /*nanosleep(&ts, NULL);*/
    usleep((millisecond << 10) - (millisecond << 4) - (millisecond << 3));
//	usleep(millisecond * 1000);
#elif defined(_WIN32)
    Sleep(millisecond);
#endif
}

#ifdef __cplusplus

#include <list>
#include <vector>

// 带延迟的数据包
class DelayPacket {
public:
    virtual ~DelayPacket() {
//        fprintf(stderr, "deleting _ptr %p\n", _ptr);
        if (_ptr) delete _ptr;
        _ptr = NULL;
    }

    DelayPacket(int size, const void *src = NULL) {
        _ptr = new unsigned char[size];
        _size = size;
        if (src) {
            memcpy(_ptr, src, size);
        }
    }

    unsigned char *ptr() { return _ptr; }

    const unsigned char *ptr() const { return _ptr; }

    int size() const { return _size; }

    IUINT32 ts() const { return _ts; }

    void setts(IUINT32 ts) { _ts = ts; }

protected:
    unsigned char *_ptr;
    int _size;
    IUINT32 _ts;
};

// 均匀分布的随机数
class Random {
public:
    Random(int size) {
        this->size = 0;
        seeds.resize(size);
    }

    int random() {
        int x, i;
        if (seeds.size() == 0) return 0;
        if (size == 0) {
            for (i = 0; i < (int) seeds.size(); i++) {
                seeds[i] = i;
            }
            size = (int) seeds.size();
        }
        i = rand() % size;
        x = seeds[i];
        seeds[i] = seeds[--size];
        return x;
    }

protected:
    int size;
    std::vector<int> seeds;
};

// 网络延迟模拟器
class LatencySimulator {
public:

    virtual ~LatencySimulator() {
        clear();
    }

    // lostrate: 往返一周丢包率的百分比，默认 10%
    // rttmin：rtt最小值，默认 60
    // rttmax：rtt最大值，默认 125
    LatencySimulator(int lostrate = 10, int rttmin = 60, int rttmax = 125, int nmax = 1000) :
            r12(100), r21(100) {
        current = iclock();
        this->lostrate = lostrate / 2;    // 上面数据是往返丢包率，单程除以2
        this->rttmin = rttmin / 2;
        this->rttmax = rttmax / 2;
        this->nmax = nmax;
        tx1 = tx2 = 0;
    }

    // 清除数据
    void clear() {
        DelayTunnel::iterator it;
        for (it = p12.begin(); it != p12.end(); it++) {
            delete *it;
        }
        for (it = p21.begin(); it != p21.end(); it++) {
            delete *it;
        }
        p12.clear();
        p21.clear();
    }

    // 发送数据
    // peer - 端点1/2，从1发送，从2接收；从2发送从1接收
    void send(int peer, const void *data, int size) {
//        DelayTunnel &tunnel = p12;
//        Random &r = r12;
//        tx1++;
        DelayTunnel *tunnel = &p12;
        if (peer == 1) {
            tx1++;
            if (r12.random() < lostrate) {
//                fprintf(stderr, "drop. lost rate test.\n");
                return;
            }
            if ((int) p12.size() >= nmax) return;
        } else {
            tunnel = &p21;
            tx2++;
            if (r21.random() < lostrate) {
//                fprintf(stderr, "drop. lost rate test.\n");
                return;  //todo: uncoment for lost rate test
            }
            if ((int) p21.size() >= nmax) return;
        }


        DelayPacket *pkt = new DelayPacket(size, data);
        current = iclock();
        IUINT32 delay = rttmin;
        if (rttmax > rttmin) delay += rand() % (rttmax - rttmin);
//		fprintf(stderr, "delay: %dms\n", delay);
        pkt->setts(current + delay);
        if (peer == 1) {
//            if (!(tx1 % 3)) {    // simulate packet loss
//                fprintf(stderr, "peer 1 send: drop packt. tx1 = %d\n", tx1);
//                delete pkt;
//                return;
//            }
            p12.push_back(pkt);
        } else {
//            if (!(tx2 % 5)) {
//                fprintf(stderr, "peer 2 send: drop packt. tx2 = %d\n", tx2);
//                delete pkt;
//                return;
//            }
            p21.push_back(pkt);
        }

//        fprintf(stderr, "send, tunnel: %p, size: %d, pkt: %p, _ptr: %p\n", tunnel, tunnel->size(),  pkt, pkt->ptr());

    }

    // 接收数据
    int recv(int peer, void *data, int maxsize) {
        DelayTunnel *tunnel = &p21;
        const char *peerstr = peer == 1 ? "2->1" : "1->2";
        if (peer == 2) {
            tunnel = &p12;
        }
        if (tunnel->empty()) {
//            fprintf(stderr, "%s tunnenl empty\n", peerstr);
            return -1;
        }

        current = iclock();
        auto it = tunnel->begin();
//        fprintf(stderr, "%s, size: %ld, time left: ", peerstr, tunnel->size());
//        for (; (it != tunnel->end()); it++) {
//            fprintf(stderr, " %d ", current - (*it)->ts());
//            if (current >= (*it)->ts()) {    // timeout
//                break;
//            }
//        }


        if (it == tunnel->end()) {
            return -2;
        }
        if ((*it)->ts() > current)
            return -2;

        fprintf(stderr, "\n");


        DelayPacket *pkt = *it;
//		if (peer == 1) {
//            if (p21.size() == 0) return -1;
//            it = p21.begin();
//		}	else {
//            if (p12.size() == 0) return -1;
//            it = p12.begin();
//		}
//		DelayPacket *pkt = *it;
//		current = iclock();
//		if (current < pkt->ts()) {
//			fprintf(stderr, "%u ms left to timeout. just return\n", pkt->ts() - current);
//			return -2;	// todo
//		}
        if (maxsize < pkt->size()) return -3;
//        fprintf(stderr, "recv, tunnel: %p, size: %d, pkt: %p,  _ptr: %p\n", tunnel, tunnel->size(), pkt, pkt->ptr());
        tunnel->erase(it);
//		if (peer == 1) {
////			fprintf(stderr, "2->1\n");
//			p21.erase(it);
//		}	else {
////			fprintf(stderr, "1->2\n");
//			p12.erase(it);
//		}
        maxsize = pkt->size();
        memcpy(data, pkt->ptr(), maxsize);
        delete pkt;
        return maxsize;
    }

public:
    int tx1;
    int tx2;

protected:
    IUINT32 current;
    int lostrate;
    int rttmin;
    int rttmax;
    int nmax;
    typedef std::list<DelayPacket *> DelayTunnel;
    DelayTunnel p12;
    DelayTunnel p21;
    Random r12;
    Random r21;
};

#endif

#endif


