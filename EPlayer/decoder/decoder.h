//
//  decoder.h
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#pragma once

#include <stdio.h>

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

#include "../packet_queue/packet_queue.h"
#include "../Global/Global.h"

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *pkt_queue;
    AVCodecContext *codec_ctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    
    SDL_cond *empty_queue_cond;
    SDL_Thread *decoder_tid;
    
} Decoder;

void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);

int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg);

int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
