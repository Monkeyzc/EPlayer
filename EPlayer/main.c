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



#define sdl_custom_envet_refresh_event_type (SDL_USEREVENT + 1)

static bool play_exit = false;
static bool play_pause = false;

int sdl_thread_handle_refreshing(void *opaque) {
    
    int frame_rate = *((int *)opaque);
    int interval = (frame_rate > 0) ? 1000/frame_rate : 40;
    
    while (!play_exit) {
        if (!play_pause) {
            Uint32 myEventType = SDL_RegisterEvents(1);
            if (myEventType != ((Uint32)-1)) {
                SDL_Event event;
                SDL_memset(&event, 0, sizeof(event)); /* or SDL_zero(event) */
                event.type = sdl_custom_envet_refresh_event_type;
                SDL_PushEvent(&event);
            }
            SDL_Delay(interval);
        }
    }
    return 0;
}


int main(int argc, const char * argv[]) {
    
    const char *input_filename = "/Users/zhaofei/Desktop/1-2 课程导学.mp4";
    
    int ret = -1;
    char errors[1024] = {0,};
    
    // FFmpeg
    AVFormatContext *fmt_ctx = NULL;
    AVInputFormat *input_fmt = NULL;
    
    AVCodecParameters *codec_par = NULL;
    AVCodec *codec = NULL;
    AVCodecContext *codec_ctx = NULL;
    
    AVFrame *frame_raw = NULL; // 帧, 由包解码得到原始帧
    AVFrame *frame_yuv = NULL; // 帧, 由原始帧 色彩转换得到
    
    AVPacket *packet = NULL; // 包, 从流中读取的一段数据
    
    struct SwsContext *sws_ctx = NULL;
    
    int video_stream_index; // 视频流索引
    int frame_rate;
    
    
    // SDL
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Surface *bmp = NULL;
    SDL_Texture *texture = NULL;
    SDL_Thread *sdl_refresh_thread = NULL;
    
    av_log_set_level(AV_LOG_DEBUG);
    const char *version_info = av_version_info();
    av_log(NULL, AV_LOG_INFO, "FFmpeg version info: %s\n", version_info);
    
    ret = avformat_open_input(&fmt_ctx, input_filename, input_fmt, NULL);
    
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "could not open file: %s, errors: %s", input_filename, errors);
        goto __Destroy;
    }
    
    
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "could not fild stream info errors: %s", errors);
        goto __Destroy;
    }
    
    video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (video_stream_index < 0) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "could not fild video stream errors: %s", errors);
        goto __Destroy;
    }
    
    // 打印输入文件的信息
    av_dump_format(fmt_ctx, video_stream_index, input_filename, 0);
    
    codec_par = fmt_ctx->streams[video_stream_index]->codecpar;
    
    // 解码器
    codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "could not find decoder by id: %d, errors: %s", codec_par->codec_id, errors);
        goto __Destroy;
    }
    
    // 解码上下文
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "could not alloc code context errors: %s", errors);
        goto __Destroy;
    }
    
    ret = avcodec_parameters_to_context(codec_ctx, codec_par);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "fill codec_ctx with codec parameters errors: %s", errors);
        goto __Destroy;
    }
    
    
    // 打开解码器
    ret = avcodec_open2(codec_ctx, codec, NULL);
    if (ret < 0) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "open codec errors: %s", errors);
        goto __Destroy;
    }
    
    // 分配AVFrame空间, 并不会分配 data buffer (AVFrame.*data[])
    frame_raw = av_frame_alloc();
    frame_yuv = av_frame_alloc();
    
    
    /*
     为frame_yuv->data手动分配缓冲区，用于存储sws_scale()中目的帧视频数据
     frame_raw的data_buffer由av_read_frame()分配，因此不需手工分配
     frame_yuv的data_buffer无处分配，因此在此处手工分配
     */
    int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, codec_ctx->width, codec_ctx->height, 1);
    if (buffer_size < 0) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "image get buferr size errors: %s", errors);
        goto __Destroy;
    }
    // buffer将作为frame_yuv的视频数据缓冲区
    void *buffer = av_malloc(buffer_size);
    if (!buffer) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "malloc buffer errors: %s", errors);
        goto __Destroy;
    }
    // 使用给定参数设定frame_yuv->data和frame_yuv->linesize
    av_image_fill_arrays(frame_yuv->data, frame_yuv->linesize, buffer, AV_PIX_FMT_YUV420P, codec_ctx->width, codec_ctx->height, 1);
    
    
    // 初始化 sws_context, 用户后续图像转换
    sws_ctx = sws_getContext(codec_ctx->width,
                   codec_ctx->height, codec_ctx->pix_fmt,
                   codec_ctx->width,
                   codec_ctx->height,
                   AV_PIX_FMT_YUV420P,
                   SWS_BICUBIC,
                   NULL,
                   NULL,
                   NULL);
    if (!sws_ctx) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "sws_getContext errors: %s", errors);
        goto __Destroy;
    }
    
    // SDL
    SDL_version compiled;
    SDL_version linked;
    
    SDL_VERSION(&compiled);
    SDL_GetVersion(&linked);
    
    SDL_Log("We compiled against SDL version %d.%d.%d ...\n", compiled.major, compiled.minor, compiled.patch);
    SDL_Log("But we are linking against SDL version %d.%d.%d.\n", linked.major, linked.minor, linked.patch);
    
    
    ret = SDL_Init(SDL_INIT_EVERYTHING);
    
    if (ret != 0) {
        SDL_Log("Unabel to init SDL: %s", SDL_GetError());
        goto __Destroy;
    }
    
    // 窗口
    window = SDL_CreateWindow("Hello world", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, codec_ctx->width, codec_ctx->height, SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    
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


//    bmp = SDL_LoadBMP("/Users/zhaofei/Desktop/1598025169612.bmp");
//    if (!bmp) {
//        SDL_Log("load bmp fail: %s", SDL_GetError());
//        goto __Destroy;
//    }

//    texture = SDL_CreateTextureFromSurface(renderer, bmp);
    
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, codec_ctx->width, codec_ctx->height);
    if (!texture) {
        SDL_Log("create texture from surface fail: %s", SDL_GetError());
        goto __Destroy;
    }

    SDL_Rect sdl_rect;
    sdl_rect.x = 0;
    sdl_rect.y = 0;
    sdl_rect.w = codec_ctx->width;
    sdl_rect.h = codec_ctx->height;
    
    packet = av_packet_alloc();
    
    if (!packet) {
        av_strerror(ret, errors, 1024);
        av_log(NULL, AV_LOG_ERROR, "alloc packet errors: %s", errors);
        goto __Destroy;
    }
    
    sdl_refresh_thread = SDL_CreateThread(sdl_thread_handle_refreshing, "refresh_thread", (void *)&frame_rate);
    if (!sdl_refresh_thread) {
        SDL_Log("create sdl refresh thread fail: %s", SDL_GetError());
        goto __Destroy;
    }
    
    SDL_Event e;
    
    while (1) {
        
        SDL_WaitEvent(&e);
        
        if (e.type == sdl_custom_envet_refresh_event_type) {
            // 读取 AVPacket
            while (av_read_frame(fmt_ctx, packet) == 0) {
                // 仅处理视频
                if (packet->stream_index == video_stream_index) {
                    
                    ret = avcodec_send_packet(codec_ctx, packet);
                    
                    if (ret < 0) {
                        av_strerror(ret, errors, 1024);
                        av_log(NULL, AV_LOG_ERROR, "send packet to codec error: %s\n", errors);
                        goto __Destroy;
                    }
                    
                    while (ret >= 0) {
                        ret = avcodec_receive_frame(codec_ctx, frame_raw);
                        
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else {
                            
                            if (ret < 0) {
                                av_strerror(ret, errors, 1024);
                                av_log(NULL, AV_LOG_ERROR, "error during decoding: %s\n", errors);
                                goto __Destroy;
                            }
                            
                            
                            sws_scale(sws_ctx,
                                      (const uint8_t *const *)frame_raw->data,
                                      frame_raw->linesize,
                                      0,
                                      codec_ctx->height,
                                      frame_yuv->data,
                                      frame_yuv->linesize);
                            
                            SDL_UpdateYUVTexture(texture,
                                                 &sdl_rect,
                                                 frame_yuv->data[0], frame_yuv->linesize[0],
                                                 frame_yuv->data[1], frame_yuv->linesize[1],
                                                 frame_yuv->data[2], frame_yuv->linesize[2]
                                                 );
                            
                            
                            // 使用特定颜色清空当前渲染目标
                            SDL_RenderClear(renderer);
                            // 使用部分图像数据(texture)更新当前渲染目标
                            SDL_RenderCopy(renderer, texture, NULL, &sdl_rect);
                            // 执行渲染，更新屏幕显示
                            SDL_RenderPresent(renderer);
                        }
                    }
                    
                    av_packet_unref(packet);
                }
            }

        } else if (e.type == SDL_QUIT) {
            goto __Destroy;
        }
    }
    
    
//    int quit = 0;
//    while (!quit){
//        while (SDL_PollEvent(&e)){
//            if (e.type == SDL_QUIT){
//                quit = 1;
//                break;
//            } else {
//
//
//
//            }
//        }
//    }
    
        
//    SDL_FreeSurface(bmp);
//    bmp = NULL;
    
//    SDL_RenderClear(renderer);
//    SDL_RenderCopy(renderer, texture, NULL, NULL);
//    SDL_RenderPresent(renderer);
    
    
//    SDL_Event e;
//    int quit = 0;
//    while (!quit){
//        while (SDL_PollEvent(&e)){
//            if (e.type == SDL_QUIT){
//                quit = 1;
//                break;
//            }
//        }
//    }
    
__Destroy:
    
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    
    if (fmt_ctx) {
        avformat_free_context(fmt_ctx);
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
