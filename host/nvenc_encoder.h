#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <functional>
#include <string>

#include <ffnvcodec/nvEncodeAPI.h>

// NVENC H.265 encoder. Creates its own D3D11 device on the NVIDIA GPU
// (the DDA capture device may be on a different adapter, e.g. IddCx virtual display).
// Copies frames from the source device to the NVIDIA device before encoding.
class NvencEncoder {
public:
    using PacketSink = std::function<void(const uint8_t* data, size_t size, bool keyframe, bool config)>;

    ~NvencEncoder();

    // srcDevice: the D3D11 device that owns the capture texture (may NOT be NVIDIA).
    // NVENC creates its own NVIDIA device internally.
    bool init(ID3D11Device* srcDevice, uint32_t width, uint32_t height,
              uint32_t fps, uint32_t qp, std::wstring& err);
    // Copies bgraTex from src device to NVIDIA device, encodes, calls sink per packet.
    bool encode(ID3D11Texture2D* bgraTex, bool forceIdr, PacketSink sink, std::wstring& err);
    void requestSpsPps();
    bool spsPpsSent() const { return m_spsPpsSent; }
    bool sameAdapter() const { return m_sameAdapter; }
    bool usingSharedGpu() const { return m_useShared; }

private:
    bool createNvidiaDevice(std::wstring& err);
    bool createInputBuffers(std::wstring& err);
    bool createBitstreamBuffer(std::wstring& err);
    bool createConverter(uint32_t w, uint32_t h, std::wstring& err);

    // Source device (from DDA — may be a different adapter)
    Microsoft::WRL::ComPtr<ID3D11Device> m_srcDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_srcCtx;

    // NVIDIA device (for NVENC)
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;

    // CPU staging fallback
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_upload;

    // GPU shared texture (src -> NVIDIA, no CPU copy)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_shared;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedNv;
    HANDLE m_sharedHandle = nullptr;
    bool m_useShared = false;
    bool m_sameAdapter = false;
    bool m_useSharedLogged = false;

    // Cached video processor views (avoid recreating every frame)
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_cachedInViews[4];
    ID3D11Texture2D* m_cachedInTex[4] = {};
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_cachedOutViews[4];
    ID3D11Texture2D* m_cachedOutTex[4] = {};

    NV_ENCODE_API_FUNCTION_LIST* m_api = nullptr;
    HMODULE m_dll = nullptr;
    void* m_encoder = nullptr;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_fps = 60;
    bool m_spsPpsSent = false;
    bool m_pendingSpsPps = true;

    struct InputSurface {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        NV_ENC_REGISTERED_PTR registered = nullptr;
    };
    std::vector<InputSurface> m_inputs;
    size_t m_nextInput = 0;

    NV_ENC_OUTPUT_PTR m_bitstream = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoCtx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_venum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_videoProc;
};
