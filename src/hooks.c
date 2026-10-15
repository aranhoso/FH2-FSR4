/* FH2 FSR: FSR 4 Native AA for Azimuth (jitter, full-resolution motion vectors, debug overlay).
   Loaded as dxgi.dll next to azimuth.exe. Hooks D3D12CreateDevice through the exe's
   import table, then device / command-list / factory / swap-chain vtables. */
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

/* DXBC checksums of the game's pixel shaders we key on. */
static const DWORD MV_PS_HASH[4] = {0x2cfbd506, 0x9e9deb9f, 0x0ea59850, 0x7cc88f30};      /* motion blur velocity */
static const DWORD RESOLVE_PS_HASH[4] = {0x576974a4, 0xf0c91dfd, 0xc3ca28aa, 0xe1f4ac4d}; /* scene colour+depth resolve */
static const DWORD TONEMAP_PS_HASH[4] = {0x8d1cc763, 0x3a5f1f21, 0xb8f6e46b, 0x66bf6d19}; /* composite / tonemap */

enum { ROLE_NONE, ROLE_MV, ROLE_RESOLVE, ROLE_TONEMAP };

typedef HRESULT(WINAPI *PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                     ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                     ID3DBlob **);

static SRWLOCK pso_lock = SRWLOCK_INIT;
static ID3D12Device *g_device;
static ID3D12CommandQueue *g_queue;
static DXGI_FORMAT g_backbuffer_format = DXGI_FORMAT_R10G10B10A2_UNORM;

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
} ViewEntry;
#define MAX_VIEWS 16384
static ViewEntry views[MAX_VIEWS];

/* Motion-vector target. */
static ID3D12Resource *mv_tex;
static UINT mv_w, mv_h;
static ID3D12DescriptorHeap *rtv_heap;   /* 0: MV, 1..4: back buffers */
static ID3D12DescriptorHeap *srv_heap;   /* shader visible, 0: MV */
static UINT rtv_inc;
static volatile LONG64 mv_frame;         /* present count when MV was last written */
static volatile LONG64 present_count;
static ID3D12Resource *retired[16];
static LONG64 retired_at[16];
static SRWLOCK mv_lock = SRWLOCK_INIT;

