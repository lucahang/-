#include "ScreenCapturer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

ScreenCapturer::ScreenCapturer() {}
ScreenCapturer::~ScreenCapturer() { Cleanup(); }

bool ScreenCapturer::InitGPU() {
    HRESULT hr = S_OK;
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);

    IDXGIAdapter1* adapter = nullptr;
    // 1. 遍历适配器：寻找真正连接显示器的显卡（通常集显 Index 0）
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {

        // 创建采集设备（这里不筛选NVIDIA，为了绕过 0x887A0004 报错）
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context);

        if (FAILED(hr)) {
            adapter->Release();
            continue;
        }

        // 2. 尝试获取显示输出 (Output 0 通常为主屏幕)
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
            IDXGIOutput1* output1 = nullptr;
            //QueryInterface is like asking the val that do u have the function
            //in this situation, the val is output, and the function is IDXGIOutput1
            output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

            // 3. 关联桌面采集副本
            hr = output1->DuplicateOutput(m_device, &m_deskDupl);
            if (SUCCEEDED(hr)) {
                DXGI_OUTDUPL_DESC duplDesc;
                m_deskDupl->GetDesc(&duplDesc);
                
                converter.Init(m_device, duplDesc.ModeDesc.Width, duplDesc.ModeDesc.Height);
                
                // 4. 创建【跨显卡共享纹理】
                // 这个纹理在集显里创建，但可以通过 SharedHandle 被独显读取
                D3D11_TEXTURE2D_DESC stDesc = { 0 };
                stDesc.Width = duplDesc.ModeDesc.Width;
                stDesc.Height = duplDesc.ModeDesc.Height;
                stDesc.MipLevels = 1;
                stDesc.ArraySize = 1;
                stDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // 采集默认格式
                stDesc.SampleDesc.Count = 1;
                stDesc.Usage = D3D11_USAGE_DEFAULT;
                stDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                stDesc.CPUAccessFlags = 0;

                // ⭐ 核心修改：添加跨显卡共享标志
                //stDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                //    D3D11_RESOURCE_MISC_SHARED;
                //using the device to create the texture
                hr = m_device->CreateTexture2D(&stDesc, nullptr, &m_stagingTex);

                if (SUCCEEDED(hr)) {
                    // 初始化成功，清理临时对象并返回
                    output1->Release();
                    output->Release();
                    adapter->Release();
                    factory->Release();
                    return true;
                }
                else {
                    std::cout << "CreateTexture2D error" << hr << std::endl;
                }
            }
            if (output1) output1->Release();
            output->Release();
        }

        // 如果此显卡虽然能创建Device但不能采集（比如没接显示器），则清理后试下一块
        if (m_context) m_context->ClearState();
        if (m_device) { m_device->Release(); m_device = nullptr; }
        adapter->Release();
    }
    
    if (factory) factory->Release();
    return false;
}

/**
 * @brief 将采集到的集显纹理“桥接”到另一个设备（如独显）
 * @param targetDevice 目标设备（即 NVIDIA 编码器的 m_device）
 * @param outTexture 转换后的独显版纹理指针（输出参数）
 * @return HRESULT 成功返回 S_OK
 */
HRESULT ScreenCapturer::GetSharedTextureForDevice(ID3D11Device* targetDevice, ID3D11Texture2D** outTexture) {
    if (!m_stagingTex || !targetDevice) { 
        std::cout << "m_stagingTex or targetDevice error" << std::endl;
        return E_INVALIDARG; 
    }

    // 1. 转为 Resource1 接口
    IDXGIResource1* pRes1 = nullptr;
    m_stagingTex->QueryInterface(__uuidof(IDXGIResource1), (void**)&pRes1);

    // 2. 创建 NT 句柄 (注意：不再是 GetSharedHandle)
    HANDLE sharedHandle = nullptr;
    HRESULT hr = pRes1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedHandle);
    pRes1->Release();
    
    if (FAILED(hr)) {
        std::cout << "GetSharedHandle error"<<hr << std::endl;
        return hr;
    }
    std::cout << "Handle Value: " << sharedHandle << std::endl;
    
    // 3. 在 NVIDIA 设备上打开 (注意：必须使用 OpenSharedResource1)
    ID3D11Device1* targetDevice1 = nullptr;
    targetDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&targetDevice1);

    hr = targetDevice1->OpenSharedResource1(sharedHandle, __uuidof(ID3D11Texture2D), (void**)outTexture);
    targetDevice1->Release();
    return hr;
}

