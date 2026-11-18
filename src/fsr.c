/* FSR 4 through AMD's FSR API loader (amd_fidelityfx_loader_dx12.dll, which picks
   amd_fidelityfx_upscaler_dx12.dll from the same folder).

   Two modes:
   - Native AA: out size == render size == the HDR scene colour (R11G11B10_FLOAT); the caller
     copies the result back into the scene colour.
   - Upscaling: `color` is the game chain's back buffer at render resolution, the output is at
     display resolution in the window chain's format; the caller copies it into the window
     back buffer. Azimuth 1.x renders the game at `resolution=` from azimuth.ini into a
   render-sized game swap chain and stretches it onto the display-sized window chain itself, so
   the mod replaces that stretch with FSR. */
#define COBJMACROS
#include "common.h"
#include <stdbool.h>
#include <string.h>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

static PfnFfxCreateContext p_create;
static PfnFfxDestroyContext p_destroy;
static PfnFfxQuery p_query;
static PfnFfxDispatch p_dispatch;
static int load_failed;

static ID3D12Device *device;
static ffxContext ctx;
static UINT ctx_w, ctx_h;          /* render size */
static UINT ctx_out_w, ctx_out_h;  /* upscale size */
static DXGI_FORMAT ctx_out_fmt;
static int ctx_failed;

static ID3D12Resource *out_tex;

static int hdr_enabled = 1;
static float cfg_sharpness = -1.0f;   /* < 0: sharpening off */

static volatile LONG enabled = 1;
static int need_reset = 1;
static int jitter_live;              /* FSR ran last frame, so jitter the scene */
static int phase_count = 8, jitter_index;
static float jx, jy;                 /* jitter for the frame being recorded */
static int jitter_used;              /* the scene of this frame was rendered with (jx, jy) */
static float frame_ms = 16.7f;
static LARGE_INTEGER qpc_freq, qpc_last;

static void ffx_message(uint32_t type, const wchar_t *message) {
    char buf[600];
    WideCharToMultiByte(CP_UTF8, 0, message, -1, buf, sizeof(buf), NULL, NULL);
    log_msg("FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", buf);
}

static int load(void) {
    if (p_dispatch) {
        return 1;
    }
    if (load_failed) {
        return 0;
    }
    load_failed = 1;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') {
        --n;
    }
    lstrcpyW(path + n, L"amd_fidelityfx_loader_dx12.dll");
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        log_msg("FSR: amd_fidelityfx_loader_dx12.dll not found next to azimuth.exe (error %lu)", GetLastError());
        return 0;
    }
    *(FARPROC *)&p_create = GetProcAddress(m, "ffxCreateContext");
    *(FARPROC *)&p_destroy = GetProcAddress(m, "ffxDestroyContext");
    *(FARPROC *)&p_query = GetProcAddress(m, "ffxQuery");
    *(FARPROC *)&p_dispatch = GetProcAddress(m, "ffxDispatch");
    if (!p_create || !p_destroy || !p_query || !p_dispatch) {
        log_msg("FSR: loader exports missing");
        p_dispatch = NULL;
        return 0;
    }
    load_failed = 0;
    return 1;
}

void fsr_init(ID3D12Device *d) {
    device = d;
    QueryPerformanceFrequency(&qpc_freq);
}

void fsr_config(int hdr, float sharpness) {
    hdr_enabled = hdr;
    cfg_sharpness = sharpness;
}

int fsr_enabled(void) {
    return enabled;
}

void fsr_toggle(void) {
    enabled = !enabled;
    need_reset = 1;
    jitter_live = 0;
    log_msg("FSR %s", enabled ? "ON" : "OFF");
}

int fsr_jitter(float *x, float *y) {
    if (!enabled || !jitter_live || !ctx) {
        return 0;
    }
    *x = jx;
    *y = jy;
    jitter_used = 1;
    return 1;
}

