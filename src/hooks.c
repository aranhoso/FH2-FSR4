/* FH2 FSR: FSR 4 for Azimuth (jitter, full-resolution motion vectors, Native AA / upscaling).
   Loaded as dxgi.dll next to azimuth.exe. Hooks D3D12CreateDevice through the exe's
   import table, then device / command-list / factory vtables.

   Azimuth 1.x compatibility (ported from the Azimuth-DLAA codebase, which runs on
   Azimuth 1.0.3/1.0.4):
   - No Present hooks: the Steam overlay hooks Present too, and double-hooking the swap
     chain vtable crashed the game at startup when launched from Steam. Frame counting and
     fsr_end_frame are driven from the scene resolve instead; F11 is polled from a thread.
   - The recognised passes are the Azimuth 1.x shader hashes; passes may be indexed draws,
     so DrawIndexedInstanced is hooked too.

   Upscaling: Azimuth 1.x creates two swap chains — a display-sized window chain
   (OS-presented) and a game chain created at render resolution (azimuth.ini
   resolution/aspect) which it then resizes to display res, so the game itself stretches
   its frame into it and the window copy is 1:1. To upscale with FSR we clamp the game
   chain's ResizeBuffers back to render res (the game then renders 1:1 into a render-sized
   back buffer and Azimuth's game->window display pass does the stretch), and we replace
   that display-pass draw with an FSR dispatch: color = the game chain's current back
   buffer, output copied into the window back buffer. When render == display, Native AA at
   the scene resolve is used instead (unchanged behavior). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "shaders.h"
#include "common.h"

HMODULE proxy_system_dxgi(void);

/* ---------------------------------------------------------------- logging */

static CRITICAL_SECTION log_lock;
static HANDLE log_file = INVALID_HANDLE_VALUE;

void log_msg(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 3) {
        n = sizeof(buf) - 3;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&log_lock);
    if (log_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(log_file, buf, (DWORD)n, &written, NULL);
    }
    LeaveCriticalSection(&log_lock);
}

/* ---------------------------------------------------------------- patching */

