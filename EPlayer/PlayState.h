//
//  PlayState.h
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#pragma once

#include "packet_queue/packet_queue.h"
#include "frame_queue/frame_queue.h"
#include "clock/clock.h"
#include "decoder/decoder.h"

typedef struct AudioParams {
    int freq;                   // 采样率
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;             // 每个采样的大小
    int bytes_per_sec;          // 每秒播放采样大小
} AudioParams;

typedef struct PlayState {
    
    char *filename;
    
    AVFormatContext *fmt_ctx;
    AVInputFormat *input_fmt;
    
    // audio
    int auido_stream_index;
    AVStream *audio_stream;
    Decoder audio_decoder;
    PacketQueue audio_packet_queue;
    FrameQueue audio_frame_queue;
    Clock audio_clock;
    int audio_clock_serial;
    int audio_volume;
    double audio_pts_c;
    
    int audio_hw_buf_size;
    
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    
    unsigned int audio_buf_size;    /* in bytes */
    unsigned int audio_buf_size1;
    
    int audio_buf_index;            /* in bytes */
    int audio_write_buf_size;
    
    struct SwrContext *swr_ctx;
    
    AudioParams audio_tgt;
    
    // video
    int video_stream_index;
    AVStream *video_stream;
    Decoder video_decoder;
    PacketQueue video_packet_queue;
    FrameQueue video_frame_queue;
    Clock video_clock;
    
    int width, height;
    struct SwsContext *sws_scaler_ctx;
    
    // SDL
    SDL_Thread *read_tid;
    SDL_cond *continue_read_thread;
    
    double max_frame_duration;
    double frame_timer;
    
} PlayState;
