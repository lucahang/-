#pragma once
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool Init();

    std::vector<unsigned char> DecodeFrame(unsigned char* data, int size, int& outW, int& outH);

private:
    void Cleanup();

    AVCodecContext* m_codecContext = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_rgbFrame = nullptr;
    SwsContext* m_swsContext = nullptr;
    AVPacket* m_packet = nullptr;
};