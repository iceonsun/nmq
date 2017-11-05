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

#include "enc.h"
#include "test.h"
#include "dlist.h"
#include "nmq.h"

#define MAX_SN 1000
// 模拟网络
LatencySimulator *vnet;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, const int len, NMQ *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
	fprintf(stderr, "udp_output: source: %ld, data: %p, len: %d\n\n", (long)user, buf, len);
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

// 测试用例
void test(int mode)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(10, 60, 125);

	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
//	NMQ *kcp1 = ikcp_create(0x11223344, (void*)0);
//	NMQ *kcp2 = ikcp_create(0x11223344, (void*)1);
    NMQ *kcp1 = nmq_new(0x11223344, (void*)1);
    NMQ *kcp2 = nmq_new(0x11223344, (void*)2);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
	nmq_set_output_func(kcp1, udp_output);
    nmq_set_output_func(kcp2, udp_output);
//	kcp1->output = udp_output;
//	kcp2->output = udp_output;

	IUINT32 current = iclock();
	IUINT32 slap = current + 20;
//	IUINT32 slap = current;
	IUINT32 index = 0;
	IUINT32 next = 0;
	IINT64 sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
//	ikcp_wndsize(kcp1, 128, 128);
//	ikcp_wndsize(kcp2, 128, 128);

	// 判断测试用例的模式
//	if (mode == 0) {
//		// 默认模式
//		ikcp_nodelay(kcp1, 0, 10, 0, 0);
//		ikcp_nodelay(kcp2, 0, 10, 0, 0);
//	}
//	else if (mode == 1) {
//		// 普通模式，关闭流控等
//		ikcp_nodelay(kcp1, 0, 10, 0, 1);
//		ikcp_nodelay(kcp2, 0, 10, 0, 1);
//	}	else {
//		// 启动快速模式
//		// 第二个参数 nodelay-启用以后若干常规加速将启动
//		// 第三个参数 interval为内部处理时钟，默认设置为 10ms
//		// 第四个参数 resend为快速重传指标，设置为2
//		// 第五个参数 为是否禁用常规流控，这里禁止
//		ikcp_nodelay(kcp1, 1, 10, 2, 1);
//		ikcp_nodelay(kcp2, 1, 10, 2, 1);
//		kcp1->rx_minrto = 10;
//		kcp1->fastresend = 1;
//	}


	char buffer[2000];
	int hr;
	IUINT32 cnt = 0;

	IUINT32 ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
//		fprintf(stderr, "nmq_update 1: ---------------------------------\n");
		nmq_update(kcp1, iclock());

//		fprintf(stderr, "nmq_update 2: ---------------------------------\n");
		nmq_update(kcp2, iclock());

//		fprintf(stderr, "peer 1 nmq_send: ---------------------------------\n");
		// 每隔 20ms，kcp1发送数据
//		for (int i = 0; (i < 2) && (cnt < MAX_SN); i++) {
//			((IUINT32*)buffer)[0] = cnt;
//			((IUINT32*)buffer)[1] = current;
//
////            fprintf(stderr, "current: %u, slap: %u, index: %d\n", current, slap, index);
//			// 发送上层协议包
////            fprintf(stderr, "peer 1, nmq_send,\n");
////            IINT32 ret = nmq_send(kcp1, buffer, 8); caution
//            IINT32 ret = nmq_send(kcp1, buffer, 8);
//
////            nmq_update(kcp1, iclock());
////            nmq_update(kcp2, iclock());
//			if (ret > 0) {
//				 cnt++;
//			 } else if (ret < 0) {
//				 break;
//			 }
//		}

        for (; current >= slap; slap += 20) {
            ((IUINT32*)buffer)[0] = index++;
            ((IUINT32*)buffer)[1] = current;

            // 发送上层协议包
            nmq_send(kcp1, buffer, 8);
        }

//		fprintf(stderr, "peer 1 -> 2 nmq_input: ---------------------------------\n");
		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(2, buffer, 2000);
