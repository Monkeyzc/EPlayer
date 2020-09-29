//
//  dumex.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "dumex.h"

static int stream_component_open(PlayState *is, int stream_index) {
    int ret = 0;
    AVFormatContext *fmt_ctx = is->fmt_ctx;
    AVCodecContext *codec_ctx = NULL;
    AVCodec *codec = NULL;
    
    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        return AVERROR(ENOMEM);
    }
    
    AVStream *stream = fmt_ctx->streams[stream_index];
    //av_log(NULL, AV_LOG_DEBUG, "num: %d, den: %d\n", stream->time_base.num, stream->time_base.den);
    ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    if (ret < 0) {
        //av_log(NULL, AV_LOG_FATAL, "Fail avcodec_parameters_to_context");
        return ret;
    }
    
    codec_ctx->pkt_timebase = is->fmt_ctx->streams[stream_index]->time_base;
    codec = avcodec_find_decoder(codec_ctx->codec_id);
    if (!codec) {
        //av_log(NULL, AV_LOG_FATAL, "Could not find decoder by id: %d", codec_ctx->codec_id);
        return -1;
    }
    
    // 打开解码器
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        //av_log(NULL, AV_LOG_ERROR, "avcodec_open2 failed\n");
        return -1;
    };
    
    switch (codec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO: {
            is->audio_decoder.codec_ctx = codec_ctx;
            is->audio_stream = is->fmt_ctx->streams[stream_index];
            
            // prepare audio ouput
            if ((ret = audio_open(is, codec_ctx->channel_layout, codec_ctx->channels, codec_ctx->sample_rate, &is->audio_tgt)) < 0) {
                return -1;
            }
            is->audio_hw_buf_size = ret;
            decoder_init(&is->audio_decoder, codec_ctx, &is->audio_packet_queue, is->continue_read_thread);
            // 开始解码
            if (decoder_start(&is->audio_decoder, audio_thread, "audio_decoder", is) < 0) {
                goto __OUT;
            };
            SDL_PauseAudio(0);
        }
            break;
        case AVMEDIA_TYPE_VIDEO: {
            is->video_decoder.codec_ctx = codec_ctx;
            decoder_init(&is->video_decoder, codec_ctx, &is->video_packet_queue, is->continue_read_thread);
            if (decoder_start(&is->video_decoder, video_thread, "video_thread", is) < 0) {
                goto __OUT;
            }
        }
            break;
        default:
            break;
    }
__OUT:
    return 0;
}


static int read_thread(void *arg) {
    PlayState *is = arg;
    //av_log(NULL, AV_LOG_DEBUG, "read_thread: %p\n", is);
    
    int ret = 0;
    
    AVFormatContext *fmt_ctx;
    AVPacket pkt1, *pkt = &pkt1;
    
    avformat_network_init();
    
    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        //av_log(NULL, AV_LOG_FATAL, "Could not alloc AVFrameContext\n");
        ret = AVERROR(ENOMEM);
        goto __Fail;
    }
    is->fmt_ctx = fmt_ctx;
    
    ret = avformat_open_input(&fmt_ctx, is->filename, is->input_fmt, NULL);
    if (ret < 0) {
        char error[1024] = {0, };
        av_strerror(ret, error, 1024);
        //av_log(NULL, AV_LOG_FATAL, "Could not open avformat_open_input: %s\n", error);
        goto __Fail;
    }
    
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        //av_log(NULL, AV_LOG_FATAL, "Could not open avformat_open_input\n");
        goto __Fail;
    }
    
    is->max_frame_duration = (is->fmt_ctx->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
//    is->max_frame_duration = 10.0;
    // audio
    int auido_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    is->auido_stream_index = auido_stream_index;
    
    // video
    int video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    is->video_stream_index = video_stream_index;
    
    
    if (is->auido_stream_index >= 0) {
        stream_component_open(is, is->auido_stream_index);
    }
    
    if (is->video_stream_index >= 0) {
        stream_component_open(is, is->video_stream_index);
    }
    
    if (!pkt) {
        //av_log(NULL, AV_LOG_FATAL, "Could not alloc packet\n");
        goto __Fail;
    }
    
    while (1) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                break;
            } else {
                //av_log(NULL, AV_LOG_FATAL, "av_read_frame error\n");
                goto __Fail;
            }
        } else {
            // 将 packet 写入到 audio_packet_queue中
            
            if (pkt->stream_index == is->auido_stream_index) {
                packet_queue_put(&is->audio_packet_queue, pkt, &flush_pkt);
            }
            else if (pkt->stream_index == is->video_stream_index) {
                packet_queue_put(&is->video_packet_queue, pkt, &flush_pkt);
            }
        }
    }
    
__Fail:
    if (fmt_ctx) {
        avformat_close_input(&fmt_ctx);
    }
    return ret;
}


PlayState *stream_open(const char *filename, AVInputFormat *input_fmt) {
    PlayState *is;
    
    is = av_mallocz(sizeof(PlayState));
    if (!is) {
        return NULL;
    }
    
    is->auido_stream_index = -1;
    is->video_stream_index = -1;
    is->filename = av_strdup(filename);
    is->input_fmt = input_fmt;
    
    // Frame Queue
    if (frame_queue_init(&is->audio_frame_queue, &is->audio_packet_queue, SAMPLE_QUEUE_SIZE, 1) < 0) {
        return NULL;
    }
    is->audio_frame_queue.type = 1;
    
    if (frame_queue_init(&is->video_frame_queue, &is->video_packet_queue, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0) {
        return NULL;
    }
    is->video_frame_queue.type = 2;
    
    // Packet Queue
    if (packet_queue_init(&is->audio_packet_queue) < 0) {
        return NULL;
    }
    
    if (packet_queue_init(&is->video_packet_queue) < 0) {
        return NULL;
    }
    
    if ((is->continue_read_thread = SDL_CreateCond()) == NULL) {
        //av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond error: %s\n", SDL_GetError());
        return NULL;
    }
    
    // Clock
    clock_init(&is->audio_clock, &is->audio_packet_queue.serial);
    clock_init(&is->video_clock, &is->video_packet_queue.serial);
    is->audio_clock_serial = -1;
    
    // 音量
    if (startup_volume < 0) {
        //av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, settting to 0\n", startup_volume);
    }
    if (startup_volume > 100) {
        //av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    }
    startup_volume = av_clip_c(startup_volume, 0, 100);
    startup_volume = av_clip_c(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = startup_volume;
    
    // read_thread
    is->read_tid = SDL_CreateThread(read_thread, filename, is);
    
    return is;
}
