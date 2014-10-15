//====================================================================
// Main entrypoint for the patch DLL
//====================================================================

#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include "hacks.h"
#include "Direct3D9Hooks.h"


//====================================================================
// Static helper data used in the patches
//====================================================================

typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t s_system_Direct3DCreate9;

static IDirect3D9* WINAPI hook_Direct3DCreate9(UINT SDKVersion)
{
    IDirect3D9* inner = s_system_Direct3DCreate9(SDKVersion);
    return new Direct3D9Hooks(inner);
}

//====================================================================
// Rolling CRC implementation used for fingerprinting code
//====================================================================

// Based on the Adler-32 implementation in rsync
void compute_fingerprint (rolling_crc* fingerprint, unsigned block_size, const unsigned char block[])
{
    fingerprint->block_size = block_size;
    fingerprint->a = 0;
    fingerprint->b = 0;
	for (size_t cursor = 0; cursor < block_size; ++cursor)
    {
	    fingerprint->a += block[cursor];
	    fingerprint->b += fingerprint->a;
	}
}

static void rotate_rolling_crc (rolling_crc* crc, unsigned char out, unsigned char in)
{
    crc->a += in - out;
    crc->b += crc->a - crc->block_size * out;
}

void read_code (uintptr_t address, size_t code_size, void* dest)
{
    DWORD old_rights;
    DWORD new_rights = PAGE_READONLY;
    VirtualProtect((LPVOID)address, code_size, new_rights, &old_rights);
    memcpy(dest, (LPVOID)address, code_size);
    VirtualProtect((LPVOID)address, code_size, old_rights, &new_rights);
}

//====================================================================
// Function for traversing the import address table and hooking a
// function imported there.
//====================================================================

void install_hook (const char module_name[], const char import_name[], LPVOID new_handler, LPVOID* old_handler_out)
{
    // Get the import descriptors.
    HANDLE module = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* image_header = (IMAGE_DOS_HEADER*)module;
    IMAGE_NT_HEADERS* nt_headers = (IMAGE_NT_HEADERS*)(image_header->e_lfanew + (size_t)module);
    IMAGE_IMPORT_DESCRIPTOR* import_descriptors = (IMAGE_IMPORT_DESCRIPTOR*)(nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress + (size_t)module);

    // Find the KERNEL32 import descriptor.
    IMAGE_IMPORT_DESCRIPTOR* cursor;
    for (cursor = import_descriptors; cursor->Name; ++cursor)
    {
        if (_stricmp((char*)(cursor->Name + (size_t)module), module_name) == 0)
        {
            break;
        }
    }
    if (!cursor->Name)
    {
        return;
    }

    // Find the definition of LoadLibraryA.
    LPVOID* address_cursor = 0;
    int import_index = 0;
    for (address_cursor = (LPVOID*)(cursor->FirstThunk + (size_t)module); *address_cursor; ++address_cursor, ++import_index)
    {
        char* cursor_import_name = (char*)(*((size_t*)(cursor->OriginalFirstThunk + (size_t)module) + import_index) + (size_t)module + 2);
        if (strcmp(cursor_import_name, import_name) == 0)
        {
            break;
        }
    }
    if (!*address_cursor)
    {
        return;
    }

    // Unprotect the page so that we can rewrite the import address
    DWORD old_rights;
    DWORD new_rights = PAGE_READWRITE;
    VirtualProtect(address_cursor, sizeof(LPVOID), new_rights, &old_rights);

    // Swap function poitners to hook it for ourself
    *old_handler_out = *address_cursor;
    *address_cursor = new_handler;
    
    // Restore the page protections
    VirtualProtect(address_cursor, sizeof(LPVOID), old_rights, &new_rights);
}

//====================================================================
// Function for traversing through the application's .text segment
// (which is where all the code lives) and looking for a specific
// fingerprint in order to patch a function.
//====================================================================

uintptr_t find_fingerprint (const rolling_crc& fingerprint)
{
    HANDLE module = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* image_header = (IMAGE_DOS_HEADER*)module;
    IMAGE_NT_HEADERS* nt_headers = (IMAGE_NT_HEADERS*)(image_header->e_lfanew + (size_t)module);
    IMAGE_SECTION_HEADER* section_header = (IMAGE_SECTION_HEADER*)(nt_headers + 1);
    IMAGE_SECTION_HEADER* section_header_term = section_header + nt_headers->FileHeader.NumberOfSections;
    for (; section_header != section_header_term; ++section_header)
    {
        if (memcmp(section_header->Name, ".text", 5) == 0)
        {
            break;
        }
    }
    if (section_header == section_header_term)
    {
        return 0;
    }
    unsigned char* address = (unsigned char*)module + section_header->VirtualAddress;
    unsigned char* section_term = address + section_header->Misc.VirtualSize - fingerprint.block_size;

    rolling_crc section_crc;
    compute_fingerprint(&section_crc, fingerprint.block_size, address);
    while (address != section_term)
    {
        if (section_crc.a == fingerprint.a && section_crc.b == fingerprint.b)
        {
            return (uintptr_t)address;
        }
        rotate_rolling_crc(&section_crc, address[0], address[fingerprint.block_size]);
        ++address;
    }
    return 0;
}

void install_patch (uintptr_t address, size_t patch_size, const void* patch)
{
    unsigned char* patch_address = (unsigned char*)address;
    // Modify the page rights for the address
    DWORD old_rights;
    DWORD new_rights = PAGE_READWRITE;
    VirtualProtect(patch_address, patch_size, new_rights, &old_rights);
    
    memcpy(patch_address, patch, patch_size);

    // Restore the page protections
    VirtualProtect(patch_address, patch_size, old_rights, &new_rights);
}


//====================================================================
// Function for installing the hacks we need to support VR
//====================================================================

static void install_hacks ()
{
    // Install a hook for device create
    install_hook("d3d9.dll", "Direct3DCreate9", (LPVOID)hook_Direct3DCreate9, (LPVOID*)&s_system_Direct3DCreate9);
}


//====================================================================
// DLL entry point
//====================================================================

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD dwReason, PVOID lpReserved)
{	
	switch (dwReason) 
	{
        case DLL_PROCESS_ATTACH:
            install_hacks();
		    break;
	    case DLL_PROCESS_DETACH:
		    break;		
	}
	return TRUE;
}