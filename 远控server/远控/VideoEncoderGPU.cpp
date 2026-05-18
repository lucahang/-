#include "VideoEncoderGPU.h"
#include <iostream>

GpuEncoder::GpuEncoder() {}
GpuEncoder::~GpuEncoder() { Cleanup(); }
// 封装后的转换函数
bool GpuEncoder::ProcessColorConversion(ID3D11Texture2D* bgraTex) {
    if (!bgraTex) return false;

    // 1. 延迟初始化：只在第一次调用或分辨率改变时初始化
    if (!m_vProcessor) {
        D3D11_TEXTURE2D_DESC desc;
        bgraTex->GetDesc(&desc);
        m_width = desc.Width;
        m_height = desc.Height;

        // 获取视频处理接口
        if (FAILED(m_device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&m_vDevice))) return false;
        if (FAILED(m_ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&m_vContext))) return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cDesc = {};
        cDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cDesc.InputWidth = m_width;
        cDesc.InputHeight = m_height;
        cDesc.OutputWidth = m_width;
        cDesc.OutputHeight = m_height;
        cDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        m_vDevice->CreateVideoProcessorEnumerator(&cDesc, &m_vEnumerator);
        m_vDevice->CreateVideoProcessor(m_vEnumerator, 0, &m_vProcessor);

        // 创建固定的 NV12 纹理用于编码
        D3D11_TEXTURE2D_DESC nvDesc = desc;
        nvDesc.Format = DXGI_FORMAT_NV12;
        nvDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        m_device->CreateTexture2D(&nvDesc, nullptr, &m_nV12Tex);
    }

    // 2. 执行 GPU 内部转换 (BGRA -> NV12)
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = { 0, D3D11_VPIV_DIMENSION_TEXTURE2D, {0,0} };
    ID3D11VideoProcessorInputView* inView = nullptr;
    m_vDevice->CreateVideoProcessorInputView(bgraTex, m_vEnumerator, &inDesc, &inView);

    // 1. 先声明并清空结构体
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};

    // 2. 设置维度
    outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

    // 3. 设置具体的 Texture2D 参数 (对应 Union 里的 Texture2D 成员)
    outDesc.Texture2D.MipSlice = 0;

    // 4. 然后再调用创建函数
    ID3D11VideoProcessorOutputView* outView = nullptr;
    HRESULT hr = m_vDevice->CreateVideoProcessorOutputView(m_nV12Tex, m_vEnumerator, &outDesc, &outView);
    D3D11_VIDEO_PROCESSOR_STREAM stream = { TRUE, 0, 0, 0, 0, NULL, inView, NULL };
    m_vContext->VideoProcessorBlt(m_vProcessor, outView, 0, 1, &stream);

    inView->Release();
    outView->Release();
    return true;
}

ID3D11Device* GpuEncoder::GetDevice() {
    return m_device;
}


