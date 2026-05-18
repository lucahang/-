#pragma once
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <d3d11_1.h>
#include "VideoConverter.h"
// 定义一个结构体，方便主程序获取当前屏幕的状态
struct CaptureInfo {
    int width;
    int height;
    DXGI_FORMAT format;
    int rowPitch;
};

class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    // 初始化：自动寻找显卡并建立采集通道
    bool InitGPU();
    bool Init();
    unsigned char* CaptureNextFrameTex(CaptureInfo& info);
    // 获取下一帧：返回指向像素数据的指针
    // 注意：返回的指针在下一次调用 CaptureNextFrame 或 Unmap 之前有效
    unsigned char* CaptureNextFrame(CaptureInfo& info);
    HRESULT GetSharedTextureForDevice(ID3D11Device* targetDevice, ID3D11Texture2D** outTexture);
    // 释放当前帧（必须在处理完像素后调用）
    void ReleaseFrame();
    void ReleaseFrameGPU();
    // 清理所有资源
    void Cleanup();
    ID3D11Device* getDevice();
private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIOutputDuplication* m_deskDupl = nullptr;
    ID3D11Texture2D* m_stagingTex = nullptr;
    VideoConverter converter;
    bool m_isFrameAcquired = false;
};

