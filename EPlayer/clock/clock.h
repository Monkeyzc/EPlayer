//
//  clock.h
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#pragma once

typedef struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    int *queue_serial;
} Clock;

void clock_init(Clock *c, int *queue_serial);
