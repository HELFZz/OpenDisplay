#include "nvenc_encoder.h"

#include <windows.h>
#include <dxgi1_2.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

bool check(NVENCSTATUS s, const wchar_t* what, std::wstring& err) {
    if (s != NV_ENC_SUCCESS) {
        wchar_t buf[160];
        swprintf_s(buf, L"%s failed: NVENCSTATUS %d", what, (int)s);
        err = buf;
        return true; // failed
    }
    return false;
}

} // namespace

NvencEncoder::~NvencEncoder() {
    if (m_encoder && m_api) {
        for (auto& in : m_inputs) {
            if (in.registered)
                m_api->nvEncUnregisterResource(m_encoder, in.registered);
        }
        if (m_bitstream)
            m_api->nvEncDestroyBitstreamBuffer(m_encoder, m_bitstream);
        m_api->nvEncDestroyEncoder(m_encoder);
    }
    if (m_dll)
        FreeLibrary(m_dll);
    delete m_api;
}

bool NvencEncoder::createNvidiaDevice(std::wstring& err) {
    // Find the NVIDIA adapter
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        err = L"CreateDXGIFactory1 failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> nvAdapter;
    for (UINT i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
        if (factory->EnumAdapters(i, &ad) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC desc = {};
        ad->GetDesc(&desc);
        if (wcsstr(desc.Description, L"NVIDIA") != nullptr) {
            nvAdapter = ad;
            break;
        }
    }
    if (!nvAdapter) {
        err = L"NVIDIA adapter not found";
        return false;
    }

    D3D11_CREATE_DEVICE_FLAG flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nvAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags, levels, 2, D3D11_SDK_VERSION,
                                   &m_device, nullptr, &m_ctx);
    if (FAILED(hr)) {
        err = L"D3D11CreateDevice(NVIDIA) failed";
        return false;
    }
    return true;
}

bool NvencEncoder::init(ID3D11Device* srcDevice, uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t qp, std::wstring& err) {
    m_srcDevice = srcDevice;
    srcDevice->GetImmediateContext(&m_srcCtx);
    m_width = width;
    m_height = height;
    m_fps = fps ? fps : 60;

    // Create NVIDIA device for NVENC
    if (!createNvidiaDevice(err))
        return false;

    // Check if source is already on NVIDIA (skip cross-adapter copy)
    Microsoft::WRL::ComPtr<IDXGIDevice> srcDxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> srcAdapter;
    Microsoft::WRL::ComPtr<IDXGIDevice> nvDxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> nvAdapter;
    if (SUCCEEDED(srcDevice->QueryInterface(IID_PPV_ARGS(&srcDxgi))) && SUCCEEDED(srcDxgi->GetAdapter(&srcAdapter)) &&
        SUCCEEDED(m_device.As(&nvDxgi)) && SUCCEEDED(nvDxgi->GetAdapter(&nvAdapter))) {
        DXGI_ADAPTER_DESC srcDesc = {}, nvDesc = {};
        srcAdapter->GetDesc(&srcDesc);
        nvAdapter->GetDesc(&nvDesc);
        if (srcDesc.AdapterLuid.HighPart == nvDesc.AdapterLuid.HighPart &&
            srcDesc.AdapterLuid.LowPart == nvDesc.AdapterLuid.LowPart) {
            m_sameAdapter = true;
        }
    }

    m_dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_dll) {
        err = L"nvEncodeAPI64.dll not found";
        return false;
    }
    auto createInstance = reinterpret_cast< decltype(&NvEncodeAPICreateInstance) >(
        GetProcAddress(m_dll, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        err = L"NvEncodeAPICreateInstance missing";
        return false;
    }
    auto api = new NV_ENCODE_API_FUNCTION_LIST{};
    api->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (createInstance(api) != NV_ENC_SUCCESS) {
        err = L"NvEncodeAPICreateInstance failed";
        delete api;
        return false;
    }
    m_api = api;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS ses = {};
    ses.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    ses.apiVersion = NVENCAPI_VERSION;
    ses.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    ses.device = m_device.Get();
    if (check(api->nvEncOpenEncodeSessionEx(&ses, &m_encoder), L"nvEncOpenEncodeSessionEx", err))
        return false;

    // Skip preset config — build config manually (avoids version issues)
    NV_ENC_CONFIG cfg = {};
    cfg.version = NV_ENC_CONFIG_VER;
    cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    cfg.gopLength = m_fps * 2;
    cfg.frameIntervalP = 1;
    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    cfg.rcParams.constQP.qpIntra = qp;
    cfg.rcParams.constQP.qpInterP = qp;
    cfg.rcParams.constQP.qpInterB = qp;
    cfg.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    cfg.encodeCodecConfig.hevcConfig.idrPeriod = m_fps * 2;
    cfg.encodeCodecConfig.hevcConfig.chromaFormatIDC = 1;
    cfg.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 = 0;

    NV_ENC_INITIALIZE_PARAMS init = {};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_HEVC_GUID;
    init.presetGUID = NV_ENC_PRESET_P4_GUID;
    init.encodeWidth = width;
    init.encodeHeight = height;
    init.darWidth = width;
    init.darHeight = height;
    init.frameRateNum = m_fps;
    init.frameRateDen = 1;
    init.enableEncodeAsync = 0;
    init.enablePTD = 1;
    init.encodeConfig = &cfg;
    init.maxEncodeWidth = width;
    init.maxEncodeHeight = height;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    if (check(api->nvEncInitializeEncoder(m_encoder, &init), L"nvEncInitializeEncoder", err))
        return false;

    return createInputBuffers(err) && createBitstreamBuffer(err) && createConverter(width, height, err);
}