static int create_context(UINT w, UINT h, UINT out_w, UINT out_h, DXGI_FORMAT out_fmt) {
    if (ctx && ctx_w == w && ctx_h == h && ctx_out_w == out_w && ctx_out_h == out_h && ctx_out_fmt == out_fmt) {
        return 1;
    }
    if (ctx_failed || !device || !load()) {
        return 0;
    }
    if (ctx) {
        /* Size change: the old context may still be referenced by in-flight work, so it is
           leaked rather than destroyed mid-frame. Rare (only when a size changes). Guard
           against thrashing (which would leak a context + texture per frame). */
        static int recreate_count;
        if (++recreate_count > 8) {
            log_msg("FSR: context recreated %d times; disabling FSR to avoid thrashing", recreate_count);
            ctx_failed = 1;
            return 0;
        }
        log_msg("FSR: size changed to render %ux%u -> out %ux%u fmt %d, recreating context (#%d)", w, h, out_w, out_h,
                (int)out_fmt, recreate_count);
        ctx = NULL;
        if (out_tex) {
            out_tex = NULL; /* leaked on purpose: SRV descriptors may still point at it */
        }
    }
    struct ffxCreateBackendDX12Desc backend;
    memset(&backend, 0, sizeof(backend));
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    struct ffxCreateContextDescUpscaleVersion version;
    memset(&version, 0, sizeof(version));
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.header.pNext = &backend.header;
    version.version = FFX_UPSCALER_VERSION;
    struct ffxCreateContextDescUpscale desc;
    memset(&desc, 0, sizeof(desc));
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &version.header;
    desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | (hdr_enabled ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0);
    desc.maxRenderSize.width = w;
    desc.maxRenderSize.height = h;
    desc.maxUpscaleSize.width = out_w;
    desc.maxUpscaleSize.height = out_h;
    desc.fpMessage = ffx_message;
    ffxReturnCode_t rc = p_create(&ctx, &desc.header, NULL);
    if (rc != FFX_API_RETURN_OK) {
        log_msg("FSR: ffxCreateContext failed (%u)", rc);
        ctx = NULL;
        ctx_failed = 1;
        return 0;
    }
    struct ffxQueryGetProviderVersion pv;
    memset(&pv, 0, sizeof(pv));
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (p_query(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) {
        log_msg("FSR context render %ux%u -> out %ux%u fmt %d, provider: %s (id 0x%llx)", w, h, out_w, out_h,
                (int)out_fmt, pv.versionName, (unsigned long long)pv.versionId);
    }
    struct ffxQueryDescUpscaleGetJitterPhaseCount pc;
    memset(&pc, 0, sizeof(pc));
    pc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    pc.renderWidth = w;
    pc.displayWidth = out_w;
    int32_t phases = 0;
    pc.pOutPhaseCount = &phases;
    if (p_query(&ctx, &pc.header) == FFX_API_RETURN_OK && phases > 0) {
        phase_count = phases;
    }
    log_msg("FSR jitter phase count %d (render %u, display %u)", phase_count, w, out_w);

    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = out_w;
    rd.Height = out_h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = out_fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                    &IID_ID3D12Resource, (void **)&out_tex))) {
        log_msg("FSR: output texture %ux%u fmt %d creation failed", out_w, out_h, (int)out_fmt);
        out_tex = NULL;
        ctx_failed = 1;
        return 0;
    }
    ID3D12Resource_SetName(out_tex, L"FH2FSR Output");
    ctx_w = w;
    ctx_h = h;
    ctx_out_w = out_w;
    ctx_out_h = out_h;
    ctx_out_fmt = out_fmt;
    need_reset = 1;
    return 1;
}

static struct FfxApiResource ffx_resource(ID3D12Resource *r, uint32_t format, uint32_t usage, uint32_t state) {
    struct FfxApiResource res;
    memset(&res, 0, sizeof(res));
    D3D12_RESOURCE_DESC d;
    r->lpVtbl->GetDesc(r, &d);
    res.resource = r;
    res.state = state;
    res.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    res.description.format = format;
    res.description.width = (uint32_t)d.Width;
    res.description.height = (uint32_t)d.Height;
    res.description.depth = 1;
    res.description.mipCount = 1;
    res.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    res.description.usage = usage;
    return res;
}

#define READ_STATE (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)

static int to_ffx_format(DXGI_FORMAT f, uint32_t *out) {
    switch (f) {
    case DXGI_FORMAT_R11G11B10_FLOAT: *out = FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT; return 1;
    case DXGI_FORMAT_R10G10B10A2_UNORM: *out = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM; return 1;
    case DXGI_FORMAT_R8G8B8A8_UNORM: *out = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM; return 1;
    default: return 0;
    }
}

