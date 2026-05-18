// FFmpegUtils.h
#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <memory>

// 自定义删除器，实现 FFmpeg 资源的 RAII 管理
struct AVPacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct AVFrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct AVCodecContextDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct SwsContextDeleter { void operator()(SwsContext* s) const { sws_freeContext(s); } };

using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using SwsCtxPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;