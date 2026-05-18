#pragma once
#include <vector>
#include <d3d11_1.h>
#include <d3d11.h>
#include <mutex>
#include "nvEncodeAPI.h"

class GpuEncoder {
public:
    GpuEncoder();
    ~GpuEncoder();
    bool ProcessColorConversion(ID3D11Texture2D* bgraTex);
    bool Init( int width, int height, int fps);
    // 动态修改 bitrate（无需重建编码器，立即生效）
    bool SetBitrate(int newBitrate);

    // 动态修改分辨率（重建编码器 + 重新注册资源）
    bool SetResolution(int newWidth, int newHeight);

    // 获取当前配置（供外部读取）
    int GetWidth()   const { return m_width; }
    int GetHeight()  const { return m_height; }
    int GetBitrate() const { return m_bitrate; }

    std::vector<uint8_t> EncodeZeroCopy(ID3D11Texture2D* nv12Tex);
    std::vector<uint8_t> Encode(unsigned char* bgraDataPtr, int map_pRowPitch);
    void Cleanup();
    void ConvertBgraToNv12(ID3D11Texture2D* bgraInput);
    ID3D11Device* GetDevice();
private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    NVENCSTATUS status;
    void* m_encoder = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_fn = { NV_ENCODE_API_FUNCTION_LIST_VER };

    NV_ENC_INITIALIZE_PARAMS m_initParams{};
    NV_ENC_CONFIG m_config{};

    int m_width = 0;
    int m_height = 0;

    NV_ENC_OUTPUT_PTR m_outputBuffer = nullptr;

    // ⭐ 注册一次
    NV_ENC_REGISTERED_PTR m_registeredResource = nullptr;
    ID3D11Texture2D* m_nvInputTex = nullptr; // 统一用这个名字
    // 视频处理相关的缓存
    ID3D11Texture2D* m_nvBGRATex = nullptr;
    ID3D11VideoDevice* m_vDevice = nullptr;
    ID3D11VideoContext* m_vContext = nullptr;
    ID3D11VideoProcessor* m_vProcessor = nullptr;
    ID3D11VideoProcessorEnumerator* m_vEnumerator = nullptr;
    ID3D11Texture2D* m_nV12Tex = nullptr; // 转换后的中转纹理

    bool ReinitEncoder(int newWidth, int newHeight);

    // 清理已注册的 NVENC 资源（重建前必须先释放）
    void UnregisterResources();

    int  m_bitrate = 4000000;  // 记录当前 bitrate
    int  m_fps = 30;       // 记录 fps，重建时复用

    std::mutex m_mutex;        // 保护编码器重建期间线程安全
};