bool GpuEncoder::Init( int width, int height, int fps) {
    // 1. 枚举并创建 NVIDIA 设备 (保持原逻辑，确保是独显)
    IDXGIFactory1* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
    if (FAILED(hr) ){
        std::cout << "CreateDXGIFactory1 error"<<hr << std::endl;
        return false;
    }
    IDXGIAdapter1* pAdapter = nullptr;
    ID3D11Device* nvDevice = nullptr;
    ID3D11DeviceContext* nvContext = nullptr;

    for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        pAdapter->GetDesc1(&desc);
        if (wcsstr(desc.Description, L"NVIDIA")) {
            if (SUCCEEDED(D3D11CreateDevice(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 
                        0, D3D11_SDK_VERSION, &nvDevice, nullptr, &nvContext))) {
                pAdapter->Release();
                break;
            }
        }
        pAdapter->Release();
    }
    pFactory->Release();

    if (!nvDevice) {
        std::cout << "nvDevice error" << std::endl;
        return false;
    }
    m_device = nvDevice;
    m_ctx = nvContext;
    m_width = width;
    m_height = height;

    // 2. 加载 NVENC API
    if (NvEncodeAPICreateInstance(&m_fn) != NV_ENC_SUCCESS) {
        std::cout << "NvEncodeAPICreateInstance error" << std::endl;
        return false;
    }
    // 3. 开启编码会话
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    openParams.device = m_device;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    openParams.apiVersion = NVENCAPI_VERSION;

    if (m_fn.nvEncOpenEncodeSessionEx(&openParams, &m_encoder) != NV_ENC_SUCCESS) {
        std::cout << "nvEncOpenEncodeSessionEx error" << std::endl;
        return false;
    }
    // 4. 获取预设配置 (P1: 最快速度)
    GUID presetGUID = NV_ENC_PRESET_P1_GUID;
    NV_ENC_PRESET_CONFIG presetCfg = { NV_ENC_PRESET_CONFIG_VER };
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;
    status = m_fn.nvEncGetEncodePresetConfigEx(m_encoder, NV_ENC_CODEC_H264_GUID, presetGUID,
            NV_ENC_TUNING_INFO_LOW_LATENCY, &presetCfg);
    if (status != NV_ENC_SUCCESS) {
        // 打印十六进制错误码，比如 15 (NV_ENC_ERR_INVALID_VERSION)
        std::cout << "nvEncGetEncodePresetConfigEx error: " << status << std::endl;
        return false;
    }
    // NVENC 设置
    m_config = presetCfg.presetCfg;
    m_config.gopLength = fps;
    m_config.frameIntervalP = 1;

    // --- 核心修复：开启 SPS/PPS 重复 ---
    m_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    // 强制开启 IDR 标志（而不是普通 I 帧）
    m_config.encodeCodecConfig.h264Config.idrPeriod = 60;
    m_config.gopLength = 60;

    m_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    m_config.rcParams.averageBitRate = 4000000; // 4Mbps
    m_config.rcParams.maxBitRate = 8000000;
    // --- 1. 确保 Config 自身版本正确 ---
    m_config.version = NV_ENC_CONFIG_VER;

    // --- 2. 填充初始化参数 ---
    m_initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    m_initParams.encodeConfig = &m_config;           // 链接刚才配置好的 config
    m_initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
    m_initParams.presetGUID = presetGUID;            // P1 GUID
    m_initParams.encodeWidth = width;
    m_initParams.encodeHeight = height;
    m_initParams.darWidth = width;                   // 保持 16:9
    m_initParams.darHeight = height;
    m_initParams.frameRateNum = fps;
    m_initParams.frameRateDen = 1;
    m_initParams.enablePTD = 1;                      // 必须为 1，交给驱动处理图片类型

    // --- 3. 额外的安全检查 (防止某些驱动要求显示设置) ---
    // 有时驱动要求显式设置调优信息
    m_initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    status = m_fn.nvEncInitializeEncoder(m_encoder, &m_initParams);
    if (status != NV_ENC_SUCCESS) {
        std::cout << "nvEncInitializeEncoder error. error code: " << status << std::endl;
        return false;
    }
    // ⭐ [核心修改 1]: 初始化接收纹理 (改为 BGRA 格式)
    // 此时 m_nV12Tex 实际上存储的是 BGRA 数据
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    // 必须是 BGRA 格式，因为我们要从 Intel 侧直接 memcpy 这种原始数据
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.CPUAccessFlags = 0;
    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_nvInputTex))) {
        std::cout << "错误：NVIDIA 接收纹理创建失败" << std::endl;
        return false;
    }
    else {
        // 此时 m_nvInputTex 已经指向了 NVIDIA 显存中的一块地址
        std::cout << "创建成功！指针地址: " << m_nvInputTex << std::endl;
    }

    // 6. 创建输出 Bitstream Buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER createBS = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    if (m_fn.nvEncCreateBitstreamBuffer(m_encoder, &createBS) != NV_ENC_SUCCESS) {
        std::cout << "nvEncCreateBitstreamBuffer error" << std::endl;
        return false;
    }
    m_outputBuffer = createBS.bitstreamBuffer;

    std::cout << "成功：NVIDIA 硬件编码器已切换至 BGRA 直接输入模式。" << std::endl;
    return true;
}

// zero copy

std::vector<uint8_t> GpuEncoder::EncodeZeroCopy(ID3D11Texture2D* bgraTex) {
    std::lock_guard<std::mutex> Lock(m_mutex);  // ← 新增
    std::vector<uint8_t> out;
    // ⭐ 调用新封装的转换函数
    if (!ProcessColorConversion(bgraTex)) {
        return out;
    }

    // ⭐ 注册资源 (使用转换后的 m_nV12Tex)
    if (!m_registeredResource) {
        NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = (void*)m_nV12Tex; // 永远注册这个固定的纹理
        reg.width = m_width;
        reg.height = m_height;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        if (m_fn.nvEncRegisterResource(m_encoder, &reg) != NV_ENC_SUCCESS) return out;
        m_registeredResource = reg.registeredResource;
    }

    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = m_registeredResource;

    if (m_fn.nvEncMapInputResource(m_encoder, &map) != NV_ENC_SUCCESS) {
        std::cout << "map failed\n";
        return out;
    }

    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    pic.inputWidth = m_width;
    pic.inputHeight = m_height;
    pic.outputBitstream = m_outputBuffer;

    if (m_fn.nvEncEncodePicture(m_encoder, &pic) != NV_ENC_SUCCESS) {
        std::cout << "encode failed\n";
        return out;
    }

    NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
    lock.outputBitstream = m_outputBuffer;

    m_fn.nvEncLockBitstream(m_encoder, &lock);

    uint8_t* data = (uint8_t*)lock.bitstreamBufferPtr;
    out.insert(out.end(), data, data + lock.bitstreamSizeInBytes);

    m_fn.nvEncUnlockBitstream(m_encoder, m_outputBuffer);

    m_fn.nvEncUnmapInputResource(m_encoder, map.mappedResource);

    return out;
}

