#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.h>
#include <cstdint>
#include <string>

// DXGI Desktop Duplication capture on a chosen output (display).
// Produces BGRA ID3D11Texture2D frames.
class DdaCapture {
public:
    ~DdaCapture();

    // outputIndex: DXGI output index (0-based, across adapters).
    // Returns false and fills err on failure.
    bool init(uint32_t outputIndex, std::wstring& err);
    // Acquires next frame. Returns:
    //   1 = new frame (tex valid until releaseFrame)
    //   0 = timeout / no new frame
    //  -1 = access lost (display mode changed; re-init needed)
    int acquire(uint32_t timeoutMs, ID3D11Texture2D** tex, std::wstring& err);
    void releaseFrame();

    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    ID3D11Device* device() const { return m_device.Get(); }

private:
    void drawCursor(const CURSORINFO& ci, int cx, int cy);
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dupl;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frame;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_output;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_cursorStaging;
    uint32_t m_width = 0, m_height = 0;
    int m_outputLeft = 0, m_outputTop = 0;
    int m_lastCursorX = -9999, m_lastCursorY = -9999;
    HCURSOR m_lastCursorHandle = nullptr;
    bool m_hasContent = false; // m_output has at least one full frame
};
