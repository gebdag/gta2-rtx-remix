// Captures what is actually on the display, via DXGI output duplication.
// GDI's CopyFromScreen returns black for both fullscreen-exclusive D3D and
// Vulkan swap chains, so it cannot tell "our window is covered" from "our
// window is showing"; this can.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "screen.bmp";

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        printf("D3D11CreateDevice failed\n");
        return 1;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->EnumOutputs(0, &output);
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    HRESULT hr = output1->DuplicateOutput(device, &duplication);
    if (FAILED(hr)) {
        printf("DuplicateOutput failed 0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    // The first frames after duplication starts are often empty; take a few.
    ID3D11Texture2D* frame = nullptr;
    for (int attempt = 0; attempt < 30; ++attempt) {
        DXGI_OUTDUPL_FRAME_INFO info = {};
        IDXGIResource* resource = nullptr;
        hr = duplication->AcquireNextFrame(1000, &info, &resource);
        if (SUCCEEDED(hr)) {
            if (frame) frame->Release();
            frame = nullptr;
            resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frame);
            resource->Release();
            duplication->ReleaseFrame();
            if (info.LastPresentTime.QuadPart != 0 && attempt > 3) break;
        }
        Sleep(30);
    }
    if (!frame) {
        printf("no frame captured\n");
        return 1;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    frame->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        printf("staging texture failed\n");
        return 1;
    }
    context->CopyResource(staging, frame);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        printf("map failed\n");
        return 1;
    }

    // Downscale by 3 so the file stays small, and report how much of it is not
    // black - the whole point of the capture.
    const int step = 3;
    const int width = desc.Width / step;
    const int height = desc.Height / step;
    std::vector<uint8_t> pixels((size_t)width * height * 3);
    long long lit = 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = (const uint8_t*)mapped.pData + (size_t)(y * step) * mapped.RowPitch;
        uint8_t* dest = &pixels[(size_t)(height - 1 - y) * width * 3];  // BMP is bottom-up
        for (int x = 0; x < width; ++x) {
            const uint8_t* src = row + (size_t)(x * step) * 4;  // BGRA
            dest[x * 3 + 0] = src[0];
            dest[x * 3 + 1] = src[1];
            dest[x * 3 + 2] = src[2];
            if (src[0] > 12 || src[1] > 12 || src[2] > 12) ++lit;
        }
    }
    context->Unmap(staging, 0);

    const int rowBytes = (width * 3 + 3) & ~3;
    const int padding = rowBytes - width * 3;
    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + rowBytes * height;
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 24;
    FILE* file = fopen(out, "wb");
    if (!file) { printf("cannot write %s\n", out); return 1; }
    fwrite(&fileHeader, sizeof(fileHeader), 1, file);
    fwrite(&infoHeader, sizeof(infoHeader), 1, file);
    const uint8_t zero[4] = {};
    for (int y = 0; y < height; ++y) {
        fwrite(&pixels[(size_t)y * width * 3], 3, width, file);
        if (padding) fwrite(zero, 1, padding, file);
    }
    fclose(file);

    printf("captured %dx%d (source %ux%u) -> %s; %.1f%% of pixels are not black\n", width, height,
           desc.Width, desc.Height, out, 100.0 * lit / ((double)width * height));
    return 0;
}