// cpu copy once
std::vector<uint8_t> GpuEncoder::Encode(unsigned char* bgraDataPtr, int map_pRowPitch) {
    std::lock_guard<std::mutex> lock(m_mutex);  // ← 新增
    if (!m_ctx || !m_nvInputTex || !bgraDataPtr) return {};

    std::vector<uint8_t> out;
    // 1. 上传数据到 NVIDIA 纹理
    m_ctx->UpdateSubresource(m_nvInputTex, 0, NULL, bgraDataPtr, map_pRowPitch, 0);

    // 2. 注册资源 (只执行一次)
    //在显卡内部，为你的屏幕截图纹理（DirectX 资源）和编码器引擎之间打通一条专用的高速隧道
    //向编码器“挂号”这张纹理
    if (!m_registeredResource) {
        NV_ENC_REGISTER_RESOURCE reg = { NV_ENC_REGISTER_RESOURCE_VER };
        //这个是一个DirectX 的纹理对象
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.resourceToRegister = (void*)m_nvInputTex;
        reg.width = m_width;
        reg.height = m_height;
        reg.pitch = map_pRowPitch;
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        if (m_fn.nvEncRegisterResource(m_encoder, &reg) != NV_ENC_SUCCESS) return out;
        //这是注册成功后的“凭证”。以后你再跟编码器聊天，直接出示这个凭证，它就知道是指向哪张纹理了
        m_registeredResource = reg.registeredResource;
    }

    // 3. 映射资源
    //锁住纹理，开始传输
    NV_ENC_MAP_INPUT_RESOURCE map = { NV_ENC_MAP_INPUT_RESOURCE_VER };
    map.registeredResource = m_registeredResource;
    map.mappedBufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    if (m_fn.nvEncMapInputResource(m_encoder, &map) != NV_ENC_SUCCESS) return out;

    // 4. 配置编码参数
    NV_ENC_PIC_PARAMS pic = { NV_ENC_PIC_PARAMS_VER };
    pic.inputBuffer = map.mappedResource;
    pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    pic.inputWidth = m_width;
    pic.inputHeight = m_height;
    pic.outputBitstream = m_outputBuffer;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    // 只有每 60 帧才强制发送 SPS/PPS 和 IDR
    //SPS: Sequence Parameter Set   解码器的“说明书”
    //PPS: Picture Parameter Set    某一帧如何被压缩的具体规则
    //IDR: Instantaneous Decoding Refresh     它本质是 I 帧的加强版
    static int frameCounter = 0;
    if (frameCounter % 60 == 0) {
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }
    else {
        pic.encodePicFlags = 0;
    }
    frameCounter++;

    // 5. 执行编码
    if (m_fn.nvEncEncodePicture(m_encoder, &pic) == NV_ENC_SUCCESS) {
        NV_ENC_LOCK_BITSTREAM lock = { NV_ENC_LOCK_BITSTREAM_VER };
        lock.outputBitstream = m_outputBuffer;
        if (m_fn.nvEncLockBitstream(m_encoder, &lock) == NV_ENC_SUCCESS) {
            uint8_t* data = (uint8_t*)lock.bitstreamBufferPtr;
            out.assign(data, data + lock.bitstreamSizeInBytes);
            m_fn.nvEncUnlockBitstream(m_encoder, m_outputBuffer);
        }
    }

    m_fn.nvEncUnmapInputResource(m_encoder, map.mappedResource);
    return out;
}

void GpuEncoder::UnregisterResources() {
    if (m_registeredResource) {
        m_fn.nvEncUnregisterResource(m_encoder, m_registeredResource);
        m_registeredResource = nullptr;
    }
    if (m_outputBuffer) {
        m_fn.nvEncDestroyBitstreamBuffer(m_encoder, m_outputBuffer);
        m_outputBuffer = nullptr;
    }
    if (m_nvInputTex) {
        m_nvInputTex->Release();
        m_nvInputTex = nullptr;
    }
    // 视频处理资源也需要重建（分辨率变了）
    if (m_vProcessor) { m_vProcessor->Release();   m_vProcessor = nullptr; }
    if (m_vEnumerator) { m_vEnumerator->Release();  m_vEnumerator = nullptr; }
    if (m_nV12Tex) { m_nV12Tex->Release();      m_nV12Tex = nullptr; }
}

