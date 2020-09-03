//
//  main.c
//  EPlayer
//
//  Created by zhaofei on 2020/8/21.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include <stdio.h>
#include <stdbool.h>
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

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

const char *out_file_name = "/Users/zhaofei/Downloads/test_record_audio_aac_decode.pcm";
FILE *outfile = NULL;

int startup_volume = 100;
static AVPacket flush_pkt;
static SDL_AudioDeviceID audio_device_id;

typedef struct AudioParams {
    int freq;                   // 采样率
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;             // 每个采样的大小
    int bytes_per_sec;          // 每秒播放采样大小
} AudioParams;

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


typedef struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    int *queue_serial;
} Clock;


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

typedef struct PlayState {
    
    char *filename;
    
    AVFormatContext *fmt_ctx;
    AVInputFormat *input_fmt;
    
    // audio
    int auido_stream_index;
    Decoder audio_decoder;
    PacketQueue audio_packet_queue;
    FrameQueue audio_frame_queue;
    Clock audio_clock;
    int audio_clock_serial;
    int audio_volume;
    
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    
    unsigned int audio_buf_size;    /* in bytes */
    unsigned int audio_buf_size1;
    
    int audio_buf_index;            /* in bytes */
    int auido_write_but_size;
    
    struct SwrContext *swr_ctx;
    
    AudioParams audio_tgt;
    
    // video
    int video_stream_index;
    Decoder video_decoder;
    PacketQueue video_packet_queue;
    FrameQueue video_frame_queue;
    Clock video_clock;
    
    // SDL
    SDL_Thread *read_tid;
    SDL_cond *continue_read_thread;
    
} PlayState;

void set_av_log_level_and_check_ffmpeg_version() {
    av_log_set_level(AV_LOG_DEBUG);
    const char *version_info = av_version_info();
    av_log(NULL, AV_LOG_INFO, "FFmpeg version info: %s\n", version_info);
}

void check_sdl_version() {
    // SDL
    SDL_version compiled;
    SDL_version linked;
    
    SDL_VERSION(&compiled);
    SDL_GetVersion(&linked);
    
    SDL_Log("We compiled against SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
    SDL_Log("But we are linking against SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
}

#pragma mark - Frame queue
static int frame_queue_init(FrameQueue *f_q, PacketQueue *pkt_q, int max_size, int keep_last) {
    
    memset(f_q, 0, sizeof(FrameQueue));
    
    if ((f_q->mutex = SDL_CreateMutex()) == NULL) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex error: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    if ((f_q->cond = SDL_CreateCond()) == NULL) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond error: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    f_q->pkt_queue = pkt_q;
    f_q->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f_q->keep_last = !!keep_last;
    
    // 为 FrameQueue 中的每个frame 分配空间
    for (int i = 0; i < f_q->max_size; i++) {
        if ((f_q->queue[i].frame = av_frame_alloc()) == NULL) {
            av_log(NULL, AV_LOG_FATAL, "av_frame_alloc error\n");
            return AVERROR(ENOMEM);
        }
    }
    
    return 0;
}

static void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
}

static Frame *frame_queue_peek_writable(FrameQueue *f) {
    
    SDL_LockMutex(f->mutex);
    if (f->size == f->max_size) {
//        av_log(NULL, AV_LOG_DEBUG, "frame_queue 满了\n");
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);
    
    return &f->queue[f->write_index];
}

static void frame_queue_push(FrameQueue *f) {
    if (++f->write_index == f->max_size) {
        f->write_index = 0;
    }
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek_readable(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);
    return &f->queue[(f->read_index + f->rindex_shown) % f->max_size];
}

static void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->read_index]);
    if (++f->read_index == f->max_size) {
        f->read_index = 0;
    }
    SDL_LockMutex(f->mutex);
    f->size--;
//    av_log(NULL, AV_LOG_DEBUG, "frame_queue_next SDL_CondSignal: 通知 frame queue 空余出空间了\n");
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