static void patch_slot(void **slot, void *hook, void **original) {
    if (*slot == hook) {
        return;
    }
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    if (original && !*original) {
        *original = *slot;
    }
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

#define VT_SLOT(obj, Vtbl, Method) ((void **)&((Vtbl *)(*(void **)(obj)))->Method)

/* ---------------------------------------------------------------- globals */

/* DXBC checksums of the game's pixel shaders we key on (Azimuth 1.0.3/1.0.4). */
static const DWORD MV_PS_HASH[4] = {0x8a12870f, 0x60f7c6e9, 0x21d28459, 0x07b63a84};      /* motion blur velocity */
static const DWORD RESOLVE_PS_HASH[4] = {0x1549a8f9, 0xd8526d4b, 0x60608dd5, 0x6ae7bb32}; /* scene colour+depth resolve */

enum { ROLE_NONE, ROLE_MV, ROLE_RESOLVE };

typedef HRESULT(WINAPI *PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                     ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                     ID3DBlob **);

static UINT g_srv_inc;
static SRWLOCK pso_lock = SRWLOCK_INIT;
static ID3D12Device *g_device;

/* Game PSOs we recognise, with our full-resolution variant for the motion-vector pass. */
#define MAX_VARIANTS 32
static struct {
    ID3D12PipelineState *original, *variant;
    int role;
} variants[MAX_VARIANTS];

/* Scene resources, found at the resolve draw. States are tracked from the game's barriers. */
static ID3D12Resource *scene_color, *scene_depth;
static volatile LONG scene_color_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static volatile LONG scene_depth_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

/* RTV/DSV descriptor -> resource, from the device's view-creation calls. */
typedef struct {
    SIZE_T handle;
    ID3D12Resource *resource;
    DXGI_FORMAT format;   /* view format, or resource format when the view had no desc */
    UINT width, height;
    UINT8 kind;           /* 0 none, 1 RTV, 2 DSV, 3 SRV */
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;  /* saved desc when kind == 3 */
} ViewEntry;
#define MAX_VIEWS 65536
static ViewEntry views[MAX_VIEWS];

/* Motion-vector target. */
static ID3D12Resource *mv_tex;
static UINT mv_w, mv_h;
static ID3D12DescriptorHeap *rtv_heap;   /* 0: MV */
static UINT rtv_inc;
static volatile LONG64 present_count;
static ID3D12Resource *retired[16];
static LONG64 retired_at[16];
static SRWLOCK mv_lock = SRWLOCK_INIT;

/* The two swap chains. Only the creation desc and the raw object pointer are recorded in
   the factory hook (no COM calls in the creation path: with ReShade also hooking the
   factory, side effects there made the second CreateSwapChainForHwnd fail). Everything
   else happens later, outside the creation path. */
static IUnknown *chain_obj[2];              /* 0: window (display), 1: game (render) — creation order */
static DXGI_SWAP_CHAIN_DESC1 chain_desc[2];
static int chain_count;
static UINT disp_w, disp_h;                 /* window chain size */
static UINT scene_w, scene_h;               /* render size, from the scene colour */
static ID3D12Resource *volatile g_upscale_tex;
static D3D12_GPU_DESCRIPTOR_HANDLE g_up_gpu;
static UINT rend_w, rend_h;                 /* game chain creation size */
static int g_upscale;                       /* render < display */
static ID3D12Resource *window_bb[8];       /* window chain back buffers (raw refs) */
static UINT window_bb_n;

/* fh2_fsr.ini next to the DLL: [fsr] mode=auto|native|upscale, hdr=auto|on|off, sharpness, debug_blink */
static int cfg_mode;                        /* 0 auto, 1 native, 2 upscale */
static int cfg_hdr = -1;                    /* -1 auto, 0 off, 1 on */
static float cfg_sharpness = -1.0f;
static int cfg_debug_blink;
static int cfg_debug_raw;

/* ---------------------------------------------------------------- originals */

static HRESULT(WINAPI *real_D3D12CreateDevice)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *real_CreateGraphicsPipelineState)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *real_CreateCommandList)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *real_Reset)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
static void(STDMETHODCALLTYPE *real_SetPipelineState)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
static void(STDMETHODCALLTYPE *real_OMSetRenderTargets)(ID3D12GraphicsCommandList *, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE *);
static void(STDMETHODCALLTYPE *real_RSSetViewports)(ID3D12GraphicsCommandList *, UINT, const D3D12_VIEWPORT *);
static void(STDMETHODCALLTYPE *real_RSSetScissorRects)(ID3D12GraphicsCommandList *, UINT, const D3D12_RECT *);
static void(STDMETHODCALLTYPE *real_DrawInstanced)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
static void(STDMETHODCALLTYPE *real_DrawIndexedInstanced)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
static void(STDMETHODCALLTYPE *real_ResourceBarrier)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
static void(STDMETHODCALLTYPE *real_SetDescriptorHeaps)(ID3D12GraphicsCommandList *, UINT, ID3D12DescriptorHeap *const *);
static void(STDMETHODCALLTYPE *real_SetComputeRootSignature)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
static void(STDMETHODCALLTYPE *real_SetGraphicsRootSignature)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
static void(STDMETHODCALLTYPE *real_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE *real_SetGraphicsRootShaderResourceView)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE *real_CreateRenderTargetView)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE *real_CreateDepthStencilView)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE *real_CreateShaderResourceView)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static HRESULT(WINAPI *real_CreateDXGIFactory)(REFIID, void **);
static HRESULT(WINAPI *real_CreateDXGIFactory1)(REFIID, void **);
static HRESULT(WINAPI *real_CreateDXGIFactory2)(UINT, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *real_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

/* ---------------------------------------------------------------- shaders */

static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    static PFN_Compile fn;
    if (!fn) {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        fn = m ? (PFN_Compile)(void *)GetProcAddress(m, "D3DCompile") : NULL;
        if (!fn) {
            log_msg("d3dcompiler_47 unavailable");
            return NULL;
        }
    }
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = fn(src, strlen(src), entry, NULL, NULL, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        log_msg("compile %s/%s failed 0x%08lx: %s", entry, target, hr,
             errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
    }
    if (errors) {
        ID3D10Blob_Release(errors);
    }
    return SUCCEEDED(hr) ? code : NULL;
}

static int shader_role(const D3D12_SHADER_BYTECODE *ps) {
    if (!ps->pShaderBytecode || ps->BytecodeLength < 20) {
        return ROLE_NONE;
    }
    const BYTE *b = ps->pShaderBytecode;
    if (memcmp(b, "DXBC", 4)) {
        return ROLE_NONE;
    }
    DWORD h[4];
    memcpy(h, b + 4, 16);
    if (!memcmp(h, MV_PS_HASH, 16)) {
        return ROLE_MV;
    }
    if (!memcmp(h, RESOLVE_PS_HASH, 16)) {
        return ROLE_RESOLVE;
    }
    return ROLE_NONE;
}

/* ---------------------------------------------------------------- PSO hook */

static int pso_known(ID3D12PipelineState *pso);

static void remember_pso(ID3D12PipelineState *pso, ID3D12PipelineState *variant, int role) {
    AcquireSRWLockExclusive(&pso_lock);
    int i = 0;
    for (; i < MAX_VARIANTS && variants[i].original; ++i) {
    }
    if (i < MAX_VARIANTS) {
        variants[i].original = pso;
        variants[i].variant = variant;
        variants[i].role = role;
        variant = NULL;
    }
    ReleaseSRWLockExclusive(&pso_lock);
    if (variant) {
        ID3D12PipelineState_Release(variant);
        log_msg("PSO table full");
    }
}

/* Records the game passes we key on; builds our full-resolution variant of the motion-vector pass. */
static void consider_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, ID3D12PipelineState *pso,
                         const char *source) {
    if (!desc || !pso || !g_device) {
        return;
    }
    int role = shader_role(&desc->PS);
    if (role == ROLE_NONE || pso_known(pso)) {
        return;
    }
    if (role != ROLE_MV) {
        remember_pso(pso, NULL, role);
        log_msg("scene resolve pass found (%s)", source);
        return;
    }
    ID3DBlob *ps = compile(MV_PS_HLSL, "main", "ps_5_0");
    if (!ps) {
        return;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
    d.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    d.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    d.NumRenderTargets = 1;
    for (int i = 0; i < 8; ++i) {
        d.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    }
    d.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
    d.DSVFormat = DXGI_FORMAT_UNKNOWN;
    d.DepthStencilState.DepthEnable = FALSE;
    d.DepthStencilState.StencilEnable = FALSE;
    memset(&d.BlendState, 0, sizeof(d.BlendState));
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.SampleMask = UINT_MAX;
    d.CachedPSO.pCachedBlob = NULL;
    d.CachedPSO.CachedBlobSizeInBytes = 0;
    ID3D12PipelineState *variant = NULL;
    HRESULT vhr = real_CreateGraphicsPipelineState(g_device, &d, &IID_ID3D12PipelineState, (void **)&variant);
    ID3D10Blob_Release(ps);
    if (FAILED(vhr)) {
        log_msg("motion-vector variant PSO failed 0x%08lx", vhr);
        return;
    }
    remember_pso(pso, variant, ROLE_MV);
    log_msg("motion-vector pass found (%s); full-resolution variant ready", source);
}

static HRESULT STDMETHODCALLTYPE hook_CreateGraphicsPipelineState(
    ID3D12Device *dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out) {
    HRESULT hr = real_CreateGraphicsPipelineState(dev, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        consider_pso(desc, (ID3D12PipelineState *)*out, "created");
    }
    return hr;
}

static HRESULT(STDMETHODCALLTYPE *real_LoadGraphicsPipeline)(ID3D12PipelineLibrary *, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *real_CreatePipelineLibrary)(ID3D12Device1 *, const void *, SIZE_T, REFIID, void **);

static HRESULT STDMETHODCALLTYPE hook_LoadGraphicsPipeline(ID3D12PipelineLibrary *lib, LPCWSTR name,
                                                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                           REFIID riid, void **out) {
    HRESULT hr = real_LoadGraphicsPipeline(lib, name, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        consider_pso(desc, (ID3D12PipelineState *)*out, "pipeline library");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_CreatePipelineLibrary(ID3D12Device1 *dev, const void *blob, SIZE_T size,
                                                            REFIID riid, void **out) {
    HRESULT hr = real_CreatePipelineLibrary(dev, blob, size, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        patch_slot(VT_SLOT(*out, ID3D12PipelineLibraryVtbl, LoadGraphicsPipeline), (void *)hook_LoadGraphicsPipeline,
                   (void **)&real_LoadGraphicsPipeline);
        log_msg("pipeline library hooked (%llu bytes)", (unsigned long long)size);
    }
    return hr;
}

static int pso_lookup(ID3D12PipelineState *pso, ID3D12PipelineState **variant) {
    int role = ROLE_NONE;
    if (!pso) {
        return role;
    }
    AcquireSRWLockShared(&pso_lock);
    for (int i = 0; i < MAX_VARIANTS && variants[i].original; ++i) {
        if (variants[i].original == pso) {
            role = variants[i].role;
            if (variant) {
                *variant = variants[i].variant;
            }
            break;
        }
    }
    ReleaseSRWLockShared(&pso_lock);
    return role;
}

static int pso_known(ID3D12PipelineState *pso) {
    return pso_lookup(pso, NULL) != ROLE_NONE;
}

/* ---------------------------------------------------------------- command-list state */

typedef struct {
    ID3D12GraphicsCommandList *list;
    ID3D12PipelineState *pso;
    UINT num_rtv;
    BOOL rtv_single;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[8];
    BOOL win_bb;         /* rtv[0] is a window-chain back buffer: Azimuth's display pass */
    BOOL has_dsv;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    BOOL dsv_scene;      /* the scene depth buffer is bound: viewports get the FSR jitter */
    BOOL vp_jittered;
    UINT num_vp, num_sc;
    D3D12_VIEWPORT vp[4]; /* as the game set them (unjittered) */
    D3D12_RECT sc[4];
    UINT num_heaps;
    ID3D12DescriptorHeap *heaps[2];
    D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu[2];
    D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu[2];
    UINT heap_num[2];
    ID3D12RootSignature *compute_rs;
    UINT root_srv_mask;
    D3D12_GPU_DESCRIPTOR_HANDLE root_srv[32];
    UINT tables_mask;
    D3D12_GPU_DESCRIPTOR_HANDLE tables[32];
} ListState;

/* Azimuth creates command lists continually and never tells us when one dies, so this
   is an open-addressed table that evicts on overflow; Reset reinitialises an entry
   when a freed address is reused for a new list. */
#define MAX_LISTS 4096
#define PROBE 16
static ListState lists[MAX_LISTS];
static SRWLOCK list_lock = SRWLOCK_INIT;

static UINT list_hash(ID3D12GraphicsCommandList *cl) {
    UINT64 v = (UINT64)(uintptr_t)cl >> 4;
    return (UINT)((v * 0x9E3779B97F4A7C15ull) >> 52) & (MAX_LISTS - 1);
}

static ListState *state_for(ID3D12GraphicsCommandList *cl) {
    UINT h = list_hash(cl);
    for (UINT i = 0; i < PROBE; ++i) {
        ListState *s = &lists[(h + i) & (MAX_LISTS - 1)];
        if (s->list == cl) {
            return s;
        }
    }
    AcquireSRWLockExclusive(&list_lock);
    ListState *s = NULL;
    for (UINT i = 0; i < PROBE; ++i) {
        ListState *c = &lists[(h + i) & (MAX_LISTS - 1)];
        if (c->list == cl) {
            s = c;
            break;
        }
        if (!s && !c->list) {
            s = c;
        }
    }
    if (!s) {
        static UINT victim;
        s = &lists[(h + (victim++ % PROBE)) & (MAX_LISTS - 1)];
    }
    if (s->list != cl) {
        memset(s, 0, sizeof(*s));
        s->list = cl;
    }
    ReleaseSRWLockExclusive(&list_lock);
    return s;
}

/* ---------------------------------------------------------------- view map */

static UINT view_hash(SIZE_T handle) {
    return (UINT)(((UINT64)handle * 0x9E3779B97F4A7C15ull) >> 50) & (MAX_VIEWS - 1);
}

static void view_record(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res, DXGI_FORMAT view_format,
                        UINT8 kind, const D3D12_SHADER_RESOURCE_VIEW_DESC *srv_desc) {
    UINT i = view_hash(h.ptr);
    ViewEntry *e = NULL;
    for (UINT k = 0; k < 64; ++k) {
        ViewEntry *c = &views[(i + k) & (MAX_VIEWS - 1)];
        if (c->handle == h.ptr || !c->handle) {
            e = c;
            break;
        }
    }
    if (!e) {
        e = &views[i]; /* evict */
    }
    UINT w = 0, ht = 0;
    DXGI_FORMAT f = view_format;
    if (res) {
        D3D12_RESOURCE_DESC d;
        res->lpVtbl->GetDesc(res, &d);
        w = (UINT)d.Width;
        ht = d.Height;
        if (f == DXGI_FORMAT_UNKNOWN) {
            f = d.Format;
        }
    }
    e->format = f;
    e->width = w;
    e->height = ht;
    e->resource = res;
    e->handle = h.ptr;
    e->kind = kind;
    if (kind == 3) {
        if (srv_desc) {
            e->srv_desc = *srv_desc;
        } else {
            memset(&e->srv_desc, 0, sizeof(e->srv_desc));
            e->srv_desc.Format = DXGI_FORMAT_UNKNOWN;
            e->srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            e->srv_desc.Texture2D.MipLevels = 1;
        }
    }
}

static const ViewEntry *view_find(SIZE_T handle) {
    UINT i = view_hash(handle);
    for (UINT k = 0; k < 64; ++k) {
        const ViewEntry *c = &views[(i + k) & (MAX_VIEWS - 1)];
        if (c->handle == handle) {
            return c;
        }
        if (!c->handle) {
            break;
        }
    }
    return NULL;
}

static void STDMETHODCALLTYPE hook_CreateRenderTargetView(ID3D12Device *dev, ID3D12Resource *res,
                                                          const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_CreateRenderTargetView(dev, res, desc, h);
    view_record(h, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 1, NULL);
}

static void STDMETHODCALLTYPE hook_CreateDepthStencilView(ID3D12Device *dev, ID3D12Resource *res,
                                                          const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_CreateDepthStencilView(dev, res, desc, h);
    view_record(h, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 2, NULL);
}

static void STDMETHODCALLTYPE hook_CreateShaderResourceView(ID3D12Device *dev, ID3D12Resource *res,
                                                           const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_CreateShaderResourceView(dev, res, desc, h);
    view_record(h, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN, 3, desc);
}

/* The scene depth buffer: depth+stencil at the scene colour's size (found at the resolve). */
static BOOL is_scene_dsv(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const ViewEntry *e = view_find(h.ptr);
    if (!e || !scene_color) {
        return FALSE;
    }
    BOOL depth_stencil = e->format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || e->format == DXGI_FORMAT_R32G8X24_TYPELESS;
    D3D12_RESOURCE_DESC d;
    scene_color->lpVtbl->GetDesc(scene_color, &d);
    return depth_stencil && e->width == d.Width && e->height == d.Height;
}

static BOOL is_window_bb(ID3D12Resource *r) {
    for (UINT i = 0; i < window_bb_n; ++i) {
        if (window_bb[i] == r) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------- list hooks */

static void apply_viewports(ID3D12GraphicsCommandList *cl, ListState *s) {
    float jx, jy;
    if (s->dsv_scene && s->num_vp && fsr_jitter(&jx, &jy)) {
        D3D12_VIEWPORT vp[4];
        memcpy(vp, s->vp, s->num_vp * sizeof(*vp));
        for (UINT i = 0; i < s->num_vp; ++i) {
            vp[i].TopLeftX += jx;
            vp[i].TopLeftY += jy;
        }
        real_RSSetViewports(cl, s->num_vp, vp);
        s->vp_jittered = TRUE;
    } else {
        if (s->num_vp) {
            real_RSSetViewports(cl, s->num_vp, s->vp);
        }
        s->vp_jittered = FALSE;
    }
}

static HRESULT STDMETHODCALLTYPE hook_Reset(ID3D12GraphicsCommandList *cl, ID3D12CommandAllocator *a,
                                            ID3D12PipelineState *pso) {
    ListState *s = state_for(cl);
    if (s) {
        memset((char *)s + sizeof(s->list), 0, sizeof(*s) - sizeof(s->list));
        s->pso = pso;
    }
    return real_Reset(cl, a, pso);
}

static void STDMETHODCALLTYPE hook_SetPipelineState(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso) {
    ListState *s = state_for(cl);
    if (s) {
        s->pso = pso;
    }
    real_SetPipelineState(cl, pso);
}

static void STDMETHODCALLTYPE hook_OMSetRenderTargets(ID3D12GraphicsCommandList *cl, UINT n,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE *rt, BOOL single,
                                                      const D3D12_CPU_DESCRIPTOR_HANDLE *ds) {
    real_OMSetRenderTargets(cl, n, rt, single, ds);
    ListState *s = state_for(cl);
    if (s) {
        s->num_rtv = n > 8 ? 8 : n;
        s->rtv_single = single;
        if (rt && n) {
            memcpy(s->rtv, rt, (single ? 1 : s->num_rtv) * sizeof(*rt));
        }
        s->win_bb = FALSE;
        if (rt && n) {
            const ViewEntry *c = view_find(rt[0].ptr);
            if (c && c->resource && is_window_bb(c->resource)) {
                s->win_bb = TRUE;
            }
        }
        s->has_dsv = ds != NULL;
        if (ds) {
            s->dsv = *ds;
        }
        BOOL scene = ds && is_scene_dsv(*ds);
        if (scene != s->dsv_scene) {
            s->dsv_scene = scene;
            if (s->num_vp && (scene || s->vp_jittered)) {
                apply_viewports(cl, s);
            }
        }
    }
}

static void STDMETHODCALLTYPE hook_RSSetViewports(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_VIEWPORT *vp) {
    ListState *s = state_for(cl);
    if (!s || !n || !vp) {
        real_RSSetViewports(cl, n, vp);
        return;
    }
    s->num_vp = n > 4 ? 4 : n;
    memcpy(s->vp, vp, s->num_vp * sizeof(*vp));
    if (s->dsv_scene) {
        apply_viewports(cl, s);
    } else {
        real_RSSetViewports(cl, n, vp);
        s->vp_jittered = FALSE;
    }
}

static void STDMETHODCALLTYPE hook_RSSetScissorRects(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RECT *r) {
    ListState *s = state_for(cl);
    if (s) {
        s->num_sc = n > 4 ? 4 : n;
        memcpy(s->sc, r, s->num_sc * sizeof(*r));
    }
    real_RSSetScissorRects(cl, n, r);
}

static void STDMETHODCALLTYPE hook_SetDescriptorHeaps(ID3D12GraphicsCommandList *cl, UINT n, ID3D12DescriptorHeap *const *h) {
    real_SetDescriptorHeaps(cl, n, h);
    ListState *s = state_for(cl);
    if (s) {
        s->num_heaps = n > 2 ? 2 : n;
        memcpy(s->heaps, h, s->num_heaps * sizeof(*h));
        for (UINT i = 0; i < s->num_heaps; ++i) {
            D3D12_DESCRIPTOR_HEAP_DESC d;
            s->heaps[i]->lpVtbl->GetDesc(s->heaps[i], &d);
            s->heap_num[i] = d.NumDescriptors;
            if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
                s->heaps[i]->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->heaps[i], &s->heap_cpu[i]);
                s->heaps[i]->lpVtbl->GetGPUDescriptorHandleForHeapStart(s->heaps[i], &s->heap_gpu[i]);
            } else {
                s->heap_gpu[i].ptr = 0;
                s->heap_cpu[i].ptr = 0;
                s->heap_num[i] = 0;
            }
        }
    }
}


static void STDMETHODCALLTYPE hook_SetGraphicsRootShaderResourceView(ID3D12GraphicsCommandList *cl, UINT index,
                                                                    D3D12_GPU_DESCRIPTOR_HANDLE h) {
    ListState *s = state_for(cl);
    if (s && index < 32) {
        s->root_srv[index] = h;
        s->root_srv_mask |= 1u << index;
    }
    real_SetGraphicsRootShaderResourceView(cl, index, h);
}

static void STDMETHODCALLTYPE hook_SetComputeRootSignature(ID3D12GraphicsCommandList *cl, ID3D12RootSignature *rs) {
    ListState *s = state_for(cl);
    if (s) {
        s->compute_rs = rs;
    }
    real_SetComputeRootSignature(cl, rs);
}

static void STDMETHODCALLTYPE hook_SetGraphicsRootSignature(ID3D12GraphicsCommandList *cl, ID3D12RootSignature *rs) {
    ListState *s = state_for(cl);
    if (s) {
        s->tables_mask = 0;
        s->root_srv_mask = 0;
    }
    real_SetGraphicsRootSignature(cl, rs);
}

static void STDMETHODCALLTYPE hook_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList *cl, UINT index,
                                                                  D3D12_GPU_DESCRIPTOR_HANDLE base) {
    ListState *s = state_for(cl);
    if (s && index < 32) {
        s->tables[index] = base;
        s->tables_mask |= 1u << index;
    }
    real_SetGraphicsRootDescriptorTable(cl, index, base);
}

static void STDMETHODCALLTYPE hook_ResourceBarrier(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    ID3D12Resource *color = scene_color, *depth = scene_depth;
    if (color || depth) {
        for (UINT i = 0; i < n; ++i) {
            if (b[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }
            if (b[i].Transition.pResource == color) {
                InterlockedExchange(&scene_color_state, (LONG)b[i].Transition.StateAfter);
            } else if (b[i].Transition.pResource == depth) {
                InterlockedExchange(&scene_depth_state, (LONG)b[i].Transition.StateAfter);
            }
        }
    }
    real_ResourceBarrier(cl, n, b);
}

void hooks_transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                      D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    real_ResourceBarrier(cl, 1, &b);
}

/* After FSR has recorded its compute work into the game's list, put back what it changed. */
static void restore_after_fsr(ID3D12GraphicsCommandList *cl, ListState *s) {
    if (s->num_heaps) {
        real_SetDescriptorHeaps(cl, s->num_heaps, s->heaps);
    }
    if (s->compute_rs) {
        real_SetComputeRootSignature(cl, s->compute_rs);
    }
    for (UINT i = 0; i < 32; ++i) {
        if (s->tables_mask & (1u << i)) {
            real_SetGraphicsRootDescriptorTable(cl, i, s->tables[i]);
        }
    }
    real_SetPipelineState(cl, s->pso);
}

/* ---------------------------------------------------------------- MV target */

#define transition hooks_transition

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *heap, UINT index, UINT inc) {
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h);
    h.ptr += (SIZE_T)index * inc;
    return h;
}