/* Overlay. */
static ID3D12RootSignature *ov_root;
static ID3D12PipelineState *ov_pso;
static ID3D12CommandAllocator *ov_alloc[3];
static ID3D12GraphicsCommandList *ov_list;
static ID3D12Fence *ov_fence;
static UINT64 ov_fence_value[3], ov_fence_next = 1;
static HANDLE ov_event;
static int overlay_mode;                 /* 0 off, 1 blended, 2 opaque */
static float overlay_scale = 16.0f;      /* pixels of motion shown at full brightness */
static int ov_ready, ov_failed;

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
static void(STDMETHODCALLTYPE *real_ResourceBarrier)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
static void(STDMETHODCALLTYPE *real_SetDescriptorHeaps)(ID3D12GraphicsCommandList *, UINT, ID3D12DescriptorHeap *const *);
static void(STDMETHODCALLTYPE *real_SetComputeRootSignature)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
static void(STDMETHODCALLTYPE *real_SetGraphicsRootSignature)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
static void(STDMETHODCALLTYPE *real_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE *real_CreateRenderTargetView)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static void(STDMETHODCALLTYPE *real_CreateDepthStencilView)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
static HRESULT(WINAPI *real_CreateDXGIFactory)(REFIID, void **);
static HRESULT(WINAPI *real_CreateDXGIFactory1)(REFIID, void **);
static HRESULT(WINAPI *real_CreateDXGIFactory2)(UINT, REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *real_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
static HRESULT(STDMETHODCALLTYPE *real_Present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *real_Present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
static HRESULT(STDMETHODCALLTYPE *real_ResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static HRESULT(STDMETHODCALLTYPE *real_ResizeBuffers1)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *);

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
    if (!memcmp(h, TONEMAP_PS_HASH, 16)) {
        return ROLE_TONEMAP;
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
        log_msg("%s pass found (%s)", role == ROLE_RESOLVE ? "scene resolve" : "tonemap", source);
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
    BOOL has_dsv;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    BOOL dsv_scene;      /* the scene depth buffer is bound: viewports get the FSR jitter */
    BOOL vp_jittered;
    UINT num_vp, num_sc;
    D3D12_VIEWPORT vp[4]; /* as the game set them (unjittered) */
    D3D12_RECT sc[4];
    UINT num_heaps;
    ID3D12DescriptorHeap *heaps[2];
    ID3D12RootSignature *compute_rs;
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
static volatile LONG lists_created;

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

static void view_record(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res, DXGI_FORMAT view_format) {
    UINT i = view_hash(h.ptr);
    ViewEntry *e = NULL;
    for (UINT k = 0; k < 32; ++k) {
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
}

static const ViewEntry *view_find(SIZE_T handle) {
    UINT i = view_hash(handle);
    for (UINT k = 0; k < 32; ++k) {
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
    view_record(h, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
}

static void STDMETHODCALLTYPE hook_CreateDepthStencilView(ID3D12Device *dev, ID3D12Resource *res,
                                                          const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                                          D3D12_CPU_DESCRIPTOR_HANDLE h) {
    real_CreateDepthStencilView(dev, res, desc, h);
    view_record(h, res, desc ? desc->Format : DXGI_FORMAT_UNKNOWN);
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
    ListState *s = state_for(cl);
    if (s) {
        s->num_heaps = n > 2 ? 2 : n;
        memcpy(s->heaps, h, s->num_heaps * sizeof(*h));
    }
    real_SetDescriptorHeaps(cl, n, h);
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

static int ensure_heaps(void) {
    if (rtv_heap && srv_heap) {
        return 1;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 8;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap))) {
        return 0;
    }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(g_device, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap))) {
        return 0;
    }
    rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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
    ID3D12Device_CreateShaderResourceView(g_device, mv_tex, NULL,
                                          cpu_handle(srv_heap, 0, ID3D12Device_GetDescriptorHandleIncrementSize(g_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    mv_w = w;
    mv_h = h;
    log_msg("motion-vector texture %ux%u R16G16_FLOAT", w, h);
    return 1;
}

static volatile LONG64 mv_seq;          /* motion-vector passes recorded */
static LONG64 fsr_mv_seq;                /* the motion-vector pass the last FSR dispatch used */
static volatile LONG fsr_ran_since_present;

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
        InterlockedExchange64(&mv_frame, present_count);
        InterlockedIncrement64(&mv_seq);
        static int once;
        if (!once) {
            once = 1;
            log_msg("first full-resolution motion-vector pass recorded (%ux%u)", w, h);
        }
    }
    ReleaseSRWLockExclusive(&mv_lock);
}

static void note_scene_resources(ListState *s) {
    if (!s->num_rtv || !s->has_dsv) {
        return;
    }
    const ViewEntry *c = view_find(s->rtv[0].ptr), *d = view_find(s->dsv.ptr);
    if (!c || !d || !c->resource || !d->resource) {
        static int warned;
        if (!warned++) {
            log_msg("scene resolve: render-target/depth views not in the view map");
        }
        return;
    }
    if (c->resource != scene_color || d->resource != scene_depth) {
        scene_color = c->resource;
        scene_depth = d->resource;
        log_msg("scene colour %p (%ux%u fmt %d), scene depth %p (%ux%u fmt %d)", (void *)scene_color, c->width,
                c->height, (int)c->format, (void *)scene_depth, d->width, d->height, (int)d->format);
    }
}

static void run_fsr(ID3D12GraphicsCommandList *cl, ListState *s) {
    LONG64 seq = mv_seq;
    if (!fsr_enabled() || !scene_color || !scene_depth || !mv_tex || seq == fsr_mv_seq) {
        return;
    }
    fsr_mv_seq = seq;
    static int logged_states;
    if (!logged_states++) {
        log_msg("FSR inputs: colour state 0x%lx, depth state 0x%lx", scene_color_state, scene_depth_state);
    }
    /* FSR records through the same (hooked) list methods, so keep the game's view of the list
       and of the scene resources' states, and put both back afterwards. */
    ListState saved = *s;
    LONG color_state = scene_color_state, depth_state = scene_depth_state;
    AcquireSRWLockShared(&mv_lock);
    int ok = fsr_run(cl, scene_color, (D3D12_RESOURCE_STATES)color_state, scene_depth,
                     (D3D12_RESOURCE_STATES)depth_state, mv_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ReleaseSRWLockShared(&mv_lock);
    InterlockedExchange(&scene_color_state, color_state);
    InterlockedExchange(&scene_depth_state, depth_state);
    *s = saved;
    restore_after_fsr(cl, s);
    if (ok) {
        InterlockedExchange(&fsr_ran_since_present, 1);
    }
}

/* The resolve draw by its targets, for builds of the shader we have no hash for (the game's
   resolve shader differs with Azimuth's MSAA setting): one R11G11B10 colour target plus a
   depth-only D32 target, both at the motion-vector (render) size. */
static BOOL looks_like_resolve(ListState *s) {
    if (s->num_rtv != 1 || !s->has_dsv || !mv_w) {
        return FALSE;
    }
    const ViewEntry *c = view_find(s->rtv[0].ptr);
    if (!c || c->format != DXGI_FORMAT_R11G11B10_FLOAT || c->width != mv_w || c->height != mv_h) {
        return FALSE;
    }
    const ViewEntry *d = view_find(s->dsv.ptr);
    if (!d || (d->format != DXGI_FORMAT_D32_FLOAT && d->format != DXGI_FORMAT_R32_TYPELESS) ||
        d->width != mv_w || d->height != mv_h) {
        return FALSE;
    }
    static ID3D12PipelineState *logged;
    if (logged != s->pso) {
        logged = s->pso;
        log_msg("scene resolve recognised by its targets (PSO %p)", (void *)s->pso);
    }
    return TRUE;
}

static void STDMETHODCALLTYPE hook_DrawInstanced(ID3D12GraphicsCommandList *cl, UINT vc, UINT ic, UINT sv, UINT si) {
    ListState *s = state_for(cl);
    ID3D12PipelineState *variant = NULL;
    int role = s ? pso_lookup(s->pso, &variant) : ROLE_NONE;
    if (role == ROLE_NONE && s && looks_like_resolve(s)) {
        role = ROLE_RESOLVE;
    }
    if (role == ROLE_RESOLVE) {
        note_scene_resources(s);
    } else if (role == ROLE_MV && variant && s->num_vp) {
        record_mv_pass(cl, s, variant, vc, ic, sv, si);
    } else if (role == ROLE_TONEMAP) {
        run_fsr(cl, s);
    }
    real_DrawInstanced(cl, vc, ic, sv, si);
}

/* ---------------------------------------------------------------- device hooks */

static void hook_command_list(ID3D12GraphicsCommandList *cl) {
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, Reset), (void *)hook_Reset, (void **)&real_Reset);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetPipelineState), (void *)hook_SetPipelineState, (void **)&real_SetPipelineState);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, OMSetRenderTargets), (void *)hook_OMSetRenderTargets, (void **)&real_OMSetRenderTargets);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, RSSetViewports), (void *)hook_RSSetViewports, (void **)&real_RSSetViewports);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, RSSetScissorRects), (void *)hook_RSSetScissorRects, (void **)&real_RSSetScissorRects);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, DrawInstanced), (void *)hook_DrawInstanced, (void **)&real_DrawInstanced);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, ResourceBarrier), (void *)hook_ResourceBarrier, (void **)&real_ResourceBarrier);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps), (void *)hook_SetDescriptorHeaps, (void **)&real_SetDescriptorHeaps);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetComputeRootSignature), (void *)hook_SetComputeRootSignature, (void **)&real_SetComputeRootSignature);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature), (void *)hook_SetGraphicsRootSignature, (void **)&real_SetGraphicsRootSignature);
    patch_slot(VT_SLOT(cl, ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable), (void *)hook_SetGraphicsRootDescriptorTable, (void **)&real_SetGraphicsRootDescriptorTable);
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
            LONG n = InterlockedIncrement(&lists_created);
            if (n == 1 || n == 16 || n == 256 || n % 4096 == 0) {
                log_msg("direct command lists created so far: %ld", n);
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
            patch_slot(VT_SLOT(dev, ID3D12Device1Vtbl, CreatePipelineLibrary), (void *)hook_CreatePipelineLibrary, (void **)&real_CreatePipelineLibrary);
            fsr_init(dev);
            log_msg("D3D12 device hooked");
        }
    }
    return hr;
}

