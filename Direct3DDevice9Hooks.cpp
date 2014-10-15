//====================================================================
// Hooked IDirect3DDevice9 interface implementation.
//
// This is where most of the magic happens. This module sits in
// between Pinball Arcade and DirectX and augments the call stream
// to create stereo rendering, implement head tracking, etc.
//
// Most function implementations are straight thunks, but many have
// been augmented to track important state or redirect calls for
// VR rendering purposes.
//====================================================================

#include <d3dx9.h>
#include "Direct3DDevice9Hooks.h"
#include "hacks.h"

#define OVR_D3D_VERSION 9
#include <OVR_CAPI_D3D.h>

static const float s_identity_matrix[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

static void patch_timestep (int frame_hz)
{
    rolling_crc crc;
    crc.block_size = 9;
    crc.a = 0x05cc;
    crc.b = 0x1acf;

    uintptr_t address = find_fingerprint(crc);
    uintptr_t ms_per_tick_compare = address + 0x00C5D0B5 - 0x00C5D046;

    // Get the address of the intervals_per_tick global by reading its address from
    // an instruction that uses it as an operand.
    uintptr_t ms_per_tick_address;
    read_code(ms_per_tick_compare + 2, sizeof(ms_per_tick_address), &ms_per_tick_address);

    // Reassign it to the frame hz.
    double intervals_per_tick = 1000 / (double)frame_hz;
    install_patch(ms_per_tick_address, sizeof(intervals_per_tick), &intervals_per_tick);

    // Get the global step scale and divide it by two
    uintptr_t global_step_address;
    uintptr_t global_step_load = address + 0x00C5D0F7 - 0x00C5D046;
    read_code(global_step_load + 2, sizeof(global_step_address), &global_step_address);
    float* global_step = (float*)global_step_address;
    float new_global_step = *global_step * (60 / (float)frame_hz);
    install_patch(global_step_address, sizeof(new_global_step), &new_global_step);

    // Get the address of the instruction that assigns the simulation steps
    unsigned milliseconds_per_frame = 1000 / frame_hz;
    uintptr_t frame_interval_load = address + 0x00C5D100 - 0x00C5D046;
    install_patch(frame_interval_load + 1, sizeof(milliseconds_per_frame), &milliseconds_per_frame);
}

Direct3DDevice9Hooks::Direct3DDevice9Hooks (IDirect3D9* parent, IDirect3DDevice9* inner, const D3DPRESENT_PARAMETERS& present_parameters, ovrHmd hmd)
{
    this->parent = parent;
    this->inner = inner;
    this->present_parameters = present_parameters;
    this->stereo_quad_buffer = 0;
    this->hmd = hmd;
    this->stereo = false;
    this->render_distorted = true;
    this->reset_pressed = false;
    this->frame_index = 0;
    memset(&this->current_stream, 0, sizeof(this->current_stream));
    this->inner->GetRenderTarget(0, &this->back_buffer_surface);

    if (present_parameters.Windowed == 0)
    {
        D3DDISPLAYMODE display_mode;
        this->inner->GetDisplayMode(0, &display_mode);
        patch_timestep(display_mode.RefreshRate);
    }

    if (this->hmd)
    {
        OVR::Sizei left_size = ovrHmd_GetFovTextureSize(hmd, ovrEye_Left, hmd->DefaultEyeFov[0], 1.0f);
        OVR::Sizei right_size = ovrHmd_GetFovTextureSize(hmd, ovrEye_Right, hmd->DefaultEyeFov[1], 1.0f);
        this->target_size = OVR::Sizei(left_size.w + right_size.w, max(left_size.h, right_size.h));
        this->target_size = OVR::Sizei(this->present_parameters.BackBufferWidth, this->present_parameters.BackBufferHeight);

        ovrD3D9Config cfg;
        cfg.D3D9.Header.API = ovrRenderAPI_D3D9;
        cfg.D3D9.Header.RTSize = OVR::Sizei(this->present_parameters.BackBufferWidth, this->present_parameters.BackBufferHeight);
        cfg.D3D9.Header.Multisample = this->present_parameters.MultiSampleQuality;
        cfg.D3D9.pDevice = this->inner;
        cfg.D3D9.pSwapChain = 0;
        unsigned caps =
            ovrDistortionCap_Chromatic
            | ovrDistortionCap_NoRestore
            | ovrDistortionCap_SRGB
            | ovrDistortionCap_Overdrive;
        if (!ovrHmd_ConfigureRendering(this->hmd, &cfg.Config, caps, hmd->DefaultEyeFov, this->eye_render_desc))
        {
            this->hmd = 0;
            this->hmd_texture = 0;
        }
        else
        {
            this->inner->CreateTexture(
                this->present_parameters.BackBufferWidth,
                this->present_parameters.BackBufferHeight,
                1,  // Levels
                D3DUSAGE_RENDERTARGET,
                this->present_parameters.BackBufferFormat,
                D3DPOOL_DEFAULT,
                &this->hmd_texture,
                NULL // pSharedHandle
            );

            // As Pinball Arcade gets patched, its code is likely to move around, so
            // we keep a fingerprint hash of some stable code close to the matrix
            // call site we need to patch. We can search the code segment for things
            // matching this fingerprint and rediscover the location of the code we
            // want to patch.
            rolling_crc VIEW_PROJECTION_MULTIPLY_FINGERPRINT;
            VIEW_PROJECTION_MULTIPLY_FINGERPRINT.block_size = 0x18;
            VIEW_PROJECTION_MULTIPLY_FINGERPRINT.a = 0x0f20;
            VIEW_PROJECTION_MULTIPLY_FINGERPRINT.b = 0xb638;
            size_t VIEW_PROJECTION_MULTIPLY_FINGERPRINT_OFFSET = 0x25;

            // Create a patch that loads two identity matrices instead of the view and
            // projection matrices so that when C_WORLDVIEWPROJ shader constants get set,
            // get only the WORLD part of the transformation and can apply our own
            // view and projection matrices
            unsigned char patch[10];
            patch[0] = 0xB8; // MOV eax
            *(uintptr_t*)&patch[1] = (uintptr_t)s_identity_matrix;
            patch[5] = 0xB9; // MOV ecx
            *(uintptr_t*)&patch[6] = (uintptr_t)s_identity_matrix;

            uintptr_t address = find_fingerprint(VIEW_PROJECTION_MULTIPLY_FINGERPRINT);
            install_patch(address + VIEW_PROJECTION_MULTIPLY_FINGERPRINT_OFFSET, sizeof(patch), patch);
        }
    }
}

HRESULT Direct3DDevice9Hooks::QueryInterface (REFIID riid, void** ppvObj)
{
    return this->inner->QueryInterface(riid, ppvObj);
}

ULONG Direct3DDevice9Hooks::AddRef ()
{
    return this->inner->AddRef();
}

ULONG Direct3DDevice9Hooks::Release ()
{
    return this->inner->Release();
}

/*** IDirect3DDevice9 methods ***/
HRESULT Direct3DDevice9Hooks::TestCooperativeLevel ()
{
    return this->inner->TestCooperativeLevel();
}

UINT Direct3DDevice9Hooks::GetAvailableTextureMem ()
{
    return this->inner->GetAvailableTextureMem();
}

HRESULT Direct3DDevice9Hooks::EvictManagedResources ()
{
    return this->inner->EvictManagedResources();
}

HRESULT Direct3DDevice9Hooks::GetDirect3D (IDirect3D9** ppD3D9)
{
    *ppD3D9 = this->parent;
    return D3D_OK;
}

HRESULT Direct3DDevice9Hooks::GetDeviceCaps (D3DCAPS9* pCaps)
{
    return this->inner->GetDeviceCaps(pCaps);
}

HRESULT Direct3DDevice9Hooks::GetDisplayMode (UINT iSwapChain,D3DDISPLAYMODE* pMode)
{
    return this->inner->GetDisplayMode(iSwapChain, pMode);
}

HRESULT Direct3DDevice9Hooks::GetCreationParameters (D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
    return this->inner->GetCreationParameters(pParameters);
}

HRESULT Direct3DDevice9Hooks::SetCursorProperties (UINT XHotSpot,UINT YHotSpot,IDirect3DSurface9* pCursorBitmap)
{
    return this->inner->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
}

void Direct3DDevice9Hooks::SetCursorPosition (int X,int Y,DWORD Flags)
{
    return this->inner->SetCursorPosition(X, Y, Flags);
}

BOOL Direct3DDevice9Hooks::ShowCursor (BOOL bShow)
{
    return this->inner->ShowCursor(bShow);
}

HRESULT Direct3DDevice9Hooks::CreateAdditionalSwapChain (D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DSwapChain9** pSwapChain)
{
    return this->inner->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
}

HRESULT Direct3DDevice9Hooks::GetSwapChain (UINT iSwapChain,IDirect3DSwapChain9** pSwapChain)
{
    return this->inner->GetSwapChain(iSwapChain, pSwapChain);
}

UINT Direct3DDevice9Hooks::GetNumberOfSwapChains ()
{
    return this->inner->GetNumberOfSwapChains();
}

HRESULT Direct3DDevice9Hooks::Reset (D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    return this->inner->Reset(pPresentationParameters);
}

HRESULT Direct3DDevice9Hooks::Present (CONST RECT* pSourceRect,CONST RECT* pDestRect,HWND hDestWindowOverride,CONST RGNDATA* pDirtyRegion)
{
    if (this->hmd && this->render_distorted)
    {
        // Wrap up the previous frame
        if (this->frame_index != 0)
        {
            // Dismiss the health and saftey warning
            ovrHmd_DismissHSWDisplay(this->hmd);

            // Copy the back buffer into the hmd surface
            this->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &this->back_buffer_surface);
            IDirect3DSurface9* hmd_surface;
            this->hmd_texture->GetSurfaceLevel(0, &hmd_surface);
            HRESULT result = this->StretchRect(this->back_buffer_surface, NULL, hmd_surface, NULL, D3DTEXF_LINEAR);

            // Hand over the surface to ovr for distortion
            ovrD3D9Texture eye_textures[2];
            eye_textures[0].D3D9.Header.API = ovrRenderAPI_D3D9;
            eye_textures[0].D3D9.Header.TextureSize = this->target_size;
            eye_textures[0].D3D9.Header.RenderViewport.Pos.x = 0;
            eye_textures[0].D3D9.Header.RenderViewport.Pos.y = 0;
            eye_textures[0].D3D9.Header.RenderViewport.Size.w = this->target_size.w / 2;
            eye_textures[0].D3D9.Header.RenderViewport.Size.h = this->target_size.h;
            eye_textures[0].D3D9.pTexture = this->hmd_texture;
            eye_textures[1] = eye_textures[0];
            eye_textures[1].D3D9.Header.RenderViewport.Pos.x = this->target_size.w / 2;
            ovrHmd_EndFrame(this->hmd, this->head_pose, &eye_textures[0].Texture);
        }
        if (GetAsyncKeyState(VK_F12) != 0)
        {
            if (!this->reset_pressed)
            {
                ovrHmd_RecenterPose(this->hmd);
                this->reset_pressed = true;
            }
        }
        else
        {
            this->reset_pressed = false;
        }

        ovrHmd_BeginFrame(this->hmd, this->frame_index++);
        this->head_pose[ovrEye_Left] = ovrHmd_GetEyePose(this->hmd, ovrEye_Left);
        this->head_pose[ovrEye_Right] = ovrHmd_GetEyePose(this->hmd, ovrEye_Right);
        return D3D_OK;
    }
    else
    {
        HRESULT result = this->inner->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        this->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &this->back_buffer_surface);
        this->inner->GetRenderTarget(0, &this->back_buffer_surface);
        return result;
    }
}

