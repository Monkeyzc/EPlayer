//
//  video.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "video.h"
#include "libavutil/time.h"
#include "../clock/clock.h"

static double compute_target_delay(double duration, PlayState *is) {
    
    double delay = duration;
    double sync_threshold = 0.0;
    double diff = 0.0;
    
    // 视频时钟 与 音频时钟 差值
    
    double video_c = get_clock(&is->video_clock);
    double audio_c = get_clock(&is->audio_clock);
    //av_log(NULL, AV_LOG_DEBUG, "video_c: %f, audio_c: %f\n", video_c, audio_c);
    
    diff = get_clock(&is->video_clock) - get_clock(&is->audio_clock);
//    is->video_clock.pts - is->audio_clock.pts;
//    //av_log(NULL, AV_LOG_DEBUG, "diff: %f\n", diff);
    
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, duration));
    
    double max_frame_duration = is->max_frame_duration;
    
    if (!isnan(diff) && fabs(diff) < max_frame_duration) {
        
        if (diff <= -sync_threshold) {
            delay = FFMAX(0, delay + diff);
        }
        else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
            delay = delay + diff;
        }
        else if (diff >= sync_threshold) {
            delay = 2 * delay;
        }
    }
    
    //av_log(NULL, AV_LOG_DEBUG, "video: delay=%0.3f A-V=%f\n", delay, -diff);
    return delay;
}

static double vp_duration(PlayState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration) {
            return vp->duration;
        } else {
            return duration;
        }
        
    } else {
        return 0.0;
    }
}

