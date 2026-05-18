#include "VideoDecoder.h"
#include <iostream>

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

VideoDecoder::VideoDecoder() {
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();
}

VideoDecoder::~VideoDecoder() {
    Cleanup();
}

bool VideoDecoder::Init() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) return false;

    if (avcodec_open2(m_codecContext, codec, NULL) < 0) return false;

    return true;
}

std::vector<unsigned char> VideoDecoder::DecodeFrame(unsigned char* data, int size, int& outW, int& outH) {
    if (!m_codecContext) return {};

    m_packet->data = data;
    m_packet->size = size;

    int ret = avcodec_send_packet(m_codecContext, m_packet);
    if (ret < 0) return {};

    ret = avcodec_receive_frame(m_codecContext, m_frame);
    if (ret < 0) return {};

    outW = m_frame->width;
    outH = m_frame->height;

    // 1. 初始化或更新 SwsContext
    m_swsContext = sws_getCachedContext(
        m_swsContext,
        outW, outH, static_cast<AVPixelFormat>(m_frame->format),
        outW, outH, AV_PIX_FMT_BGR24,
        SWS_FAST_BILINEAR, NULL, NULL, NULL
    );

    if (!m_swsContext) return {};

    // 2. 确保输出帧已分配空间
    if (!m_rgbFrame->data[0]) {
        // 使用 1 字节对齐（如果不打算在 GPU 上直接用，1 字节对齐可以让数据紧凑无空隙）
        av_image_alloc(m_rgbFrame->data, m_rgbFrame->linesize, outW, outH, AV_PIX_FMT_BGR24, 1);
    }

    // 3. 颜色空间转换 (YUV -> BGR24)
    sws_scale(m_swsContext, m_frame->data, m_frame->linesize, 0, outH,
        m_rgbFrame->data, m_rgbFrame->linesize);

    // 4. 计算有效数据大小并拷贝
    // BGR24 每个像素 3 字节，且因为上面用了 1 字节对齐，这里可以直接计算
    int dataSize = outW * outH * 3;
    std::vector<unsigned char> tem_v(m_rgbFrame->data[0], m_rgbFrame->data[0] + dataSize);

    // 5. 清理状态
    av_frame_unref(m_frame);
    // 注意：av_packet_unref 建议在循环外部使用，除非你每次都 new packet

    return tem_v;
}
void VideoDecoder::Cleanup() {
    if (m_codecContext) avcodec_free_context(&m_codecContext);
    if (m_frame) av_frame_free(&m_frame);
    if (m_rgbFrame) {
        if (m_rgbFrame->data[0]) av_freep(&m_rgbFrame->data[0]);
        av_frame_free(&m_rgbFrame);
    }
    if (m_packet) av_packet_free(&m_packet);
    if (m_swsContext) sws_freeContext(m_swsContext);
}