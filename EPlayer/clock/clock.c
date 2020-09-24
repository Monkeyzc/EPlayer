//
//  clock.c
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#include "clock.h"

void clock_init(Clock *c, int *queue_serial) {
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
}
