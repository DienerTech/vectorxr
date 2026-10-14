#include <Windows.h>

#include "depthxr/process_exit.h"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved) {
    // A non-null reserved at DLL_PROCESS_DETACH means the process is exiting
    // normally (null is a FreeLibrary). Every other thread is already gone and
    // the loader lock is held: no locks, no allocations, no other modules.
    // The CRT runs this DLL's static destructors after this returns.
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        depthxr::MarkProcessExiting();
        depthxr::RemoveProcessExitFiles();
    }
    return TRUE;
}