#pragma mark - Packet queue
static int packet_queue_init(PacketQueue *pkt_q) {
    
    memset(pkt_q, 0, sizeof(PacketQueue));
    
    if ((pkt_q->mutex = SDL_CreateMutex()) == NULL) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex error: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    if ((pkt_q->cond = SDL_CreateCond()) == NULL) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond error: %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    
    return 0;
}


static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;
    // 将 pkt 封装成 MyAVPacketList
    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
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
 return < 0 if aborted, 0 if no packet and > 0 if packet.
 读取packet
 block 为1 阻塞线程读取, 0 非阻塞读取
 */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
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
static void packet_queue_flush(PacketQueue *q) {
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

/**
    将packet 写入队列
 */
static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);
    
    if (ret < 0) {
        av_packet_unref(pkt);
    }

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

static void packet_queue_start(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

#pragma mark - Clock
static void clock_init(Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
}


#pragma mark - Decoder
static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->codec_ctx = avctx;
    d->pkt_queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

/**
    启动解码
 */
static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg) {
    packet_queue_start(d->pkt_queue);
    // 创建 解码线程
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}


static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    
    int ret = AVERROR(EAGAIN);
    
    for (; ; ) {
        AVPacket pkt;
        
        if (d->pkt_queue->serial == d->pkt_serial) {
            
            do {
                
                // 中止
                if (d->pkt_queue->abort_request) {
                    return -1;
                }
                
                switch (d->codec_ctx->codec_type) {
                    case AVMEDIA_TYPE_AUDIO: {
                        // 音频
                        ret = avcodec_receive_frame(d->codec_ctx, frame);

                        if (ret < 0) {
                            char error[1024] = {0,};
                            av_strerror(ret, error, 1024);
//                            av_log(NULL, AV_LOG_ERROR, "avcodec_receive_frame error: %s\n", error);
                        }
                        
                        if (ret >= 0) {
//                            av_log(NULL, AV_LOG_ERROR, "avcodec_receive_frame frame: %d\n", frame->sample_rate);
                            // frame
                            return 1;
                            // 将 frame 写入到文件中
                            int data_size = av_get_bytes_per_sample(d->codec_ctx->sample_fmt);

                            for (int i = 0; i < frame->nb_samples; i++) { // 每个声道的采样个数
                                for (int ch = 0; ch < d->codec_ctx->channels; ch++) { // 声道数
                                    fwrite(frame->data[ch] + data_size * i, 1, data_size, outfile);
                                    fflush(outfile);
                                }
                            }
                        }
                    }
                        break;
                    default:
                        break;
                }
                
                if (ret == AVERROR_EOF) {
                    // 没有数据了
                    return 0;
                }
                
                if (ret >= 0) {
                    return 1;
                }
                
            } while (ret != AVERROR(EAGAIN));
        }
        
        
        do {
            if (d->pkt_queue->nb_packets == 0) {
                // pkt_queue中没有数据
            }
            
            // 读取 packet
            if (packet_queue_get(d->pkt_queue, &pkt, 1, &d->pkt_serial) < 0) {
                return -1;
            }
            
        } while (d->pkt_queue->serial != d->pkt_serial);
        
        // 发送packet数据包给解码器
        int send_ret = avcodec_send_packet(d->codec_ctx, &pkt);
        if (send_ret == AVERROR(EAGAIN)) {
            av_packet_move_ref(&d->pkt, &pkt);
        }
        if (send_ret < 0) {
            char error[1024] = {0,};
            av_strerror(ret, error, 1024);
            av_log(NULL, AV_LOG_ERROR, "send packet to codec error: %s\n", error);
        }
        av_packet_unref(&pkt);
    }
    
    return 0;
}

/**
    从frame_queue 中取出 frame 数据, 并重采样
 */
