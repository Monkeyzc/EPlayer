//
//  clock.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "clock.h"
#include "../PlayState.h"
#include "libavutil/time.h"

void clock_init(Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
}

double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial) {
        return NAN;
    }
    if (c->paused) {
        return c->pts;
    } else {
        // 秒
        double time = av_gettime_relative() / 1000000.0;
//        //av_log(NULL, AV_LOG_DEBUG, "time 秒: %f\n", time);
//        //av_log(NULL, AV_LOG_DEBUG, "c->pts_drift: %f\n", c->pts_drift);
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->serial = serial;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    
//    //av_log(NULL, AV_LOG_DEBUG, "time 秒: %f\n", time);
//    double current_time = av_gettime_relative() / 1000000.0;
//    //av_log(NULL, AV_LOG_DEBUG, "current_time 秒: %f\n", current_time);
//    
//    //av_log(NULL, AV_LOG_DEBUG, "c->pts: %f\n", c->pts * 1000000);
//    //av_log(NULL, AV_LOG_DEBUG, "c->last_updated: %f\n", c->last_updated);
//    //av_log(NULL, AV_LOG_DEBUG, "c->pts_drift: %f\n", c->pts_drift);
}

void set_clock(Clock *c, double pts, int serial) {
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (!isnan(clock) || fabs(clock -slave_clock) > AV_NOSYNC_THRESHOLD)) {
        set_clock(c, slave_clock, slave->serial);
    }
}




