#include "GpuRenderer.h"
#include <cstdio>

bool GpuRenderer::Init(HWND hWnd, int width, int height, ID3D11Device* device) {
    m_width = width;
    m_height = height;

    // -------------------------------------------------------
    // 1. 接管外部 D3D11 设备
    // -------------------------------------------------------
    m_device = device; // ComPtr::operator= 会 AddRef
    m_device->GetImmediateContext(&m_ctx);

    // -------------------------------------------------------
    // 2. 开启多线程保护（解码和渲染在不同线程访问同一 Device）
    // -------------------------------------------------------
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt)))
        mt->SetMultithreadProtected(TRUE);

    // -------------------------------------------------------
    // 3. 获取 VideoDevice / VideoContext 接口
    // -------------------------------------------------------
    if (FAILED(m_device.As(&m_videoDevice))) {
        printf("[Renderer] 显卡不支持 D3D11VideoDevice\n");
        return false;
    }
    m_ctx.As(&m_videoContext);

    // -------------------------------------------------------
    // 4. 创建 SwapChain（BGRA，Flip 模式）
    // -------------------------------------------------------
    ComPtr<IDXGIDevice>  dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    m_device.As(&dxgiDev);
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = (UINT)width;
    sd.Height = (UINT)height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SampleDesc.Count = 1;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HRESULT hr = factory->CreateSwapChainForHwnd(
        m_device.Get(), hWnd, &sd, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) {
        printf("[Renderer] 创建 SwapChain 失败: 0x%08X\n", hr);
        return false;
    }

    printf("[Renderer] 初始化成功: %dx%d\n", width, height);
    return true;
}

bool GpuRenderer::RebuildVideoProcessor(UINT inputW, UINT inputH) {
    m_videoProcessor.Reset();
    m_videoEnumerator.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vcd = {};
    vcd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vcd.InputWidth = inputW;
    vcd.InputHeight = inputH;
    vcd.OutputWidth = (UINT)m_width;
    vcd.OutputHeight = (UINT)m_height;
    vcd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(m_videoDevice->CreateVideoProcessorEnumerator(&vcd, &m_videoEnumerator))) {
        printf("[Renderer] CreateVideoProcessorEnumerator 失败\n");
        return false;
    }
    if (FAILED(m_videoDevice->CreateVideoProcessor(m_videoEnumerator.Get(), 0, &m_videoProcessor))) {
        printf("[Renderer] CreateVideoProcessor 失败\n");
        return false;
    }

    // -------------------------------------------------------
    // 颜色空间设置：
    //
    // FFmpeg d3d11va 解码出的 NV12 帧：
    //   H.264 规范中 < 720 线使用 BT.601，>= 720 线使用 BT.709
    //   但 NVENC 编码时基本默认 BT.601 Limited（即使是 1080p，因为
    //   编码器按 BGRA→NV12 的默认颜色矩阵是 601）
    //
    // 最保险的做法：
    //   - 不手动设置 InputColorSpace（让驱动按 NV12 默认处理）
    //   - 只设置 OutputColorSpace 为 full range RGB
    //   这样 NVIDIA VideoProcessor 会自动选择正确的 YCbCr 矩阵
    //
    // 如果仍有颜色偏差，只需改 inputCS.YCbCr_Matrix：0=BT.601, 1=BT.709
    // -------------------------------------------------------

    // 输入颜色空间（NV12，Limited Range）
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputCS = {};
    inputCS.Usage = 0; // 0 = playback
    inputCS.RGB_Range = 1; // 对 YCbCr 输入无实际意义，置 1
    inputCS.YCbCr_Matrix = 1; // 0 = BT.601（若视频是 BT.709 改为 1）
    inputCS.YCbCr_xvYCC = 0;
    inputCS.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235; // Limited
    m_videoContext->VideoProcessorSetStreamColorSpace(m_videoProcessor.Get(), 0, &inputCS);

    // 输出颜色空间（BGRA，Full Range）
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputCS = {};
    outputCS.Usage = 0;
    outputCS.RGB_Range = 0; // 0 = full range (0–255)
    outputCS.YCbCr_Matrix = 0;
    outputCS.YCbCr_xvYCC = 0;
    outputCS.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255; // Full
    m_videoContext->VideoProcessorSetOutputColorSpace(m_videoProcessor.Get(), &outputCS);

    m_inputWidth = inputW;
    m_inputHeight = inputH;
    printf("[Renderer] VideoProcessor 重建完成: 输入 %dx%d -> 输出 %dx%d\n",
        inputW, inputH, m_width, m_height);
    return true;
}

void GpuRenderer::Render(ID3D11Texture2D* nv12Texture, UINT arraySlice) {
    if (!m_swapChain || !nv12Texture) return;

    D3D11_TEXTURE2D_DESC td;
    nv12Texture->GetDesc(&td);

    // 分辨率变化时重建 VideoProcessor
    if (!m_videoProcessor || td.Width != m_inputWidth || td.Height != m_inputHeight) {
        if (!RebuildVideoProcessor(td.Width, td.Height)) return;
    }

    // 获取后台缓冲区
    ComPtr<ID3D11Texture2D> backBuffer;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

    // -------------------------------------------------------
    // 创建输入视图（指定 ArraySlice！这是 FFmpeg 纹理数组的关键）
    // -------------------------------------------------------
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
    inDesc.FourCC = 0;
    inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inDesc.Texture2D.ArraySlice = arraySlice; // ← FFmpeg 指定的切片索引

    ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
        nv12Texture, m_videoEnumerator.Get(), &inDesc, &inputView);
    if (FAILED(hr)) {
        printf("[Renderer] CreateVideoProcessorInputView 失败: 0x%08X (slice=%u)\n", hr, arraySlice);
        return;
    }

    // -------------------------------------------------------
    // 创建输出视图（后台缓冲区，BGRA）
    // -------------------------------------------------------
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};
    outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outDesc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    hr = m_videoDevice->CreateVideoProcessorOutputView(
        backBuffer.Get(), m_videoEnumerator.Get(), &outDesc, &outputView);
    if (FAILED(hr)) {
        printf("[Renderer] CreateVideoProcessorOutputView 失败: 0x%08X\n", hr);
        return;
    }

    // -------------------------------------------------------
    // 执行 Blt（NV12 -> BGRA，GPU 全程完成，含颜色空间转换）
    // -------------------------------------------------------
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    hr = m_videoContext->VideoProcessorBlt(
        m_videoProcessor.Get(), outputView.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        printf("[Renderer] VideoProcessorBlt 失败: 0x%08X\n", hr);
        return;
    }

    m_swapChain->Present(0, 0);
}

void GpuRenderer::Cleanup() {
    m_videoProcessor.Reset();
    m_videoEnumerator.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_swapChain.Reset();
    m_ctx.Reset();
    m_device.Reset();
}