static int audio_decode_frame(PlayState *is) {
    
    int data_size, resample_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;
    
    do {
        if (!(af = frame_queue_peek_readable(&is->audio_frame_queue))) {
            return -1;
        }
        frame_queue_next(&is->audio_frame_queue);
    } while (af->serial != is->audio_packet_queue.serial);
    
    data_size = av_samples_get_buffer_size(NULL, af->frame->channels, af->frame->nb_samples, af->frame->format, 1);
    
    // TODO: synchronize
    wanted_nb_samples = af->frame->nb_samples;
    
    
    if (af->frame->format == AV_SAMPLE_FMT_FLTP) {
        av_log(NULL, AV_LOG_DEBUG, "AV_SAMPLE_FMT_FLTP\n");
    }
    
    // 是否需要重采样
    if (!is->swr_ctx) {
        
        is->swr_ctx = swr_alloc_set_opts(is->swr_ctx,
                                         af->frame->channel_layout,
                                         AV_SAMPLE_FMT_S16,
                                         af->frame->sample_rate,
                                         af->frame->channel_layout,
                                         af->frame->format,
                                         af->frame->sample_rate, 0, NULL);
        swr_init(is->swr_ctx);
    }
    
    const uint8_t **in = (const uint8_t **)af->frame->extended_data;
    uint8_t **out = &is->audio_buf1;
    // 重采样后的 nb_samples
    int out_count = wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
    int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
    int len2;
    
    if (wanted_nb_samples != af->frame->nb_samples) {
        if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate, wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
            av_log(NULL, AV_LOG_DEBUG, "swr_set_compensation() failed\n");
            return -1;
        }
    }
    
    av_fast_malloc(&is->audio_buf1, &is->audio_buf_size1, out_size);
    if (!is->audio_buf1) {
        return AVERROR(ENOMEM);
    }
    len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
        av_log(NULL, AV_LOG_ERROR, "swr_convert failed\n");
        return -1;
    }
    
    if (len2 == out_count) {
        av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
    }
    
    is->audio_buf = is->audio_buf1;
    resample_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    
    // 不需要重采样
//    is->audio_buf = af->frame->data[0];
//    resample_data_size = data_size;
//    printf("resample_data_size: %d\n", resample_data_size);
    return resample_data_size;
}

/**
    混音
 */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    PlayState *is = opaque;
    int audio_size, len1;
    
//    av_log(NULL, AV_LOG_DEBUG, "sdl_audio_callback\n");
    
    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            // frame 数据
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /*  error */
                is->audio_buf = NULL;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        
        len1 = is->audio_buf_size - is->audio_buf_index;
        
        if (len1 > len) {
            len1 = len;
        }
        if (is->audio_buf) {
            memset(stream, 0, len1);
//            SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
            SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
        }
        
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->auido_write_but_size = is->auido_write_but_size - is->audio_buf_index;
}

/**
    打开音频设备
 */
static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params) {
    
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};             // ???
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;           // ???
    
    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_DEBUG, "Invalid sample rate or channel count\n");
        return -1;
    }
    
    // ???
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq) {
        next_sample_rate_idx--;
    }
    
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    
    
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
       av_log(NULL, AV_LOG_WARNING, "SDL_Open Audio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        return -1;
    }
    
//    while (!(audio_device_id = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
//        av_log(NULL, AV_LOG_WARNING, "SDL_Open Audio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());
//
//        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
//        if (!wanted_spec.channels) {
//            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
//            wanted_spec.channels = wanted_nb_channels;
//            if (!wanted_spec.freq) {
//                av_log(NULL, AV_LOG_ERROR, "NO more combinations to try, audio open failed\n");
//                return -1;
//            }
//        }
//        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
//    }
//
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR, "SDL advised audio channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }
    
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    
    return spec.size;
}