static ID3D12DescriptorHeap *srv_heap;   /* shader visible, slot 0: FSR output SRV */
static int ensure_heaps(void) {
    if (rtv_heap && srv_heap) {
        return 1;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap))) {
        return 0;
    }
    rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 8;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_device, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap))) {
        return 0;
    }
    return 1;
}

/* Called under mv_lock (exclusive). */
static int ensure_mv(UINT w, UINT h) {
    if (mv_tex && mv_w == w && mv_h == h) {
        return 1;
    }
    if (!g_device || !ensure_heaps()) {
        return 0;
    }
    if (mv_tex) {
        for (int i = 0; i < 16; ++i) {
            if (!retired[i]) {
                retired[i] = mv_tex;
                retired_at[i] = present_count;
                mv_tex = NULL;
                break;
            }
        }
        if (mv_tex) {
            return 0; /* nowhere to park the old one; try again later */
        }
    }
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16G16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE cv;
    memset(&cv, 0, sizeof(cv));
    cv.Format = rd.Format;
    if (FAILED(ID3D12Device_CreateCommittedResource(g_device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                                    &IID_ID3D12Resource, (void **)&mv_tex))) {
        log_msg("motion-vector texture %ux%u creation failed", w, h);
        mv_tex = NULL;
        return 0;
    }
    ID3D12Resource_SetName(mv_tex, L"FH2FSR MotionVectors");
    ID3D12Device_CreateRenderTargetView(g_device, mv_tex, NULL, cpu_handle(rtv_heap, 0, rtv_inc));
    mv_w = w;
    mv_h = h;
    log_msg("motion-vector texture %ux%u R16G16_FLOAT", w, h);
    return 1;
}

