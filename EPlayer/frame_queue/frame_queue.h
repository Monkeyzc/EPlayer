//
//  frame_queue.h
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

#include "../packet_queue/packet_queue.h"

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct Frame {
    AVFrame *frame;
    int serial;
    double pts;             /* presentation timestamp for the frame */
    double duration;
    int64_t pos;            /* byte position of the frame in the input file */
    
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int read_index;
    int write_index;
    
    int size;               // 总帧数
    int max_size;           // 队列可存储最大帧数
    int keep_last;
    int rindex_shown;       // 当前是否有帧在显示
    
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pkt_queue;
    
} FrameQueue;


int frame_queue_init(FrameQueue *f_q, PacketQueue *pkt_q, int max_size, int keep_last);

void frame_queue_unref_item(Frame *vp);

Frame *frame_queue_peek_writable(FrameQueue *f);
void frame_queue_push(FrameQueue *f);


Frame *frame_queue_peek_readable(FrameQueue *f);
void frame_queue_next(FrameQueue *f);
Frame *frame_queue_peek_last(FrameQueue *f);

int frame_queue_nb_remaining(FrameQueue *f);


