#include "screenshot.hpp"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

namespace ariel::screenshot {

namespace {

struct com_guard {
    bool initialized = false;
    com_guard() {
        initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    }
    ~com_guard() {
        if (initialized) CoUninitialize();
    }
};

}  // namespace

capture_result capture_desktop() {
    capture_result result{false, {}, {}};
    com_guard com;
    if (!com.initialized) {
        result.error = "COM init failed";
        return result;
    }

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        result.error = "no display";
        return result;
    }

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT);

    const size_t stride = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> pixels(stride * static_cast<size_t>(height));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int lines = GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    SelectObject(memory, previous);

    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    if (lines != height) {
        result.error = "pixel read failed";
        return result;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWICImagingFactory,
                                      reinterpret_cast<void**>(&factory));
    if (FAILED(status)) {
        result.error = "WIC factory unavailable";
        return result;
    }

    IWICBitmap* source = nullptr;
    status = factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                             GUID_WICPixelFormat32bppBGRA,
                                             static_cast<UINT>(stride),
                                             static_cast<UINT>(pixels.size()),
                                             pixels.data(), &source);
    if (FAILED(status)) {
        factory->Release();
        result.error = "bitmap creation failed";
        return result;
    }

    IStream* stream = nullptr;
    HGLOBAL memory_handle = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!memory_handle || FAILED(CreateStreamOnHGlobal(memory_handle, TRUE, &stream))) {
        source->Release();
        factory->Release();
        result.error = "stream creation failed";
        return result;
    }

    IWICBitmapEncoder* encoder = nullptr;
    status = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(status))
        status = encoder->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    if (SUCCEEDED(status))
        status = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(status))
        status = frame->Initialize(properties);
    if (SUCCEEDED(status))
        status = frame->WriteSource(source, nullptr);
    if (SUCCEEDED(status))
        status = frame->Commit();
    if (SUCCEEDED(status))
        status = encoder->Commit();

    if (properties) properties->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    source->Release();
    factory->Release();

    if (FAILED(status)) {
        stream->Release();
        result.error = "PNG encode failed";
        return result;
    }

    STATSTG stats{};
    status = stream->Stat(&stats, STATFLAG_NONAME);
    if (FAILED(status) || stats.cbSize.QuadPart == 0) {
        stream->Release();
        result.error = "encoded stream empty";
        return result;
    }

    void* locked = GlobalLock(memory_handle);
    if (!locked) {
        stream->Release();
        result.error = "stream lock failed";
        return result;
    }
    const auto* bytes = static_cast<const uint8_t*>(locked);
    result.data.assign(bytes, bytes + static_cast<size_t>(stats.cbSize.QuadPart));
    GlobalUnlock(memory_handle);
    stream->Release();

    result.ok = true;
    return result;
}

}  // namespace ariel::screenshot
