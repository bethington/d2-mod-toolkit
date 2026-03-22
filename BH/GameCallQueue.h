#pragma once

// GameCallQueue - Queue function calls to execute on the game thread.
// The HTTP/MCP thread queues a call, the game loop executes it,
// and the result is returned to the waiting HTTP thread.

#include <windows.h>
#include <atomic>

namespace GameCallQueue {
    struct PendingCall {
        DWORD address;
        DWORD args[8];
        int argCount;
        int convention;     // 0=stdcall, 1=cdecl, 2=fastcall
        DWORD result;
        bool completed;
        bool crashed;
    };

    // Queue a call and wait for result (called from HTTP thread)
    // Returns true if completed, false if timeout or crash
    bool CallOnGameThread(PendingCall& call, int timeoutMs = 5000);

    // Process pending calls (called from game loop)
    void ProcessPending();
}
