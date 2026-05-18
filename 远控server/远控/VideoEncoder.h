#pragma once
#include <vector>

// 引入 FFmpeg 头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

struct DirtyRect {
    int x1, y1, x2, y2;
    bool valid = false;
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    bool Init(int width, int height, int fps);
    std::vector<unsigned char> EncodeFrame(unsigned char* rawPixels, int rowPitch);
    void Cleanup();

private:
    AVCodecContext* m_codecContext = nullptr;
    AVFrame* m_yuvFrame = nullptr;
    SwsContext* m_swsContext = nullptr;
    AVPacket* m_packet = nullptr;
    int width = 0;
    int height = 0;
};
