//
//  packet_queue.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "packet_queue.h"

int packet_queue_init(PacketQueue *pkt_q) {
    memset(pkt_q, 0, sizeof(PacketQueue));

    if ((pkt_q->mutex = SDL_CreateMutex()) == NULL) {
        //av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex error: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    if ((pkt_q->cond = SDL_CreateCond()) == NULL) {
        //av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond error: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

void packet_queue_start(PacketQueue *q, AVPacket *flush_pkt) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, flush_pkt, flush_pkt);
    SDL_UnlockMutex(q->mutex);
}


int packet_queue_put_private(PacketQueue *q, AVPacket *pkt, AVPacket *flush_pkt) {

    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;
    // 将 pkt 封装成 MyAVPacketList
    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    // 判断队队列是否是空的
    if (!q->last_pkt)
        // 空的, 设置 first_pkt
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    // 通知 读取线程 队列里有数据了
    SDL_CondSignal(q->cond);
    return 0;
}

/**
    将packet 写入队列
 */
int packet_queue_put(PacketQueue *q, AVPacket *pkt, AVPacket *flush_pkt) {
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt, flush_pkt);
    SDL_UnlockMutex(q->mutex);

    if (ret < 0) {
        av_packet_unref(pkt);
    }

    if (pkt != flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}


/**
 return < 0 if aborted, 0 if no packet and > 0 if packet.
 读取packet
 block 为1 阻塞线程读取, 0 非阻塞读取
 */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            // 移动 first_pkt 和 last_pkt 指针地址
            q->first_pkt = pkt1->next;
            // 没有 packet 了
            if (!q->first_pkt)
                q->last_pkt = NULL;

            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            // *pkt  浅拷贝 pkt1->pkt 指针
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            // 释放pkt1 内存
            av_free(pkt1);
            ret = 1;
            // 退出循环
            break;
        } else if (!block) {
            // 非阻塞读取, 直接退出循环
            ret = 0;
            break;
        } else {
            // 阻塞读取, 等待 pakcet 队列写入数据
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/**
    清空 packet queue
 */
void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}