static volatile LONG64 mv_seq;          /* motion-vector passes recorded */
static LONG64 fsr_mv_seq;                /* the motion-vector pass the last FSR dispatch used */
static volatile LONG fallback_active;

static void record_mv_pass(ID3D12GraphicsCommandList *cl, ListState *s, ID3D12PipelineState *variant,
                           UINT vc, UINT ic, UINT sv, UINT si) {
    /* The game's pass is at half the render resolution. */
    UINT w = (UINT)(s->vp[0].Width * 2.0f + 0.5f), h = (UINT)(s->vp[0].Height * 2.0f + 0.5f);
    AcquireSRWLockExclusive(&mv_lock);
    if (ensure_mv(w, h)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_handle(rtv_heap, 0, rtv_inc);
        D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
        transition(cl, mv_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        real_SetPipelineState(cl, variant);
        real_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        real_RSSetViewports(cl, 1, &vp);
        real_RSSetScissorRects(cl, 1, &sc);
        real_DrawInstanced(cl, vc, ic, sv, si);
        transition(cl, mv_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        /* Restore the game's state exactly as it set it. */
        real_SetPipelineState(cl, s->pso);
        real_OMSetRenderTargets(cl, s->num_rtv, s->num_rtv ? s->rtv : NULL, s->rtv_single,
                                s->has_dsv ? &s->dsv : NULL);
        apply_viewports(cl, s);
        if (s->num_sc) {
            real_RSSetScissorRects(cl, s->num_sc, s->sc);
        }
        InterlockedIncrement64(&mv_seq);
        static int once;
        if (!once) {
            once = 1;
            log_msg("first full-resolution motion-vector pass recorded (%ux%u)", w, h);
        }
    }
    ReleaseSRWLockExclusive(&mv_lock);
}

static void record_mv_pass_indexed(ID3D12GraphicsCommandList *cl, ListState *s, ID3D12PipelineState *variant,
                                   UINT index_count, UINT instance_count, UINT start_index,
                                   INT base_vertex, UINT start_instance) {
    /* The game's pass is at half the render resolution. */
    UINT w = (UINT)(s->vp[0].Width * 2.0f + 0.5f), h = (UINT)(s->vp[0].Height * 2.0f + 0.5f);
    AcquireSRWLockExclusive(&mv_lock);
    if (ensure_mv(w, h)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_handle(rtv_heap, 0, rtv_inc);
        D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
        transition(cl, mv_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        real_SetPipelineState(cl, variant);
        real_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        real_RSSetViewports(cl, 1, &vp);
        real_RSSetScissorRects(cl, 1, &sc);
        real_DrawIndexedInstanced(cl, index_count, instance_count, start_index, base_vertex, start_instance);
        transition(cl, mv_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        real_SetPipelineState(cl, s->pso);
        real_OMSetRenderTargets(cl, s->num_rtv, s->num_rtv ? s->rtv : NULL, s->rtv_single,
                                s->has_dsv ? &s->dsv : NULL);
        apply_viewports(cl, s);
        if (s->num_sc) {
            real_RSSetScissorRects(cl, s->num_sc, s->sc);
        }
        InterlockedIncrement64(&mv_seq);
        static int once;
        if (!once) {
            once = 1;
            log_msg("motion-vector replay using indexed draw");
        }
    }
    ReleaseSRWLockExclusive(&mv_lock);
}

/* The scene resolve's targets must match what FSR expects before we capture them. */
static BOOL note_scene_resources(ListState *s) {
    if (!s->num_rtv || !s->has_dsv) {
        return FALSE;
    }
    const ViewEntry *c = view_find(s->rtv[0].ptr), *d = view_find(s->dsv.ptr);
    if (!c || !d || !c->resource || !d->resource) {
        static int warned;
        if (!warned++) {
            log_msg("scene resolve: render-target/depth views not in the view map");
        }
        return FALSE;
    }
    if (c->format != DXGI_FORMAT_R11G11B10_FLOAT || d->format != DXGI_FORMAT_D32_FLOAT) {
        static int format_warned;
        if (!format_warned++) {
            log_msg("scene resolve format mismatch: color=%d expected=%d depth=%d expected=%d; FSR capture skipped",
                    (int)c->format, (int)DXGI_FORMAT_R11G11B10_FLOAT,
                    (int)d->format, (int)DXGI_FORMAT_D32_FLOAT);
        }
        return FALSE;
    }
    if (c->resource != scene_color || d->resource != scene_depth) {
        scene_color = c->resource;
        scene_depth = d->resource;
        scene_w = c->width;
        scene_h = c->height;
        log_msg("scene colour %p (%ux%u fmt %d), scene depth %p (%ux%u fmt %d)", (void *)scene_color, c->width,
                c->height, (int)c->format, (void *)scene_depth, d->width, d->height, (int)d->format);
    }
    return TRUE;
}

/* Once per scene frame, from the resolve draw: old motion-vector textures are released 8
   frames after they were replaced, when the GPU is done with them. Not driven from Present
   (Steam overlay double-hook crash; Azimuth 1.x owns multiple swap chains). */
static void count_frame(void) {
    InterlockedIncrement64(&present_count);
    AcquireSRWLockExclusive(&mv_lock);
    for (int i = 0; i < 16; ++i) {
        if (retired[i] && present_count - retired_at[i] > 8) {
            ID3D12Resource_Release(retired[i]);
            retired[i] = NULL;
        }
    }
    ReleaseSRWLockExclusive(&mv_lock);
}

/* 1: fresh MV pass since the last dispatch, 0: nothing new (skip), -1: stale (fallback). */
static int mv_status(void) {
    LONG64 seq = mv_seq;
    static LONG stale_resolves;
    if (seq == fsr_mv_seq) {
        /* Some game states keep running while the velocity pass stops (pause/photo).
           Never feed stale vectors back into FSR; after two consecutive frames with no
           fresh MV pass, switch to a history-reset fallback. */
        if (InterlockedIncrement(&stale_resolves) < 2) {
            return 0;
        }
        return -1;
    }
    InterlockedExchange(&stale_resolves, 0);
    fsr_mv_seq = seq;
    return 1;
}

/* R16G16_FLOAT zero motion is valid for a disconnected/static frame. */
static void clear_mv(ID3D12GraphicsCommandList *cl) {
    const FLOAT zero[4] = {0, 0, 0, 0};
    D3D12_CPU_DESCRIPTOR_HANDLE mv_rtv = cpu_handle(rtv_heap, 0, rtv_inc);
    transition(cl, mv_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, mv_rtv, zero, 0, NULL);
    transition(cl, mv_tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

static void publish_upscale_tex(ID3D12Resource *up);

/* Native AA: FSR on the HDR scene colour at render resolution, result copied back in place. */
static void run_fsr(ID3D12GraphicsCommandList *cl, ListState *s) {
    if (!fsr_enabled() || !scene_color || !scene_depth || !mv_tex) {
        return;
    }
    int st = mv_status();
    if (st == 0) {
        return;
    }
    int fallback = st < 0;
    if (fallback && !InterlockedExchange(&fallback_active, 1)) {
        log_msg("FSR pause/photo fallback active: no fresh MV pass; using zero motion vectors with history reset");
    } else if (!fallback && InterlockedExchange(&fallback_active, 0)) {
        log_msg("FSR fresh motion vectors resumed; leaving pause/photo fallback");
    }

    static int logged_states;
    if (!logged_states++) {
        log_msg("FSR inputs: colour state 0x%lx, depth state 0x%lx", scene_color_state, scene_depth_state);
    }
    /* FSR records through the same (hooked) list methods, so keep the game's view of the list
       and of the scene resources' states, and put both back afterwards. */
    ListState saved = *s;
    LONG color_state = scene_color_state, depth_state = scene_depth_state;
    D3D12_RESOURCE_DESC cd;
    scene_color->lpVtbl->GetDesc(scene_color, &cd);
    UINT w = (UINT)cd.Width, h = cd.Height;
    AcquireSRWLockShared(&mv_lock);
    if (fallback) {
        clear_mv(cl);
    }
    UINT out_w = w, out_h = h;
    if (g_upscale && disp_w) {
        float scale = (float)disp_w / (float)w;
        if ((float)disp_h / (float)h < scale) {
            scale = (float)disp_h / (float)h;
        }
        out_w = (UINT)(w * scale + 0.5f);
        out_h = (UINT)(h * scale + 0.5f);
        if (out_w > disp_w) out_w = disp_w;
        if (out_h > disp_h) out_h = disp_h;
    }
    int ok = fsr_run(cl, scene_color, (D3D12_RESOURCE_STATES)color_state, scene_depth,
                     (D3D12_RESOURCE_STATES)depth_state, mv_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     out_w, out_h, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT);
    ID3D12Resource *out = ok ? fsr_output() : NULL;
    if (ok && out && g_upscale) {
        publish_upscale_tex(out); /* HDR output; the game's present pass tonemaps it */
    }
    if (ok && out && !g_upscale) {
        transition(cl, scene_color, (D3D12_RESOURCE_STATES)color_state, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->lpVtbl->CopyResource(cl, scene_color, out);
        transition(cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(cl, scene_color, D3D12_RESOURCE_STATE_COPY_DEST, (D3D12_RESOURCE_STATES)color_state);
    }
    ReleaseSRWLockShared(&mv_lock);
    InterlockedExchange(&scene_color_state, color_state);
    InterlockedExchange(&scene_depth_state, depth_state);
    *s = saved;
    restore_after_fsr(cl, s);
    fsr_end_frame((!fallback && ok) ? 1 : 0);
    static LONG fsr_frames;
    LONG n = InterlockedIncrement(&fsr_frames);
    if (ok && (n == 1 || n % 600 == 0)) {
        log_msg("FSR frame %ld (%s, render %ux%u -> out %ux%u)", n, g_upscale ? "upscale" : "native", w, h, out_w, out_h);
    }
}


/* Collect the scene's SRV handles and make our heap slot 0 the FSR output's SRV. */
static void publish_upscale_tex(ID3D12Resource *up) {
    /* Only publish our own copy: never touch the game's descriptors (that crashes
       amdxc64 / corrupts the present pass). The tonemap pass samples slot 0 of
       srv_heap directly. */
    g_upscale_tex = up;
    if (!up || !g_device) {
        return;
    }
    if (ensure_heaps()) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = cpu_handle(srv_heap, 0, g_srv_inc);
        ID3D12Device_CreateShaderResourceView(g_device, up, NULL, c);
        srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap, &g_up_gpu);
    }
    static LONG pub;
    if (InterlockedIncrement(&pub) == 1) {
        D3D12_RESOURCE_DESC ud;
        up->lpVtbl->GetDesc(up, &ud);
        log_msg("upscale output %p %ux%u fmt %d flags 0x%x", (void *)up, (UINT)ud.Width, (UINT)ud.Height,
                (int)ud.Format, (unsigned)ud.Flags);
    }
}



/* ---------------------------------------------------------------- tonemap pass */
/* In upscale mode we never touch the game's descriptors (that crashes AMD's amdxc64 shim).
   Instead, right after the game's present-pass draw, we draw our own fullscreen pass that
   samples the FSR output (HDR, display-fitted) and tonemaps it into the back buffer. */
static ID3D12RootSignature *tm_root;
static ID3D12PipelineState *tm_pso;
static DXGI_FORMAT tm_pso_fmt = DXGI_FORMAT_UNKNOWN;

static int ensure_tonemap(DXGI_FORMAT fmt) {
    static int tm_failed;
    if (tm_failed) {
        return 0;
    }
    if (!g_device) {
        return 0;
    }
    if (tm_root && tm_pso && tm_pso_fmt == fmt) {
        return 1;
    }
    if (!tm_root) {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER param[2];
        memset(param, 0, sizeof(param));
        param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param[0].DescriptorTable.NumDescriptorRanges = 1;
        param[0].DescriptorTable.pDescriptorRanges = &range;
        param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param[1].Constants.ShaderRegister = 0;
        param[1].Constants.Num32BitValues = 3;
        param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs;
        memset(&rs, 0, sizeof(rs));
        rs.NumParameters = 2;
        rs.pParameters = param;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        typedef HRESULT(WINAPI * PFN_Ser)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
        PFN_Ser ser = (PFN_Ser)(void *)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
        ID3DBlob *blob = NULL, *err = NULL;
        if (!ser || FAILED(ser(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
            log_msg("tonemap root signature serialization failed");
            return 0;
        }
        HRESULT hr = ID3D12Device_CreateRootSignature(g_device, 0, ID3D10Blob_GetBufferPointer(blob),
                                                      ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                                      (void **)&tm_root);
        ID3D10Blob_Release(blob);
        if (FAILED(hr)) {
            return 0;
        }
    }
    ID3DBlob *vs = compile(TONEMAP_HLSL, "vs", "vs_5_0"), *ps = compile(TONEMAP_HLSL, "ps", "ps_5_0");
    if (!vs || !ps) {
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    memset(&d, 0, sizeof(d));
    d.pRootSignature = tm_root;
    d.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    d.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    d.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    d.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = fmt;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = real_CreateGraphicsPipelineState(g_device, &d, &IID_ID3D12PipelineState, (void **)&pso);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
    if (FAILED(hr) || !pso) {
        tm_failed = 1;
        log_msg("tonemap PSO failed 0x%08lx (fmt %d); disabling tonemap pass", hr, (int)fmt);
        return 0;
    }
    tm_pso = pso;
    tm_pso_fmt = fmt;
    log_msg("tonemap pass ready (fmt %d)", (int)fmt);
    return 1;
}

static void run_tonemap(ID3D12GraphicsCommandList *cl, ListState *s) {
    if (!g_upscale || !fsr_enabled() || !g_upscale_tex || !s->num_rtv) {
        return;
    }
    if (cfg_debug_blink) {
        static LONG bc;
        if ((InterlockedIncrement(&bc) / 30) % 2 == 1) {
            return;
        }
    }
    const ViewEntry *dstv = view_find(s->rtv[0].ptr);
    if (!dstv || !dstv->resource || !ensure_heaps()) {
        return;
    }
    DXGI_FORMAT fmt = dstv->format != DXGI_FORMAT_UNKNOWN ? dstv->format : DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!ensure_tonemap(fmt)) {
        return;
    }
    ListState saved = *s;
    ID3D12Resource *up = g_upscale_tex;
    static int tm_log;
    if (!tm_log) {
        tm_log = 1;
        log_msg("tonemap draw: up %p srv_heap %p gpu %llx rtv %llx pso %p", (void *)up, (void *)srv_heap,
                (unsigned long long)g_up_gpu.ptr, (unsigned long long)s->rtv[0].ptr, (void *)tm_pso);
    }
    transition(cl, up, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *heap = srv_heap;
    real_SetDescriptorHeaps(cl, 1, &heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, tm_root);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, g_up_gpu);
    UINT rw = g_upscale_tex ? (UINT)({ D3D12_RESOURCE_DESC d; g_upscale_tex->lpVtbl->GetDesc(g_upscale_tex, &d); d.Width; }) : 1;
    UINT rh = g_upscale_tex ? (UINT)({ D3D12_RESOURCE_DESC d; g_upscale_tex->lpVtbl->GetDesc(g_upscale_tex, &d); d.Height; }) : 1;
    UINT hdr_mode = cfg_debug_raw ? 2u : (cfg_hdr ? 1u : 0u);
    float consts[3] = {(float)hdr_mode, 1.0f / (float)rw, 1.0f / (float)rh};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 1, 3, consts, 0);
    real_SetPipelineState(cl, tm_pso);
    real_OMSetRenderTargets(cl, 1, &s->rtv[0], TRUE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)dstv->width, (float)dstv->height, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)dstv->width, (LONG)dstv->height};
    real_RSSetViewports(cl, 1, &vp);
    real_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    real_DrawInstanced(cl, 3, 1, 0, 0);
    transition(cl, up, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    *s = saved;
    restore_after_fsr(cl, s);
    static LONG tm_frames;
    LONG n = InterlockedIncrement(&tm_frames);
    if (n == 1 || n % 600 == 0) {
        log_msg("tonemap pass frame %ld (%ux%u fmt %d)", n, dstv->width, dstv->height, (int)fmt);
    }
}

/* Log each distinct PSO that draws into a window back buffer (the display pass), once. */
static void diag_display_draw(ListState *s) {
    static ID3D12PipelineState *seen[8];
    static int seen_n;
    for (int i = 0; i < seen_n; ++i) {
        if (seen[i] == s->pso) {
            return;
        }
    }
    if (seen_n < 8) {
        seen[seen_n++] = s->pso;
    }
    const ViewEntry *c = s->num_rtv ? view_find(s->rtv[0].ptr) : NULL;
    log_msg("display-pass draw: pso %p vp %.0fx%.0f+%.0f+%.0f rtv %ux%u fmt %d", (void *)s->pso,
            s->num_vp ? s->vp[0].Width : 0.0f, s->num_vp ? s->vp[0].Height : 0.0f,
            s->num_vp ? s->vp[0].TopLeftX : 0.0f, s->num_vp ? s->vp[0].TopLeftY : 0.0f,
            c ? c->width : 0, c ? c->height : 0, c ? (int)c->format : -1);
}

static void setup_upscale_chains(void);
static void run_tonemap(ID3D12GraphicsCommandList *cl, ListState *s);

static void STDMETHODCALLTYPE hook_DrawInstanced(ID3D12GraphicsCommandList *cl, UINT vc, UINT ic, UINT sv, UINT si) {
    ListState *s = state_for(cl);
    ID3D12PipelineState *variant = NULL;
    int role = s ? pso_lookup(s->pso, &variant) : ROLE_NONE;
    if (role == ROLE_RESOLVE && s) {
        BOOL scene_ok = note_scene_resources(s);
        real_DrawInstanced(cl, vc, ic, sv, si);
        count_frame();
        setup_upscale_chains();
        if (scene_ok) {
            run_fsr(cl, s); /* both modes: native copies back, upscale publishes the HDR output */
        }
        return;
    } else if (role == ROLE_MV && variant && s && s->num_vp) {
        record_mv_pass(cl, s, variant, vc, ic, sv, si);
    }
    real_DrawInstanced(cl, vc, ic, sv, si);
    if (s && s->win_bb) {
        diag_display_draw(s);
        run_tonemap(cl, s);
    }
}

static void STDMETHODCALLTYPE hook_DrawIndexedInstanced(ID3D12GraphicsCommandList *cl, UINT index_count,
                                                        UINT instance_count, UINT start_index,
                                                        INT base_vertex, UINT start_instance) {
    ListState *s = state_for(cl);
    ID3D12PipelineState *variant = NULL;
    int role = s ? pso_lookup(s->pso, &variant) : ROLE_NONE;
    if (role == ROLE_RESOLVE && s) {
        /* FH2's resolve is a plain (non-indexed) draw on Azimuth 1.x; just track its targets. */
        note_scene_resources(s);
    } else if (role == ROLE_MV && variant && s && s->num_vp) {
        record_mv_pass_indexed(cl, s, variant, index_count, instance_count, start_index, base_vertex, start_instance);
    }
    real_DrawIndexedInstanced(cl, index_count, instance_count, start_index, base_vertex, start_instance);
    if (s && s->win_bb) {
        diag_display_draw(s);
        run_tonemap(cl, s);
    }
}

/* ---------------------------------------------------------------- device hooks */

static void hook_command_list(ID3D12GraphicsCommandList *cl) {
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, Reset), (void *)hook_Reset, (void **)&real_Reset);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetPipelineState), (void *)hook_SetPipelineState, (void **)&real_SetPipelineState);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, OMSetRenderTargets), (void *)hook_OMSetRenderTargets, (void **)&real_OMSetRenderTargets);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, RSSetViewports), (void *)hook_RSSetViewports, (void **)&real_RSSetViewports);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, RSSetScissorRects), (void *)hook_RSSetScissorRects, (void **)&real_RSSetScissorRects);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, DrawInstanced), (void *)hook_DrawInstanced, (void **)&real_DrawInstanced);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced), (void *)hook_DrawIndexedInstanced, (void **)&real_DrawIndexedInstanced);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, ResourceBarrier), (void *)hook_ResourceBarrier, (void **)&real_ResourceBarrier);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps), (void *)hook_SetDescriptorHeaps, (void **)&real_SetDescriptorHeaps);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetComputeRootSignature), (void *)hook_SetComputeRootSignature, (void **)&real_SetComputeRootSignature);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature), (void *)hook_SetGraphicsRootSignature, (void **)&real_SetGraphicsRootSignature);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable), (void *)hook_SetGraphicsRootDescriptorTable, (void **)&real_SetGraphicsRootDescriptorTable);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetGraphicsRootShaderResourceView), (void *)hook_SetGraphicsRootShaderResourceView, (void **)&real_SetGraphicsRootShaderResourceView);
}