int fsr_run(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
            ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource *motion,
            D3D12_RESOURCE_STATES motion_state, UINT out_w, UINT out_h, DXGI_FORMAT out_fmt,
            DXGI_FORMAT color_fmt) {
    if (!enabled) {
        return 0;
    }
    D3D12_RESOURCE_DESC cd;
    color->lpVtbl->GetDesc(color, &cd);
    UINT w = (UINT)cd.Width, h = cd.Height;
    if (!create_context(w, h, out_w, out_h, out_fmt)) {
        return 0;
    }
    uint32_t color_f, out_f;
    if (!to_ffx_format(color_fmt, &color_f) || !to_ffx_format(out_fmt, &out_f)) {
        static int warned_fmt;
        if (!warned_fmt++) {
            log_msg("FSR: unsupported format pair color %d out %d", (int)color_fmt, (int)out_fmt);
        }
        return 0;
    }
    /* Everything FSR reads goes to the shader-read state it is told about. */
    if (color_state != READ_STATE) {
        hooks_transition(cl, color, color_state, READ_STATE);
    }
    if (depth_state != READ_STATE) {
        hooks_transition(cl, depth, depth_state, READ_STATE);
    }
    if (motion_state != READ_STATE) {
        hooks_transition(cl, motion, motion_state, READ_STATE);
    }
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof(d));
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = ffx_resource(color, color_f, FFX_API_RESOURCE_USAGE_RENDERTARGET, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = ffx_resource(depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = ffx_resource(motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_RENDERTARGET, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = ffx_resource(out_tex, out_f, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x = jitter_used ? jx : 0.0f;
    d.jitterOffset.y = jitter_used ? jy : 0.0f;
    /* Our motion vectors are UV-space offsets from current to previous position. */
    d.motionVectorScale.x = (float)w;
    d.motionVectorScale.y = (float)h;
    d.renderSize.width = w;
    d.renderSize.height = h;
    d.upscaleSize.width = out_w;
    d.upscaleSize.height = out_h;
    d.enableSharpening = cfg_sharpness >= 0.0f;
    d.sharpness = cfg_sharpness < 0.0f ? 0.0f : (cfg_sharpness > 1.0f ? 1.0f : cfg_sharpness);
    d.frameTimeDelta = frame_ms;
    d.preExposure = 1.0f;
    d.reset = need_reset != 0;
    d.cameraNear = 0.1f;
    d.cameraFar = 10000.0f;
    d.cameraFovAngleVertical = 1.0472f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;
    ffxReturnCode_t rc = p_dispatch(&ctx, &d.header);
    int ok = rc == FFX_API_RETURN_OK;
    if (!ok) {
        static int warned;
        if (warned++ < 5) {
            log_msg("FSR: ffxDispatch failed (%u)", rc);
        }
    }
    if (color_state != READ_STATE) {
        hooks_transition(cl, color, READ_STATE, color_state);
    }
    if (depth_state != READ_STATE) {
        hooks_transition(cl, depth, READ_STATE, depth_state);
    }
    if (motion_state != READ_STATE) {
        hooks_transition(cl, motion, READ_STATE, motion_state);
    }
    if (ok) {
        static int once;
        if (!once) {
            once = 1;
            log_msg("first FSR dispatch recorded (render %ux%u -> out %ux%u, jitter %.3f %.3f)", w, h, out_w,
                    out_h, d.jitterOffset.x, d.jitterOffset.y);
        }
        need_reset = 0;
        /* Next frame's jitter. Advancing here ties the sequence to rendered scenes. */
        jitter_index = (jitter_index + 1) % phase_count;
        struct ffxQueryDescUpscaleGetJitterOffset jo;
        memset(&jo, 0, sizeof(jo));
        jo.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        jo.index = jitter_index;
        jo.phaseCount = phase_count;
        jo.pOutX = &jx;
        jo.pOutY = &jy;
        if (p_query(&ctx, &jo.header) != FFX_API_RETURN_OK) {
            jx = jy = 0.0f;
        }
        jitter_used = 0;
    }
    return ok;
}

ID3D12Resource *fsr_output(void) {
    return out_tex;
}

void fsr_end_frame(int ran) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (qpc_last.QuadPart && qpc_freq.QuadPart) {
        float ms = (float)((double)(now.QuadPart - qpc_last.QuadPart) * 1000.0 / (double)qpc_freq.QuadPart);
        frame_ms = ms > 0.0f && ms < 1000.0f ? ms : frame_ms;
    }
    qpc_last = now;
    jitter_live = ran && enabled;
    if (!ran) {
        need_reset = 1;
        jitter_used = 0;
    }
}
