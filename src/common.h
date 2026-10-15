/* Shared between hooks.c and fsr.c. */
#pragma once
#include <windows.h>
#include <d3d12.h>

void log_msg(const char *fmt, ...);

/* Legacy transition barrier issued through the unhooked ResourceBarrier. */
void hooks_transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                      D3D12_RESOURCE_STATES to);

/* fsr.c */
void fsr_init(ID3D12Device *device);
int fsr_enabled(void);
void fsr_toggle(void);
/* Jitter for the frame currently being recorded, in render pixels. Returns 0 when off. */
int fsr_jitter(float *x, float *y);
/* Runs FSR Native AA on `color` in place (through an internal output texture and a copy).
   States are the resources' current legacy states; they are left unchanged. */
int fsr_run(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
            ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource *motion,
            D3D12_RESOURCE_STATES motion_state);
/* Called once per Present: advances the jitter sequence. `ran` says whether FSR ran this frame. */
void fsr_end_frame(int ran);
