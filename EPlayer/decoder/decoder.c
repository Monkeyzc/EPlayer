//
//  decoder.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "decoder.h"

void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
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
int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg) {
    packet_queue_start(d->pkt_queue, &flush_pkt);
    // 创建 解码线程
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}


int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    
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
                        if (ret == AVERROR_EOF) {
                            // 没有数据了
                            d->finished = d->pkt_serial;
                            avcodec_flush_buffers(d->codec_ctx);
                            return 0;
                        }
                        
                        if (ret >= 0) {
                            return 1;
                        }

                    }
                        break;
                    case AVMEDIA_TYPE_VIDEO: {
                        ret = avcodec_receive_frame(d->codec_ctx, frame);
                        if (ret == AVERROR_EOF) {
                            // 没有数据了
                            d->finished = d->pkt_serial;
                            avcodec_flush_buffers(d->codec_ctx);
                            return 0;
                        }
                        
                        if (ret >= 0) {
                            return 1;
                        }

                    }
                        break;
                    default:
                        break;
                }
            } while (ret != AVERROR(EAGAIN));
        }
        
        
        do {
            if (d->pkt_queue->nb_packets == 0) {
                // pkt_queue中没有数据
                SDL_CondSignal(d->empty_queue_cond);
            }
            
            // 读取 packet
            if (packet_queue_get(d->pkt_queue, &pkt, 1, &d->pkt_serial) < 0) {
                return -1;
            }
            
        } while (d->pkt_queue->serial != d->pkt_serial);
        
        
        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->codec_ctx);
            d->finished = 0;
        } else {
            
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
    }
    
    return 0;
}

