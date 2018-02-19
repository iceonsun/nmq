//=====================================================================
//
// test.cpp - kcp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <sys/errno.h>

#include "test.h"
#include "../nmq.h"

#define MAX_SN 5000
// 模拟网络
LatencySimulator *vnet;

int g_done = 0;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, const int len, NMQ *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
//	fprintf(stderr, "udp_output: source: %ld, data: %p, len: %d\n", (long)user, buf, len);
	parameter.ptr = user;
    if (len >= 0) {
        vnet->send(parameter.id, buf, len);
    } else if (len == NMQ_SEND_EOF) {
//        fprintf(stderr, "udp_output: source %ld  eof.\n", (long)user);
        g_done = 1;
    }
	return 0;
}

// 测试用例
void test(int mode)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(50, 60, 125);

	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
    NMQ *kcp1 = nmq_new(0x11223344, (void*)1);
    NMQ *kcp2 = nmq_new(0x11223344, (void*)2);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
    nmq_set_output_cb(kcp1, udp_output);
    nmq_set_output_cb(kcp2, udp_output);
	nmq_set_interval(kcp1, 50);
	nmq_set_interval(kcp2, 50);
	nmq_set_dup_acks_limit(kcp1, 2);
	nmq_set_dup_acks_limit(kcp2, 2);
    nmq_set_wnd_size(kcp1, 128, 128);
    nmq_set_wnd_size(kcp2, 128, 128);
    nmq_start(kcp1);
    nmq_start(kcp2);

	uint32_t current = iclock();
	uint32_t slap = current + 20;
