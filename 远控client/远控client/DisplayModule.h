#pragma once
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <vector>

class DisplayModule {
public:
    DisplayModule();
    ~DisplayModule();

    void ShowFrame(const std::vector<unsigned char>& packet);

private:
    AVCodecContext* m_codecContext = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_rgbFrame = nullptr;
    SwsContext* m_swsContext = nullptr;

    int m_currentWidth = 0;
    int m_currentHeight = 0;
};