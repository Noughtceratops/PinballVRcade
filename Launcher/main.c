//====================================================================
// Code for the custom Pinball Arcade launcher that injects the
// patch DLL
//====================================================================

#include <Windows.h>

#define HOOK_DLL "PinballVRcade.dll"

int CALLBACK WinMain (HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    BOOL success;
    char exe_path[MAX_PATH];
    char exe_dir[MAX_PATH];
    char appid_path[MAX_PATH];
    char launcher_path[MAX_PATH];
    char dll_path[MAX_PATH];
    HANDLE appid_file;
    char* file_part;
    LPVOID path_memory;
    HANDLE hook_init_thread;

    // Try to find Pinball Arcade
    success = 0;
    if (*lpCmdLine)
    {
        strcpy(exe_path, lpCmdLine);
        success = GetFileAttributesA(exe_path) != INVALID_FILE_ATTRIBUTES;
    }
    if (!success)
    {
        strcpy(exe_path, "c:\\Program Files (x86)\\Steam\\steamapps\\common\\PinballArcade\\PinballArcade.exe");
        success = GetFileAttributesA(exe_path) != INVALID_FILE_ATTRIBUTES;
    }
    if (!success)
    {
        strcpy(exe_path, "c:\\Program Files\\Steam\\steamapps\\common\\PinballArcade\\PinballArcade.exe");
        success = GetFileAttributesA(exe_path) != INVALID_FILE_ATTRIBUTES;
    }
    if (!success)
    {
        MessageBoxA(NULL, "Locate PinballArcade.exe on your local system and drag it onto the launcher.", "Couldn't locate Pinball Arcade.", MB_ICONERROR | MB_OK);
        return 0;
    }

    GetFullPathNameA(exe_path, sizeof(exe_dir), exe_dir, &file_part);
    *file_part = '\0';

    // First try to write a steam appid file if it doesn't already exist
    strcpy(appid_path, exe_dir);
    strcat(appid_path, "steam_appid.txt");
    if (GetFileAttributesA(appid_path) == INVALID_FILE_ATTRIBUTES)
    {
        appid_file = CreateFileA(appid_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (appid_file == INVALID_HANDLE_VALUE)
        {
            MessageBoxA(NULL, "Try running this launcher as an administrator.", "Couldn't set up Pinball Arcade app id file.", MB_ICONERROR | MB_OK);
            return 0;
        }
        WriteFile(appid_file, "238260", 6, NULL, NULL);
        FlushFileBuffers(appid_file);
        CloseHandle(appid_file);
    }

    // Try to launch Pinball Arcade
    memset(&startup_info, 0, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    success = CreateProcessA(
        exe_path,
        NULL, // lpCommandLine
        NULL, // lpProcessAttributes
        NULL, // lpThreadAttributes
        FALSE, // bInheritHandles
        CREATE_SUSPENDED, // dwCreationFlags
        NULL, // lpEnvironment
        exe_dir,
        &startup_info,
        &process_info
    );
    if (!success)
    {
        MessageBoxA(NULL, "An error occurred trying to launch Pinball Arcade from the specified path.", "Failed to launch Pinball Arcade.", MB_ICONERROR | MB_OK);
        return;
    }
    
    // Get the local path to our launcher
    GetModuleFileNameA(NULL, launcher_path, sizeof(launcher_path));

    // Get the directory from the path
    GetFullPathNameA(launcher_path, sizeof(dll_path), dll_path, &file_part);
    *file_part = '\0';
    strncat((char*)dll_path, HOOK_DLL, MAX_PATH);

    // Allocate memory in the process space of Pinball Arcade
    path_memory = VirtualAllocEx(process_info.hProcess, NULL, MAX_PATH, MEM_COMMIT, PAGE_READWRITE);
    WriteProcessMemory(process_info.hProcess, path_memory, dll_path, MAX_PATH, NULL);

    // Create a new thread in the process that loads our DLL
    hook_init_thread = CreateRemoteThread(
        process_info.hProcess,
        NULL, // lpThreadAttributes
        0, // dwStackSize
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("KERNEL32.DLL"), "LoadLibraryA"),
        path_memory,
        0, // dwCreationFlags
        NULL // lpThreadId
    );

    // Wait for our hook to load
    WaitForSingleObject(hook_init_thread, INFINITE);
    CloseHandle(hook_init_thread);

    ResumeThread(process_info.hThread);
    return 0;
}