static HRESULT STDMETHODCALLTYPE hook_CreateCommandList(ID3D12Device *dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                                        ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso,
                                                        REFIID riid, void **out) {
    HRESULT hr = real_CreateCommandList(dev, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out && type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12GraphicsCommandList *cl = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_ID3D12GraphicsCommandList, (void **)&cl))) {
            hook_command_list(cl);
            ListState *s = state_for(cl);
            if (s) {
                s->pso = pso;
            }
            ID3D12GraphicsCommandList_Release(cl);
        }
    }
    return hr;
}

static HRESULT WINAPI hook_D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **out) {
    HRESULT hr = real_D3D12CreateDevice(adapter, fl, riid, out);
    if (SUCCEEDED(hr) && out && *out && !g_device) {
        ID3D12Device *dev = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_ID3D12Device, (void **)&dev))) {
            g_device = dev; /* keep the reference for the process lifetime */
            patch_slot(VT_SLOT(dev, ID3D12DeviceVtbl, CreateGraphicsPipelineState), (void *)hook_CreateGraphicsPipelineState, (void **)&real_CreateGraphicsPipelineState);
            patch_slot(VT_SLOT(dev, ID3D12DeviceVtbl, CreateCommandList), (void *)hook_CreateCommandList, (void **)&real_CreateCommandList);
            patch_slot(VT_SLOT(dev, ID3D12DeviceVtbl, CreateRenderTargetView), (void *)hook_CreateRenderTargetView, (void **)&real_CreateRenderTargetView);
            patch_slot(VT_SLOT(dev, ID3D12DeviceVtbl, CreateDepthStencilView), (void *)hook_CreateDepthStencilView, (void **)&real_CreateDepthStencilView);
            patch_slot(VT_SLOT(dev, ID3D12DeviceVtbl, CreateShaderResourceView), (void *)hook_CreateShaderResourceView, (void **)&real_CreateShaderResourceView);
            g_srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            patch_slot(VT_SLOT(dev, ID3D12Device1Vtbl, CreatePipelineLibrary), (void *)hook_CreatePipelineLibrary, (void **)&real_CreatePipelineLibrary);
            fsr_init(dev);
            log_msg("D3D12 device hooked");
        }
    }
    return hr;
}

