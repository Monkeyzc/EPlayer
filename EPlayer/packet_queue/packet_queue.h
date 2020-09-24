//
//  packet_queue.h
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#pragma once

#include "SDL.h"

#include "avdevice.h"
#include "avformat.h"
#include "avcodec.h"
#include "swresample.h"
#include "avutil.h"
#include "swscale.h"
#include "imgutils.h"
#include "avfilter.h"
#include "buffersink.h"
#include "buffersrc.h"

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;


typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;


int packet_queue_init(PacketQueue *pkt_q);

void packet_queue_start(PacketQueue *q, AVPacket *flush_pkt);

int packet_queue_put_private(PacketQueue *q, AVPacket *pkt, AVPacket *flush_pkt);

int packet_queue_put(PacketQueue *q, AVPacket *pkt, AVPacket *flush_pkt);

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

void packet_queue_flush(PacketQueue *q);