static int audio_thread(void* arg) {
    PlayState *is = arg;
    int ret = 0;
    av_log(NULL, AV_LOG_DEBUG, "audio_thread arg: %p\n", arg);
    
    AVPacket *pkt;
    pkt = av_packet_alloc();
    
    AVFrame *frame = NULL;
    frame = av_frame_alloc();
    
    Frame *af_frame;
    
    do {
        
        printf("audio_thread do while\n");
        
        ret = decoder_decode_frame(&is->audio_decoder, frame, NULL);
        if (ret < 0) {
            return ret;
        }
        if (ret) {
            while (frame->sample_rate) {
                if (!(af_frame = frame_queue_peek_writable(&is->audio_frame_queue))) {
                    goto __END_audio_thread;
                }
//                printf("=========================??????\n");
                af_frame->serial = is->audio_decoder.pkt_serial;
//                av_log(NULL, AV_LOG_DEBUG, "frame: %d\n", frame->sample_rate);
                av_frame_move_ref(af_frame->frame, frame);
                
//                av_log(NULL, AV_LOG_DEBUG, "af_frame->frame: %d\n", af_frame->frame->sample_rate);
                // 写入 frame queue
                frame_queue_push(&is->audio_frame_queue);
                if (is->audio_packet_queue.serial != is->audio_decoder.pkt_serial) {
                    break;
                }
            }
        }
                
    } while (ret >=0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    
__END_audio_thread:
    av_frame_free(&frame);
    
    return 0;
}

#pragma mark - stream compnent open
static int stream_component_open(PlayState *is, int stream_index) {
    int ret = 0;
    AVFormatContext *fmt_ctx = is->fmt_ctx;
    AVCodecContext *codec_ctx = NULL;
    AVCodec *codec = NULL;
    
    codec_ctx = avcodec_alloc_context3(NULL);
    if (!codec_ctx) {
        return AVERROR(ENOMEM);
    }
    
    ret = avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[stream_index]->codecpar);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Fail avcodec_parameters_to_context");
        return ret;
    }
    is->audio_decoder.codec_ctx = codec_ctx;
    
    codec = avcodec_find_decoder(codec_ctx->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_FATAL, "Could not find decoder by id: %d", codec_ctx->codec_id);
        return -1;
    }
    
    // 打开解码器
    avcodec_open2(is->audio_decoder.codec_ctx, codec, NULL);
    
    // prepare audio ouput
    if ((ret = audio_open(is, codec_ctx->channel_layout, codec_ctx->channels, codec_ctx->sample_rate, &is->audio_tgt)) < 0) {
        return -1;
    }
    
    decoder_init(&is->audio_decoder, codec_ctx, &is->audio_packet_queue, is->continue_read_thread);
    // 开始解码
    decoder_start(&is->audio_decoder, audio_thread, "audio_decoder", is);
    
    SDL_PauseAudio(0);
    
    return 0;
}

/**
    解复用线程, 读取 AVPacket
 */
static int read_thread(void *arg) {
    PlayState *is = arg;
    av_log(NULL, AV_LOG_DEBUG, "read_thread: %p\n", is);
    
    int ret = 0;
    
    AVFormatContext *fmt_ctx;
    AVPacket pkt1, *pkt = &pkt1;
    
    avformat_network_init();
    
    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        av_log(NULL, AV_LOG_FATAL, "Could not alloc AVFrameContext\n");
        ret = AVERROR(ENOMEM);
        goto __Fail;
    }
    is->fmt_ctx = fmt_ctx;
    
    ret = avformat_open_input(&fmt_ctx, is->filename, is->input_fmt, NULL);
    if (ret < 0) {
        char error[1024] = {0, };
        av_strerror(ret, error, 1024);
        av_log(NULL, AV_LOG_FATAL, "Could not open avformat_open_input: %s\n", error);
        goto __Fail;
    }
    
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not open avformat_open_input\n");
        goto __Fail;
    }
    
    int auido_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (auido_stream_index < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could not find audio stream\n");
        goto __Fail;
    }
    is->auido_stream_index = auido_stream_index;
    
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not alloc packet\n");
        goto __Fail;
    }
    
    stream_component_open(is, is->auido_stream_index);
    
    while (1) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                break;
            } else {
                av_log(NULL, AV_LOG_FATAL, "av_read_frame error\n");
                goto __Fail;
            }
        } else {
            av_log(NULL, AV_LOG_DEBUG, "pkt: %p\n", pkt->data);
            av_log(NULL, AV_LOG_DEBUG, "pkt-----: %p\n", pkt);
            
            // 将 packet 写入到 audio_packet_queue中
            packet_queue_put(&is->audio_packet_queue, pkt);
        }
        
    }
    
