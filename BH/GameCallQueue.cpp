#include "GameCallQueue.h"

// Function pointer typedefs for exact arg count matching
typedef DWORD (__stdcall *Std0)();
typedef DWORD (__stdcall *Std1)(DWORD);
typedef DWORD (__stdcall *Std2)(DWORD, DWORD);
typedef DWORD (__stdcall *Std3)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *Std4)(DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__stdcall *Std5)(DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__cdecl *Cdc0)();
typedef DWORD (__cdecl *Cdc1)(DWORD);
typedef DWORD (__cdecl *Cdc2)(DWORD, DWORD);
typedef DWORD (__cdecl *Cdc3)(DWORD, DWORD, DWORD);
typedef DWORD (__cdecl *Cdc4)(DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__fastcall *Fst0)();
typedef DWORD (__fastcall *Fst1)(DWORD);
typedef DWORD (__fastcall *Fst2)(DWORD, DWORD);
typedef DWORD (__fastcall *Fst3)(DWORD, DWORD, DWORD);

static bool ExecuteCall(GameCallQueue::PendingCall& c) {
    __try {
        DWORD* a = c.args;
        DWORD ret = 0;
        if (c.convention == 2) { // fastcall
            switch (c.argCount) {
                case 0: ret = ((Fst0)c.address)(); break;
                case 1: ret = ((Fst1)c.address)(a[0]); break;
                case 2: ret = ((Fst2)c.address)(a[0], a[1]); break;
                default: ret = ((Fst3)c.address)(a[0], a[1], a[2]); break;
            }
        } else if (c.convention == 1) { // cdecl
            switch (c.argCount) {
                case 0: ret = ((Cdc0)c.address)(); break;
                case 1: ret = ((Cdc1)c.address)(a[0]); break;
                case 2: ret = ((Cdc2)c.address)(a[0], a[1]); break;
                case 3: ret = ((Cdc3)c.address)(a[0], a[1], a[2]); break;
                default: ret = ((Cdc4)c.address)(a[0], a[1], a[2], a[3]); break;
            }
        } else { // stdcall
            switch (c.argCount) {
                case 0: ret = ((Std0)c.address)(); break;
                case 1: ret = ((Std1)c.address)(a[0]); break;
                case 2: ret = ((Std2)c.address)(a[0], a[1]); break;
                case 3: ret = ((Std3)c.address)(a[0], a[1], a[2]); break;
                case 4: ret = ((Std4)c.address)(a[0], a[1], a[2], a[3]); break;
                default: ret = ((Std5)c.address)(a[0], a[1], a[2], a[3], a[4]); break;
            }
        }
        c.result = ret;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
    volatile GameCallQueue::PendingCall* g_pending = nullptr;
}

namespace GameCallQueue {

    bool CallOnGameThread(PendingCall& call, int timeoutMs) {
        call.completed = false;
        call.crashed = false;

        // Set as pending (game loop will pick it up)
        g_pending = &call;

        // Wait for game loop to process it
        DWORD start = GetTickCount();
        while (!call.completed && !call.crashed) {
            if ((int)(GetTickCount() - start) > timeoutMs) {
                g_pending = nullptr;
                return false; // timeout
            }
            Sleep(1);
        }

        g_pending = nullptr;
        return call.completed && !call.crashed;
    }

    void ProcessPending() {
        if (!g_pending) return;

        volatile PendingCall* p = g_pending;
        PendingCall local = *(PendingCall*)p;

        bool ok = ExecuteCall(local);

        // Write back results
        ((PendingCall*)p)->result = local.result;
        ((PendingCall*)p)->crashed = !ok;
        ((PendingCall*)p)->completed = true;
    }
}
