#include "dda_capture.h"

#include <dxgi1_6.h>
#include <windows.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

DdaCapture::~DdaCapture() {
    releaseFrame();
}

bool DdaCapture::init(uint32_t outputIndex, std::wstring& err) {
    releaseFrame();
    m_dupl.Reset();

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        err = L"CreateDXGIFactory1 failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    uint32_t seen = 0;
    bool found = false;
    for (UINT a = 0; ; ++a) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
        if (factory->EnumAdapters(a, &ad) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0; ; ++o) {
            Microsoft::WRL::ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            if (seen == outputIndex) {
                if (FAILED(out.As(&output1))) {
                    err = L"IDXGIOutput1 unsupported";
                    return false;
                }
                adapter = ad;
                found = true;
                break;
            }
            ++seen;
        }
        if (found)
            break;
    }
    if (!found) {
        err = L"Output index not found";
        return false;
    }

    DXGI_OUTPUT_DESC od = {};
    output1->GetDesc(&od);
    m_width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
    m_height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    m_outputLeft = od.DesktopCoordinates.left;
    m_outputTop = od.DesktopCoordinates.top;

    D3D11_CREATE_DEVICE_FLAG flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &m_device, nullptr, &m_ctx);
    if (FAILED(hr)) {
        err = L"D3D11CreateDevice failed";
        return false;
    }

    hr = output1->DuplicateOutput(m_device.Get(), &m_dupl);
    if (FAILED(hr)) {
        err = L"DuplicateOutput failed";
        return false;
    }

    // Create a writable output texture (same format as frame, but copy-dest + render-target)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_width;
    td.Height = m_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_output))) {
        err = L"CreateTexture2D(output) failed";
        return false;
    }
    return true;
}

int DdaCapture::acquire(uint32_t timeoutMs, ID3D11Texture2D** tex, std::wstring& err) {
    releaseFrame();

    DXGI_OUTDUPL_FRAME_INFO fi = {};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = m_dupl->AcquireNextFrame(timeoutMs, &fi, &resource);

    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    const bool cursorShown = GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING && ci.hCursor;
    const int curX = ci.ptScreenPos.x - m_outputLeft;
    const int curY = ci.ptScreenPos.y - m_outputTop;
    const bool cursorMoved = cursorShown && (curX != m_lastCursorX || curY != m_lastCursorY ||
                                             ci.hCursor != m_lastCursorHandle);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new desktop frame. If cursor moved, redraw it on cached content.
        if (!cursorMoved || !m_hasContent)
            return 0;
        drawCursor(ci, curX, curY);
        *tex = m_output.Get();
        return 1;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        err = L"DXGI access lost";
        return -1;
    }
    if (FAILED(hr)) {
        err = L"AcquireNextFrame failed";
        return -1;
    }

    if (FAILED(resource.As(&m_frame))) {
        m_dupl->ReleaseFrame();
        return 0;
    }

    m_ctx->CopyResource(m_output.Get(), m_frame.Get());
    m_hasContent = true;

    if (cursorShown)
        drawCursor(ci, curX, curY);

    *tex = m_output.Get();
    return 1;
}

void DdaCapture::drawCursor(const CURSORINFO& ci, int cx, int cy) {
    m_lastCursorX = ci.ptScreenPos.x - m_outputLeft;
    m_lastCursorY = ci.ptScreenPos.y - m_outputTop;
    m_lastCursorHandle = ci.hCursor;

    ICONINFO ii = {};
    if (!GetIconInfo(ci.hCursor, &ii))
        return;
    const int hotX = (int)ii.xHotspot;
    const int hotY = (int)ii.yHotspot;

    BITMAP bmColor = {}, bmMask = {};
    GetObject(ii.hbmColor, sizeof(bmColor), &bmColor);
    GetObject(ii.hbmMask, sizeof(bmMask), &bmMask);
    int cw = bmColor.bmWidth ? bmColor.bmWidth : bmMask.bmWidth / 2;
    int ch = bmColor.bmHeight ? bmColor.bmHeight : bmMask.bmHeight / 2;
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (cw <= 0 || ch <= 0) return;

    int px = cx - hotX;
    int py = cy - hotY;

    // Clip cursor rect to output
    int sx = 0, sy = 0;
    if (px < 0) { sx = -px; cw += px; px = 0; }
    if (py < 0) { sy = -py; ch += py; py = 0; }
    if (px + cw > (int)m_width)  cw = (int)m_width - px;
    if (py + ch > (int)m_height) ch = (int)m_height - py;
    if (cw <= 0 || ch <= 0) return;

    // Render cursor into small top-down 32bpp DIB
    int fullW = bmColor.bmWidth ? bmColor.bmWidth : bmMask.bmWidth / 2;
    int fullH = bmColor.bmHeight ? bmColor.bmHeight : bmMask.bmHeight / 2;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = fullW;
    bmi.bmiHeader.biHeight = -fullH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        memset(bits, 0, (size_t)fullW * fullH * 4);
        HGDIOBJ old = SelectObject(mem, bmp);
        DrawIconEx(mem, 0, 0, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        SelectObject(mem, old);

        if (!m_cursorStaging) {
            D3D11_TEXTURE2D_DESC sd = {};
            m_output->GetDesc(&sd);
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            m_device->CreateTexture2D(&sd, nullptr, &m_cursorStaging);
        }
        if (m_cursorStaging) {
            // Sync staging with current output
            m_ctx->CopyResource(m_cursorStaging.Get(), m_output.Get());
            m_ctx->Flush();

            D3D11_MAPPED_SUBRESOURCE mr = {};
            if (SUCCEEDED(m_ctx->Map(m_cursorStaging.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mr))) {
                for (int y = 0; y < ch; y++) {
                    const uint8_t* srow = (const uint8_t*)bits + (size_t)(sy + y) * fullW * 4;
                    uint8_t* drow = (uint8_t*)mr.pData + (size_t)(py + y) * mr.RowPitch;
                    for (int x = 0; x < cw; x++) {
                        const uint8_t* s = srow + (size_t)(sx + x) * 4;
                        uint8_t a = s[3];
                        if (a == 0) continue;
                        uint8_t* d = drow + (size_t)(px + x) * 4;
                        if (a == 255) {
                            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                        } else {
                            // alpha blend over dest (GDI is premultiplied)
                            d[0] = (uint8_t)(s[0] + d[0] * (255 - a) / 255);
                            d[1] = (uint8_t)(s[1] + d[1] * (255 - a) / 255);
                            d[2] = (uint8_t)(s[2] + d[2] * (255 - a) / 255);
                        }
                        d[3] = 255;
                    }
                }
                m_ctx->Unmap(m_cursorStaging.Get(), 0);
                D3D11_BOX box = { (UINT)px, (UINT)py, 0,
                                  (UINT)(px + cw), (UINT)(py + ch), 1 };
                m_ctx->CopySubresourceRegion(m_output.Get(), 0,
                                             (UINT)px, (UINT)py, 0,
                                             m_cursorStaging.Get(), 0, &box);
            }
        }
        DeleteObject(bmp);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void DdaCapture::releaseFrame() {
    if (m_frame) {
        m_frame.Reset();
        if (m_dupl)
            m_dupl->ReleaseFrame();
    }
}
