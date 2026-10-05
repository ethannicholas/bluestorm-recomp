// Reports which adapters can create a D3D12 device, which is what Microsoft's
// OpenGLOn12 (Mesa over D3D12) needs in order to provide OpenGL.
// Build: clang++ tools/d3d12probe.cpp -o build/d3d12probe.exe -ld3d12 -ldxgi -lole32
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) {
        printf("CreateDXGIFactory1 failed\n");
        return 1;
    }

    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    static const char* names[] = {"12_2", "12_1", "12_0", "11_1", "11_0"};

    for (UINT i = 0;; i++) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        printf("adapter %u: %ls%s\n", i, desc.Description, software ? "  [SOFTWARE]" : "");

        bool any = false;
        for (int l = 0; l < 5; l++) {
            ID3D12Device* dev = nullptr;
            HRESULT hr = D3D12CreateDevice(adapter, levels[l], __uuidof(ID3D12Device), (void**)&dev);
            if (SUCCEEDED(hr)) {
                printf("    D3D12 OK at feature level %s\n", names[l]);
                dev->Release();
                any = true;
                break;
            }
        }
        if (!any) printf("    no D3D12 support\n");
        adapter->Release();
    }
    factory->Release();
    return 0;
}