/* ---------------------------------------------------------------- hotkey */

static void poll_f11(void) {
    static SHORT last;
    SHORT now = GetAsyncKeyState(VK_F11) & 0x8000;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) {
        GetWindowThreadProcessId(fg, &pid);
    }
    if (pid != GetCurrentProcessId()) {
        last = now;
        return;
    }
    if (now && !last) {
        fsr_toggle();
    }
    last = now;
}

static volatile LONG hotkey_thread_started;

static DWORD WINAPI hotkey_thread_main(LPVOID unused) {
    (void)unused;
    for (;;) {
        poll_f11();
        Sleep(10);
    }
}

static void ensure_hotkey_thread(void) {
    if (InterlockedCompareExchange(&hotkey_thread_started, 1, 0) != 0) {
        return;
    }
    HANDLE thread = CreateThread(NULL, 0, hotkey_thread_main, NULL, 0, NULL);
    if (!thread) {
        InterlockedExchange(&hotkey_thread_started, 0);
        log_msg("F11 hotkey thread failed to start (error %lu)", GetLastError());
        return;
    }
    CloseHandle(thread);
}

/* ---------------------------------------------------------------- swap chains */

/* Called once both chains exist and the mode is resolved: snapshot the game chain's
   back buffers (the game's present pass draws into them). Runs on a render thread,
   never inside the factory creation path. */