// -------------------------------------------------------
// [新增] ReinitEncoder：分辨率变更时重建 NVENC 编码器
// 调用前必须持有 m_mutex
// -------------------------------------------------------
bool GpuEncoder::ReinitEncoder(int newWidth, int newHeight) {
    printf("[GpuEncoder] 重建编码器: %dx%d → %dx%d\n",
        m_width, m_height, newWidth, newHeight);

    // 1. 释放旧 NVENC 资源（但保留 m_device/m_ctx/m_encoder 会话）
    UnregisterResources();

    // 2. 用新分辨率重新配置 NVENC
    //    复用已有 encode session，只需重新 ReconfigureEncoder
    NV_ENC_RECONFIGURE_PARAMS recfg = { NV_ENC_RECONFIGURE_PARAMS_VER };
    recfg.reInitEncodeParams = m_initParams;  // 从上次 Init 保留的副本出发

    // 更新分辨率
    recfg.reInitEncodeParams.encodeWidth = newWidth;
    recfg.reInitEncodeParams.encodeHeight = newHeight;
    recfg.reInitEncodeParams.darWidth = newWidth;
    recfg.reInitEncodeParams.darHeight = newHeight;

    // 强制下一帧输出 IDR + SPS/PPS，让 client 解码器能重新同步
    recfg.forceIDR = 1;

    // 更新内部 config 副本
    NV_ENC_CONFIG newConfig = m_config;
    newConfig.version = NV_ENC_CONFIG_VER;
    newConfig.rcParams.averageBitRate = m_bitrate;
    newConfig.rcParams.maxBitRate = m_bitrate * 2;
    recfg.reInitEncodeParams.encodeConfig = &newConfig;

    NVENCSTATUS st = m_fn.nvEncReconfigureEncoder(m_encoder, &recfg);
    if (st != NV_ENC_SUCCESS) {
        printf("[GpuEncoder] nvEncReconfigureEncoder 失败: %d\n", st);
        return false;
    }

    // 3. 保存新参数
    m_width = newWidth;
    m_height = newHeight;
    m_config = newConfig;
    m_initParams.encodeWidth = newWidth;
    m_initParams.encodeHeight = newHeight;

    // 4. 重建输入纹理（新分辨率）
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = newWidth;
    texDesc.Height = newHeight;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &m_nvInputTex))) {
        printf("[GpuEncoder] 重建输入纹理失败\n");
        return false;
    }

    // 5. 重建输出 bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER createBS = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    if (m_fn.nvEncCreateBitstreamBuffer(m_encoder, &createBS) != NV_ENC_SUCCESS) {
        printf("[GpuEncoder] 重建 bitstream buffer 失败\n");
        return false;
    }
    m_outputBuffer = createBS.bitstreamBuffer;

    printf("[GpuEncoder] 重建完成: %dx%d @ %d bps\n", m_width, m_height, m_bitrate);
    return true;
}

// -------------------------------------------------------
// [新增] SetBitrate：动态修改码率，无需重建编码器
// -------------------------------------------------------
bool GpuEncoder::SetBitrate(int newBitrate) {
    std::lock_guard<std::mutex> lock(m_mutex);

    NV_ENC_RECONFIGURE_PARAMS recfg = { NV_ENC_RECONFIGURE_PARAMS_VER };
    recfg.reInitEncodeParams = m_initParams;

    NV_ENC_CONFIG newConfig = m_config;
    newConfig.version = NV_ENC_CONFIG_VER;
    newConfig.rcParams.averageBitRate = newBitrate;
    newConfig.rcParams.maxBitRate = newBitrate * 2;
    newConfig.rcParams.vbvBufferSize = newBitrate * 2;
    recfg.reInitEncodeParams.encodeConfig = &newConfig;

    NVENCSTATUS st = m_fn.nvEncReconfigureEncoder(m_encoder, &recfg);
    if (st != NV_ENC_SUCCESS) {
        printf("[GpuEncoder] SetBitrate 失败: %d\n", st);
        return false;
    }

    m_bitrate = newBitrate;
    m_config = newConfig;
    printf("[GpuEncoder] Bitrate 更新: %d bps\n", m_bitrate);
    return true;
}

// -------------------------------------------------------
// [新增] SetResolution：动态修改分辨率，重建编码器
// 同时构造 CtrlPacket 供调用方发送给 Client
// -------------------------------------------------------
bool GpuEncoder::SetResolution(int newWidth, int newHeight) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (newWidth == m_width && newHeight == m_height) return true;

    return ReinitEncoder(newWidth, newHeight);
}

void GpuEncoder::Cleanup() {

    if (m_registeredResource)
        m_fn.nvEncUnregisterResource(m_encoder, m_registeredResource);

    if (m_outputBuffer)
        m_fn.nvEncDestroyBitstreamBuffer(m_encoder, m_outputBuffer);

    if (m_encoder)
        m_fn.nvEncDestroyEncoder(m_encoder);

    if (m_ctx)
        m_ctx->Release();
}   