//	uint32_t slap = current;
	uint32_t index = 0;
	uint32_t next = 0;
	int64_t sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	char buffer[2000];
	int hr;
	uint32_t cnt = 0;

	uint32_t ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		nmq_update(kcp1, iclock());

		nmq_update(kcp2, iclock());

        // app -> peer 1
        for (; current >= slap; slap += 20) {
            ((uint32_t*)buffer)[0] = index;
            ((uint32_t*)buffer)[1] = current;

            // 发送上层协议包
			if (nmq_send(kcp1, buffer, 8) > 0) {
				index++;
			}
        }

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(2, buffer, 2000);
//            fprintf(stderr, "peer 2. read buf to peer 2. nmq_input, hr: %d\n", hr);
            if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			nmq_input(kcp2, buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
            hr = vnet->recv(1, buffer, 2000);
            if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
            int32_t ret =  nmq_input(kcp1, buffer, hr);
		}

		while (1) {
			hr = nmq_recv(kcp2, buffer, 2000);
//            fprintf(stderr, "peer 2. nmq_recv, hr: %d\n", hr);
            // 没有收到包就退出
			if (hr <= 0) {
                break;
            }
			// 如果收到包就回射
            nmq_send(kcp2, buffer, hr);     // if error occurs, then data will be lost.
		}

		while (1) {
			hr = nmq_recv(kcp1, buffer, 2000);
			// 没有收到包就退出
			if (hr <= 0) break;
			uint32_t sn = *(uint32_t*)(buffer + 0);
			uint32_t ts = *(uint32_t*)(buffer + 4);
			uint32_t rtt = current - ts;

			if (sn != next) {
				// 如果收到的包不连续
				fprintf(stderr, "ERROR sn %d<->%d\n", (int)sn, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (uint32_t)maxrtt) maxrtt = rtt;

			fprintf(stderr, "[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
		}
		if (next >= MAX_SN) break;
	}

	ts1 = iclock() - ts1;

	nmq_destroy(kcp1);
    nmq_destroy(kcp2);

	const char *names[3] = { "default", "normal", "fast" };
	printf("%s mode result (%dms):\n", names[mode], (int)ts1);
	printf("avgrtt=%d maxrtt=%d tx=%d\n", (int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1);
}

void nmq_done(NMQ *q) {
    g_done = 1;
    fprintf(stderr, "nmq_done!!!\n");
}
// 测试用例
void test_shutdown_send(int mode)
{
    // 创建模拟网络：丢包率10%，Rtt 60ms~125ms
    vnet = new LatencySimulator(50, 60, 125);

    // 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
    // 最后一个是 user参数，用来传递标识
//	NMQ *kcp1 = ikcp_create(0x11223344, (void*)0);
//	NMQ *kcp2 = ikcp_create(0x11223344, (void*)1);
    NMQ *kcp1 = nmq_new(0x11223344, (void*)1);
    NMQ *kcp2 = nmq_new(0x11223344, (void*)2);

    // 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
    nmq_set_output_cb(kcp1, udp_output);
    nmq_set_output_cb(kcp2, udp_output);
    nmq_set_interval(kcp1, 50);
    nmq_set_interval(kcp2, 50);
    nmq_set_dup_acks_limit(kcp1, 2);
    nmq_set_dup_acks_limit(kcp2, 2);
    nmq_start(kcp1);
    nmq_start(kcp2);

    uint32_t current = iclock();
    uint32_t slap = current + 20;
//	uint32_t slap = current;
    uint32_t index = 0;
    uint32_t next = 0;
    int64_t sumrtt = 0;
    int count = 0;
    int maxrtt = 0;

    char buffer[2000];
    int hr;
    uint32_t cnt = 0;

    uint32_t ts1 = iclock();

    while (!g_done) {
        isleep(1);
        current = iclock();
        nmq_update(kcp1, iclock());

        nmq_update(kcp2, iclock());

        while (cnt < 10 && !g_done) {
            ((uint32_t*)buffer)[0] = index;
            ((uint32_t*)buffer)[1] = current;

            // 发送上层协议包
            if (nmq_send(kcp1, buffer, 8) > 0) {
                index++;
            }
            cnt++;
            if (cnt == 10) {
                nmq_shutdown_send(kcp1);
            }
        }

        // 处理虚拟网络：检测是否有udp包从p1->p2
        while (!g_done) {
            hr = vnet->recv(2, buffer, 2000);
//            fprintf(stderr, "peer 2. read buf to peer 2. nmq_input, hr: %d\n", hr);
            if (hr < 0) break;
            // 如果 p2收到udp，则作为下层协议输入到kcp2
            nmq_input(kcp2, buffer, hr);
        }

        // 处理虚拟网络：检测是否有udp包从p2->p1
        while (!g_done) {
            hr = vnet->recv(1, buffer, 2000);
//            fprintf(stderr, "peer 1. read buf to peer 1. nmq_input, hr: %d\n", hr);
            if (hr < 0) break;
            // 如果 p1收到udp，则作为下层协议输入到kcp1
            int32_t ret =  nmq_input(kcp1, buffer, hr);
        }

        // kcp2接收到任何包都返回回去
        while (!g_done) {
            hr = nmq_recv(kcp2, buffer, 2000);
            // 没有收到包就退出
            if (hr <= 0) {
                break;
            }
            // 如果收到包就回射
            nmq_send(kcp2, buffer, hr);     // if error occurs, then data will be lost.
        }

        // kcp1收到kcp2的回射数据
        while (!g_done) {
            hr = nmq_recv(kcp1, buffer, 2000);
//            fprintf(stderr, "peer 1. nmq_recv, hr: %d\n", hr);
            // 没有收到包就退出
            if (hr <= 0) break;
            uint32_t sn = *(uint32_t*)(buffer + 0);
            uint32_t ts = *(uint32_t*)(buffer + 4);
            uint32_t rtt = current - ts;

            fprintf(stderr, "peer: %d, outer receive sn: %u, ts: %u\n", 1, sn, ts);

            if (sn != next) {
                // 如果收到的包不连续
                fprintf(stderr, "ERROR sn %d<->%d\n", (int)sn, (int)next);
                return;
            }

            next++;
            sumrtt += rtt;
            count++;
            if (rtt > (uint32_t)maxrtt) maxrtt = rtt;

            fprintf(stderr, "[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
        }
        if (next >= MAX_SN) break;
    }

    ts1 = iclock() - ts1;

    nmq_destroy(kcp1);
    nmq_destroy(kcp2);

    const char *names[3] = { "default", "normal", "fast" };
    printf("%s mode result (%dms):\n", names[mode], (int)ts1);
}

int main()
{
    test(0);
	return 0;
}

