/* Shared between hooks.c and fsr.c. */
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

void log_msg(const char *fmt, ...);

/* Legacy transition barrier issued through the unhooked ResourceBarrier. */
void hooks_transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                      D3D12_RESOURCE_STATES to);

/* fsr.c */
void fsr_init(ID3D12Device *device);
/* hdr: 1 = pass FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE, 0 = LDR. sharpness < 0 disables sharpening. */
void fsr_config(int hdr, float sharpness);
int fsr_enabled(void);
void fsr_toggle(void);
/* Jitter for the frame currently being recorded, in render pixels. Returns 0 when off. */
int fsr_jitter(float *x, float *y);
/* Runs FSR on the HDR scene `color` (render size) into an internal output texture of
   out_w x out_h (upscale size; same as the render size for Native AA). The result is NOT
   copied back into `color`; fetch it with fsr_output(). States are the resources' current
   legacy states; they are left unchanged. */
int fsr_run(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
            ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource *motion,
            D3D12_RESOURCE_STATES motion_state, UINT out_w, UINT out_h);
/* The internal output texture after a successful fsr_run (state: UNORDERED_ACCESS). */
ID3D12Resource *fsr_output(void);
/* Called once per scene frame at the dispatch site: updates the jitter live/reset state. `ran`
   says whether FSR ran this frame. (Not driven from Present: Azimuth 1.x swap chains are not a
   reliable frame boundary and Present is also hooked by the Steam overlay.) */
void fsr_end_frame(int ran);
