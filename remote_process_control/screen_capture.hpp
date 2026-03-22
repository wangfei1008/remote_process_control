#pragma once
extern "C" {
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

AVFormatContext* open_screen_capture(const char* source, int width, int height);
AVFrame* decode_frame(AVPacket& pkt);


