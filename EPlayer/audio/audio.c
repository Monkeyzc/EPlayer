//
//  audio.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "audio.h"
#include "libavutil/time.h"

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
    
    wanted_nb_samples = af->frame->nb_samples;
    
    
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
            //av_log(NULL, AV_LOG_DEBUG, "swr_set_compensation() failed\n");
            return -1;
        }
    }
    
    av_fast_malloc(&is->audio_buf1, &is->audio_buf_size1, out_size);
    if (!is->audio_buf1) {
        return AVERROR(ENOMEM);
    }
    len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
    if (len2 < 0) {
        //av_log(NULL, AV_LOG_ERROR, "swr_convert failed\n");
        return -1;
    }
    
    if (len2 == out_count) {
        //av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
    }
    
    is->audio_buf = is->audio_buf1;
    resample_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    
    /* update the audio clock with the pts */
    if (!isnan(af->pts)) {
        // ???: Why
        is->audio_pts_c = af->pts;
//    + (double) af->frame->nb_samples / af->frame->sample_rate;
    }
    else
        is->audio_pts_c = NAN;
    is->audio_clock_serial = af->serial;
    
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
    
    double audio_callback_time = av_gettime_relative();
    
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
            //av_log(NULL, AV_LOG_DEBUG, "sdl 播放\n");
            SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
        }
        
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_write_buf_size - is->audio_buf_index;
    
    // ???: Why? 移除sdl buf 的时间, 采用 audio_pts_c (frame_pts时间) 后 音视频 同步了
    double pts = is->audio_pts_c;
//    - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec;
    
//    av_log(NULL, AV_LOG_DEBUG, "Audio clock pts third correct: %f\n", pts);
    
    set_clock_at(&is->audio_clock, pts, is->audio_clock_serial, audio_callback_time / 1000000.0);
}



/**
    打开音频设备
 */
int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params) {
    
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
        //av_log(NULL, AV_LOG_DEBUG, "Invalid sample rate or channel count\n");
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
       //av_log(NULL, AV_LOG_WARNING, "SDL_Open Audio (%d channels, %d Hz): %s\n", wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        return -1;
    }
    
    if (spec.format != AUDIO_S16SYS) {
        //av_log(NULL, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            //av_log(NULL, AV_LOG_ERROR, "SDL advised audio channel count %d is not supported!\n", spec.channels);
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


int audio_thread(void* arg) {
    PlayState *is = arg;
    int ret = 0;
    //av_log(NULL, AV_LOG_DEBUG, "audio_thread arg: %p\n", arg);
    
    AVRational tb;
    
    AVPacket *pkt;
    pkt = av_packet_alloc();
    
    AVFrame *frame = NULL;
    frame = av_frame_alloc();
    
    Frame *af_frame;
    
    do {
        ret = decoder_decode_frame(&is->audio_decoder, frame, NULL);
        if (ret < 0) {
            goto __END_audio_thread;
        }
        if (ret) {
            while (frame->sample_rate) {
                if (!(af_frame = frame_queue_peek_writable(&is->audio_frame_queue))) {
                    goto __END_audio_thread;
                }
                
                tb = (AVRational){1, frame->sample_rate};
                
                af_frame->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
//                av_log(NULL, AV_LOG_DEBUG, "Audio af_frame->pts  second correct result: %f\n", af_frame->pts);
                
                af_frame->serial = is->audio_decoder.pkt_serial;
                
                av_frame_move_ref(af_frame->frame, frame);
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
