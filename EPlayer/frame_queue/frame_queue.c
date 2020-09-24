//
//  frame_queue.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "frame_queue.h"

int frame_queue_init(FrameQueue *f_q, PacketQueue *pkt_q, int max_size, int keep_last) {
    
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

void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
}

Frame *frame_queue_peek_writable(FrameQueue *f) {
    
    SDL_LockMutex(f->mutex);
    if (f->size == f->max_size) {
        av_log(NULL, AV_LOG_DEBUG, "frame_queue 满了\n");
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);
    
    return &f->queue[f->write_index];
}

void frame_queue_push(FrameQueue *f) {
    if (++f->write_index == f->max_size) {
        f->write_index = 0;
    }
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

Frame *frame_queue_peek_readable(FrameQueue *f) {
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);
    return &f->queue[(f->read_index + f->rindex_shown) % f->max_size];
}

void frame_queue_next(FrameQueue *f) {
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

Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->read_index];
}

int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}