__Fail:
    if (fmt_ctx) {
        avformat_free_context(fmt_ctx);
    }
    return ret;
}

static PlayState *stream_open(const char *filename, AVInputFormat *input_fmt) {
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
    
    // Packet Queue
    if (packet_queue_init(&is->audio_packet_queue) < 0) {
        return NULL;
    }
    
    if ((is->continue_read_thread = SDL_CreateCond()) == NULL) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond error: %s\n", SDL_GetError());
        return NULL;
    }
    
    // Clock
    clock_init(&is->audio_clock, &is->audio_packet_queue.serial);
    is->audio_clock_serial = -1;
    
    // 音量
    if (startup_volume < 0) {
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, settting to 0\n", startup_volume);
    }
    if (startup_volume > 100) {
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    }
    startup_volume = av_clip_c(startup_volume, 0, 100);
    startup_volume = av_clip_c(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = startup_volume;
    
    
    // read_thread
    is->read_tid = SDL_CreateThread(read_thread, filename, is);
    
    
    return NULL;
}

static void event_loop(PlayState *is) {
    SDL_Event e;
    
    
}

int main(int argc, const char * argv[]) {
    
    outfile = fopen(out_file_name, "wb");
    
    // SDL
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Surface *bmp = NULL;
    SDL_Texture *texture = NULL;
    SDL_Thread *sdl_refresh_thread = NULL;
    
    const char *input_filename = "http://ivi.bupt.edu.cn/hls/cctv1hd.m3u8";
//    "/Users/zhaofei/Desktop/filter.aac";
//    "http://ivi.bupt.edu.cn/hls/cctv1hd.m3u8";
//    "/Users/zhaofei/Desktop/out.mp3";
    
    set_av_log_level_and_check_ffmpeg_version();
    check_sdl_version();
    
    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;
    
    PlayState *is;
    is = stream_open(input_filename, NULL);
    
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SDL_Log("Unable to initialize SDL: %s", SDL_GetError());
        goto __Destroy;
    }
    
    // 窗口
    window = SDL_CreateWindow("Hello world", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    
    if (!window) {
        SDL_Log("Could not create window: %s", SDL_GetError());
        goto __Destroy;
    }
    
    /**
     渲染器
     index: -1, SDL自动选择适合我们指定的选项的驱动
     flag:
        SDL_RENDERER_ACCELERATED: 硬件加速的renderer
        SDL_RENDERER_PRESENTVSYNC: 显示器的刷新率来更新画面
     */
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!renderer) {
        SDL_Log("Could not create renderer: %s", SDL_GetError());
        goto __Destroy;
    }


    bmp = SDL_LoadBMP("/Users/zhaofei/Desktop/1598025169612.bmp");
    if (!bmp) {
        SDL_Log("load bmp fail: %s", SDL_GetError());
        goto __Destroy;
    }

    texture = SDL_CreateTextureFromSurface(renderer, bmp);
    
    SDL_FreeSurface(bmp);
    bmp = NULL;
    
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    
    
    SDL_Event e;
    int quit = 0;
    while (!quit){
        while (SDL_PollEvent(&e)){
            if (e.type == SDL_QUIT){
                quit = 1;
                goto __Destroy;
                break;
            }
        }
    }
    
    
__Destroy:
    
    if (outfile) {
        fclose(outfile);
    }
    
    if (bmp) {
        SDL_FreeSurface(bmp);
    }
    
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    
    return 0;
}