bool NvencEncoder::createInputBuffers(std::wstring& err) {
    auto& api = *m_api;
    for (int i = 0; i < 4; ++i) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        InputSurface s;
        if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &s.tex))) {
            err = L"CreateTexture2D(NV12) failed";
            return false;
        }
        NV_ENC_REGISTER_RESOURCE reg = {};
        reg.version = NV_ENC_REGISTER_RESOURCE_VER;
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        reg.width = m_width;
        reg.height = m_height;
        reg.pitch = 0;
        reg.subResourceIndex = 0;
        reg.resourceToRegister = s.tex.Get();
        reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (check(api.nvEncRegisterResource(m_encoder, &reg), L"nvEncRegisterResource", err))
            return false;
        s.registered = reg.registeredResource;
        m_inputs.push_back(std::move(s));
    }
    return true;
}

bool NvencEncoder::createBitstreamBuffer(std::wstring& err) {
    NV_ENC_CREATE_BITSTREAM_BUFFER bs = {};
    bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    if (check(m_api->nvEncCreateBitstreamBuffer(m_encoder, &bs), L"nvEncCreateBitstreamBuffer", err))
        return false;
    m_bitstream = bs.bitstreamBuffer;
    return true;
}

bool NvencEncoder::createConverter(uint32_t w, uint32_t h, std::wstring& err) {
    if (FAILED(m_device.As(&m_videoDevice))) {
        err = L"ID3D11VideoDevice unsupported";
        return false;
    }
    if (FAILED(m_ctx.As(&m_videoCtx))) {
        err = L"ID3D11VideoContext unsupported";
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator = m_fps;
    cd.InputFrameRate.Denominator = 1;
    cd.InputWidth = w;
    cd.InputHeight = h;
    cd.OutputFrameRate.Numerator = m_fps;
    cd.OutputFrameRate.Denominator = 1;
    cd.OutputWidth = w;
    cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(m_videoDevice->CreateVideoProcessorEnumerator(&cd, &m_venum))) {
        err = L"CreateVideoProcessorEnumerator failed";
        return false;
    }
    if (FAILED(m_videoDevice->CreateVideoProcessor(m_venum.Get(), 0, &m_videoProc))) {
        err = L"CreateVideoProcessor failed";
        return false;
    }
    return true;
}

void NvencEncoder::requestSpsPps() {
    m_pendingSpsPps = true;
}

