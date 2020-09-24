//
//  video.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "video.h"

int video_thread(void *arg) {
    
    int ret = 0;
    PlayState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *vp_frame;
    
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    
    do {
        ret = decoder_decode_frame(&is->video_decoder, frame, NULL);
        if (ret < 0) {
            return ret;
        }
//        av_log(NULL, AV_LOG_INFO, "Video frame width: %d, height: %d\n", frame->width, frame->height);
        if (ret) {
            while (frame->width) {
                if (!(vp_frame = frame_queue_peek_writable(&is->video_frame_queue))) {
                    return -1;
                }

                vp_frame->serial = is->video_decoder.pkt_serial;
                av_frame_move_ref(vp_frame->frame, frame);

                // 写入 frame queue
                frame_queue_push(&is->video_frame_queue);
                if (is->video_packet_queue.serial != is->video_decoder.pkt_serial) {
                    break;
                }
            }
        }
        
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
      
    av_frame_free(&frame);
    return 0;
}

int video_refresh(PlayState *is, double *remaining_time, uint8_t *frame_buffer) {
    
    Frame *vp;
    AVFrame *av_frame;
    
    if (frame_queue_nb_remaining(&is->video_frame_queue) == 0) {
        // nothing to do
        av_log(NULL, AV_LOG_DEBUG, "video_frame_queue is empty\n");
        return -1;
    } else {
        do {
            if (!(vp = frame_queue_peek_readable(&is->video_frame_queue))) {
                av_log(NULL, AV_LOG_ERROR, "video frame_queue_peek_readable is NULL\n");
                return -1;
            }
            frame_queue_next(&is->video_frame_queue);
        } while (vp->serial != is->video_packet_queue.serial);
        
        av_frame = vp->frame;
        enum AVPictureType pict_type = av_frame->pict_type;
        
        switch (pict_type) {
            case AV_PICTURE_TYPE_NONE:
                av_log(NULL, AV_LOG_DEBUG, "pict_type: 未知\n");
                break;
            case AV_PICTURE_TYPE_I:
                av_log(NULL, AV_LOG_DEBUG, "pict_type: I 帧\n");
                break;
            case AV_PICTURE_TYPE_B:
                av_log(NULL, AV_LOG_DEBUG, "pict_type: B 帧\n");
                break;
            case AV_PICTURE_TYPE_P:
                av_log(NULL, AV_LOG_DEBUG, "pict_type: P 帧\n");
                break;
            default:
                break;
        }
        av_log(NULL, AV_LOG_DEBUG, "display_picture_number: %d\n", av_frame->display_picture_number);
        av_log(NULL, AV_LOG_DEBUG, "pkt_dts: %lld\n", av_frame->pkt_dts);
        av_log(NULL, AV_LOG_DEBUG, "pts: %lld\n", av_frame->pts);
        
        if (pict_type == AV_PICTURE_TYPE_NONE) {
            return -1;
        }
        
        if (!is->sws_scaler_ctx) {
            is->sws_scaler_ctx = sws_getContext(av_frame->width, av_frame->height, is->video_decoder.codec_ctx->pix_fmt,
                                                av_frame->width, av_frame->height, AV_PIX_FMT_RGB0,
                                                SWS_BILINEAR, NULL, NULL, NULL);
        }
        
        if (!is->sws_scaler_ctx) {
            av_log(NULL, AV_LOG_ERROR, "iss->sws_scaler_ctx is NUll\n");
            return -1;
        }
        
        uint8_t* dest[4] = {frame_buffer, NULL, NULL, NULL};
        int dest_linesize[4] = {av_frame->width * 4, 0, 0, 0};
        if(sws_scale(is->sws_scaler_ctx, (const uint8_t * const *)av_frame->data, av_frame->linesize, 0, av_frame->height, dest, dest_linesize) >= 0) {
            av_log(NULL, AV_LOG_DEBUG, "sws_scale success\n");
            return 0;
        } else {
            av_log(NULL, AV_LOG_DEBUG, "sws_scale failed\n");
            return -1;
        }
    }
    return 0;
}