static void setup_upscale_chains(void) {
    static int done;
    if (done || chain_count < 2 || !chain_obj[0] || !chain_obj[1] || !g_upscale) {
        return;
    }
    done = 1;
    UINT n = chain_desc[1].BufferCount;
    if (n > 8) {
        n = 8;
    }
    for (UINT i = 0; i < n; ++i) {
        ID3D12Resource *r = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetBuffer((IDXGISwapChain *)chain_obj[1], i, &IID_ID3D12Resource, (void **)&r)) && r) {
            window_bb[window_bb_n++] = r; /* raw ref kept; the chain owns the buffers */
            ID3D12Resource_Release(r);
        }
    }
    log_msg("upscale setup: %u game-chain back buffers cached (render %ux%u, display %ux%u)", window_bb_n, rend_w, rend_h,
            disp_w, disp_h);
}

/* No Present hooks here (Steam overlay double-hook crash; multiple swap chains in 1.x).
   The factory hook only records the chains (raw pointers + desc copies, no COM calls in the
   creation path) and starts the hotkey thread. */
static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForHwnd(IDXGIFactory2 *f, IUnknown *device, HWND hwnd,
                                                             const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
                                                             IDXGIOutput *out, IDXGISwapChain1 **sc) {
    HRESULT hr = real_CreateSwapChainForHwnd(f, device, hwnd, desc, fs, out, sc);
    if (SUCCEEDED(hr) && sc && *sc) {
        ensure_hotkey_thread();
        if (chain_count < 2) {
            chain_obj[chain_count] = (IUnknown *)*sc;
            chain_desc[chain_count] = *desc;
            ++chain_count;
        }
        log_msg("swap chain %ux%u format %d buffers %u flags 0x%x hooked (#%d)", desc->Width, desc->Height,
                (int)desc->Format, desc->BufferCount, desc->Flags, chain_count);
        if (chain_count == 2) {
            UINT a0 = chain_desc[0].Width * chain_desc[0].Height;
            UINT a1 = chain_desc[1].Width * chain_desc[1].Height;
            int win = a0 >= a1 ? 0 : 1; /* the bigger one is the display-sized window chain */
            disp_w = chain_desc[win].Width;
            disp_h = chain_desc[win].Height;
            rend_w = chain_desc[1 - win].Width;
            rend_h = chain_desc[1 - win].Height;
            if (cfg_hdr < 0) {
                cfg_hdr = 1; /* the scene buffer is always linear HDR (R11G11B10_FLOAT) */
            }
            fsr_config(cfg_hdr, cfg_sharpness);
            int area_r = (int)(rend_w * rend_h), area_d = (int)(disp_w * disp_h);
            g_upscale = cfg_mode == 1 ? 0 : (area_r < area_d);
            if (cfg_mode == 2 && area_r >= area_d) {
                log_msg("upscale mode requested but render %ux%u >= display %ux%u; using native AA", rend_w, rend_h,
                        disp_w, disp_h);
            }
            log_msg("display %ux%u, game chain created at %ux%u -> mode %s, hdr %s", disp_w, disp_h, rend_w, rend_h,
                    g_upscale ? "upscale" : "native", cfg_hdr ? "on" : "off");
        }
    }
    return hr;
}