HRESULT Direct3DDevice9Hooks::GetBackBuffer (UINT iSwapChain,UINT iBackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface9** ppBackBuffer)
{
    return this->inner->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
}

HRESULT Direct3DDevice9Hooks::GetRasterStatus (UINT iSwapChain,D3DRASTER_STATUS* pRasterStatus)
{
    return this->inner->GetRasterStatus(iSwapChain, pRasterStatus);
}

HRESULT Direct3DDevice9Hooks::SetDialogBoxMode (BOOL bEnableDialogs)
{
    return this->inner->SetDialogBoxMode(bEnableDialogs);
}

void Direct3DDevice9Hooks::SetGammaRamp (UINT iSwapChain,DWORD Flags,CONST D3DGAMMARAMP* pRamp)
{
    return this->inner->SetGammaRamp(iSwapChain, Flags, pRamp);
}

void Direct3DDevice9Hooks::GetGammaRamp (UINT iSwapChain,D3DGAMMARAMP* pRamp)
{
    return this->inner->GetGammaRamp(iSwapChain, pRamp);
}

HRESULT Direct3DDevice9Hooks::CreateTexture (UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture9** ppTexture,HANDLE* pSharedHandle)
{
    return this->inner->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::CreateVolumeTexture (UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture9** ppVolumeTexture,HANDLE* pSharedHandle)
{
    return this->inner->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool,  ppVolumeTexture, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::CreateCubeTexture (UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture9** ppCubeTexture,HANDLE* pSharedHandle)
{
    return this->inner->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::CreateVertexBuffer (UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer9** ppVertexBuffer,HANDLE* pSharedHandle)
{
    if (!this->stereo_quad_buffer)
    {
        this->stereo_quad_buffer_offset = 0;
        this->stereo_quad_buffer_length = Length;
        this->inner->CreateVertexBuffer(
            Length,
            Usage,
            FVF,
            Pool,
            &this->stereo_quad_buffer,
            pSharedHandle
        );
    }
    return this->inner->CreateVertexBuffer(Length,Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::CreateIndexBuffer (UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer9** ppIndexBuffer,HANDLE* pSharedHandle)
{
    return this->inner->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::CreateRenderTarget (UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Lockable,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)
{
#if 0
    if (Width == this->present_parameters.BackBufferWidth && Height == this->present_parameters.BackBufferHeight)
    {
        Width = this->target_size.w;
        Height = this->target_size.h;
    }
#endif
    return this->inner->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::CreateDepthStencilSurface (UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,DWORD MultisampleQuality,BOOL Discard,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)
{
    return this->inner->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::UpdateSurface (IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestinationSurface,CONST POINT* pDestPoint)
{
    return this->inner->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
}

HRESULT Direct3DDevice9Hooks::UpdateTexture (IDirect3DBaseTexture9* pSourceTexture,IDirect3DBaseTexture9* pDestinationTexture)
{
    return this->inner->UpdateTexture(pSourceTexture, pDestinationTexture);
}

HRESULT Direct3DDevice9Hooks::GetRenderTargetData (IDirect3DSurface9* pRenderTarget,IDirect3DSurface9* pDestSurface)
{
    return this->inner->GetRenderTargetData(pRenderTarget, pDestSurface);
}

HRESULT Direct3DDevice9Hooks::GetFrontBufferData (UINT iSwapChain,IDirect3DSurface9* pDestSurface)
{
    return this->inner->GetFrontBufferData(iSwapChain, pDestSurface);
}

HRESULT Direct3DDevice9Hooks::StretchRect (IDirect3DSurface9* pSourceSurface,CONST RECT* pSourceRect,IDirect3DSurface9* pDestSurface,CONST RECT* pDestRect,D3DTEXTUREFILTERTYPE Filter)
{
    return this->inner->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
}

HRESULT Direct3DDevice9Hooks::ColorFill (IDirect3DSurface9* pSurface,CONST RECT* pRect,D3DCOLOR color)
{
    return this->inner->ColorFill(pSurface, pRect, color);
}

HRESULT Direct3DDevice9Hooks::CreateOffscreenPlainSurface (UINT Width,UINT Height,D3DFORMAT Format,D3DPOOL Pool,IDirect3DSurface9** ppSurface,HANDLE* pSharedHandle)
{
    return this->inner->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}

HRESULT Direct3DDevice9Hooks::SetRenderTarget (DWORD RenderTargetIndex,IDirect3DSurface9* pRenderTarget)
{
    this->stereo = this->hmd != 0;
    if (this->stereo && pRenderTarget)
    {
        D3DSURFACE_DESC desc;
        pRenderTarget->GetDesc(&desc);
        if (desc.Width != this->present_parameters.BackBufferWidth || desc.Height != this->present_parameters.BackBufferHeight)
        {
            this->stereo = false;
        }
    }
    return this->inner->SetRenderTarget(RenderTargetIndex, pRenderTarget);
}

HRESULT Direct3DDevice9Hooks::GetRenderTarget (DWORD RenderTargetIndex,IDirect3DSurface9** ppRenderTarget)
{
    return this->inner->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
}

HRESULT Direct3DDevice9Hooks::SetDepthStencilSurface (IDirect3DSurface9* pNewZStencil)
{
    return this->inner->SetDepthStencilSurface(pNewZStencil);
}

HRESULT Direct3DDevice9Hooks::GetDepthStencilSurface (IDirect3DSurface9** ppZStencilSurface)
{
    return this->inner->GetDepthStencilSurface(ppZStencilSurface);
}

HRESULT Direct3DDevice9Hooks::BeginScene ()
{
    return this->inner->BeginScene();
}

HRESULT Direct3DDevice9Hooks::EndScene ()
{
    return this->inner->EndScene();
}

HRESULT Direct3DDevice9Hooks::Clear (DWORD Count,CONST D3DRECT* pRects,DWORD Flags,D3DCOLOR Color,float Z,DWORD Stencil)
{
    this->tracking_state = ovrHmd_GetTrackingState(this->hmd, ovr_GetTimeInSeconds());
    return this->inner->Clear(Count, pRects, Flags, Color, Z, Stencil);
}

HRESULT Direct3DDevice9Hooks::SetTransform (D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix)
{
    return this->inner->SetTransform(State, pMatrix);
}

HRESULT Direct3DDevice9Hooks::GetTransform (D3DTRANSFORMSTATETYPE State,D3DMATRIX* pMatrix)
{
    return this->inner->GetTransform(State, pMatrix);
}

HRESULT Direct3DDevice9Hooks::MultiplyTransform (D3DTRANSFORMSTATETYPE State,CONST D3DMATRIX* pMatrix)
{
    return this->inner->MultiplyTransform(State, pMatrix);
}

HRESULT Direct3DDevice9Hooks::SetViewport (CONST D3DVIEWPORT9* pViewport)
{
    return this->inner->SetViewport(pViewport);
}

HRESULT Direct3DDevice9Hooks::GetViewport (D3DVIEWPORT9* pViewport)
{
    return this->inner->GetViewport(pViewport);
}

HRESULT Direct3DDevice9Hooks::SetMaterial (CONST D3DMATERIAL9* pMaterial)
{
    return this->inner->SetMaterial(pMaterial);
}

HRESULT Direct3DDevice9Hooks::GetMaterial (D3DMATERIAL9* pMaterial)
{
    return this->inner->GetMaterial(pMaterial);
}

HRESULT Direct3DDevice9Hooks::SetLight (DWORD Index,CONST D3DLIGHT9* pLight)
{
    return this->inner->SetLight(Index, pLight);
}

HRESULT Direct3DDevice9Hooks::GetLight (DWORD Index,D3DLIGHT9* pLight)
{
    return this->inner->GetLight(Index, pLight);
}

HRESULT Direct3DDevice9Hooks::LightEnable (DWORD Index,BOOL Enable)
{
    return this->inner->LightEnable(Index, Enable);
}

HRESULT Direct3DDevice9Hooks::GetLightEnable (DWORD Index,BOOL* pEnable)
{
    return this->inner->GetLightEnable(Index, pEnable);
}

HRESULT Direct3DDevice9Hooks::SetClipPlane (DWORD Index,CONST float* pPlane)
{
    return this->inner->SetClipPlane(Index, pPlane);
}

HRESULT Direct3DDevice9Hooks::GetClipPlane (DWORD Index,float* pPlane)
{
    return this->inner->GetClipPlane(Index, pPlane);
}

HRESULT Direct3DDevice9Hooks::SetRenderState (D3DRENDERSTATETYPE State,DWORD Value)
{
    return this->inner->SetRenderState(State, Value);
}

HRESULT Direct3DDevice9Hooks::GetRenderState (D3DRENDERSTATETYPE State,DWORD* pValue)
{
    return this->inner->GetRenderState(State, pValue);
}

HRESULT Direct3DDevice9Hooks::CreateStateBlock (D3DSTATEBLOCKTYPE Type,IDirect3DStateBlock9** ppSB)
{
    return this->inner->CreateStateBlock(Type, ppSB);
}

HRESULT Direct3DDevice9Hooks::BeginStateBlock ()
{
    return this->inner->BeginStateBlock();
}

HRESULT Direct3DDevice9Hooks::EndStateBlock (IDirect3DStateBlock9** ppSB)
{
    return this->inner->EndStateBlock(ppSB);
}

HRESULT Direct3DDevice9Hooks::SetClipStatus (CONST D3DCLIPSTATUS9* pClipStatus)
{
    return this->inner->SetClipStatus(pClipStatus);
}

HRESULT Direct3DDevice9Hooks::GetClipStatus (D3DCLIPSTATUS9* pClipStatus)
{
    return this->inner->GetClipStatus(pClipStatus);
}

HRESULT Direct3DDevice9Hooks::GetTexture (DWORD Stage,IDirect3DBaseTexture9** ppTexture)
{
    return this->inner->GetTexture(Stage, ppTexture);
}

HRESULT Direct3DDevice9Hooks::SetTexture (DWORD Stage,IDirect3DBaseTexture9* pTexture)
{
    return this->inner->SetTexture(Stage, pTexture);
}

HRESULT Direct3DDevice9Hooks::GetTextureStageState (DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD* pValue)
{
    return this->inner->GetTextureStageState(Stage, Type, pValue);
}

HRESULT Direct3DDevice9Hooks::SetTextureStageState (DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value)
{
    return this->inner->SetTextureStageState(Stage, Type, Value);
}

HRESULT Direct3DDevice9Hooks::GetSamplerState (DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD* pValue)
{
    return this->inner->GetSamplerState(Sampler, Type, pValue);
}

HRESULT Direct3DDevice9Hooks::SetSamplerState (DWORD Sampler,D3DSAMPLERSTATETYPE Type,DWORD Value)
{
    return this->inner->SetSamplerState(Sampler, Type, Value);
}

HRESULT Direct3DDevice9Hooks::ValidateDevice (DWORD* pNumPasses)
{
    return this->inner->ValidateDevice(pNumPasses);
}

HRESULT Direct3DDevice9Hooks::SetPaletteEntries (UINT PaletteNumber,CONST PALETTEENTRY* pEntries)
{
    return this->inner->SetPaletteEntries(PaletteNumber, pEntries);
}

HRESULT Direct3DDevice9Hooks::GetPaletteEntries (UINT PaletteNumber,PALETTEENTRY* pEntries)
{
    return this->inner->GetPaletteEntries(PaletteNumber, pEntries);
}

HRESULT Direct3DDevice9Hooks::SetCurrentTexturePalette (UINT PaletteNumber)
{
    return this->inner->SetCurrentTexturePalette(PaletteNumber);
}

HRESULT Direct3DDevice9Hooks::GetCurrentTexturePalette (UINT *PaletteNumber)
{
    return this->inner->GetCurrentTexturePalette(PaletteNumber);
}

HRESULT Direct3DDevice9Hooks::SetScissorRect (CONST RECT* pRect)
{
    return this->inner->SetScissorRect(pRect);
}

HRESULT Direct3DDevice9Hooks::GetScissorRect (RECT* pRect)
{
    return this->inner->GetScissorRect(pRect);
}

HRESULT Direct3DDevice9Hooks::SetSoftwareVertexProcessing (BOOL bSoftware)
{
    return this->inner->SetSoftwareVertexProcessing(bSoftware);
}

BOOL Direct3DDevice9Hooks::GetSoftwareVertexProcessing ()
{
    return this->inner->GetSoftwareVertexProcessing();
}

HRESULT Direct3DDevice9Hooks::SetNPatchMode (float nSegments)
{
    return this->inner->SetNPatchMode(nSegments);
}

float Direct3DDevice9Hooks::GetNPatchMode ()
{
    return this->inner->GetNPatchMode();
}

HRESULT Direct3DDevice9Hooks::DrawPrimitive (D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount)
{
    if (!this->stereo)
    {
        return this->inner->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }

    // Get the current viewport
    D3DVIEWPORT9 viewport;
    this->inner->GetViewport(&viewport);

    // Lock the vertices that were going to be rendered
    ui_vertex* vertices;
    this->current_stream.data->Lock(StartVertex * sizeof(ui_vertex), 2 * sizeof(ui_vertex), (void**)&vertices, D3DLOCK_READONLY);

    ui_vertex copy[4];
    memcpy(copy, vertices, sizeof(copy));
    this->current_stream.data->Unlock();
    vertices = copy;

    // Lock our quad buffer
    ui_vertex* quad_vertices;
    this->stereo_quad_buffer->Lock(this->stereo_quad_buffer_offset, sizeof(ui_vertex) * 8, (void**)&quad_vertices, D3DLOCK_NOOVERWRITE);
    memcpy(quad_vertices, vertices, sizeof(ui_vertex) * 4);
    memcpy(quad_vertices + 4, vertices, sizeof(ui_vertex) * 4);
    quad_vertices[0].position.x = (vertices[0].position.x + 0.5f) * 0.5f + -0.5f + viewport.X;
    quad_vertices[0].position.y = (vertices[0].position.y + 0.5f) * 0.5f + -0.5f + viewport.Y + viewport.Height * 0.25f;
    quad_vertices[1].position.x = (vertices[1].position.x + 0.5f) * 0.5f + -0.5f + viewport.X;
    quad_vertices[1].position.y = (vertices[1].position.y + 0.5f) * 0.5f + -0.5f + viewport.Y + viewport.Height * 0.25f;
    quad_vertices[2].position.x = (vertices[2].position.x + 0.5f) * 0.5f + -0.5f + viewport.X;
    quad_vertices[2].position.y = (vertices[2].position.y + 0.5f) * 0.5f + -0.5f + viewport.Y + viewport.Height * 0.25f;
    quad_vertices[3].position.x = (vertices[3].position.x + 0.5f) * 0.5f + -0.5f + viewport.X;
    quad_vertices[3].position.y = (vertices[3].position.y + 0.5f) * 0.5f + -0.5f + viewport.Y + viewport.Height * 0.25f;
    quad_vertices[4].position.x = quad_vertices[0].position.x + viewport.Width * 0.5f;
    quad_vertices[4].position.y = quad_vertices[0].position.y;
    quad_vertices[5].position.x = quad_vertices[1].position.x + viewport.Width * 0.5f;
    quad_vertices[5].position.y = quad_vertices[1].position.y;
    quad_vertices[6].position.x = quad_vertices[2].position.x + viewport.Width * 0.5f;
    quad_vertices[6].position.y = quad_vertices[2].position.y;
    quad_vertices[7].position.x = quad_vertices[3].position.x + viewport.Width * 0.5f;
    quad_vertices[7].position.y = quad_vertices[3].position.y;
    this->stereo_quad_buffer->Unlock();
    this->inner->SetStreamSource(current_stream.number, this->stereo_quad_buffer, this->stereo_quad_buffer_offset, sizeof(ui_vertex));

    this->stereo_quad_buffer_offset += sizeof(ui_vertex) * 8;
    if (this->stereo_quad_buffer_offset + sizeof(ui_vertex) * 8 > this->stereo_quad_buffer_length)
    {
        this->stereo_quad_buffer_offset = 0;
    }

    D3DVIEWPORT9 left_viewport = viewport;
    left_viewport.Width /= 2;
    left_viewport.Height /= 2;
    left_viewport.Y += left_viewport.Height / 2;
    this->inner->SetViewport(&left_viewport);
    this->inner->DrawPrimitive(PrimitiveType, 0, PrimitiveCount);

    // Render to the right side
    D3DVIEWPORT9 right_viewport = left_viewport;
    right_viewport.X += left_viewport.Width;
    this->inner->SetViewport(&right_viewport);
    this->inner->DrawPrimitive(PrimitiveType, 4, PrimitiveCount);

    // Restore the original viewport
    this->inner->SetStreamSource(current_stream.number, current_stream.data, current_stream.offset, current_stream.stride); 
    this->inner->SetViewport(&viewport);
    return D3D_OK;
}

HRESULT Direct3DDevice9Hooks::DrawIndexedPrimitive (D3DPRIMITIVETYPE PrimitiveType,INT BaseVertexIndex,UINT MinVertexIndex,UINT NumVertices,UINT startIndex,UINT primCount)
{
    if (!this->stereo)
    {
        return this->inner->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
    }

    // Compose new view and projection matrices for each eye based on the head tracking
    // Oculus coordinate system:
    //    y  -z
    //    | /
    //    |/___x
    //
    // (origin is reference head position)
    // (units are meters)

    // Pinball arcade coordinate system:
    //    z  -y
    //    | /
    //    |/___x
    //
    // (origin is middle of the table)
    // (units are millimeters - ?)
    // p_v = (o_v.x, o_v.z, -o_v.y)

    OVR::Matrix4f axis_conversion = OVR::Matrix4f::AxisConversion(
        OVR::WorldAxes(OVR::Axis_Right, OVR::Axis_Out, OVR::Axis_Down),
        OVR::WorldAxes(OVR::Axis_Right, OVR::Axis_Up, OVR::Axis_Out)
    );

    D3DXMATRIX transforms[2];
    for (int eye = 0; eye < 2; ++eye)
    {
        ovrPosef head_pose = this->tracking_state.HeadPose.ThePose;
        OVR::Vector3f hmd_position = head_pose.Position;
        OVR::Quatf hmd_orientation = head_pose.Orientation;

        float unit_scale = 5000.0f;
        OVR::Vector3f ovr_world_offset(0, 3000.0f, 5000.0f);
        OVR::Matrix4f ovr_translation = OVR::Matrix4f::Translation(-ovr_world_offset - hmd_position * unit_scale);
        OVR::Matrix4f ovr_view = OVR::Matrix4f(hmd_orientation.Inverted()) * ovr_translation;
        OVR::Matrix4f ovr_eye_view = OVR::Matrix4f::Translation(this->eye_render_desc[eye].ViewAdjust) * ovr_view * axis_conversion;

        D3DXMATRIX view;
        D3DXMatrixTranspose(&view, (D3DXMATRIX*)&ovr_eye_view);
        D3DXMATRIX model_view = this->model_matrix * view;

        ovrMatrix4f ovr_projection = ovrMatrix4f_Projection(this->eye_render_desc[eye].Fov, 1.0f, 100000.0f, true);
        D3DXMATRIX projection;
        D3DXMatrixTranspose(&projection, (D3DXMATRIX*)&ovr_projection);
        D3DXMatrixMultiply(&transforms[eye], &model_view, &projection);
    }

    // Get the current viewport
    D3DVIEWPORT9 viewport;
    this->inner->GetViewport(&viewport);

    // Render to the left viewport
    D3DVIEWPORT9 left_viewport = viewport;
    left_viewport.Width /= 2;
    this->inner->SetViewport(&left_viewport);
    this->inner->SetVertexShaderConstantF(11, (const float*)&transforms[ovrEye_Left], 4);
    this->inner->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);

    // Render to the right viewport
    D3DVIEWPORT9 right_viewport = viewport;
    right_viewport.Width /= 2;
    right_viewport.X += right_viewport.Width;
    this->inner->SetViewport(&right_viewport);
    this->inner->SetVertexShaderConstantF(11, (const float*)&transforms[ovrEye_Right], 4);
    this->inner->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);

    // Restore the viewport
    this->inner->SetViewport(&viewport);
    return D3D_OK;
}

HRESULT Direct3DDevice9Hooks::DrawPrimitiveUP (D3DPRIMITIVETYPE PrimitiveType,UINT PrimitiveCount,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride)
{
    return this->inner->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
}

HRESULT Direct3DDevice9Hooks::DrawIndexedPrimitiveUP (D3DPRIMITIVETYPE PrimitiveType,UINT MinVertexIndex,UINT NumVertices,UINT PrimitiveCount,CONST void* pIndexData,D3DFORMAT IndexDataFormat,CONST void* pVertexStreamZeroData,UINT VertexStreamZeroStride)
{
    return this->inner->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
}

HRESULT Direct3DDevice9Hooks::ProcessVertices (UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer9* pDestBuffer,IDirect3DVertexDeclaration9* pVertexDecl,DWORD Flags)
{
    return this->inner->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
}

HRESULT Direct3DDevice9Hooks::CreateVertexDeclaration (CONST D3DVERTEXELEMENT9* pVertexElements,IDirect3DVertexDeclaration9** ppDecl)
{
    return this->inner->CreateVertexDeclaration(pVertexElements, ppDecl);
}

HRESULT Direct3DDevice9Hooks::SetVertexDeclaration (IDirect3DVertexDeclaration9* pDecl)
{
    return this->inner->SetVertexDeclaration(pDecl);
}

HRESULT Direct3DDevice9Hooks::GetVertexDeclaration (IDirect3DVertexDeclaration9** ppDecl)
{
    return this->inner->GetVertexDeclaration(ppDecl);
}

HRESULT Direct3DDevice9Hooks::SetFVF (DWORD FVF)
{
    return this->inner->SetFVF(FVF);
}

HRESULT Direct3DDevice9Hooks::GetFVF (DWORD* pFVF)
{
    return this->inner->GetFVF(pFVF);
}

HRESULT Direct3DDevice9Hooks::CreateVertexShader (CONST DWORD* pFunction,IDirect3DVertexShader9** ppShader)
{
    return this->inner->CreateVertexShader(pFunction, ppShader);
}

HRESULT Direct3DDevice9Hooks::SetVertexShader (IDirect3DVertexShader9* pShader)
{
    return this->inner->SetVertexShader(pShader);
}

HRESULT Direct3DDevice9Hooks::GetVertexShader (IDirect3DVertexShader9** ppShader)
{
    return this->inner->GetVertexShader(ppShader);
}

HRESULT Direct3DDevice9Hooks::SetVertexShaderConstantF (UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount)
{
    // The ModelViewProjection matrix is stored in register slot 11 for all
    // Pinball Arcade vertex shaders. Because of our patch that keeps the
    // viewprojection matrix at identity, this matrix is actually just the
    // model transform.
    if (StartRegister == 11 && Vector4fCount == 4)
    {
        this->model_matrix = *(D3DXMATRIX*)pConstantData;
    }
    return this->inner->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT Direct3DDevice9Hooks::GetVertexShaderConstantF (UINT StartRegister,float* pConstantData,UINT Vector4fCount)
{
    return this->inner->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT Direct3DDevice9Hooks::SetVertexShaderConstantI (UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount)
{
    return this->inner->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT Direct3DDevice9Hooks::GetVertexShaderConstantI (UINT StartRegister,int* pConstantData,UINT Vector4iCount)
{
    return this->inner->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT Direct3DDevice9Hooks::SetVertexShaderConstantB (UINT StartRegister,CONST BOOL* pConstantData,UINT  BoolCount)
{
    return this->inner->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT Direct3DDevice9Hooks::GetVertexShaderConstantB (UINT StartRegister,BOOL* pConstantData,UINT BoolCount)
{
    return this->inner->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT Direct3DDevice9Hooks::SetStreamSource (UINT StreamNumber,IDirect3DVertexBuffer9* pStreamData,UINT OffsetInBytes,UINT Stride)
{
    current_stream.number = StreamNumber;
    current_stream.data = pStreamData;
    current_stream.offset = OffsetInBytes;
    current_stream.stride = Stride;
    return this->inner->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
}

HRESULT Direct3DDevice9Hooks::GetStreamSource (UINT StreamNumber,IDirect3DVertexBuffer9** ppStreamData,UINT* pOffsetInBytes,UINT* pStride)
{
    return this->inner->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride);
}

HRESULT Direct3DDevice9Hooks::SetStreamSourceFreq (UINT StreamNumber,UINT Setting)
{
    return this->inner->SetStreamSourceFreq(StreamNumber, Setting);
}

HRESULT Direct3DDevice9Hooks::GetStreamSourceFreq (UINT StreamNumber,UINT* pSetting)
{
    return this->inner->GetStreamSourceFreq(StreamNumber, pSetting);
}

HRESULT Direct3DDevice9Hooks::SetIndices (IDirect3DIndexBuffer9* pIndexData)
{
    return this->inner->SetIndices(pIndexData);
}

HRESULT Direct3DDevice9Hooks::GetIndices (IDirect3DIndexBuffer9** ppIndexData)
{
    return this->inner->GetIndices(ppIndexData);
}

HRESULT Direct3DDevice9Hooks::CreatePixelShader (CONST DWORD* pFunction,IDirect3DPixelShader9** ppShader)
{
    return this->inner->CreatePixelShader(pFunction, ppShader);
}

HRESULT Direct3DDevice9Hooks::SetPixelShader (IDirect3DPixelShader9* pShader)
{
    return this->inner->SetPixelShader(pShader);
}

HRESULT Direct3DDevice9Hooks::GetPixelShader (IDirect3DPixelShader9** ppShader)
{
    return this->inner->GetPixelShader(ppShader);
}

HRESULT Direct3DDevice9Hooks::SetPixelShaderConstantF (UINT StartRegister,CONST float* pConstantData,UINT Vector4fCount)
{
    return this->inner->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT Direct3DDevice9Hooks::GetPixelShaderConstantF (UINT StartRegister,float* pConstantData,UINT Vector4fCount)
{
    return this->inner->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT Direct3DDevice9Hooks::SetPixelShaderConstantI (UINT StartRegister,CONST int* pConstantData,UINT Vector4iCount)
{
    return this->inner->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT Direct3DDevice9Hooks::GetPixelShaderConstantI (UINT StartRegister,int* pConstantData,UINT Vector4iCount)
{
    return this->inner->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT Direct3DDevice9Hooks::SetPixelShaderConstantB (UINT StartRegister,CONST BOOL* pConstantData,UINT  BoolCount)
{
    return this->inner->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT Direct3DDevice9Hooks::GetPixelShaderConstantB (UINT StartRegister,BOOL* pConstantData,UINT BoolCount)
{
    return this->inner->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT Direct3DDevice9Hooks::DrawRectPatch (UINT Handle,CONST float* pNumSegs,CONST D3DRECTPATCH_INFO* pRectPatchInfo)
{
    return this->inner->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
}

HRESULT Direct3DDevice9Hooks::DrawTriPatch (UINT Handle,CONST float* pNumSegs,CONST D3DTRIPATCH_INFO* pTriPatchInfo)
{
    return this->inner->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
}

HRESULT Direct3DDevice9Hooks::DeletePatch (UINT Handle)
{
    return this->inner->DeletePatch(Handle);
}

HRESULT Direct3DDevice9Hooks::CreateQuery (D3DQUERYTYPE Type,IDirect3DQuery9** ppQuery)
{
    return this->inner->CreateQuery(Type, ppQuery);
}