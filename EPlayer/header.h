


#include <stdio.h>
#include <stdbool.h>

// std image
#define STB_IMAGE_IMPLEMENTATION
#include "std_image.h"

#include "SDL.h"

// glad
#include "glad.h"

// glfw
#include "glfw3.h"

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

#include "packet_queue/packet_queue.h"
#include "frame_queue/frame_queue.h"
#include "decoder/decoder.h"
#include "clock/clock.h"
#include "dumex/dumex.h"

#include "Global.h"

int picture_index = 0;

const char *out_file_name = "/Users/zhaofei/Downloads/test_record_audio_aac_decode.pcm";
FILE *outfile = NULL;


/**
 *  测试视频
 *  http://devimages.apple.com.edgekey.net/streaming/examples/bipbop_4x3/gear2/fileSequence20.ts
 */


/**
 
 文章:
 https://juejin.im/post/6844903815934640136
 
 */
