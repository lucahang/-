#pragma once

#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// GPU 渲染器：接收 NV12 纹理（数组切片），通过 D3D11 VideoProcessor 转换为
// BGRA 并 Present 到 SwapChain，全程 GPU，零 CPU 拷贝
class GpuRenderer {
public:
    GpuRenderer() = default;
    ~GpuRenderer() { Cleanup(); }

    // device: 与 GpuDecoder 共享的同一个 D3D11 设备
    bool Init(HWND hWnd, int width, int height, ID3D11Device* device);

    // nv12Texture: FFmpeg 解码出的纹理数组，arraySlice 是当前帧的切片索引
    void Render(ID3D11Texture2D* nv12Texture, UINT arraySlice);

    void Cleanup();

    ID3D11Device* GetDevice() const { return m_device.Get(); }

private:
    bool RebuildVideoProcessor(UINT inputW, UINT inputH);

    ComPtr<ID3D11Device>                   m_device;
    ComPtr<ID3D11DeviceContext>            m_ctx;
    ComPtr<IDXGISwapChain1>                m_swapChain;
    ComPtr<ID3D11VideoDevice>              m_videoDevice;
    ComPtr<ID3D11VideoContext>             m_videoContext;
    ComPtr<ID3D11VideoProcessor>           m_videoProcessor;
    ComPtr<ID3D11VideoProcessorEnumerator> m_videoEnumerator;

    int  m_width = 0;
    int  m_height = 0;
    UINT m_inputWidth = 0;
    UINT m_inputHeight = 0;
};