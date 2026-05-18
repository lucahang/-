#include "DisplayModule.h"
#include <iostream>

DisplayModule::DisplayModule() {
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        std::cerr << "找不到 H264 解码器！" << std::endl;
        return;
    }

    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        std::cerr << "无法分配解码器上下文！" << std::endl;
        return;
    }

    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        std::cerr << "无法打开解码器上下文！" << std::endl;
        avcodec_free_context(&m_codecContext);
    }
}

DisplayModule::~DisplayModule() {
    if (m_codecContext) avcodec_free_context(&m_codecContext);
    if (m_packet) av_packet_free(&m_packet);
    if (m_frame) av_frame_free(&m_frame);
    if (m_rgbFrame) {
        if (m_rgbFrame->data[0]) av_freep(&m_rgbFrame->data[0]);
        av_frame_free(&m_rgbFrame);
    }
    if (m_swsContext) sws_freeContext(m_swsContext);
}

void DisplayModule::ShowFrame(const std::vector<unsigned char>& packetData) {
    if (packetData.empty() || !m_codecContext) return;

    m_packet->data = const_cast<uint8_t*>(packetData.data());
    m_packet->size = static_cast<int>(packetData.size());

    int ret = avcodec_send_packet(m_codecContext, m_packet);
    if (ret < 0) {
        //avcodec_flush_buffers(m_codecContext); // 🔥关键
        return;
    }
    int maxDecode = 3;
    while (ret >= 0 && maxDecode--) {
        ret = avcodec_receive_frame(m_codecContext, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return;

        if (m_swsContext == nullptr ||
            m_currentWidth != m_frame->width ||
            m_currentHeight != m_frame->height) {

            if (m_swsContext) {
                sws_freeContext(m_swsContext);
                m_swsContext = nullptr;
            }

            if (m_rgbFrame->data[0]) {
                av_freep(&m_rgbFrame->data[0]);
            }

            m_currentWidth = m_frame->width;
            m_currentHeight = m_frame->height;

            m_swsContext = sws_getContext(
                m_currentWidth, m_currentHeight, static_cast<AVPixelFormat>(m_frame->format),
                m_currentWidth, m_currentHeight, AV_PIX_FMT_BGR24,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );

            if (!m_swsContext) return;

            av_image_alloc(
                m_rgbFrame->data,
                m_rgbFrame->linesize,
                m_currentWidth,
                m_currentHeight,
                AV_PIX_FMT_BGR24,
                1
            );
        }

        sws_scale(
            m_swsContext,
            m_frame->data,
            m_frame->linesize,
            0,
            m_currentHeight,
            m_rgbFrame->data,
            m_rgbFrame->linesize
        );

        cv::Mat img(
            m_currentHeight,
            m_currentWidth,
            CV_8UC3,
            m_rgbFrame->data[0],
            m_rgbFrame->linesize[0]
        );

        cv::imshow("Remote Control Stream", img);

        av_frame_unref(m_frame);
        av_packet_unref(m_packet);
    }
}