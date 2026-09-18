#if _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Windows.h>

void AbortWithError(const char *reason)
{
    MessageBox(NULL, reason, "Fatal Error", MB_OK | MB_ICONERROR);
    abort();
}

void RemoteCallWithString(HANDLE process, FARPROC proc, const char *value, const char *description)
{
    size_t valueLength = strlen(value);

    LPVOID remoteValue = VirtualAllocEx(process, NULL, valueLength + 1, MEM_COMMIT, PAGE_READWRITE);
    if (remoteValue == NULL)
        AbortWithError("Could not allocate remote string");
    if (!WriteProcessMemory(process, remoteValue, value, valueLength + 1, NULL))
        AbortWithError("Could not write remote string");

    HANDLE remoteThread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)proc, remoteValue, 0, NULL);
    if (remoteThread == NULL)
        AbortWithError("Could not create remote thread");

    WaitForSingleObject(remoteThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(remoteThread, &exitCode);
    if (exitCode == 0)
    {
        char details[1024];
        sprintf(details, "%s failed in the game process.\nValue: %s", description, value);
        AbortWithError(details);
    }

    VirtualFreeEx(process, remoteValue, 0, MEM_RELEASE);
    CloseHandle(remoteThread);
}

void InjectDLL(HANDLE process, const char *dllPath, const char *dllDirectory)
{
    HMODULE kernel32 = GetModuleHandleA("Kernel32.dll");
    FARPROC setDllDirectoryProc = GetProcAddress(kernel32, "SetDllDirectoryA");
    FARPROC loadLibraryProc = GetProcAddress(kernel32, "LoadLibraryA");
    if (setDllDirectoryProc == NULL)
        AbortWithError("Could not locate SetDllDirectoryA");
    if (loadLibraryProc == NULL)
        AbortWithError("Could not locate LoadLibraryA");

    RemoteCallWithString(process, setDllDirectoryProc, dllDirectory, "SetDllDirectoryA");
    RemoteCallWithString(process, loadLibraryProc, dllPath, "LoadLibraryA");
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prevInstance, LPSTR cmdLine, int cmdShow)
{
    (void)instance; (void)prevInstance; (void)cmdLine; (void)cmdShow;

    int argC = __argc;
    char **argV = __argv;

    const char *defaultGamePath = "subrosa.exe";
    const char *defaultDllPath = "Sandium.dll";

    const char *gamePath = defaultGamePath;
    const char *dllPath = defaultDllPath;

    for (int argNum = 1; argNum < argC; argNum++) // TODO: this is shit
    {
        if (strcmp(argV[argNum], "--help") == 0)
        {
            MessageBox(NULL, "SandiumLauncher.exe [gamePath=\"subrosa.exe\"] [dllPath=\"Sandium.dll\"]", "Help", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        if (argNum == 1)
            gamePath = argV[argNum];
        else if (argNum == 2)
            dllPath = argV[argNum];
        else
            AbortWithError("Too much args, try --help");
    }

    // Resolve relative paths against the launcher's own folder instead of the
    // unpredictable working directory, so double-click and terminal launches
    // behave identically.
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char *slash = strrchr(exePath, '/');
        char *backslashSlash = strrchr(exePath, '\\');
        if (!slash || (backslashSlash && backslashSlash > slash))
            slash = backslashSlash;
        if (slash)
            *(slash + 1) = 0;
        else
            exePath[0] = 0;

        char *resolved;
        size_t exeDirLength = strlen(exePath);
        int gamePathAbsolute = gamePath[0] == '/' || (gamePath[1] == ':' &&
            (gamePath[2] == '/' || gamePath[2] == '\\'));
        if (gamePathAbsolute)
            resolved = _strdup(gamePath);
        else
        {
            resolved = malloc(exeDirLength + strlen(gamePath) + 1);
            strcpy(resolved, exePath);
            strcat(resolved, gamePath);
        }
        gamePath = resolved;

        char *resolvedDll;
        int dllPathAbsolute = dllPath[0] == '/' || (dllPath[1] == ':' &&
            (dllPath[2] == '/' || dllPath[2] == '\\'));
        if (dllPathAbsolute)
            resolvedDll = _strdup(dllPath);
        else
        {
            resolvedDll = malloc(exeDirLength + strlen(dllPath) + 1);
            strcpy(resolvedDll, exePath);
            strcat(resolvedDll, dllPath);
        }
        dllPath = resolvedDll;
    }

    size_t gameDirectorySize = strlen(gamePath);
    char *gameDirectory = malloc(gameDirectorySize + 1);

    memcpy(gameDirectory, gamePath, gameDirectorySize);
    gameDirectory[gameDirectorySize] = '\0';

    // We need to get the game directory, I'm simply gonna get the "up" folder for now
    while (gameDirectorySize > 0)
    {
        if (gameDirectory[gameDirectorySize] == '\\' || gameDirectory[gameDirectorySize] == '/')
            break;

        gameDirectory[gameDirectorySize] = '\0';
        gameDirectorySize--;
    }
    
    realloc(gameDirectory, gameDirectorySize + 1);
    gameDirectory[gameDirectorySize] = '\0';

    STARTUPINFO startupInfo;
    ZeroMemory(&startupInfo, sizeof(STARTUPINFO));
    startupInfo.cb = sizeof(STARTUPINFO);
    PROCESS_INFORMATION processInfo;
    //ZeroMemory(&processInfo, sizeof(PROCESS_INFORMATION));

    BOOL success = CreateProcess(
        gamePath, NULL,
        NULL, NULL,
        FALSE,
        CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS, // We gotta create a suspended process so we can inject Sandium.dll before anything runs
        NULL, gameDirectorySize > 0 ? gameDirectory : NULL,
        &startupInfo, &processInfo
    );
    if (!success)
        AbortWithError("Could not open process"); // TODO: Add better error description?

    InjectDLL(processInfo.hProcess, dllPath, gameDirectorySize > 0 ? gameDirectory : ".");
    ResumeThread(processInfo.hThread); // We created a process using CREATE_SUSPENDED, remember? (Of course you do, if you don't, seek a doctor)

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    return 0;
}

#endif
