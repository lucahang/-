#include "VideoEncoder.h"
#include <iostream>

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

VideoEncoder::VideoEncoder() {}
VideoEncoder::~VideoEncoder() { Cleanup(); }

bool VideoEncoder::Init(int width, int height, int fps) {
    // 1. 查找 H.264 编码器
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return false;

    m_codecContext = avcodec_alloc_context3(codec);
    m_codecContext->width = width;
    m_codecContext->height = height;
    m_codecContext->time_base = { 1, fps };
    m_codecContext->framerate = { fps, 1 };
    m_codecContext->pix_fmt = AV_PIX_FMT_YUV420P; // H.264 通用格式

    //// ⭐ 关键参数（低配优化）
    //m_codecContext->bit_rate = 2000000;      // 2 Mbps（低配稳定）
    //m_codecContext->rc_max_rate = 2500000;
    //m_codecContext->rc_buffer_size = 3000000;

    //m_codecContext->gop_size = 40;           // 2秒一个I帧（降低压力）
    //m_codecContext->max_b_frames = 0;        // ❗必须0（低延迟）

    //// ⭐ 低配重点
    //av_opt_set(m_codecContext->priv_data, "preset", "ultrafast", 0);
    //av_opt_set(m_codecContext->priv_data, "tune", "zerolatency", 0);

    //// ⭐ 防止爆帧（非常重要）
    //av_opt_set(m_codecContext->priv_data, "slice-max-size", "1200", 0);

    //// ⭐ 降低编码复杂度
    //av_opt_set(m_codecContext->priv_data, "nal-hrd", "cbr", 0);
    //av_opt_set(m_codecContext->priv_data, "scenecut", "0", 0);
    m_codecContext->bit_rate = 8000000;
    m_codecContext->rc_max_rate = 8000000;
    m_codecContext->rc_buffer_size = 8000000;
    m_codecContext->max_b_frames = 0;
    m_codecContext->gop_size = 10;
    av_opt_set(m_codecContext->priv_data, "preset", "veryfast", 0);
    av_opt_set(m_codecContext->priv_data, "tune", "zerolatency", 0);
    av_opt_set(m_codecContext->priv_data, "crf", "23", 0);
    av_opt_set(m_codecContext->priv_data, "x264-params",
        "aq-mode=1:aq-strength=1.0:rc-lookahead=0", 0);
    
    if (avcodec_open2(m_codecContext, codec, NULL) < 0) return false;
    // 2. 初始化 YUV 帧容器
    m_yuvFrame = av_frame_alloc();
    m_yuvFrame->format = m_codecContext->pix_fmt;
    m_yuvFrame->width = width;
    m_yuvFrame->height = height;
    av_frame_get_buffer(m_yuvFrame, 32);

    // 3. 初始化格式转换器 (BGRA -> YUV420P)
    m_swsContext = sws_getContext(width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BICUBIC, NULL, NULL, NULL);

    m_packet = av_packet_alloc();
    return true;
}

std::vector<unsigned char> VideoEncoder::EncodeFrame(unsigned char* rawPixels, int rowPitch) {
    std::vector<unsigned char> result;

    // A. 格式转换：从显卡的原始像素转为 YUV
    const uint8_t* srcSlice[] = { rawPixels };
    int srcStride[] = { rowPitch };
    sws_scale(m_swsContext, srcSlice, srcStride, 0, m_codecContext->height,
        m_yuvFrame->data, m_yuvFrame->linesize);

    // B. 送入编码器
    static int pts = 0;
    m_yuvFrame->pts = pts++;
    if (avcodec_send_frame(m_codecContext, m_yuvFrame) >= 0) {
        // C. 接收压缩后的包 (NALU)
        if (pts % 30 == 0) {
            m_yuvFrame->pict_type = AV_PICTURE_TYPE_I;
        }
        while (avcodec_receive_packet(m_codecContext, m_packet) >= 0) {
            result.insert(result.end(), m_packet->data, m_packet->data + m_packet->size);
            av_packet_unref(m_packet);
        }
    }
    return result;
}

void VideoEncoder::Cleanup() {
    if (m_codecContext) avcodec_free_context(&m_codecContext);
    if (m_yuvFrame) av_frame_free(&m_yuvFrame);
    if (m_swsContext) sws_freeContext(m_swsContext);
    if (m_packet) av_packet_free(&m_packet);
}