/* ---------------------------------------------------------------- overlay */

static int ensure_overlay(void) {
    if (ov_ready || ov_failed) {
        return ov_ready;
    }
    ov_failed = 1;
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2];
    memset(params, 0, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = {2, params, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    typedef HRESULT(WINAPI * PFN_Ser)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    PFN_Ser ser = (PFN_Ser)(void *)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    ID3DBlob *blob = NULL, *err = NULL;
    if (!ser || FAILED(ser(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        log_msg("overlay root signature serialization failed");
        return 0;
    }
    HRESULT hr = ID3D12Device_CreateRootSignature(g_device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                  &IID_ID3D12RootSignature, (void **)&ov_root);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) {
        return 0;
    }
    ID3DBlob *vs = compile(OVERLAY_HLSL, "vs", "vs_5_0"), *ps = compile(OVERLAY_HLSL, "ps", "ps_5_0");
    if (!vs || !ps) {
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    memset(&d, 0, sizeof(d));
    d.pRootSignature = ov_root;
    d.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    d.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    d.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    d.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    D3D12_RENDER_TARGET_BLEND_DESC *bt = &d.BlendState.RenderTarget[0];
    bt->BlendEnable = TRUE;
    bt->SrcBlend = D3D12_BLEND_SRC_ALPHA;
    bt->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    bt->BlendOp = D3D12_BLEND_OP_ADD;
    bt->SrcBlendAlpha = D3D12_BLEND_ONE;
    bt->DestBlendAlpha = D3D12_BLEND_ZERO;
    bt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bt->LogicOp = D3D12_LOGIC_OP_NOOP;
    bt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = g_backbuffer_format;
    d.SampleDesc.Count = 1;
    hr = real_CreateGraphicsPipelineState(g_device, &d, &IID_ID3D12PipelineState, (void **)&ov_pso);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
    if (FAILED(hr)) {
        log_msg("overlay PSO failed 0x%08lx", hr);
        return 0;
    }
    for (int i = 0; i < 3; ++i) {
        if (FAILED(ID3D12Device_CreateCommandAllocator(g_device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ov_alloc[i]))) {
            return 0;
        }
    }
    if (FAILED(real_CreateCommandList(g_device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ov_alloc[0], NULL,
                                      &IID_ID3D12GraphicsCommandList, (void **)&ov_list))) {
        return 0;
    }
    ID3D12GraphicsCommandList_Close(ov_list);
    if (FAILED(ID3D12Device_CreateFence(g_device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&ov_fence))) {
        return 0;
    }
    ov_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ov_failed = 0;
    ov_ready = 1;
    log_msg("overlay ready (back buffer format %d)", (int)g_backbuffer_format);
    return 1;
}

static void poll_keys(void) {
    static SHORT last9, last10, last11;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) {
        GetWindowThreadProcessId(fg, &pid);
    }
    if (pid != GetCurrentProcessId()) {
        return;
    }
    SHORT k9 = GetAsyncKeyState(VK_F9) & 0x8000, k10 = GetAsyncKeyState(VK_F10) & 0x8000;
    if (k9 && !last9) {
        overlay_mode = (overlay_mode + 1) % 3;
        log_msg("overlay mode %d", overlay_mode);
    }
    if (k10 && !last10) {
        overlay_scale = overlay_scale >= 64.0f ? 2.0f : overlay_scale * 2.0f;
        log_msg("overlay full brightness at %.0f px of motion", overlay_scale);
    }
    SHORT k11 = GetAsyncKeyState(VK_F11) & 0x8000;
    if (k11 && !last11) {
        fsr_toggle();
    }
    last9 = k9;
    last10 = k10;
    last11 = k11;
}

static void draw_overlay(IDXGISwapChain *sc) {
    if (!overlay_mode || !g_queue || !mv_tex || present_count - mv_frame > 1 || !ensure_overlay()) {
        return;
    }
    IDXGISwapChain3 *sc3 = NULL;
    if (FAILED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3))) {
        return;
    }
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(sc3);
    ID3D12Resource *bb = NULL;
    HRESULT hr = IDXGISwapChain3_GetBuffer(sc3, index, &IID_ID3D12Resource, (void **)&bb);
    IDXGISwapChain3_Release(sc3);
    if (FAILED(hr)) {
        return;
    }
    D3D12_RESOURCE_DESC bd;
    bb->lpVtbl->GetDesc(bb, &bd);
    UINT slot = (UINT)(ov_fence_next % 3);
    if (ID3D12Fence_GetCompletedValue(ov_fence) < ov_fence_value[slot]) {
        ID3D12Fence_SetEventOnCompletion(ov_fence, ov_fence_value[slot], ov_event);
        WaitForSingleObject(ov_event, 100);
    }
    ID3D12CommandAllocator_Reset(ov_alloc[slot]);
    real_Reset(ov_list, ov_alloc[slot], ov_pso);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_handle(rtv_heap, 1 + (index & 3), rtv_inc);
    D3D12_RENDER_TARGET_VIEW_DESC rv;
    memset(&rv, 0, sizeof(rv));
    rv.Format = g_backbuffer_format;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ID3D12Device_CreateRenderTargetView(g_device, bb, &rv, rtv);
    transition(ov_list, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    AcquireSRWLockShared(&mv_lock);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(ov_list, 1, &srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(ov_list, ov_root);
    D3D12_GPU_DESCRIPTOR_HANDLE gh;
    srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap, &gh);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(ov_list, 0, gh);
    struct {
        UINT mode;
        float scale, w, h;
    } c = {(UINT)overlay_mode, overlay_scale, (float)bd.Width, (float)bd.Height};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(ov_list, 1, 4, &c, 0);
    real_OMSetRenderTargets(ov_list, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)bd.Width, (float)bd.Height, 0, 1};
    D3D12_RECT r = {0, 0, (LONG)bd.Width, (LONG)bd.Height};
    real_RSSetViewports(ov_list, 1, &vp);
    real_RSSetScissorRects(ov_list, 1, &r);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(ov_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    real_DrawInstanced(ov_list, 3, 1, 0, 0);
    transition(ov_list, bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ID3D12GraphicsCommandList_Close(ov_list);
    ID3D12CommandList *lists_[1] = {(ID3D12CommandList *)ov_list};
    ID3D12CommandQueue_ExecuteCommandLists(g_queue, 1, lists_);
    ReleaseSRWLockShared(&mv_lock);
    ov_fence_value[slot] = ov_fence_next;
    ID3D12CommandQueue_Signal(g_queue, ov_fence, ov_fence_next++);
    ID3D12Resource_Release(bb);
}

static void before_present(IDXGISwapChain *sc) {
    poll_keys();
    draw_overlay(sc);
    fsr_end_frame(InterlockedExchange(&fsr_ran_since_present, 0));
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

/* ---------------------------------------------------------------- swap chain */

static HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        before_present(sc);
    }
    return real_Present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hook_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        before_present((IDXGISwapChain *)sc);
    }
    return real_Present1(sc, sync, flags, p);
}