//            fprintf(stderr, "peer 2. read buf to peer 2. nmq_input, hr: %d\n", hr);
            if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			nmq_input(kcp2, buffer, hr);
//            nmq_update(kcp1, iclock());
//            nmq_update(kcp2, iclock());
		}

//		fprintf(stderr, "peer 2 -> 1 nmq_input: ---------------------------------\n");
		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
            hr = vnet->recv(1, buffer, 2000);
//            fprintf(stderr, "peer 1. read buf to peer 1. nmq_input, hr: %d\n", hr);
            if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
            IINT32 ret =  nmq_input(kcp1, buffer, hr);
//            nmq_update(kcp1, iclock());
//            nmq_update(kcp2, iclock());
//            fprintf(stderr, "peer: %lu, nmq_input: ret %d\n", (long)kcp1->arg, ret);
		}

//		fprintf(stderr, "peer 2 send: ---------------------------------\n");
		// kcp2接收到任何包都返回回去
		while (1) {
			hr = nmq_recv(kcp2, buffer, 2000);
//            fprintf(stderr, "peer 2. nmq_recv, hr: %d\n", hr);
            // 没有收到包就退出
			if (hr < 0) {
                break;
            }
//            fprintf(stderr, "peer 2. nmq_send\n");
			// 如果收到包就回射
            nmq_send(kcp2, buffer, hr);     // if error occurs, then data will be lost.
//            nmq_update(kcp1, iclock());
//            nmq_update(kcp2, iclock());
		}

//		fprintf(stderr, "peer 1 output: ---------------------------------\n");
		// kcp1收到kcp2的回射数据
		while (1) {
			hr = nmq_recv(kcp1, buffer, 2000);
//            fprintf(stderr, "peer 1. nmq_recv, hr: %d\n", hr);
			// 没有收到包就退出
			if (hr < 0) break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT32 rtt = current - ts;

			fprintf(stderr, "peer: %d, outer receive sn: %u, ts: %u\n", 1, sn, ts);
			
			if (sn != next) {
				// 如果收到的包不连续
				fprintf(stderr, "ERROR sn %d<->%d\n", (int)sn, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

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
//	printf("press enter to next ...\n");
//	char ch; scanf("%c", &ch);
}

int main()
{
//	segment *s = nmq_new_segment(0);
//	dlnode *dlnode1 = &s->head;
//	IINT32 *rot = &s->rto;
//	printf("real offset: %lu\n", (char*)rot - (char*)s);
//	segment *s2 = ADDRESS_FOR(segment, rto, rot);
//	fprintf(stderr, "s: %p, node: %p, s2: %p, OFFSET: %d\n ", s, dlnode1, s2, OFFSETOF(segment, head));
//	dlist list;
//    dlist_init(&list);
//    for (IUINT32 i = 0; i < 10; i++) {
//        segment *s = nmq_new_segment(0);
//        s->sn = i;
//        dlist_add_after(&list, &s->head);
//    }
//
//    dlnode *node, *nxt;
//    FOR_EACH(node, nxt, &list) {
//        segment *s = ADDRESS_FOR(segment, head, node);
//        fprintf(stderr, "sn: %d\n", s->sn);
//        if (s->sn % 2) {
//            dlist_remove_node(node);
//            delete_segment(s);
//        }
//    }
//
//    fprintf(stderr, "\n");
//    FOR_EACH(node, nxt, &list) {
//        segment *s = ADDRESS_FOR(segment, head, node);
//        fprintf(stderr, "sn: %d\n", s->sn);
//    }
    close(2);
//    dup(1);
//    char buf[100000] = {0};
//    setvbuf(stderr, buf, _IOFBF, 100000);

	test(0);	// 默认模式，类似 TCP：正常模式，无快速重传，常规流控
//	test(1);	// 普通模式，关闭流控等
//	test(2);	// 快速模式，所有开关都打开，且关闭流控
//    fclose(stderr);
	return 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/

