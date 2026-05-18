#pragma once
#include <d3d11_1.h>
#include <wrl/client.h> // 方便使用 Microsoft::WRL::ComPtr

using Microsoft::WRL::ComPtr;

class VideoConverter {
public:
    VideoConverter() = default;

    // 初始化转换器
    bool Init(ID3D11Device* device, int width, int height) {
        m_width = width;
        m_height = height;

        // 1. 获取视频设备接口
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m_videoDevice)))) return false;

        // 2. 配置转换描述符 (BGRA -> NV12)
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = { 60, 1 };
        desc.InputWidth = width;
        desc.InputHeight = height;
        desc.OutputFrameRate = { 60, 1 };
        desc.OutputWidth = width;
        desc.OutputHeight = height;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        // 3. 创建枚举器
        if (FAILED(m_videoDevice->CreateVideoProcessorEnumerator(&desc, &m_enumerator))) return false;

        // 4. 创建视频处理器
        if (FAILED(m_videoDevice->CreateVideoProcessor(m_enumerator.Get(), 0, &m_processor))) return false;

        return true;
    }

    // 执行转换逻辑
    bool ConvertRGBToNV12(ID3D11DeviceContext* context, ID3D11Texture2D* inputRGB, ID3D11Texture2D* outputNV12) {
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        ComPtr<ID3D11VideoProcessorOutputView> outputView;

        // --- 修正后的 InputView 配置 ---
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc;
        ZeroMemory(&inputDesc, sizeof(inputDesc));

        inputDesc.FourCC = 0; // 使用纹理默认格式
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = 0;

        // --- 修正后的 OutputView 配置 ---
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc;
        ZeroMemory(&outputDesc, sizeof(outputDesc));

        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = 0;

        // 创建视图
        if (FAILED(m_videoDevice->CreateVideoProcessorInputView(inputRGB, m_enumerator.Get(), &inputDesc, &inputView))) return false;
        if (FAILED(m_videoDevice->CreateVideoProcessorOutputView(outputNV12, m_enumerator.Get(), &outputDesc, &outputView))) return false;

        // 配置流信息
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = inputView.Get();

        // 这一步在 GPU 硬件上完成转换，极快
        ComPtr<ID3D11VideoContext> videoContext;
        if (FAILED(context->QueryInterface(IID_PPV_ARGS(&videoContext)))) return false;

        videoContext->VideoProcessorBlt(m_processor.Get(), outputView.Get(), 0, 1, &stream);

        return true;
    }

private:
    int m_width = 0;
    int m_height = 0;
    ComPtr<ID3D11VideoDevice> m_videoDevice;
    ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
    ComPtr<ID3D11VideoProcessor> m_processor;
};