static void flush_overlay(void) {
    if (ov_fence && g_queue && ov_fence_next > 1) {
        if (ID3D12Fence_GetCompletedValue(ov_fence) < ov_fence_next - 1) {
            ID3D12Fence_SetEventOnCompletion(ov_fence, ov_fence_next - 1, ov_event);
            WaitForSingleObject(ov_event, 1000);
        }
    }
}

static HRESULT STDMETHODCALLTYPE hook_ResizeBuffers(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    flush_overlay();
    if (f != DXGI_FORMAT_UNKNOWN && f != g_backbuffer_format) {
        log_msg("back buffer format changed to %d; overlay disabled", (int)f);
        ov_failed = 1;
    }
    return real_ResizeBuffers(sc, n, w, h, f, fl);
}

static HRESULT STDMETHODCALLTYPE hook_ResizeBuffers1(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                                     const UINT *mask, IUnknown *const *queues) {
    flush_overlay();
    if (f != DXGI_FORMAT_UNKNOWN && f != g_backbuffer_format) {
        log_msg("back buffer format changed to %d; overlay disabled", (int)f);
        ov_failed = 1;
    }
    return real_ResizeBuffers1(sc, n, w, h, f, fl, mask, queues);
}

static HRESULT STDMETHODCALLTYPE hook_CreateSwapChainForHwnd(IDXGIFactory2 *f, IUnknown *device, HWND hwnd,
                                                             const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
                                                             IDXGIOutput *out, IDXGISwapChain1 **sc) {
    HRESULT hr = real_CreateSwapChainForHwnd(f, device, hwnd, desc, fs, out, sc);
    if (SUCCEEDED(hr) && sc && *sc) {
        ID3D12CommandQueue *q = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&q))) {
            if (g_queue) {
                ID3D12CommandQueue_Release(g_queue);
            }
            g_queue = q;
        }
        g_backbuffer_format = desc->Format;
        patch_slot(VT_SLOT(*sc, IDXGISwapChain1Vtbl, Present), (void *)hook_Present, (void **)&real_Present);
        patch_slot(VT_SLOT(*sc, IDXGISwapChain1Vtbl, Present1), (void *)hook_Present1, (void **)&real_Present1);
        patch_slot(VT_SLOT(*sc, IDXGISwapChain1Vtbl, ResizeBuffers), (void *)hook_ResizeBuffers, (void **)&real_ResizeBuffers);
        IDXGISwapChain3 *sc3 = NULL;
        if (SUCCEEDED(IDXGISwapChain1_QueryInterface(*sc, &IID_IDXGISwapChain3, (void **)&sc3))) {
            patch_slot(VT_SLOT(sc3, IDXGISwapChain3Vtbl, ResizeBuffers1), (void *)hook_ResizeBuffers1, (void **)&real_ResizeBuffers1);
            IDXGISwapChain3_Release(sc3);
        }
        log_msg("swap chain %ux%u format %d hooked (queue %p)", desc->Width, desc->Height, (int)desc->Format, (void *)g_queue);
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
        log_msg("FH2 FSR 4 Native AA. F11: FSR on/off, F9: motion-vector overlay off/blended/opaque, F10: overlay scale");
        log_msg(patch_exe_import() ? "D3D12CreateDevice import patched" : "D3D12CreateDevice import NOT found");
    }
    return TRUE;
}
