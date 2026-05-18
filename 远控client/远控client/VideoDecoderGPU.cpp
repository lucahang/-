#include "VideoDecoderGPU.h"
#include <cstdio>

enum AVPixelFormat GpuDecoder::GetHwFormat(AVCodecContext* ctx,
            const enum AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11)
            return AV_PIX_FMT_D3D11;
    }
    printf("[GpuDecoder] 警告: 找不到 AV_PIX_FMT_D3D11，回退软解\n");
    return pix_fmts[0];
}

bool GpuDecoder::Init(ID3D11Device* device, int /*width*/, int /*height*/) {
    m_device = device;

    // -------------------------------------------------------
    // 1. 用外部 D3D11 Device 初始化 FFmpeg hwdevice context
    //    解码和渲染共用同一个 GPU 设备，零拷贝
    // -------------------------------------------------------
    m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!m_hwDeviceCtx) {
        printf("[GpuDecoder] av_hwdevice_ctx_alloc 失败\n");
        return false;
    }

    auto* hwCtx = reinterpret_cast<AVHWDeviceContext*>(m_hwDeviceCtx->data);
    auto* d3d11vaCtx = reinterpret_cast<AVD3D11VADeviceContext*>(hwCtx->hwctx);
    d3d11vaCtx->device = device;                        // FFmpeg 会 AddRef
    device->GetImmediateContext(&d3d11vaCtx->device_context);

    if (av_hwdevice_ctx_init(m_hwDeviceCtx) < 0) {
        printf("[GpuDecoder] av_hwdevice_ctx_init 失败\n");
        av_buffer_unref(&m_hwDeviceCtx);
        return false;
    }

    // -------------------------------------------------------
    // 2. 打开 H.264 解码器，绑定 hwdevice
    // -------------------------------------------------------
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { printf("[GpuDecoder] 找不到 H264 解码器\n"); return false; }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { printf("[GpuDecoder] alloc context 失败\n"); return false; }

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    m_codecCtx->get_format = GetHwFormat;
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        printf("[GpuDecoder] avcodec_open2 失败\n");
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();

    printf("[GpuDecoder] 初始化成功 — FFmpeg d3d11va hwaccel\n");
    return true;
}

// 返回值：false=解码错误；true=正常（outTexture 可为 nullptr 表示还没帧出来）
// 成功时 *outTexture 已 AddRef，调用者用完后需 Release()
bool GpuDecoder::Decode(const uint8_t* data, size_t size,
            ID3D11Texture2D** outTexture, UINT* outArraySlice) {
    std::lock_guard<std::mutex> lock(m_mutex);  // ← 新增这一行
    *outTexture = nullptr;
    *outArraySlice = 0;
    if (!m_codecCtx || !data || size == 0) return false;

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = static_cast<int>(size);

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        if (ret != AVERROR_INVALIDDATA) {
            char buf[128]; av_strerror(ret, buf, sizeof(buf));
            printf("[GpuDecoder] send_packet: %s\n", buf);
        }
        return false;
    }

    ret = avcodec_receive_frame(m_codecCtx, m_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true; // 正常积累中
    if (ret < 0) {
        char buf[128]; av_strerror(ret, buf, sizeof(buf));
        printf("[GpuDecoder] receive_frame: %s\n", buf);
        return false;
    }

    if (m_frame->format != AV_PIX_FMT_D3D11) {
        printf("[GpuDecoder] 非硬件帧格式: %d\n", m_frame->format);
        av_frame_unref(m_frame);
        return false;
    }

    // data[0] = ID3D11Texture2D*（FFmpeg 内部纹理数组）
    // data[1] = ArraySlice（intptr_t 强转）
    auto* tex = reinterpret_cast<ID3D11Texture2D*>(m_frame->data[0]);
    UINT  slice = static_cast<UINT>(reinterpret_cast<intptr_t>(m_frame->data[1]));

    tex->AddRef(); // 调用者负责 Release
    *outTexture = tex;
    *outArraySlice = slice;

    av_frame_unref(m_frame);
    return true;
}

bool GpuDecoder::ReinitDecoder(int newWidth, int newHeight) {
    printf("[GpuDecoder] 重建解码器: %dx%d → %dx%d\n",
        m_width, m_height, newWidth, newHeight);

    // 1. 销毁旧解码器（保留 hwDeviceCtx，避免重新绑定 D3D11 设备）
    if (m_frame) { av_frame_free(&m_frame);   m_frame = nullptr; }
    if (m_packet) { av_packet_free(&m_packet); m_packet = nullptr; }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); m_codecCtx = nullptr; }

    // 2. 重建 AVCodecContext（hwDeviceCtx 复用，不重新创建）
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("[GpuDecoder] ReinitDecoder: 找不到 H264 解码器\n");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx); // 复用已有 hwdevice
    m_codecCtx->get_format = GetHwFormat;
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    // 给解码器提示新分辨率（非强制，但有助于内部缓冲区预分配）
    m_codecCtx->width = newWidth;
    m_codecCtx->height = newHeight;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        printf("[GpuDecoder] ReinitDecoder: avcodec_open2 失败\n");
        return false;
    }

    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();

    // 3. 更新记录的分辨率
    m_width = newWidth;
    m_height = newHeight;

    printf("[GpuDecoder] 重建完成: %dx%d\n", m_width, m_height);
    return true;
}

// -------------------------------------------------------
// [新增] HandleCtrlPacket：处理 Server 下发的控制包
// -------------------------------------------------------
void GpuDecoder::HandleCtrlPacket(const CtrlPacket& pkt) {
    if (pkt.type == 0x01) {
        // 分辨率变更 → 重建解码器
        int newW = static_cast<int>(pkt.width);
        int newH = static_cast<int>(pkt.height);
        if (newW == m_width && newH == m_height) return; // 无变化，忽略

        std::lock_guard<std::mutex> lock(m_mutex);
        ReinitDecoder(newW, newH);

    }
    else if (pkt.type == 0x02) {
        // bitrate 变更 → 解码器无需重建，仅记录
        m_bitrate = static_cast<int>(pkt.bitrate);
        printf("[GpuDecoder] Bitrate 更新: %d bps\n", m_bitrate);
    }
}

void GpuDecoder::Cleanup() {
    if (m_frame)       av_frame_free(&m_frame);
    if (m_packet)      av_packet_free(&m_packet);
    if (m_codecCtx)    avcodec_free_context(&m_codecCtx);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    m_device = nullptr;
}
