//
//  dumex.h
//  EPlayer
//
//  Created by zhaofei on 2020/9/24.
//  Copyright © 2020 zhaofei. All rights reserved.
//

#pragma once

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

#include "../packet_queue/packet_queue.h"
#include "../frame_queue/frame_queue.h"
#include "../clock/clock.h"
#include "../decoder/decoder.h"
#include "../audio/audio.h"
#include "../video/video.h"

#include "../PlayState.h"
#include "../Global/Global.h"

PlayState *stream_open(const char *filename, AVInputFormat *input_fmt);
