#pragma once

#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include <mutex>

// 控制包结构体（与 Server 端保持一致）
// type: 0x01 = 分辨率变更, 0x02 = bitrate 变更
#pragma pack(push, 1)
struct CtrlPacket {
    uint8_t  type;
    uint16_t width;
    uint16_t height;
    uint32_t bitrate;
    uint32_t timestamp;
};
#pragma pack(pop)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

using Microsoft::WRL::ComPtr;

// GPU 解码器：使用 FFmpeg d3d11va hwaccel
// Decode() 返回的纹理 ArraySlice 由 m_frameIndex 指定，格式为 DXGI_FORMAT_NV12
class GpuDecoder {
public:
    GpuDecoder() = default;
    ~GpuDecoder() { Cleanup(); }

    // device: 外部已创建好的 D3D11 设备（必须有 VIDEO_SUPPORT flag）
    bool Init(ID3D11Device* device, int width, int height);

    // 送入一包 H.264 码流，解码成功时 outTexture/outArraySlice 有效
    // 返回 true=处理正常（outTexture 可为 nullptr 表示还没帧出来），false=错误
    bool Decode(const uint8_t* data, size_t size,
        ID3D11Texture2D** outTexture, UINT* outArraySlice);

    void Cleanup();
    void HandleCtrlPacket(const CtrlPacket& pkt);

    // 查询当前状态（供渲染层判断是否需要重建 swap chain）
    int GetWidth()   const { return m_width; }
    int GetHeight()  const { return m_height; }
    int GetBitrate() const { return m_bitrate; }
private:
    static enum AVPixelFormat GetHwFormat(AVCodecContext* ctx,
        const enum AVPixelFormat* pix_fmts);
    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    ID3D11Device* m_device = nullptr;  // 弱引用，外部持有

    // 内部重建解码器，分辨率变更时调用
    bool ReinitDecoder(int newWidth, int newHeight);

    int m_width = 0;
    int m_height = 0;
    int m_bitrate = 0;   // 仅记录，供上层读取

    std::mutex m_mutex;  // 保护重建期间线程安全
};