bool ScreenCapturer::Init() {
    HRESULT hr = S_OK;
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) !=
        DXGI_ERROR_NOT_FOUND; ++i) {
        // 尝试创建设备
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
        if (FAILED(hr)) { adapter->Release(); continue; }

        // 寻找输出口
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
            IDXGIOutput1* output1 = nullptr;
            output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

            hr = output1->DuplicateOutput(m_device, &m_deskDupl);
            if (SUCCEEDED(hr)) {
                DXGI_OUTDUPL_DESC duplDesc;
                m_deskDupl->GetDesc(&duplDesc);

                // 创建中转纹理 (卸货区)
                D3D11_TEXTURE2D_DESC stDesc = { 0 };
                stDesc.Width = duplDesc.ModeDesc.Width;
                stDesc.Height = duplDesc.ModeDesc.Height;
                stDesc.MipLevels = 1;
                stDesc.ArraySize = 1;
                stDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                stDesc.SampleDesc.Count = 1;
                stDesc.Usage = D3D11_USAGE_STAGING;
                stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

                m_device->CreateTexture2D(&stDesc, nullptr, &m_stagingTex);

                output1->Release();
                output->Release();
                adapter->Release();
                factory->Release();
                return true;
            }
            if (output1) output1->Release();
            output->Release();
        }
        Cleanup(); // 如果这块显卡不行，清理后看下一块
        adapter->Release();
    }
    if (factory) factory->Release();
    return false;
}

ID3D11Device* ScreenCapturer::getDevice() {
    return m_device;
}

//零拷贝的思路
unsigned char* ScreenCapturer::CaptureNextFrameTex(CaptureInfo& info) {
    if (!m_deskDupl) {
        std::cout << "m_deskDupl error" << std::endl;
        return nullptr;
    }
    //it just is resource,we don't know what it is exactly
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    // 1. 获取硬件帧（数据在显存中）
    HRESULT hr = m_deskDupl->AcquireNextFrame(16, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        printf("AcquireNextFrame error! HRESULT: 0x%08X\n", hr);
        return nullptr;
    }

    m_deskDupl->ReleaseFrame();
    m_isFrameAcquired = true;

    // 2. 获取接口
    ID3D11Texture2D* gpuTex = nullptr;
    desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gpuTex);
    desktopResource->Release();

    if (gpuTex) {
        // ⭐ 关键：通过 Desc 获取元数据，而不是通过 Map 获取内存地址
        D3D11_TEXTURE2D_DESC desc;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // 必须有这一行！
        gpuTex->GetDesc(&desc);
        
        info.width = desc.Width;
        info.height = desc.Height;
        info.format = desc.Format;
        // 在零拷贝中，这个值仅供参考
        // 实际上 GPU 内部拷贝不依赖这个手动计算的值
        //info.rowPitch = desc.Width * 4;
    }
    converter.ConvertRGBToNV12(m_context, gpuTex, m_stagingTex);
    ID3D11Resource* intelNV12Tex=nullptr;
    m_context->CopyResource(m_stagingTex, intelNV12Tex);
    // 2. 映射 Intel 显存，让 CPU 获得指针
    D3D11_MAPPED_SUBRESOURCE mapped;
    unsigned char*   pData=nullptr;
    if (SUCCEEDED(m_context->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
        D3D11_TEXTURE2D_DESC desc;
        m_stagingTex->GetDesc(&desc);

        info.width = desc.Width;
        info.height = desc.Height;
        info.rowPitch = mapped.RowPitch;
        pData = (unsigned char*)mapped.pData;
    }

    gpuTex->Release();
    desktopResource->Release();

    return pData;
}

unsigned char* ScreenCapturer::CaptureNextFrame(CaptureInfo& info) {
    if (!m_deskDupl) return nullptr;

    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    // 获取下一帧
    HRESULT hr = m_deskDupl->AcquireNextFrame(10, &frameInfo, &desktopResource);
    if (FAILED(hr)) return nullptr;

    m_isFrameAcquired = true;

    ID3D11Texture2D* gpuTex = nullptr;
    desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gpuTex);

    // 显存 -> 卸货区
    m_context->CopyResource(m_stagingTex, gpuTex);

    D3D11_MAPPED_SUBRESOURCE mapped;
    unsigned char* pData = nullptr;
    if (SUCCEEDED(m_context->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
        D3D11_TEXTURE2D_DESC desc;
        m_stagingTex->GetDesc(&desc);

        info.width = desc.Width;
        info.height = desc.Height;
        info.rowPitch = mapped.RowPitch;
        pData = (unsigned char*)mapped.pData;
    }

    gpuTex->Release();
    desktopResource->Release();
    return pData;
}

void ScreenCapturer::ReleaseFrame() {
    if (m_isFrameAcquired) {
        m_context->Unmap(m_stagingTex, 0);
        m_deskDupl->ReleaseFrame();
        m_isFrameAcquired = false;
    }
}

void ScreenCapturer::ReleaseFrameGPU() {
    if (m_isFrameAcquired) {
        //m_context->Unmap(m_stagingTex, 0);
        m_deskDupl->ReleaseFrame();
        m_isFrameAcquired = false;
    }
}

void ScreenCapturer::Cleanup() {
    if (m_stagingTex) { m_stagingTex->Release(); m_stagingTex = nullptr; }
    if (m_deskDupl) { m_deskDupl->Release(); m_deskDupl = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