int video_thread(void *arg) {
    
    int ret = 0;
    PlayState *is = arg;
    AVFrame *frame = av_frame_alloc();
    Frame *vp_frame;
    
    AVRational tb = is->fmt_ctx->streams[is->video_stream_index]->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->fmt_ctx, is->fmt_ctx->streams[is->video_stream_index], NULL);
    
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    
    do {
        ret = decoder_decode_frame(&is->video_decoder, frame, NULL);
        if (ret < 0) {
            return ret;
        }

        if (ret) {
            while (frame->width) {
                
                if (!is->width) {
                    is->width = frame->width;
                    is->height = frame->height;
                }
                
                if (!(vp_frame = frame_queue_peek_writable(&is->video_frame_queue))) {
                    return -1;
                }
                
                vp_frame->serial = is->video_decoder.pkt_serial;
                
                double duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
                vp_frame->duration = duration;
                
                vp_frame->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                
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
    
//    av_log(NULL, AV_LOG_DEBUG, "remaining_time: %f\n", *remaining_time);
    if (*remaining_time > 0.0) {
        //av_log(NULL, AV_LOG_DEBUG, "av_usleep: %lld\n", (int64_t)(*remaining_time * 1000000.0));
//        av_usleep((unsigned int)(*remaining_time * 1000000.0));
    }
    *remaining_time = 0.01;
    Frame *last_vp;
    Frame *vp;
    
    AVFrame *av_frame;
    
    Frame *will_vp;
    
    double time;
    
    int video_frame_remaining_count;
    
__retry:
    
    video_frame_remaining_count = frame_queue_nb_remaining(&is->video_frame_queue);
    //av_log(NULL, AV_LOG_DEBUG, "video_frame_remaining_count: %d\n", video_frame_remaining_count);
    
    if (video_frame_remaining_count == 0) {
        // nothing to do
        // 继续显示上一帧
        //av_log(NULL, AV_LOG_DEBUG, "video_frame_queue is empty 继续显示上一帧\n");
    } else {
        
        double last_duration, duration, delay;
        
        // 上一帧
        last_vp = frame_queue_peek_last(&is->video_frame_queue);
        // 即将显示的帧
        vp = frame_queue_peek(&is->video_frame_queue);
        
        //av_log(NULL, AV_LOG_DEBUG, "last_vp: %p\n", last_vp);
        //av_log(NULL, AV_LOG_DEBUG, "vp: %p\n", vp);

        // 即将显示的帧 与 视频packet 序列不一致, 表示发生过 seek, 丢掉 last_vp 帧
        if (vp->serial != is->video_packet_queue.serial) {
            //av_log(NULL, AV_LOG_DEBUG, "video_refresh retry\n");
            frame_queue_next(&is->video_frame_queue);
            goto __retry;
        }
        
        //
        if (last_vp->serial != vp->serial) {
            is->frame_timer = av_gettime_relative();
        }
        
        // 计算 上一帧的 显示时长 delay
        // last_duration
        last_duration = vp_duration(is, last_vp, vp);
        // delay
        delay = compute_target_delay(last_duration, is);
        
        time = av_gettime_relative() / 1000000.0;
        
//        //av_log(NULL, AV_LOG_DEBUG, "time: %f\n", time);
//        //av_log(NULL, AV_LOG_DEBUG, "is->frame_timer: %f\n", is->frame_timer);
//        //av_log(NULL, AV_LOG_DEBUG, "is->frame_timer + delay: %f\n", is->frame_timer + delay);
        
        // is->frame_timer 帧显示时刻
        if (time < is->frame_timer + delay) {
            // 系统时间还未到达, 上一帧的结束时刻, 继续 显示 上一帧
            double result = FFMIN(is->frame_timer + delay - time, *remaining_time);
//            av_log(NULL, AV_LOG_DEBUG, "FFMIN result: %f\n", result);
            *remaining_time = result;
            // goto display
            //av_log(NULL, AV_LOG_DEBUG, "goto __display 系统时间还未到达, 上一帧的结束时刻, 继续 显示 上一帧 time < is->frame_timer + delay: %f < %f + %f = %f\n", time, is->frame_timer, delay, is->frame_timer + delay);
//            //av_log(NULL, AV_LOG_DEBUG, "goto __display 系统时间还未到达, 上一帧的结束时刻, 继续 显示 上一帧\n");
            goto __display;
        }
        
        // 更新frame_timer, 表示 vp 的 显示时刻
        is->frame_timer += delay;
        //av_log(NULL, AV_LOG_DEBUG, "更新frame_timer, 表示 vp 的 显示时刻: is->frame_time = %f\n", is->frame_timer);
        if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
            // 如果和系统时间差距过大, 则更新为 系统时间
            is->frame_timer = time;
            //av_log(NULL, AV_LOG_DEBUG, "如果和系统时间差距过大, 则更新为 系统时间: is->frame_time = %f\n", is->frame_timer);
        }
        
        SDL_LockMutex(is->video_frame_queue.mutex);
        if (!isnan(vp->pts)) {
            //av_log(NULL, AV_LOG_DEBUG, "update video clock pts 更新 视频时钟\n");
            // update video clock pts
            set_clock(&is->video_clock, vp->pts, vp->serial);
            // 外部时钟
//            sync_clock_to_slave(<#Clock *c#>, <#Clock *slave#>)
        }
        SDL_UnlockMutex(is->video_frame_queue.mutex);
        
        
        
        if (frame_queue_nb_remaining(&is->video_frame_queue) > 1) {
            // 只有有nextvp才会丢帧
            Frame *nextvp = frame_queue_peek_next(&is->video_frame_queue);
            duration = vp_duration(is, vp, nextvp);
            if (time > is->frame_timer + duration) {
                // 丢帧
                frame_queue_next(&is->video_frame_queue);
                //av_log(NULL, AV_LOG_DEBUG, "丢帧\n");
                goto __retry;
            }
        }
        
        frame_queue_next(&is->video_frame_queue);
        
 __display:
        
        will_vp = frame_queue_peek_last(&is->video_frame_queue);
        //av_log(NULL, AV_LOG_DEBUG, "展示 视频 帧 will_vp: %p\n", will_vp);
        av_frame = will_vp->frame;

        enum AVPictureType pict_type = av_frame->pict_type;
        
        switch (pict_type) {
            case AV_PICTURE_TYPE_NONE:
                //av_log(NULL, AV_LOG_DEBUG, "pict_type: 未知\n");
                break;
            case AV_PICTURE_TYPE_I:
                //av_log(NULL, AV_LOG_DEBUG, "pict_type: I 帧\n");
                break;
            case AV_PICTURE_TYPE_B:
                //av_log(NULL, AV_LOG_DEBUG, "pict_type: B 帧\n");
                break;
            case AV_PICTURE_TYPE_P:
                //av_log(NULL, AV_LOG_DEBUG, "pict_type: P 帧\n");
                break;
            default:
                break;
        }
//        //av_log(NULL, AV_LOG_DEBUG, "display_picture_number: %d\n", av_frame->display_picture_number);
//        //av_log(NULL, AV_LOG_DEBUG, "pkt_dts: %lld\n", av_frame->pkt_dts);
//        //av_log(NULL, AV_LOG_DEBUG, "pts: %lld\n", av_frame->pts);
        
        if (pict_type == AV_PICTURE_TYPE_NONE) {
            return -1;
        }
        
        // 视频缩放 色值转换
        if (!is->sws_scaler_ctx) {
            is->sws_scaler_ctx = sws_getContext(av_frame->width, av_frame->height, is->video_decoder.codec_ctx->pix_fmt,
                                                av_frame->width, av_frame->height, AV_PIX_FMT_RGB0,
                                                SWS_BILINEAR, NULL, NULL, NULL);
        }
        
        if (!is->sws_scaler_ctx) {
            //av_log(NULL, AV_LOG_ERROR, "iss->sws_scaler_ctx is NUll\n");
            return -1;
        }
        
        uint8_t* dest[4] = {frame_buffer, NULL, NULL, NULL};
        int dest_linesize[4] = {av_frame->width * 4, 0, 0, 0};
        if(sws_scale(is->sws_scaler_ctx, (const uint8_t * const *)av_frame->data, av_frame->linesize, 0, av_frame->height, dest, dest_linesize) >= 0) {
            //av_log(NULL, AV_LOG_DEBUG, "sws_scale success\n");
            return 0;
        } else {
            //av_log(NULL, AV_LOG_DEBUG, "sws_scale failed\n");
            return -1;
        }
    }
    return 0;
}
