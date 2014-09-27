//====================================================================
// Hooked IDirect3D9 interface implementation.
//
// All this interface does is initialize LibOVR and hook devices
// that get created.
//====================================================================

#include "Direct3D9Hooks.h"
#include "Direct3DDevice9Hooks.h"

#include <OVR.h>

Direct3D9Hooks::Direct3D9Hooks (IDirect3D9* inner)
{
    this->inner = inner;

    // Initialize LibOVR
    ovr_Initialize();
    this->hmd = ovrHmd_Create(0);
    if (!this->hmd)
    {
        const char* error = ovrHmd_GetLastError(this->hmd);
        OutputDebugStringA(error);
    }
    ovrHmd_ConfigureTracking(this->hmd, ovrTrackingCap_Orientation | ovrTrackingCap_MagYawCorrection | ovrTrackingCap_Position, 0);
}

/*** IUnknown methods ***/
HRESULT Direct3D9Hooks::QueryInterface (REFIID riid, void** ppvObj)
{
    return this->inner->QueryInterface(riid, ppvObj);
}

ULONG Direct3D9Hooks::AddRef ()
{
    return this->inner->AddRef();
}
ULONG Direct3D9Hooks::Release ()
{
    return this->inner->Release();
}

/*** IDirect3D9 methods ***/
HRESULT Direct3D9Hooks::RegisterSoftwareDevice (void* pInitializeFunction)
{
    return this->inner->RegisterSoftwareDevice(pInitializeFunction);
}

UINT Direct3D9Hooks::GetAdapterCount ()
{
    return this->inner->GetAdapterCount();
}

HRESULT Direct3D9Hooks::GetAdapterIdentifier (UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
{
    return this->inner->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
}

UINT Direct3D9Hooks::GetAdapterModeCount (UINT Adapter, D3DFORMAT Format)
{
    return this->inner->GetAdapterModeCount(Adapter, Format);
}

HRESULT Direct3D9Hooks::EnumAdapterModes (UINT Adapter,D3DFORMAT Format,UINT Mode,D3DDISPLAYMODE* pMode)
{
    return this->inner->EnumAdapterModes(Adapter, Format, Mode, pMode);
}

HRESULT Direct3D9Hooks::GetAdapterDisplayMode (UINT Adapter,D3DDISPLAYMODE* pMode)
{
    return this->inner->GetAdapterDisplayMode(Adapter, pMode);
}

HRESULT Direct3D9Hooks::CheckDeviceType (UINT Adapter,D3DDEVTYPE DevType,D3DFORMAT AdapterFormat,D3DFORMAT BackBufferFormat,BOOL bWindowed)
{
    return this->inner->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
}

HRESULT Direct3D9Hooks::CheckDeviceFormat (UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT AdapterFormat,DWORD Usage,D3DRESOURCETYPE RType,D3DFORMAT CheckFormat)
{
    return this->inner->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}

HRESULT Direct3D9Hooks::CheckDeviceMultiSampleType (UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT SurfaceFormat,BOOL Windowed,D3DMULTISAMPLE_TYPE MultiSampleType,DWORD* pQualityLevels)
{
    return this->inner->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
}

HRESULT Direct3D9Hooks::CheckDepthStencilMatch (UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT AdapterFormat,D3DFORMAT RenderTargetFormat,D3DFORMAT DepthStencilFormat)
{
    return this->inner->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}

HRESULT Direct3D9Hooks::CheckDeviceFormatConversion (UINT Adapter,D3DDEVTYPE DeviceType,D3DFORMAT SourceFormat,D3DFORMAT TargetFormat)
{
    return this->inner->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
}

HRESULT Direct3D9Hooks::GetDeviceCaps (UINT Adapter,D3DDEVTYPE DeviceType,D3DCAPS9* pCaps)
{
    return this->inner->GetDeviceCaps(Adapter, DeviceType, pCaps);
}

HMONITOR Direct3D9Hooks::GetAdapterMonitor (UINT Adapter)
{
    return this->inner->GetAdapterMonitor(Adapter);
}

HRESULT Direct3D9Hooks::CreateDevice (UINT Adapter,D3DDEVTYPE DeviceType,HWND hFocusWindow,DWORD BehaviorFlags,D3DPRESENT_PARAMETERS* pPresentationParameters,IDirect3DDevice9** ppReturnedDeviceInterface)
{
    IDirect3DDevice9* inner_device;
    HRESULT result = this->inner->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &inner_device);
    *ppReturnedDeviceInterface = new Direct3DDevice9Hooks(this, inner_device, *pPresentationParameters, this->hmd);
    return result;
}