bool NvencEncoder::encode(ID3D11Texture2D* bgraTex, bool forceIdr,
                          PacketSink sink, std::wstring& err) {
    auto& api = *m_api;
    InputSurface& in = m_inputs[m_nextInput];
    m_nextInput = (m_nextInput + 1) % m_inputs.size();

    ID3D11Texture2D* nvTex = nullptr;

    if (m_sameAdapter) {
        // Same GPU — use source texture directly, no copy needed
        nvTex = bgraTex;
    } else {
        // Cross-adapter: init shared texture once
        if (!m_shared && !m_staging) {
            D3D11_TEXTURE2D_DESC sd = {};
            bgraTex->GetDesc(&sd);

            // Try shared texture with keyed mutex for GPU sync
            sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.CPUAccessFlags = 0;
            if (SUCCEEDED(m_srcDevice->CreateTexture2D(&sd, nullptr, &m_shared))) {
                Microsoft::WRL::ComPtr<IDXGIResource> dxgiRes;
                if (SUCCEEDED(m_shared.As(&dxgiRes)) &&
                    SUCCEEDED(dxgiRes->GetSharedHandle(&m_sharedHandle)) && m_sharedHandle) {
                    D3D11_TEXTURE2D_DESC nd = sd;
                    nd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                    nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    if (SUCCEEDED(m_device->OpenSharedResource(m_sharedHandle, IID_PPV_ARGS(&m_sharedNv)))) {
                        m_sharedNv.As(&m_sharedKeyedMutex);
                        m_useShared = m_sharedKeyedMutex != nullptr;
                    }
                }
            }

            if (!m_useShared) {
                // Fallback: CPU staging
                m_shared.Reset();
                m_sharedNv.Reset();
                sd.MiscFlags = 0;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(m_srcDevice->CreateTexture2D(&sd, nullptr, &m_staging))) {
                    err = L"CreateTexture2D(staging) failed";
                    return false;
                }
                D3D11_TEXTURE2D_DESC ud = {};
                bgraTex->GetDesc(&ud);
                ud.Usage = D3D11_USAGE_DEFAULT;
                ud.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                ud.CPUAccessFlags = 0;
                ud.MiscFlags = 0;
                if (FAILED(m_device->CreateTexture2D(&ud, nullptr, &m_upload))) {
                    err = L"CreateTexture2D(upload) failed";
                    return false;
                }
            }
        }

        if (m_useShared) {
            // Acquire keyed mutex on source, copy, release
            if (m_sharedKeyedMutex)
                m_sharedKeyedMutex->AcquireSync(0, INFINITE);
            m_srcCtx->CopyResource(m_shared.Get(), bgraTex);
            if (m_sharedKeyedMutex)
                m_sharedKeyedMutex->ReleaseSync(1);
            // Acquire on NVIDIA side, encode, release
            if (m_sharedKeyedMutex)
                m_sharedKeyedMutex->AcquireSync(1, INFINITE);
            nvTex = m_sharedNv.Get();
        } else {
            // CPU fallback (should not normally hit if shared works)
            m_srcCtx->CopyResource(m_staging.Get(), bgraTex);
            m_srcCtx->Flush();
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (FAILED(m_srcCtx->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                err = L"Map(staging) failed";
                return false;
            }
            m_ctx->UpdateSubresource(m_upload.Get(), 0, nullptr, mapped.pData, mapped.RowPitch, 0);
            m_srcCtx->Unmap(m_staging.Get(), 0);
            nvTex = m_upload.Get();
        }
    }

    // Cache video processor views (avoid recreating every frame)
    size_t inSlot = m_nextInput % 4;
    size_t outSlot = m_nextInput % 4;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView = m_cachedInViews[inSlot];
    if (!inView || m_cachedInTex[inSlot] != nvTex) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = 0;
        if (FAILED(m_videoDevice->CreateVideoProcessorInputView(nvTex, m_venum.Get(), &ivd, &inView))) {
            err = L"CreateVideoProcessorInputView failed";
            return false;
        }
        m_cachedInViews[inSlot] = inView;
        m_cachedInTex[inSlot] = nvTex;
    }

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView = m_cachedOutViews[outSlot];
    if (!outView || m_cachedOutTex[outSlot] != in.tex.Get()) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        if (FAILED(m_videoDevice->CreateVideoProcessorOutputView(in.tex.Get(), m_venum.Get(), &ovd, &outView))) {
            err = L"CreateVideoProcessorOutputView failed";
            return false;
        }
        m_cachedOutViews[outSlot] = outView;
        m_cachedOutTex[outSlot] = in.tex.Get();
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    if (FAILED(m_videoCtx->VideoProcessorBlt(m_videoProc.Get(), outView.Get(), 0, 1, &stream))) {
        err = L"VideoProcessorBlt failed";
        return false;
    }

    // Release keyed mutex after conversion
    if (m_useShared && m_sharedKeyedMutex)
        m_sharedKeyedMutex->ReleaseSync(0);

    NV_ENC_MAP_INPUT_RESOURCE map = {};
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = in.registered;
    if (check(api.nvEncMapInputResource(m_encoder, &map), L"nvEncMapInputResource", err))
        return false;

    const bool spspps = m_pendingSpsPps;
    uint32_t flags = 0;
    if (forceIdr || spspps)
        flags |= NV_ENC_PIC_FLAG_FORCEIDR;
    if (spspps)
        flags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = m_width;
    pic.inputHeight = m_height;
    pic.inputPitch = 0;
    pic.encodePicFlags = flags;
    pic.inputBuffer = map.mappedResource;
    pic.outputBitstream = m_bitstream;
    pic.bufferFmt = map.mappedBufferFmt;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    NVENCSTATUS es = api.nvEncEncodePicture(m_encoder, &pic);
    if (es != NV_ENC_SUCCESS) {
        api.nvEncUnmapInputResource(m_encoder, map.mappedResource);
        check(es, L"nvEncEncodePicture", err);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = m_bitstream;
    lock.doNotWait = 0;
    if (check(api.nvEncLockBitstream(m_encoder, &lock), L"nvEncLockBitstream", err)) {
        api.nvEncUnmapInputResource(m_encoder, map.mappedResource);
        return false;
    }

    const bool keyframe = forceIdr || spspps ||
        lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
    sink(reinterpret_cast<const uint8_t*>(lock.bitstreamBufferPtr),
         lock.bitstreamSizeInBytes, keyframe, spspps);

    api.nvEncUnlockBitstream(m_encoder, m_bitstream);
    api.nvEncUnmapInputResource(m_encoder, map.mappedResource);
    m_pendingSpsPps = false;
    if (spspps)
        m_spsPpsSent = true;
    return true;
}
