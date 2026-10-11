#include "dda_capture.h"

#include <dxgi1_6.h>
#include <windows.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

DdaCapture::~DdaCapture() {
    releaseFrame();
    if (m_cursorDib) DeleteObject(m_cursorDib);
    if (m_cursorMemDC) DeleteDC(m_cursorMemDC);
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

    // Query current display mode for refresh rate
    DXGI_MODE_DESC modeDesc = {};
    modeDesc.Width = m_width;
    modeDesc.Height = m_height;
    modeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_MODE_DESC matched = {};
    if (SUCCEEDED(output1->FindClosestMatchingMode(&modeDesc, &matched, nullptr)) && matched.RefreshRate.Denominator) {
        m_refreshRate = matched.RefreshRate.Numerator / matched.RefreshRate.Denominator;
    }

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

    // Re-render cursor bitmap only when cursor shape changes
    if (ci.hCursor != m_cachedCursorIcon) {
        m_cachedCursorIcon = ci.hCursor;

        ICONINFO ii = {};
        if (!GetIconInfo(ci.hCursor, &ii))
            return;

        BITMAP bmColor = {}, bmMask = {};
        GetObject(ii.hbmColor, sizeof(bmColor), &bmColor);
        GetObject(ii.hbmMask, sizeof(bmMask), &bmMask);
        m_cursorW = bmColor.bmWidth ? bmColor.bmWidth : bmMask.bmWidth / 2;
        m_cursorH = bmColor.bmHeight ? bmColor.bmHeight : bmMask.bmHeight / 2;
        m_cursorHotX = (int)ii.xHotspot;
        m_cursorHotY = (int)ii.yHotspot;
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);

        if (m_cursorW <= 0 || m_cursorH <= 0) return;

        // Create cached DIB and render cursor into it
        if (m_cursorDib) { DeleteObject(m_cursorDib); m_cursorDib = nullptr; }
        if (!m_cursorMemDC) {
            HDC screen = GetDC(nullptr);
            m_cursorMemDC = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
        }

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_cursorW;
        bmi.bmiHeader.biHeight = -m_cursorH; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_cursorDib = CreateDIBSection(m_cursorMemDC, &bmi, DIB_RGB_COLORS, &m_cursorBits, nullptr, 0);
        if (!m_cursorDib || !m_cursorBits) return;

        memset(m_cursorBits, 0, (size_t)m_cursorW * m_cursorH * 4);
        HGDIOBJ old = SelectObject(m_cursorMemDC, m_cursorDib);
        DrawIconEx(m_cursorMemDC, 0, 0, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        SelectObject(m_cursorMemDC, old);
    }

    if (!m_cursorBits) return;

    int px = cx - m_cursorHotX;
    int py = cy - m_cursorHotY;

    // Clip cursor rect to output
    int sx = 0, sy = 0;
    int cw = m_cursorW, ch = m_cursorH;
    if (px < 0) { sx = -px; cw += px; px = 0; }
    if (py < 0) { sy = -py; ch += py; py = 0; }
    if (px + cw > (int)m_width)  cw = (int)m_width - px;
    if (py + ch > (int)m_height) ch = (int)m_height - py;
    if (cw <= 0 || ch <= 0) return;

    // Use small staging texture sized to cursor (not full frame)
    if (!m_cursorStaging) {
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = m_cursorW;
        sd.Height = m_cursorH;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        m_device->CreateTexture2D(&sd, nullptr, &m_cursorStaging);
    }
    if (!m_cursorStaging) return;

    // Copy the small cursor region from output to staging (GPU)
    // Source: output at (px, py), size (cw, ch) -> staging at (0, 0)
    D3D11_BOX srcBox = { (UINT)px, (UINT)py, 0, (UINT)(px + cw), (UINT)(py + ch), 1 };
    m_ctx->CopySubresourceRegion(m_cursorStaging.Get(), 0,
                                 0, 0, 0,
                                 m_output.Get(), 0, &srcBox);
    m_ctx->Flush();

    // Alpha blend cursor over the small region (CPU, fast — only ~32x32 pixels)
    D3D11_MAPPED_SUBRESOURCE mr = {};
    if (SUCCEEDED(m_ctx->Map(m_cursorStaging.Get(), 0, D3D11_MAP_READ_WRITE, 0, &mr))) {
        for (int y = 0; y < ch; y++) {
            const uint8_t* srow = (const uint8_t*)m_cursorBits + (size_t)(sy + y) * m_cursorW * 4;
            uint8_t* drow = (uint8_t*)mr.pData + (size_t)y * mr.RowPitch;
            for (int x = 0; x < cw; x++) {
                const uint8_t* s = srow + (size_t)(sx + x) * 4;
                uint8_t a = s[3];
                if (a == 0) continue;
                uint8_t* d = drow + (size_t)x * 4;
                if (a == 255) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                } else {
                    // Premultiplied alpha blend (GDI outputs premultiplied)
                    d[0] = (uint8_t)(s[0] + d[0] * (255 - a) / 255);
                    d[1] = (uint8_t)(s[1] + d[1] * (255 - a) / 255);
                    d[2] = (uint8_t)(s[2] + d[2] * (255 - a) / 255);
                }
                d[3] = 255;
            }
        }
        m_ctx->Unmap(m_cursorStaging.Get(), 0);
        // Copy blended region back to output (GPU)
        // Source: staging at (0, 0), size (cw, ch) -> output at (px, py)
        D3D11_BOX dstBox = { 0, 0, 0, (UINT)cw, (UINT)ch, 1 };
        m_ctx->CopySubresourceRegion(m_output.Get(), 0,
                                     (UINT)px, (UINT)py, 0,
                                     m_cursorStaging.Get(), 0, &dstBox);
    }
}

void DdaCapture::releaseFrame() {
    if (m_frame) {
        m_frame.Reset();
        if (m_dupl)
            m_dupl->ReleaseFrame();
    }
}
