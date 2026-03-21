#include "DebugPanel.h"
#include "McpServer.h"
#include "GameState.h"
#include "AutoPotion.h"
#include "AutoPickup.h"
#include "BH.h"

#include <windows.h>
#include <shellscalingapi.h>
#include <d3d9.h>
#include <thread>
#include <atomic>

#include "../ThirdParty/imgui/imgui.h"
#include "../ThirdParty/imgui/imgui_impl_dx9.h"
#include "../ThirdParty/imgui/imgui_impl_win32.h"

#pragma comment(lib, "Shcore.lib")

// Forward declare the Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
    std::thread g_thread;
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_shutdownRequested{false};

    static const char* WINDOW_CLASS = "D2ModToolkitDebugPanel";
    static const char* WINDOW_TITLE = "d2-mod-toolkit Debug Panel";

    // Base sizes at 96 DPI (100% scaling)
    static const float BASE_FONT_SIZE = 24.0f;
    static const int   BASE_WINDOW_W = 900;
    static const int   BASE_WINDOW_H = 700;

    // Manual DPI override presets
    static const float DPI_PRESETS[] = { 0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };
    static const int   DPI_PRESET_COUNT = sizeof(DPI_PRESETS) / sizeof(DPI_PRESETS[0]);
    static int         g_dpiPresetIndex = 0;  // 0 = auto
    static float       g_dpiAutoScale = 1.0f; // the detected system DPI

    HWND                g_hwnd = nullptr;
    LPDIRECT3D9         g_pD3D = nullptr;
    LPDIRECT3DDEVICE9   g_pd3dDevice = nullptr;
    D3DPRESENT_PARAMETERS g_d3dpp = {};
    float               g_dpiScale = 1.0f;
    bool                g_fontRebuildNeeded = false;

    // Get DPI scale factor for a monitor
    float GetDpiScaleForMonitor(HMONITOR hMonitor) {
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
            return (float)dpiX / 96.0f;
        }
        // Fallback: try GetDeviceCaps
        HDC hdc = GetDC(nullptr);
        float scale = (float)GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, hdc);
        return scale;
    }

    // Get DPI scale for a specific window
    float GetDpiScaleForWindow(HWND hwnd) {
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        return GetDpiScaleForMonitor(hMon);
    }

    // Rebuild the font atlas at the current DPI scale
    void RebuildFonts() {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        float fontSize = BASE_FONT_SIZE * g_dpiScale;

        // Load a scalable TTF system font instead of the fixed-size bitmap default
        const char* fontPaths[] = {
            "C:\\Windows\\Fonts\\segoeui.ttf",   // Segoe UI (Windows 10/11)
            "C:\\Windows\\Fonts\\arial.ttf",      // Arial fallback
            "C:\\Windows\\Fonts\\tahoma.ttf",     // Tahoma fallback
            nullptr
        };

        // Include arrows and symbols glyph range for icons
        static const ImWchar glyphRanges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x2190, 0x21FF, // Arrows (includes refresh ↻ U+21BB)
            0x2700, 0x27BF, // Dingbats
            0,
        };

        bool fontLoaded = false;
        for (int i = 0; fontPaths[i]; ++i) {
            if (GetFileAttributesA(fontPaths[i]) != INVALID_FILE_ATTRIBUTES) {
                io.Fonts->AddFontFromFileTTF(fontPaths[i], fontSize, nullptr, glyphRanges);
                fontLoaded = true;
                break;
            }
        }

        if (!fontLoaded) {
            // Last resort: scaled bitmap font (won't look great but won't crash)
            ImFontConfig cfg;
            cfg.SizePixels = fontSize;
            io.Fonts->AddFontDefault(&cfg);
        }

        // Rebuild font texture
        ImGui_ImplDX9_InvalidateDeviceObjects();
        io.Fonts->Build();
        ImGui_ImplDX9_CreateDeviceObjects();

        // Scale style too
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle(); // reset to defaults
        ImGui::StyleColorsDark();
        style.WindowRounding = 0.0f;
        style.FrameRounding = 2.0f;
        style.TabRounding = 2.0f;
        style.ScaleAllSizes(g_dpiScale);
    }

    bool CreateDeviceD3D(HWND hWnd) {
        g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
        if (!g_pD3D) return false;

        ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
        g_d3dpp.Windowed = TRUE;
        g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
        g_d3dpp.EnableAutoDepthStencil = TRUE;
        g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
        g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

        HRESULT hr = g_pD3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &g_d3dpp, &g_pd3dDevice);

        if (FAILED(hr)) {
            hr = g_pD3D->CreateDevice(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                &g_d3dpp, &g_pd3dDevice);
        }

        return SUCCEEDED(hr);
    }

    void CleanupDeviceD3D() {
        if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
        if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
    }

    void ResetDevice() {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
        if (hr == D3DERR_INVALIDCALL) return;
        ImGui_ImplDX9_CreateDeviceObjects();
    }

    LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;

        switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            if (g_pd3dDevice) {
                g_d3dpp.BackBufferWidth = LOWORD(lParam);
                g_d3dpp.BackBufferHeight = HIWORD(lParam);
                ResetDevice();
            }
            return 0;
        case WM_MOVE: {
            RECT r;
            GetWindowRect(hWnd, &r);
            App.debugPanel.posX.value = r.left;
            App.debugPanel.posY.value = r.top;
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            // Save dimensions after user finishes dragging/resizing
            RECT r;
            GetWindowRect(hWnd, &r);
            App.debugPanel.posX.value = r.left;
            App.debugPanel.posY.value = r.top;
            App.debugPanel.width.value = r.right - r.left;
            App.debugPanel.height.value = r.bottom - r.top;
            return 0;
        }
        case WM_DPICHANGED: {
            // Per-monitor DPI change — window was dragged to a different monitor
            float newDpi = (float)HIWORD(wParam);
            float newScale = newDpi / 96.0f;
            g_dpiAutoScale = newScale;
            // Only apply if in auto mode
            if (g_dpiPresetIndex == 0 && newScale != g_dpiScale) {
                g_dpiScale = newScale;
                g_fontRebuildNeeded = true;
            }
            // Windows provides the suggested new rect
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hWnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    // Draw all debug panel tabs
    void DrawDebugUI() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Debug Panel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            ImGui::Text("d2-mod-toolkit v0.1");
            ImGui::Separator();

            // Clickable DPI selector — cycles through presets
            char dpiLabel[32];
            if (g_dpiPresetIndex == 0)
                snprintf(dpiLabel, sizeof(dpiLabel), "DPI: Auto (%.0f%%)", g_dpiScale * 100.0f);
            else
                snprintf(dpiLabel, sizeof(dpiLabel), "DPI: %.0f%%", g_dpiScale * 100.0f);

            if (ImGui::SmallButton(dpiLabel)) {
                g_dpiPresetIndex = (g_dpiPresetIndex + 1) % DPI_PRESET_COUNT;
                if (g_dpiPresetIndex == 0) {
                    g_dpiScale = g_dpiAutoScale;
                } else {
                    g_dpiScale = DPI_PRESETS[g_dpiPresetIndex];
                }
                g_fontRebuildNeeded = true;
                App.debugPanel.dpiPreset.value = g_dpiPresetIndex;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to cycle: Auto, 100%%, 125%%, 150%%, 175%%, 200%%, 250%%, 300%%");

            ImGui::Separator();
            if (McpServer::IsRunning()) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "MCP: :%d (%d reqs)", McpServer::GetPort(), McpServer::GetRequestCount());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "MCP: offline");
            }
            ImGui::EndMenuBar();
        }

        if (ImGui::BeginTabBar("DebugTabs")) {
            if (ImGui::BeginTabItem("Player")) {
                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Not in game");
                } else {
                    auto ps = GameState::GetPlayerState();
                    const char* classNames[] = {"Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin"};
                    const char* cls = (ps.classId >= 0 && ps.classId <= 6) ? classNames[ps.classId] : "Unknown";

                    // Colors matching BH StatsDisplay
                    ImVec4 cRed(1.0f, 0.3f, 0.3f, 1.0f);
                    ImVec4 cBlue(0.4f, 0.6f, 1.0f, 1.0f);
                    ImVec4 cYellow(1.0f, 1.0f, 0.3f, 1.0f);
                    ImVec4 cGreen(0.3f, 1.0f, 0.3f, 1.0f);
                    ImVec4 cGold(0.85f, 0.72f, 0.45f, 1.0f);
                    ImVec4 cWhite(1.0f, 1.0f, 1.0f, 1.0f);
                    ImVec4 cGray(0.6f, 0.6f, 0.6f, 1.0f);
                    ImVec4 cPurple(0.7f, 0.4f, 1.0f, 1.0f);

                    // Resistance penalty from game state (dynamic based on difficulty)
                    int penalty = ps.resPenalty;

                    // -- Name / Level / Class / Difficulty --
                    float rightEdge = ImGui::GetWindowContentRegionMax().x;
                    const char* diffNames[] = {"Normal", "Nightmare", "Hell"};
                    const char* diff = (ps.difficulty >= 0 && ps.difficulty <= 2) ? diffNames[ps.difficulty] : "?";

                    ImGui::TextColored(cGold, "Name:"); ImGui::SameLine();
                    ImGui::Text("%s", ps.name);
                    char levelBuf[64]; snprintf(levelBuf, sizeof(levelBuf), "Level: %d %s (%s)", ps.level, cls, diff);
                    ImGui::SameLine(rightEdge - ImGui::CalcTextSize(levelBuf).x);
                    ImGui::TextColored(cGold, "%s", levelBuf);

                    // XP and area info (line 2)
                    char xpBuf[64]; snprintf(xpBuf, sizeof(xpBuf), "XP: %.2f%% / Additional XP: %d%%", ps.xpPctToNext, ps.addXp);
                    ImGui::SetCursorPosX(rightEdge - ImGui::CalcTextSize(xpBuf).x);
                    ImGui::TextColored(cGold, "%s", xpBuf);

                    // Area and player count (line 3)
                    ImGui::TextColored(cGold, "Area:"); ImGui::SameLine();
                    ImGui::Text("%s (%d)", ps.areaName[0] ? ps.areaName : "Unknown", ps.area);
                    char playerBuf[32]; snprintf(playerBuf, sizeof(playerBuf), "Players: %d", ps.playerCount);
                    ImGui::SameLine(rightEdge - ImGui::CalcTextSize(playerBuf).x);
                    ImGui::TextColored(cGold, "%s", playerBuf);

                    // -- Resistances --
                    ImGui::TextColored(cRed, "Fire Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d", ps.fireRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxFireRes);

                    ImGui::TextColored(cBlue, "Cold Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d", ps.coldRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxColdRes); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "  Length: %d%%", ps.cannotBeFrozen > 0 ? 0 : (100 - 50 * (ps.halfFreeze > 2 ? 2 : ps.halfFreeze)));

                    ImGui::TextColored(cYellow, "Lightning Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d", ps.lightRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxLightRes);

                    ImGui::TextColored(cGreen, "Poison Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cGreen, "%d", ps.poisonRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxPoisonRes); ImGui::SameLine();
                    ImGui::Text("  Length: %d%%", 100 - penalty - ps.poisonLenReduce);

                    int curseEff = ps.curseRes < 75 ? ps.curseRes : 75;
                    int curseLen = 100 - ps.curseLenReduce;
                    if (curseLen < 25) curseLen = 25;
                    ImGui::TextColored(cGold, "Curse Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cGray, "%d / 75", curseEff); ImGui::SameLine();
                    ImGui::Text("Length: %d%%", curseLen);

                    ImGui::Separator();

                    // -- Absorption --
                    ImGui::TextColored(cGold, "Absorption:");
                    ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d/%d%%", ps.fireAbsorb, ps.fireAbsorbPct); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d/%d%%", ps.coldAbsorb, ps.coldAbsorbPct); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d/%d%%", ps.lightAbsorb, ps.lightAbsorbPct); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d/%d%%", ps.magicAbsorb, ps.magicAbsorbPct);

                    // -- Damage Reduction --
                    ImGui::TextColored(cGold, "Damage Reduction:"); ImGui::SameLine();
                    ImGui::Text("%d/%d%%", ps.dmgReduction, ps.dmgReductionPct); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d/%d%%", ps.magDmgReduction, ps.magDmgReductionPct);

                    ImGui::TextColored(cGold, "Attacker Takes Damage:"); ImGui::SameLine();
                    ImGui::Text("%d", ps.attackerTakesDmg); ImGui::SameLine();
                    ImGui::TextColored(cYellow, " %d", ps.attackerTakesLtng);

                    ImGui::Separator();

                    // -- Elemental Mastery & Pierce --
                    ImGui::TextColored(cGold, "Elemental Mastery:");
                    ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d%%", ps.fireMastery); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d%%", ps.coldMastery); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d%%", ps.lightMastery); ImGui::SameLine();
                    ImGui::TextColored(cGreen, "%d%%", ps.poisonMastery); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d%%", ps.magicMastery);

                    ImGui::TextColored(cGold, "Elemental Pierce:");
                    ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d%%", ps.firePierce); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d%%", ps.coldPierce); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d%%", ps.lightPierce); ImGui::SameLine();
                    ImGui::TextColored(cGreen, "%d%%", ps.poisonPierce); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d%%", ps.magicPierce);

                    // -- AR / Defense / Base Damage --
                    int dexAR = ps.dexterity * 5;
                    ImGui::TextColored(cGold, "Base AR:"); ImGui::SameLine();
                    ImGui::Text("dex: %d  equip: %d  total: %d", dexAR, ps.attackRating, dexAR + ps.attackRating);

                    int dexDef = ps.dexterity / 4;
                    ImGui::TextColored(cGold, "Base Def:"); ImGui::SameLine();
                    ImGui::Text("dex: %d  equip: %d  total: %d", dexDef, ps.defense, dexDef + ps.defense);

                    ImGui::TextColored(cGold, "Base Damage:"); ImGui::SameLine();
                    ImGui::Text("1h: %d-%d  2h: %d-%d", ps.minDmg, ps.maxDmg, ps.minDmg2, ps.maxDmg2);

                    ImGui::Separator();

                    // -- Breakpoints (includes rates) --
                    float col2 = ImGui::GetContentRegionAvail().x * 0.5f;
                    ImGui::TextColored(cGold, "Breakpoints (%s):", ps.bpSkillName[0] ? ps.bpSkillName : "N/A");

                    // Helper lambda to render a breakpoint line
                    auto drawBP = [&](const GameState::PlayerState::BreakpointInfo& bp) {
                        if (bp.count == 0) return;
                        ImGui::TextColored(cGold, "%s:", bp.label); ImGui::SameLine();
                        ImGui::TextColored(cGold, "(%d)", bp.currentValue); ImGui::SameLine();
                        for (int i = 0; i < bp.count; i++) {
                            if (i == bp.activeIndex) {
                                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.0f, 1.0f), "%d", bp.values[i]);
                            } else {
                                ImGui::Text("%d", bp.values[i]);
                            }
                            if (i < bp.count - 1) {
                                ImGui::SameLine(); ImGui::TextColored(cGray, "/"); ImGui::SameLine();
                            }
                        }
                    };

                    drawBP(ps.bpFCR);
                    drawBP(ps.bpFHR);
                    ImGui::TextColored(cGold, "IAS (Frames):"); ImGui::SameLine(); ImGui::Text("N/A");

                    // Remaining rates on one line
                    ImGui::TextColored(cGold, "Block Rate:"); ImGui::SameLine(); ImGui::Text("%d", ps.fbr);
                    ImGui::SameLine(); ImGui::TextColored(cGold, "  Run/Walk:"); ImGui::SameLine(); ImGui::Text("%d", ps.frw);
                    ImGui::SameLine(); ImGui::TextColored(cGold, "  Attack Rate:"); ImGui::SameLine(); ImGui::Text("%d", ps.attackRate);
                    ImGui::SameLine(); ImGui::TextColored(cGold, "  IAS:"); ImGui::SameLine(); ImGui::Text("%d", ps.ias);

                    // -- Combat stats (2-column) --
                    int dsMax = 75 + ps.maxDeadlyStrike;
                    if (dsMax > 100) dsMax = 100;
                    int dsVal = ps.deadlyStrike < dsMax ? ps.deadlyStrike : dsMax;

                    ImGui::TextColored(cGold, "Crushing Blow:"); ImGui::SameLine(); ImGui::Text("%d", ps.crushingBlow);
                    ImGui::SameLine(col2); ImGui::TextColored(cGold, "Open Wounds:"); ImGui::SameLine(); ImGui::Text("%d%%/+%d", ps.openWounds, ps.deepWounds);
                    ImGui::TextColored(cGold, "Deadly Strike:"); ImGui::SameLine(); ImGui::Text("%d / %d", dsVal, dsMax);
                    ImGui::SameLine(col2); ImGui::TextColored(cGold, "Critical Strike:"); ImGui::SameLine(); ImGui::Text("%d", ps.criticalStrike < 75 ? ps.criticalStrike : 75);
                    ImGui::TextColored(cRed, "Life Leech:"); ImGui::SameLine(); ImGui::TextColored(cRed, "%d", ps.lifeLeech);
                    ImGui::SameLine(col2); ImGui::TextColored(cBlue, "Mana Leech:"); ImGui::SameLine(); ImGui::TextColored(cBlue, "%d", ps.manaLeech);
                    ImGui::TextColored(cGold, "Projectile Pierce:"); ImGui::SameLine(); ImGui::Text("%d", ps.piercingAttack + ps.pierce);
                    ImGui::SameLine(col2); ImGui::TextColored(cGold, "HP/MP per Kill:"); ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d", ps.lifePerKill); ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d", ps.manaPerKill);

                    ImGui::Separator();

                    // -- Elemental damage (single line) --
                    auto applyMastery = [](int val, int mastery) { return val + (val * mastery / 100); };
                    int fMin = applyMastery(ps.minFireDmg, ps.fireMastery), fMax = applyMastery(ps.maxFireDmg, ps.fireMastery);
                    int cMin = applyMastery(ps.minColdDmg, ps.coldMastery), cMax = applyMastery(ps.maxColdDmg, ps.coldMastery);
                    int lMin = applyMastery(ps.minLightDmg, ps.lightMastery), lMax = applyMastery(ps.maxLightDmg, ps.lightMastery);
                    int pMin = applyMastery(ps.minPoisonDmg, ps.poisonMastery), pMax = applyMastery(ps.maxPoisonDmg, ps.poisonMastery);
                    int pLen = ps.poisonLenOverride > 0 ? ps.poisonLenOverride : ps.poisonLength;
                    int mMin = applyMastery(ps.minMagicDmg, ps.magicMastery), mMax = applyMastery(ps.maxMagicDmg, ps.magicMastery);

                    ImGui::TextColored(cGold, "Dmg:"); ImGui::SameLine(); ImGui::Text("+%d", ps.addedDamage);
                    ImGui::SameLine(); ImGui::TextColored(cRed, "  Fire:"); ImGui::SameLine(); ImGui::TextColored(cRed, "%d-%d", fMin, fMax);
                    ImGui::SameLine(); ImGui::TextColored(cBlue, "  Cold:"); ImGui::SameLine(); ImGui::TextColored(cBlue, "%d-%d", cMin, cMax);
                    ImGui::SameLine(); ImGui::TextColored(cYellow, "  Ltng:"); ImGui::SameLine(); ImGui::TextColored(cYellow, "%d-%d", lMin, lMax);
                    ImGui::SameLine(); ImGui::TextColored(cGreen, "  Psn:"); ImGui::SameLine();
                    if (pLen > 0) ImGui::TextColored(cGreen, "%d-%d/%.1fs", (int)(pMin / 256.0 * pLen), (int)(pMax / 256.0 * pLen), pLen / 25.0);
                    else ImGui::TextColored(cGreen, "0-0");
                    ImGui::SameLine(); ImGui::TextColored(cPurple, "  Mag:"); ImGui::SameLine(); ImGui::TextColored(cPurple, "%d-%d", mMin, mMax);

                    ImGui::Separator();

                    // -- MF / GF / Stash Gold (single line) --
                    ImGui::TextColored(cBlue, "Magic Find:"); ImGui::SameLine(); ImGui::TextColored(cBlue, "%d", ps.mf);
                    ImGui::SameLine(); ImGui::TextColored(cYellow, "  Gold Find:"); ImGui::SameLine(); ImGui::TextColored(cYellow, "%d", ps.gf);
                    ImGui::SameLine(); ImGui::TextColored(cYellow, "  Stash Gold:"); ImGui::SameLine(); ImGui::TextColored(cYellow, "%d", ps.goldStash);

                    // -- HP / Mana / Stamina bars (single row) --
                    ImGui::Separator();
                    {
                        int hp = ps.hp >> 8, maxHp = ps.maxHp >> 8;
                        int mp = ps.mana >> 8, maxMp = ps.maxMana >> 8;
                        int stam = ps.stamina >> 8, maxStam = ps.maxStamina >> 8;
                        float barW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;

                        char hpLabel[32]; snprintf(hpLabel, sizeof(hpLabel), "HP: %d/%d", hp, maxHp);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                        ImGui::ProgressBar(maxHp > 0 ? (float)hp / maxHp : 0, ImVec2(barW, 0), hpLabel);
                        ImGui::PopStyleColor();

                        ImGui::SameLine();
                        char mpLabel[32]; snprintf(mpLabel, sizeof(mpLabel), "MP: %d/%d", mp, maxMp);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.1f, 0.2f, 0.8f, 1.0f));
                        ImGui::ProgressBar(maxMp > 0 ? (float)mp / maxMp : 0, ImVec2(barW, 0), mpLabel);
                        ImGui::PopStyleColor();

                        ImGui::SameLine();
                        char stamLabel[32]; snprintf(stamLabel, sizeof(stamLabel), "Stam: %d/%d", stam, maxStam);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.6f, 0.6f, 0.1f, 1.0f));
                        ImGui::ProgressBar(maxStam > 0 ? (float)stam / maxStam : 0, ImVec2(barW, 0), stamLabel);
                        ImGui::PopStyleColor();
                    }

                    // -- Auto-Potion Controls --
                    ImGui::Separator();
                    {
                        auto apc = AutoPotion::GetConfig();
                        bool changed = false;

                        if (ImGui::Checkbox("##apEnable", &apc.enabled)) changed = true;
                        ImGui::SameLine();
                        ImGui::TextColored(cGold, "Auto-Potion:");

                        if (apc.enabled) {
                            ImGui::SameLine();
                            ImGui::PushItemWidth(60 * g_dpiScale);
                            ImGui::TextColored(cRed, "  HP:"); ImGui::SameLine();
                            if (ImGui::InputInt("##hp", &apc.hpThreshold, 0, 0)) changed = true;
                            ImGui::SameLine(); ImGui::Text("%%");
                            ImGui::SameLine();
                            ImGui::TextColored(cBlue, "  MP:"); ImGui::SameLine();
                            if (ImGui::InputInt("##mp", &apc.mpThreshold, 0, 0)) changed = true;
                            ImGui::SameLine(); ImGui::Text("%%");
                            ImGui::SameLine();
                            ImGui::TextColored(cPurple, "  Rejuv:"); ImGui::SameLine();
                            if (ImGui::InputInt("##rejuv", &apc.rejuvThreshold, 0, 0)) changed = true;
                            ImGui::SameLine(); ImGui::Text("%%");
                            ImGui::PopItemWidth();
                        }

                        if (changed) {
                            if (apc.hpThreshold < 0) apc.hpThreshold = 0;
                            if (apc.hpThreshold > 100) apc.hpThreshold = 100;
                            if (apc.mpThreshold < 0) apc.mpThreshold = 0;
                            if (apc.mpThreshold > 100) apc.mpThreshold = 100;
                            if (apc.rejuvThreshold < 0) apc.rejuvThreshold = 0;
                            if (apc.rejuvThreshold > 100) apc.rejuvThreshold = 100;
                            AutoPotion::SetConfig(apc);
                        }
                    }

                    // -- Auto-Pickup Controls --
                    {
                        auto auc = AutoPickup::GetConfig();
                        bool changed = false;

                        if (ImGui::Checkbox("##auEnable", &auc.enabled)) changed = true;
                        ImGui::SameLine();
                        ImGui::TextColored(cGold, "Auto-Pickup:");

                        if (auc.enabled) {
                            ImGui::SameLine();
                            ImGui::PushItemWidth(50 * g_dpiScale);
                            ImGui::TextColored(cGold, "  Range:"); ImGui::SameLine();
                            if (ImGui::InputInt("##range", &auc.maxDistance, 0, 0)) changed = true;
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (ImGui::Checkbox("TP##pickTP", &auc.pickTpScrolls)) changed = true;
                            ImGui::SameLine();
                            if (ImGui::Checkbox("ID##pickID", &auc.pickIdScrolls)) changed = true;
                            ImGui::SameLine();
                            if (ImGui::SmallButton("\xe2\x86\xbb##resnap")) { // U+21BB ↻
                                AutoPickup::ResnapBelt();
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-snapshot belt layout");

                            // Show snapshot info
                            auto snap = AutoPickup::GetSnapshot();
                            if (snap.valid) {
                                ImGui::SameLine();
                                ImGui::TextColored(cGray, " Snap:");
                                for (int c = 0; c < 4; c++) {
                                    ImGui::SameLine();
                                    if (snap.preferredCode[c] > 0) {
                                        ImGui::Text("[%d]", snap.preferredCode[c]);
                                    } else {
                                        ImGui::TextColored(cGray, "[--]");
                                    }
                                }
                            }
                        }

                        if (changed) {
                            if (auc.maxDistance < 1) auc.maxDistance = 1;
                            if (auc.maxDistance > 40) auc.maxDistance = 40;
                            AutoPickup::SetConfig(auc);
                        }
                    }

                    // -- Belt --
                    ImGui::Separator();
                    ImGui::TextColored(cGold, "Belt:");
                    auto belt = GameState::GetBeltState();
                    for (int row = 0; row < belt.rows; ++row) {
                        for (int col = 0; col < belt.columns; ++col) {
                            if (col > 0) ImGui::SameLine();
                            int idx = row * belt.columns + col;
                            const auto& slot = belt.slots[idx];
                            if (slot.occupied) {
                                ImGui::Button(slot.name, ImVec2(80 * g_dpiScale, 24 * g_dpiScale));
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                                ImGui::Button("--", ImVec2(80 * g_dpiScale, 24 * g_dpiScale));
                                ImGui::PopStyleColor();
                            }
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World")) {
                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Not in game");
                } else {
                    auto units = GameState::GetNearbyUnits(40);

                    // Split into monsters and items
                    ImGui::Text("Nearby Units: %d", (int)units.size());
                    ImGui::Separator();

                    if (ImGui::BeginTable("UnitsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70 * g_dpiScale);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, 80 * g_dpiScale);
                        ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 50 * g_dpiScale);
                        ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 100 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        for (const auto& u : units) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();

                            const char* typeNames[] = {"Player", "Monster", "Object", "Missile", "Item", "Tile"};
                            const char* tname = (u.type >= 0 && u.type <= 5) ? typeNames[u.type] : "?";

                            if (u.isBoss) ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "%s", tname);
                            else if (u.isChampion) ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%s", tname);
                            else ImGui::Text("%s", tname);

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", u.name);

                            ImGui::TableNextColumn();
                            if (u.type == 1 || u.type == 0) {
                                int hp = u.hp >> 8, mhp = u.maxHp >> 8;
                                if (u.mode == 0 || u.mode == 12)
                                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Dead");
                                else
                                    ImGui::Text("%d/%d", hp, mhp);
                            }

                            ImGui::TableNextColumn();
                            ImGui::Text("%d", u.distance);

                            ImGui::TableNextColumn();
                            ImGui::Text("(%d,%d)", u.x, u.y);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug")) {
                ImGui::Text("Function hooks and call log will appear here.");
                ImGui::Separator();
                ImGui::Text("Crash/Exception Log:");
                ImGui::Text("  (no exceptions captured)");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Memory")) {
                ImGui::Text("Memory read/write and watch list will appear here.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Patches")) {
                ImGui::Text("Patch manager will appear here.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Rendering")) {
                ImGui::Text("Viewport and camera debug info will appear here.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    // Find the monitor where the game window lives
    // and compute a position below the game window on that monitor
    void ComputePanelPosition(int& outX, int& outY, int& outW, int& outH) {
        // Check if we have a saved position from config
        bool hasSavedPos = (App.debugPanel.posX.value != -1 && App.debugPanel.posY.value != -1);

        if (hasSavedPos) {
            outX = App.debugPanel.posX.value;
            outY = App.debugPanel.posY.value;
            outW = App.debugPanel.width.value;
            outH = App.debugPanel.height.value;

            // Get DPI for the saved position
            POINT pt = { outX, outY };
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            g_dpiAutoScale = GetDpiScaleForMonitor(hMon);
            g_dpiScale = g_dpiAutoScale;

            // Restore DPI preset if saved
            g_dpiPresetIndex = App.debugPanel.dpiPreset.value;
            if (g_dpiPresetIndex > 0 && g_dpiPresetIndex < DPI_PRESET_COUNT) {
                g_dpiScale = DPI_PRESETS[g_dpiPresetIndex];
            }
            return;
        }

        // Auto-position: find game window and position below it
        HWND gameHwnd = nullptr;
        for (int i = 0; i < 30 && !gameHwnd; ++i) {
            gameHwnd = FindWindow(nullptr, "Diablo II");
            if (!gameHwnd) Sleep(200);
        }

        if (gameHwnd) {
            HMONITOR hMon = MonitorFromWindow(gameHwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            GetMonitorInfo(hMon, &mi);

            RECT gameRect;
            GetWindowRect(gameHwnd, &gameRect);

            g_dpiAutoScale = GetDpiScaleForMonitor(hMon);
            g_dpiScale = g_dpiAutoScale;

            outW = (int)(BASE_WINDOW_W * g_dpiScale);
            outH = (int)(BASE_WINDOW_H * g_dpiScale);

            outX = gameRect.left;
            outY = gameRect.bottom + 5;

            int monitorBottom = mi.rcWork.bottom;
            if (outY + outH > monitorBottom) {
                outY = monitorBottom - outH;
                if (outY < mi.rcWork.top) {
                    outY = mi.rcWork.top;
                    outH = monitorBottom - outY;
                }
            }

            int monitorRight = mi.rcWork.right;
            if (outX + outW > monitorRight) {
                outW = monitorRight - outX;
            }
        } else {
            HDC hdc = GetDC(nullptr);
            g_dpiAutoScale = (float)GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
            g_dpiScale = g_dpiAutoScale;
            ReleaseDC(nullptr, hdc);

            outW = (int)(BASE_WINDOW_W * g_dpiScale);
            outH = (int)(BASE_WINDOW_H * g_dpiScale);
            outX = 100;
            outY = 100;
        }
    }

    // Main thread function for the debug panel
    void PanelThread() {
        // Enable per-monitor DPI awareness for this thread
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

        // Register window class
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = WINDOW_CLASS;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassEx(&wc);

        // Compute position below the game window
        int posX, posY, winW, winH;
        ComputePanelPosition(posX, posY, winW, winH);

        g_hwnd = CreateWindowEx(
            0,
            WINDOW_CLASS, WINDOW_TITLE,
            WS_OVERLAPPEDWINDOW,
            posX, posY, winW, winH,
            nullptr, nullptr, wc.hInstance, nullptr);

        if (!g_hwnd || !CreateDeviceD3D(g_hwnd)) {
            CleanupDeviceD3D();
            UnregisterClass(WINDOW_CLASS, wc.hInstance);
            return;
        }

        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);

        // Setup ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX9_Init(g_pd3dDevice);

        // Build fonts at current DPI
        RebuildFonts();

        g_running = true;

        // Main loop
        const ImVec4 clearColor(0.06f, 0.06f, 0.10f, 1.00f);
        MSG msg;
        while (!g_shutdownRequested) {
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    g_shutdownRequested = true;
                }
            }
            if (g_shutdownRequested) break;

            // Rebuild fonts if DPI changed (from WM_DPICHANGED)
            if (g_fontRebuildNeeded) {
                RebuildFonts();
                g_fontRebuildNeeded = false;
            }

            // Start ImGui frame
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            DrawDebugUI();

            // Render
            ImGui::EndFrame();
            g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
            g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            D3DCOLOR bg = D3DCOLOR_RGBA(
                (int)(clearColor.x * 255), (int)(clearColor.y * 255),
                (int)(clearColor.z * 255), (int)(clearColor.w * 255));
            g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                bg, 1.0f, 0);

            if (g_pd3dDevice->BeginScene() >= 0) {
                ImGui::Render();
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                g_pd3dDevice->EndScene();
            }

            HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
            if (result == D3DERR_DEVICELOST &&
                g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
                ResetDevice();
            }
        }

        // Cleanup
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        UnregisterClass(WINDOW_CLASS, wc.hInstance);
        g_hwnd = nullptr;
        g_running = false;
    }
}

namespace DebugPanel {
    void Init() {
        if (g_running) return;
        g_shutdownRequested = false;
        g_thread = std::thread(PanelThread);
        g_thread.detach();
    }

    void Shutdown() {
        if (!g_running) return;
        g_shutdownRequested = true;
        if (g_hwnd) {
            PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        }
        for (int i = 0; i < 50 && g_running; ++i) {
            Sleep(100);
        }
    }

    bool IsRunning() {
        return g_running;
    }
}