static void hook_factory(void *factory) {
    IDXGIFactory2 *f2 = NULL;
    if (factory && SUCCEEDED(IUnknown_QueryInterface((IUnknown *)factory, &IID_IDXGIFactory2, (void **)&f2))) {
        patch_slot(VT_SLOT(f2, IDXGIFactory2Vtbl, CreateSwapChainForHwnd), (void *)hook_CreateSwapChainForHwnd, (void **)&real_CreateSwapChainForHwnd);
        IDXGIFactory2_Release(f2);
    }
}

#define RESOLVE(var, name)                                                                   \
    if (!var) {                                                                              \
        HMODULE m = proxy_system_dxgi();                                                     \
        *(FARPROC *)&var = m ? GetProcAddress(m, name) : NULL;                               \
    }

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void **out) {
    RESOLVE(real_CreateDXGIFactory, "CreateDXGIFactory");
    HRESULT hr = real_CreateDXGIFactory ? real_CreateDXGIFactory(riid, out) : E_FAIL;
    if (SUCCEEDED(hr)) {
        hook_factory(*out);
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **out) {
    RESOLVE(real_CreateDXGIFactory1, "CreateDXGIFactory1");
    HRESULT hr = real_CreateDXGIFactory1 ? real_CreateDXGIFactory1(riid, out) : E_FAIL;
    if (SUCCEEDED(hr)) {
        hook_factory(*out);
    }
    return hr;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void **out) {
    RESOLVE(real_CreateDXGIFactory2, "CreateDXGIFactory2");
    HRESULT hr = real_CreateDXGIFactory2 ? real_CreateDXGIFactory2(flags, riid, out) : E_FAIL;
    if (SUCCEEDED(hr)) {
        hook_factory(*out);
    }
    return hr;
}

/* ---------------------------------------------------------------- import patch */

static int patch_exe_import(void) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return 0;
    }
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; ++d) {
        if (lstrcmpiA((const char *)(base + d->Name), "d3d12.dll")) {
            continue;
        }
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            int match;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                match = IMAGE_ORDINAL(names->u1.Ordinal) == 101;
            } else {
                IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                match = !strcmp((const char *)ibn->Name, "D3D12CreateDevice");
            }
            if (match) {
                patch_slot((void **)&iat->u1.Function, (void *)hook_D3D12CreateDevice, (void **)&real_D3D12CreateDevice);
                return 1;
            }
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&log_lock);
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
        while (n && path[n - 1] != L'\\') {
            --n;
        }
        lstrcpyW(path + n, L"fh2_fsr.log");
        log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        log_msg("FH2 FSR 4 (Azimuth 1.x). F11: FSR on/off. Native AA at render==display, FSR upscaling when render < display");

        /* Optional fh2_fsr.ini next to the DLL: [fsr] mode=auto|native|upscale hdr=auto|on|off sharpness debug_blink */
        wchar_t ini[MAX_PATH], v[64];
        lstrcpyW(ini, path);
        lstrcpyW(ini + n, L"fh2_fsr.ini");
        GetPrivateProfileStringW(L"fsr", L"mode", L"auto", v, 64, ini);
        cfg_mode = !lstrcmpiW(v, L"native") ? 1 : (!lstrcmpiW(v, L"upscale") ? 2 : 0);
        GetPrivateProfileStringW(L"fsr", L"hdr", L"auto", v, 64, ini);
        cfg_hdr = !lstrcmpiW(v, L"on") ? 1 : (!lstrcmpiW(v, L"off") ? 0 : -1);
        GetPrivateProfileStringW(L"fsr", L"sharpness", L"-1", v, 64, ini);
        cfg_sharpness = (float)_wtof(v);
        GetPrivateProfileStringW(L"fsr", L"debug_blink", L"0", v, 64, ini);
        GetPrivateProfileStringW(L"fsr", L"debug_raw", L"0", v, 64, ini);
        cfg_debug_raw = _wtoi(v);
        cfg_debug_blink = _wtoi(v);
        fsr_config(cfg_hdr >= 0 ? cfg_hdr : 1, cfg_sharpness);
        log_msg("config: mode=%d hdr=%d sharpness=%.2f debug_blink=%d", cfg_mode, cfg_hdr, cfg_sharpness, cfg_debug_blink);

        log_msg(patch_exe_import() ? "D3D12CreateDevice import patched" : "D3D12CreateDevice import NOT found");
    }
    return TRUE;
}
