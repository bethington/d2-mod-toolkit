// Must include winsock2 before windows.h to avoid v1/v2 conflict
#include <winsock2.h>
#include <ws2tcpip.h>

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "McpServer.h"
#include "GameState.h"
#include "AutoPotion.h"
#include "AutoPickup.h"
#include "HookManager.h"
#include "GameNav.h"
#include "CrashCatcher.h"
#include "PatchManager.h"
#include "StructRegistry.h"
#include "GameCallQueue.h"
#include "GamePause.h"
#include "MemWatch.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <string>
#include <sstream>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <functional>
#include <fstream>
#include <psapi.h>
#include <eh.h>
#include <stdexcept>
#include <cmath>
#include <algorithm>

// cpp-httplib — single header HTTP server
#include "../ThirdParty/httplib.h"

// nlohmann JSON — already in ThirdParty for BH
#include "../ThirdParty/nlohmann/json.hpp"

using json = nlohmann::json;

namespace {
    std::thread g_thread;
    std::atomic<bool> g_running{false};
    std::atomic<int>  g_requestCount{0};
    int g_port = 21337;
    httplib::Server* g_server = nullptr;

    // SSE session management
    // Each SSE client gets a unique session ID. Responses to JSON-RPC requests
    // are pushed back through the SSE stream for that session.
    std::atomic<int> g_nextSessionId{1};

    struct SseSession {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::string> messages; // SSE-formatted messages to send
        bool closed = false;
    };

    std::mutex g_sessionsMutex;
    std::map<int, std::shared_ptr<SseSession>> g_sessions;

    void SendSseMessage(int sessionId, const std::string& event, const std::string& data) {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        auto it = g_sessions.find(sessionId);
        if (it != g_sessions.end()) {
            auto& session = it->second;
            std::lock_guard<std::mutex> slock(session->mutex);
            std::string msg = "event: " + event + "\ndata: " + data + "\n\n";
            session->messages.push(msg);
            session->cv.notify_one();
        }
    }

    void CloseSseSession(int sessionId) {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        auto it = g_sessions.find(sessionId);
        if (it != g_sessions.end()) {
            auto& session = it->second;
            std::lock_guard<std::mutex> slock(session->mutex);
            session->closed = true;
            session->cv.notify_one();
        }
    }

    // Safe memory access helpers (SEH can't be in functions with C++ objects)
    static bool SafeMemRead(DWORD addr, void* dest, size_t size) {
        __try {
            memcpy(dest, (void*)addr, size);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool SafeGetUnitName(UnitAny* pItem, char* outName, int outSize) {
        __try {
            wchar_t* wName = D2CLIENT_GetUnitName(pItem);
            if (wName) {
                WideCharToMultiByte(CP_UTF8, 0, wName, -1, outName, outSize - 1, nullptr, nullptr);
                return true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    // Safe function call with SEH — matches exact argument count to avoid stack corruption
    typedef DWORD (__stdcall *Std0)();
    typedef DWORD (__stdcall *Std1)(DWORD);
    typedef DWORD (__stdcall *Std2)(DWORD, DWORD);
    typedef DWORD (__stdcall *Std3)(DWORD, DWORD, DWORD);
    typedef DWORD (__stdcall *Std4)(DWORD, DWORD, DWORD, DWORD);
    typedef DWORD (__stdcall *Std5)(DWORD, DWORD, DWORD, DWORD, DWORD);
    typedef DWORD (__stdcall *Std6)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
    typedef DWORD (__cdecl *Cdc0)();
    typedef DWORD (__cdecl *Cdc1)(DWORD);
    typedef DWORD (__cdecl *Cdc2)(DWORD, DWORD);
    typedef DWORD (__cdecl *Cdc3)(DWORD, DWORD, DWORD);
    typedef DWORD (__cdecl *Cdc4)(DWORD, DWORD, DWORD, DWORD);
    typedef DWORD (__fastcall *Fst0)();
    typedef DWORD (__fastcall *Fst1)(DWORD);
    typedef DWORD (__fastcall *Fst2)(DWORD, DWORD);
    typedef DWORD (__fastcall *Fst3)(DWORD, DWORD, DWORD);
    typedef DWORD (__fastcall *Fst4)(DWORD, DWORD, DWORD, DWORD);

    static bool SafeCallFunction(DWORD addr, DWORD* a, int argc, int conv, DWORD* pResult) {
        __try {
            DWORD ret = 0;
            if (conv == 2) { // fastcall
                switch (argc) {
                    case 0: ret = ((Fst0)addr)(); break;
                    case 1: ret = ((Fst1)addr)(a[0]); break;
                    case 2: ret = ((Fst2)addr)(a[0], a[1]); break;
                    case 3: ret = ((Fst3)addr)(a[0], a[1], a[2]); break;
                    default: ret = ((Fst4)addr)(a[0], a[1], a[2], a[3]); break;
                }
            } else if (conv == 1) { // cdecl
                switch (argc) {
                    case 0: ret = ((Cdc0)addr)(); break;
                    case 1: ret = ((Cdc1)addr)(a[0]); break;
                    case 2: ret = ((Cdc2)addr)(a[0], a[1]); break;
                    case 3: ret = ((Cdc3)addr)(a[0], a[1], a[2]); break;
                    default: ret = ((Cdc4)addr)(a[0], a[1], a[2], a[3]); break;
                }
            } else { // stdcall
                switch (argc) {
                    case 0: ret = ((Std0)addr)(); break;
                    case 1: ret = ((Std1)addr)(a[0]); break;
                    case 2: ret = ((Std2)addr)(a[0], a[1]); break;
                    case 3: ret = ((Std3)addr)(a[0], a[1], a[2]); break;
                    case 4: ret = ((Std4)addr)(a[0], a[1], a[2], a[3]); break;
                    case 5: ret = ((Std5)addr)(a[0], a[1], a[2], a[3], a[4]); break;
                    default: ret = ((Std6)addr)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
                }
            }
            *pResult = ret;
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Resolve a DLL ordinal to an address
    static DWORD ResolveOrdinal(const char* dllName, int ordinal) {
        HMODULE hMod = GetModuleHandleA(dllName);
        if (!hMod) return 0;
        if (ordinal < 0) ordinal = -ordinal;
        FARPROC proc = GetProcAddress(hMod, (LPCSTR)(DWORD_PTR)ordinal);
        return (DWORD)proc;
    }

    static bool SafeGetItemSize(DWORD txtFileNo, int* pW, int* pH) {
        __try {
            ItemsTxt* txt = D2COMMON_GetItemText(txtFileNo);
            if (txt) {
                *pW = txt->binvwidth;
                *pH = txt->binvheight;
                if (*pW < 1) *pW = 1;
                if (*pH < 1) *pH = 1;
                return true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        *pW = 1; *pH = 1;
        return false;
    }

    static bool SafeMemWrite(DWORD addr, const void* src, size_t size) {
        __try {
            memcpy((void*)addr, src, size);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Materials tab switch helper (needs inline asm, can't be in a local struct)
    static DWORD __cdecl SwitchToMaterialsTab() {
        HMODULE pd2 = GetModuleHandle("ProjectDiablo.dll");
        if (!pd2) return 0;
        DWORD* pSt = (DWORD*)(*(DWORD*)((DWORD)pd2 + 0x00410688));
        if (*pSt != 0x0C) return 0;
        DWORD* pTab = (DWORD*)((DWORD)pd2 + 0x0040edd4);
        if (*pTab == 10) return 0;
        DWORD* pPend = (DWORD*)((DWORD)pd2 + 0x0030de48);
        if (*pPend != 0) return 0;
        DWORD dAddr = (DWORD)pd2 + 0x0023ead0;
        WORD pkt = 0x0B55;
        __asm {
            lea ecx, pkt
            mov edx, 2
            call dAddr
        }
        return 1;
    }

    // Switch stash tab (0-10) via GameCallQueue. Returns true on success.
    static bool SwitchStashTab(int tab) {
        if (tab < 0 || tab > 10) return false;
        HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
        if (!hPD2) return false;

        static const DWORD tabRVA[] = {
            0x001906c0, 0x00190700, 0x00190740, 0x00190780, 0x001907c0,
            0x00190800, 0x00190840, 0x00190880, 0x001908c0, 0x00190900,
        };

        GameCallQueue::PendingCall call = {};
        if (tab <= 9) {
            call.address = (DWORD)hPD2 + tabRVA[tab];
            call.argCount = 0;
            call.convention = 1;
        } else {
            call.address = (DWORD)&SwitchToMaterialsTab;
            call.argCount = 0;
            call.convention = 1;
        }
        bool ok = GameCallQueue::CallOnGameThread(call, 3000);
        if (ok) Sleep(300);
        return ok;
    }

    // Stat ID -> human-readable name (covers the most common/important stats)
    static const char* GetStatName(int statId) {
        static const char* names[] = {
            "strength", "energy", "dexterity", "vitality",         // 0-3
            "stat_points", "skill_points", "life", "max_life",     // 4-7
            "mana", "max_mana", "stamina", "max_stamina",          // 8-11
            "level", "experience", "gold", "gold_stash",           // 12-15
            "enhanced_defense", "enhanced_max_dmg", "enhanced_min_dmg", "attack_rating", // 16-19
            "to_block", "min_dmg", "max_dmg", "min_dmg_2h", "max_dmg_2h", // 20-24
            "enhanced_damage", "mana_recovery", "mana_recovery_bonus", "stamina_recovery", // 25-28
            "last_exp", "next_exp", "defense", "defense_vs_missile", "defense_vs_melee", // 29-33
            "dmg_reduction", "magic_dmg_reduction", "dmg_reduction_pct", "magic_dmg_reduction_pct", "max_magic_dmg_reduction_pct", // 34-38
            "fire_resist", "max_fire_resist", "lightning_resist", "max_lightning_resist", // 39-42
            "cold_resist", "max_cold_resist", "poison_resist", "max_poison_resist", // 43-46
            "damage_aura", "min_fire_dmg", "max_fire_dmg", "min_light_dmg", "max_light_dmg", // 47-51
            "min_magic_dmg", "max_magic_dmg", "min_cold_dmg", "max_cold_dmg", "cold_duration", // 52-56
            "min_poison_dmg", "max_poison_dmg", "poison_duration", // 57-59
            "life_leech", "max_life_leech", "mana_leech", "max_mana_leech", // 60-63
            "min_stamina_drain", "max_stamina_drain", "stun_length", "velocity", // 64-67
            "attack_rate", "other_anim_rate", "ammo_quantity", "value", // 68-71
            "durability", "max_durability", "replenish_life", "enhanced_max_durability", // 72-75
            "enhanced_life", "enhanced_mana", "attacker_takes_dmg", "gold_find", // 76-79
            "magic_find", "knockback", "time_duration", "class_skills", // 80-83
            "unsent_param", "add_experience", "life_per_kill", "reduce_vendor", // 84-87
            "double_herb", "light_radius", "light_color", "reduced_requirements", // 88-91
            "reduced_level_req", "ias", "reduced_level_req_pct", "last_block_frame", // 92-95
            "frw", "non_class_skill", "state", "fhr", // 96-99
            "monster_player_count", "poison_override_len", "fbr", "skill_bypass_undead", // 100-103
            "skill_bypass_demons", "fcr", "skill_bypass_beasts", "single_skill", // 104-107
            "slain_monsters_rip", "curse_resistance", "poison_length_reduction", "adds_damage", // 108-111
            "hit_causes_flee", "hit_blinds", "damage_to_mana", "ignore_target_defense", // 112-115
            "reduce_target_defense", "prevent_monster_heal", "half_freeze_duration", "to_hit_pct", // 116-119
            "monster_def_deduct", "damage_to_demons", "damage_to_undead", "ar_vs_demons", "ar_vs_undead", // 120-124
            "throwable", "elemental_skills", "all_skills", "attacker_takes_light_dmg", // 125-128
            "iron_maiden_level", "life_tap_level", "thorns_pct", "bone_armor", "max_bone_armor", // 129-133
            "freeze_target", "open_wounds", "crushing_blow", "kick_damage", // 134-137
            "mana_per_kill", "life_per_demon_kill", "extra_blood", "deadly_strike", // 138-141
            "fire_absorb_pct", "fire_absorb", "light_absorb_pct", "light_absorb", // 142-145
            "magic_absorb_pct", "magic_absorb", "cold_absorb_pct", "cold_absorb", // 146-149
            "slow", "aura", "indestructible", "cannot_be_frozen", // 150-153
            "stamina_drain_pct", "reanimate", "piercing", "fires_magic_arrows", "fires_exploding_arrows", // 154-158
            "min_throw_dmg", "max_throw_dmg", // 159-160
        };
        if (statId >= 0 && statId <= 160) return names[statId];
        // High stat IDs (skill charges, etc)
        if (statId == 188) return "skill_on_attack";
        if (statId == 195) return "skill_on_kill";
        if (statId == 196) return "skill_on_death";
        if (statId == 197) return "skill_on_hit";
        if (statId == 198) return "skill_on_levelup";
        if (statId == 199) return "skill_on_get_hit";
        if (statId == 201) return "skill_charges";
        if (statId == 204) return "skill_tab";
        if (statId == 214) return "sockets";
        if (statId == 252) return "fire_mastery";
        if (statId == 253) return "lightning_mastery";
        if (statId == 254) return "cold_mastery";
        if (statId == 255) return "poison_mastery";
        if (statId == 329) return "item_armor_perlevel";
        if (statId == 330) return "item_hp_perlevel";
        return nullptr;
    }

    // MCP protocol version
    static const char* MCP_VERSION = "2024-11-05";

    // Server info returned in initialize response
    json ServerInfo() {
        return {
            {"name", "d2-mod-toolkit"},
            {"version", "0.1.0"}
        };
    }

    // Build the tools list
    json ToolsList() {
        json tools = json::array();

        tools.push_back({
            {"name", "ping"},
            {"description", "Test connectivity to the game process. Returns basic status."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_game_info"},
            {"description", "Get basic information about the running Diablo II process."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_player_state"},
            {"description", "Get current player state: HP, MP, position, area, level, class, stats, resistances."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_belt_contents"},
            {"description", "Get contents of the player's belt slots (potions, etc)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_nearby_units"},
            {"description", "Get list of nearby units (monsters, players, items) sorted by distance."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"max_distance", {
                        {"type", "integer"},
                        {"description", "Maximum distance from player (default 40)"}
                    }}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_inventory"},
            {"description", "List all items in the player's inventory, belt, equipped, stash, and cube with their game names and codes."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"location", {{"type", "string"}, {"description", "Filter by location: 'all', 'belt', 'inventory', 'equipped', 'stash', 'cube' (default 'all')"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_auto_potion"},
            {"description", "Get auto-potion configuration and status."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "set_auto_potion"},
            {"description", "Configure auto-potion settings. Pass only the fields you want to change."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"enabled", {{"type", "boolean"}, {"description", "Enable/disable auto-potion"}}},
                    {"hp_threshold", {{"type", "integer"}, {"description", "HP % to trigger health potion (0-100)"}}},
                    {"mp_threshold", {{"type", "integer"}, {"description", "Mana % to trigger mana potion (0-100)"}}},
                    {"rejuv_threshold", {{"type", "integer"}, {"description", "HP % to trigger rejuv (0-100, 0=disabled)"}}},
                    {"cooldown_ms", {{"type", "integer"}, {"description", "Minimum ms between potions (100-5000)"}}},
                    {"skip_in_town", {{"type", "boolean"}, {"description", "Skip auto-potion in town areas"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_auto_pickup"},
            {"description", "Get auto-pickup configuration."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "set_auto_pickup"},
            {"description", "Configure auto-pickup settings. Snapshots belt layout on enable. Pass only fields to change."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"enabled", {{"type", "boolean"}, {"description", "Enable/disable auto-pickup (snapshots belt on enable)"}}},
                    {"max_distance", {{"type", "integer"}, {"description", "Max pickup range in game units (1-40)"}}},
                    {"pick_tp_scrolls", {{"type", "boolean"}, {"description", "Pick up TP scrolls when tome not full"}}},
                    {"pick_id_scrolls", {{"type", "boolean"}, {"description", "Pick up ID scrolls when tome not full"}}},
                    {"resnap", {{"type", "boolean"}, {"description", "Re-snapshot current belt layout"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_game_state"},
            {"description", "Get current game state: in_game, menu, loading, or unknown. Also reports if paused and frame count."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "get_controls"},
            {"description", "Dump all UI controls on the current screen (buttons, editboxes, etc.) with positions and states. Useful for debugging menu navigation."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "is_panel_open"},
            {"description", "Check if stash/trade/cube panel is open. Returns panel type and state."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
        });

        tools.push_back({
            {"name", "click_control"},
            {"description", "Click a UI control by index (calls OnPress directly). Works at menu screens without foreground focus. Use get_controls to find indices."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"index", {{"type", "integer"}, {"description", "Control index from get_controls"}}}
                }},
                {"required", json::array({"index"})}
            }}
        });

        tools.push_back({
            {"name", "quit_game"},
            {"description", "Fully close Diablo II. If in-game, saves first then terminates the process."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "exit_game"},
            {"description", "Exit the current game to the menu (save and return to character select). Does not close Diablo II."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "enter_game"},
            {"description", "Navigate menus to enter a game. Automates: Main Menu -> Character Select -> Difficulty -> In Game."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"character", {{"type", "string"}, {"description", "Character name to select (default: use currently selected)"}}},
                    {"difficulty", {{"type", "integer"}, {"description", "0=Normal, 1=Nightmare, 2=Hell, -1=highest available (default)"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_nav_status"},
            {"description", "Get the current status of menu navigation (enter_game progress)."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "install_hook"},
            {"description", "Install a Detours hook on a game function to log calls. Captures arguments and return values."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Hex address of function to hook (e.g., '0x6FAB1234')"}}},
                    {"name", {{"type", "string"}, {"description", "Friendly name for this hook"}}},
                    {"capture", {{"type", "string"}, {"description", "Capture level: 'light', 'medium', 'full' (default 'light')"}}},
                    {"arg_count", {{"type", "integer"}, {"description", "Number of stack arguments to capture (for medium/full, default 0)"}}}
                }},
                {"required", json::array({"address"})}
            }}
        });

        tools.push_back({
            {"name", "remove_hook"},
            {"description", "Remove a previously installed hook by address."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Hex address of hook to remove"}}},
                    {"all", {{"type", "boolean"}, {"description", "Remove all hooks (ignores address)"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "list_hooks"},
            {"description", "List all installed function hooks with call counts."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "get_call_log"},
            {"description", "Get recent function call log entries from installed hooks."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"max_entries", {{"type", "integer"}, {"description", "Max entries to return (default 50)"}}},
                    {"address", {{"type", "string"}, {"description", "Filter by hook address (default all)"}}},
                    {"clear", {{"type", "boolean"}, {"description", "Clear the log after reading"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "pause_game"},
            {"description", "Pause the game loop. Game freezes but MCP server stays responsive. Use step_game to advance one frame, resume_game to continue."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "resume_game"},
            {"description", "Resume the game loop after pausing."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "step_game"},
            {"description", "Advance one game frame while paused, then re-pause. Use to step through game logic frame by frame."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "get_crash_log"},
            {"description", "Get captured crash/exception records with registers, stack trace, and module info."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"clear", {{"type", "boolean"}, {"description", "Clear log after reading"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "list_patches"},
            {"description", "List all managed patches with addresses, status, and original/patched bytes."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "apply_patch"},
            {"description", "Apply a named binary patch at an address. Stores original bytes for undo."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}, {"description", "Unique name for this patch"}}},
                    {"address", {{"type", "string"}, {"description", "Hex address (e.g., '0x6FAB1234')"}}},
                    {"bytes", {{"type", "string"}, {"description", "Hex bytes to write (e.g., '90 90 90')"}}}
                }},
                {"required", json::array({"name", "address", "bytes"})}
            }}
        });

        tools.push_back({
            {"name", "toggle_patch"},
            {"description", "Toggle a patch on/off (apply/revert)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}, {"description", "Patch name to toggle"}}}
                }},
                {"required", json::array({"name"})}
            }}
        });

        tools.push_back({
            {"name", "import_patches"},
            {"description", "Import patches from a JSON array. Format: [{\"name\":\"...\", \"address\":\"0x...\", \"bytes\":\"90 90\"}]"},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"patches", {{"type", "string"}, {"description", "JSON array string of patch definitions"}}}
                }},
                {"required", json::array({"patches"})}
            }}
        });

        tools.push_back({
            {"name", "export_patches"},
            {"description", "Export all managed patches as a JSON file that can be used to permanently patch DLL files on disk."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"file", {{"type", "string"}, {"description", "Output file path (default: patches_export.json in game dir)"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "add_watch"},
            {"description", "Add a memory address to the watch list. Monitors for value changes each frame."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}, {"description", "Watch name"}}},
                    {"address", {{"type", "string"}, {"description", "Hex address (e.g., '0x6FAB1234')"}}},
                    {"type", {{"type", "string"}, {"description", "Value type: 'byte', 'word', 'dword', 'float' (default 'dword')"}}}
                }},
                {"required", json::array({"name", "address"})}
            }}
        });

        tools.push_back({
            {"name", "remove_watch"},
            {"description", "Remove a watch by name, or remove all watches."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}, {"description", "Watch name to remove"}}},
                    {"all", {{"type", "boolean"}, {"description", "Remove all watches"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_watches"},
            {"description", "Get all watched addresses with current values, previous values, and change counts."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "list_struct_defs"},
            {"description", "List all known struct definitions (built-in D2 structs + loaded from structs.json)."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "get_struct_def"},
            {"description", "Get a struct definition by name with all field definitions."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}, {"description", "Struct name (e.g., 'UnitAny')"}}}
                }},
                {"required", json::array({"name"})}
            }}
        });

        tools.push_back({
            {"name", "read_struct"},
            {"description", "Read memory at an address as a typed struct. Returns all field values with names and types."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Hex address to read from"}}},
                    {"struct_name", {{"type", "string"}, {"description", "Struct type name (e.g., 'UnitAny')"}}},
                    {"follow_pointers", {{"type", "boolean"}, {"description", "Follow pointer fields one level (default false)"}}}
                }},
                {"required", json::array({"address", "struct_name"})}
            }}
        });

        tools.push_back({
            {"name", "read_region"},
            {"description", "Read raw memory and classify each DWORD as pointer/string/int/float/zero. For discovering unknown struct layouts."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Hex address"}}},
                    {"size", {{"type", "integer"}, {"description", "Bytes to analyze (default 64, max 1024)"}}}
                }},
                {"required", json::array({"address"})}
            }}
        });

        tools.push_back({
            {"name", "save_struct_defs"},
            {"description", "Save all struct definitions to structs.json in the game directory."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "walk_to"},
            {"description", "Walk or run the character to a world coordinate. Use get_player_state for current position."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "integer"}, {"description", "Target world X coordinate"}}},
                    {"y", {{"type", "integer"}, {"description", "Target world Y coordinate"}}},
                    {"run", {{"type", "boolean"}, {"description", "Run instead of walk (default true)"}}}
                }},
                {"required", json::array({"x", "y"})}
            }}
        });

        tools.push_back({
            {"name", "get_waypoints"},
            {"description", "Get list of available waypoint destinations. Must be near a waypoint with panel open."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
        });

        tools.push_back({
            {"name", "interact_entity"},
            {"description", "Reliably interact with an entity (stash, NPC, waypoint). Walks to entity, sends interact, verifies panel opened. Returns when interaction confirmed or timeout."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"unit_id", {{"type", "integer"}, {"description", "Entity unit ID"}}},
                    {"unit_type", {{"type", "integer"}, {"description", "1=NPC/monster, 2=object (default 2)"}}},
                    {"expected_panel", {{"type", "string"}, {"description", "Expected panel: 'stash', 'trade', 'waypoint', 'any' (default 'any')"}}},
                    {"timeout_ms", {{"type", "integer"}, {"description", "Max wait in ms (default 10000)"}}}
                }},
                {"required", json::array({"unit_id"})}
            }}
        });

        tools.push_back({
            {"name", "wait_until"},
            {"description", "Wait for a condition to be met. Polls every 200ms until condition is true or timeout."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"condition", {{"type", "string"}, {"description", "Condition: 'panel_open', 'panel_closed', 'area_changed', 'position_reached', 'in_game', 'cursor_empty'"}}},
                    {"timeout_ms", {{"type", "integer"}, {"description", "Max wait in ms (default 10000)"}}},
                    {"area_id", {{"type", "integer"}, {"description", "For area_changed: target area ID"}}},
                    {"x", {{"type", "integer"}, {"description", "For position_reached: target X"}}},
                    {"y", {{"type", "integer"}, {"description", "For position_reached: target Y"}}},
                    {"distance", {{"type", "integer"}, {"description", "For position_reached: max distance (default 5)"}}}
                }},
                {"required", json::array({"condition"})}
            }}
        });

        tools.push_back({
            {"name", "cast_skill"},
            {"description", "Cast the currently selected skill at a location or on a unit. Use right-click skill by default."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "integer"}, {"description", "Target X coordinate (for location cast)"}}},
                    {"y", {{"type", "integer"}, {"description", "Target Y coordinate (for location cast)"}}},
                    {"unit_id", {{"type", "integer"}, {"description", "Target unit ID (for unit cast, overrides x/y)"}}},
                    {"unit_type", {{"type", "integer"}, {"description", "Target unit type (default 1=monster)"}}},
                    {"left", {{"type", "boolean"}, {"description", "Use left skill instead of right (default false)"}}}
                }}
            }}
        });

        tools.push_back({
            {"name", "use_waypoint"},
            {"description", "Use a waypoint to travel. Walk near waypoint first. destination = D2 area ID (1=Rogue Encampment, 2=Cold Plains, 40=Lut Gholein, 75=Kurast Docks, 109=Harrogath)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"waypoint_id", {{"type", "integer"}, {"description", "Waypoint unit ID (from get_nearby_objects)"}}},
                    {"destination", {{"type", "integer"}, {"description", "Destination waypoint index (0-38)"}}}
                }},
                {"required", json::array({"waypoint_id", "destination"})}
            }}
        });

        tools.push_back({
            {"name", "sell_item"},
            {"description", "Sell an item to the currently open NPC trade window. NPC trade must be open (use interact_object on NPC first)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID to sell"}}},
                    {"npc_id", {{"type", "integer"}, {"description", "NPC unit ID (the vendor)"}}}
                }},
                {"required", json::array({"item_id", "npc_id"})}
            }}
        });

        tools.push_back({
            {"name", "interact_object"},
            {"description", "Interact with a game object (stash, waypoint, NPC, shrine, etc). Auto-walks to and interacts. Use get_nearby_objects to find objects first."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"unit_id", {{"type", "integer"}, {"description", "Object unit ID"}}},
                    {"unit_type", {{"type", "integer"}, {"description", "Unit type (default 2 for objects, 1 for NPCs)"}}}
                }},
                {"required", json::array({"unit_id"})}
            }}
        });

        tools.push_back({
            {"name", "switch_stash_tab"},
            {"description", "Switch to a stash tab. 0=Personal, 1-9=Shared I-IX, 10=Materials. Stash must be open."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"tab", {{"type", "integer"}, {"description", "Tab number: 0=Personal, 1-9=Shared"}}}
                }},
                {"required", json::array({"tab"})}
            }}
        });

        tools.push_back({
            {"name", "open_stash"},
            {"description", "Find the stash chest in town and open it. Auto-walks to stash and interacts."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "open_cube"},
            {"description", "Open the Horadric Cube panel (must have cube in inventory)."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "close_panels"},
            {"description", "Close all open UI panels (stash, cube, trade, etc)."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "click_screen"},
            {"description", "Simulate a mouse click at screen coordinates on the game window. Useful for clicking UI buttons that aren't standard D2WIN controls (like PD2 stash tabs)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "integer"}, {"description", "Screen X coordinate"}}},
                    {"y", {{"type", "integer"}, {"description", "Screen Y coordinate"}}}
                }},
                {"required", json::array({"x", "y"})}
            }}
        });

        tools.push_back({
            {"name", "get_stash_grid"},
            {"description", "Get the stash occupancy grid showing which cells are free. Each cell shows the unit_id of the item occupying it, or 0 for empty."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"container", {{"type", "string"}, {"description", "'inventory' (10x4), 'stash' (10x16), or 'cube' (3x4). Default 'inventory'."}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "get_cursor_item"},
            {"description", "Check if an item is currently on the cursor."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
        });

        tools.push_back({
            {"name", "get_nearby_objects"},
            {"description", "List nearby game objects (stash, waypoint, shrines, chests, portals) with unit IDs and positions."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"max_distance", {{"type", "integer"}, {"description", "Max distance (default 50)"}}}
                }},
                {"required", json::array()}
            }}
        });

        tools.push_back({
            {"name", "use_item"},
            {"description", "Use an item by unit ID (drink potion, read scroll, etc). Works for belt, inventory, stash, cube items."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID (from get_inventory or get_belt_contents)"}}}
                }},
                {"required", json::array({"item_id"})}
            }}
        });

        tools.push_back({
            {"name", "drop_item"},
            {"description", "Drop an item from cursor to the ground."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID to drop"}}}
                }},
                {"required", json::array({"item_id"})}
            }}
        });

        tools.push_back({
            {"name", "pickup_item"},
            {"description", "Pick up a ground item by unit ID."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Ground item unit ID"}}}
                }},
                {"required", json::array({"item_id"})}
            }}
        });

        tools.push_back({
            {"name", "item_to_cursor"},
            {"description", "Pick up an item to cursor from inventory/belt/body."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID to pick up to cursor"}}}
                }},
                {"required", json::array({"item_id"})}
            }}
        });

        tools.push_back({
            {"name", "cursor_to_container"},
            {"description", "Place cursor item into a container (inventory, stash, cube) at a specific position."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID on cursor"}}},
                    {"x", {{"type", "integer"}, {"description", "Grid X position in container"}}},
                    {"y", {{"type", "integer"}, {"description", "Grid Y position in container"}}},
                    {"container", {{"type", "string"}, {"description", "'inventory' (default), 'stash', 'cube', 'trade'"}}}
                }},
                {"required", json::array({"item_id"})}
            }}
        });

        tools.push_back({
            {"name", "move_item"},
            {"description", "Move an item from its current location to a target container+position. Handles: pick to cursor, optional tab switch, place at target, verify. Returns cursor state after move (for swap detection)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID to move"}}},
                    {"dest_container", {{"type", "string"}, {"description", "'inventory', 'stash', 'cube'. Default: 'stash'"}}},
                    {"dest_x", {{"type", "integer"}, {"description", "Target grid X"}}},
                    {"dest_y", {{"type", "integer"}, {"description", "Target grid Y"}}},
                    {"dest_tab", {{"type", "integer"}, {"description", "Target stash tab (0-10). Only needed for cross-tab moves. -1=same tab (default)"}}},
                    {"pick_wait_ms", {{"type", "integer"}, {"description", "Wait after pick (ms). Default: 200"}}},
                    {"place_wait_ms", {{"type", "integer"}, {"description", "Wait after place (ms). Default: 200"}}}
                }},
                {"required", json::array({"item_id", "dest_x", "dest_y"})}
            }}
        });

        tools.push_back({
            {"name", "get_skills"},
            {"description", "Get all skill allocations for the player. Returns skill IDs, names, base levels, and total levels. Useful for detecting build type (e.g., Lightning Sorc, Hammerdin)."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
        });

        tools.push_back({
            {"name", "get_item_stats"},
            {"description", "Read all stats/affixes on an item by unit ID. Returns quality, level, sockets, ethereal, all stat values with names. Works for any item in inventory, stash, belt, or on cursor."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"item_id", {{"type", "integer"}, {"description", "Item unit ID"}}}
                }},
                {"required", json::array({"item_id"})}
            }}
        });

        tools.push_back({
            {"name", "resolve_function"},
            {"description", "Resolve a DLL function address by ordinal or name. Use for D2COMMON/D2CLIENT functions."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"dll", {{"type", "string"}, {"description", "DLL name (e.g., 'D2COMMON.dll', 'D2CLIENT.dll')"}}},
                    {"ordinal", {{"type", "integer"}, {"description", "Ordinal number (positive). For D2 functions, use absolute value of negative ordinal."}}},
                    {"name", {{"type", "string"}, {"description", "Function name (alternative to ordinal)"}}}
                }},
                {"required", json::array({"dll"})}
            }}
        });

        tools.push_back({
            {"name", "call_function"},
            {"description", "Call a game function by address with up to 8 arguments. Returns EAX. Supports __stdcall (default), __cdecl, and __fastcall conventions."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Hex address of function to call"}}},
                    {"args", {{"type", "array"}, {"description", "Array of DWORD arguments (hex strings or integers)"}}},
                    {"convention", {{"type", "string"}, {"description", "'stdcall' (default), 'cdecl', or 'fastcall'"}}}
                }},
                {"required", json::array({"address"})}
            }}
        });

        tools.push_back({
            {"name", "read_memory"},
            {"description", "Read bytes from a memory address in the game process. Returns hex dump and interpreted values."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {
                        {"type", "string"},
                        {"description", "Hex address to read from (e.g., '0x6FAB0000')"}
                    }},
                    {"size", {
                        {"type", "integer"},
                        {"description", "Number of bytes to read (default 64, max 4096)"}
                    }},
                    {"format", {
                        {"type", "string"},
                        {"description", "Output format: 'hex' (default), 'dwords', 'ascii', 'all'"}
                    }}
                }},
                {"required", json::array({"address"})}
            }}
        });

        tools.push_back({
            {"name", "write_memory"},
            {"description", "Write bytes to a memory address in the game process. Use with caution."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {
                        {"type", "string"},
                        {"description", "Hex address to write to (e.g., '0x6FAB0000')"}
                    }},
                    {"bytes", {
                        {"type", "string"},
                        {"description", "Hex string of bytes to write (e.g., '90 90 90' or '909090')"}
                    }},
                    {"dword", {
                        {"type", "integer"},
                        {"description", "Write a 32-bit DWORD value instead of raw bytes"}
                    }}
                }},
                {"required", json::array({"address"})}
            }}
        });

        return tools;
    }

    // Handle tool calls
    json HandleToolCall(const std::string& name, const json& arguments) {
        if (name == "ping") {
            return {
                {"content", {{
                    {"type", "text"},
                    {"text", "pong - d2-mod-toolkit is running inside Diablo II"}
                }}}
            };
        }

        if (name == "get_game_info") {
            DWORD pid = GetCurrentProcessId();
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);

            json info = {
                {"process_id", pid},
                {"executable", exePath},
                {"mcp_port", g_port},
                {"request_count", g_requestCount.load()},
                {"game_ready", GameState::IsGameReady()},
                {"status", "running"}
            };

            return {
                {"content", {{
                    {"type", "text"},
                    {"text", info.dump(2)}
                }}}
            };
        }

        if (name == "get_player_state") {
            if (!GameState::IsGameReady()) {
                return {
                    {"content", {{
                        {"type", "text"},
                        {"text", "Not in game"}
                    }}},
                    {"isError", true}
                };
            }

            auto ps = GameState::GetPlayerState();
            const char* classNames[] = {"Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin"};
            const char* className = (ps.classId >= 0 && ps.classId <= 6) ? classNames[ps.classId] : "Unknown";
            const char* diffNames[] = {"Normal", "Nightmare", "Hell"};
            const char* diff = (ps.difficulty >= 0 && ps.difficulty <= 2) ? diffNames[ps.difficulty] : "Unknown";

            // Apply mastery to elemental damage for display
            auto applyMastery = [](int val, int mastery) { return val + (val * mastery / 100); };
            int pLen = ps.poisonLenOverride > 0 ? ps.poisonLenOverride : ps.poisonLength;

            // Build breakpoint arrays
            auto bpToJson = [](const GameState::PlayerState::BreakpointInfo& bp) {
                json j = {
                    {"label", bp.label},
                    {"current_value", bp.currentValue},
                    {"active_index", bp.activeIndex}
                };
                json vals = json::array();
                for (int i = 0; i < bp.count; i++) vals.push_back(bp.values[i]);
                j["thresholds"] = vals;
                return j;
            };

            json info = {
                {"name", ps.name},
                {"class", className},
                {"class_id", ps.classId},
                {"level", ps.level},
                {"difficulty", diff},
                {"area", ps.area},
                {"area_name", ps.areaName},
                {"act", ps.act + 1},
                {"position", {{"x", ps.x}, {"y", ps.y}}},
                {"players", ps.playerCount},
                {"xp_pct_to_next", ps.xpPctToNext},
                {"xp_current", ps.currentXp},
                {"xp_last_level", ps.lastLevelXp},
                {"xp_next_level", ps.nextLevelXp},
                {"additional_xp", ps.addXp},
                {"hp", ps.hp >> 8},
                {"max_hp", ps.maxHp >> 8},
                {"mana", ps.mana >> 8},
                {"max_mana", ps.maxMana >> 8},
                {"stamina", ps.stamina >> 8},
                {"max_stamina", ps.maxStamina >> 8},
                {"gold", ps.gold},
                {"gold_stash", ps.goldStash},
                {"resistances", {
                    {"fire", {{"value", ps.fireRes + ps.resPenalty}, {"max", ps.maxFireRes}}},
                    {"cold", {{"value", ps.coldRes + ps.resPenalty}, {"max", ps.maxColdRes}}},
                    {"lightning", {{"value", ps.lightRes + ps.resPenalty}, {"max", ps.maxLightRes}}},
                    {"poison", {{"value", ps.poisonRes + ps.resPenalty}, {"max", ps.maxPoisonRes}}},
                    {"curse", {{"value", ps.curseRes < 75 ? ps.curseRes : 75}, {"max", 75}}},
                    {"penalty", ps.resPenalty}
                }},
                {"absorption", {
                    {"fire", {{"flat", ps.fireAbsorb}, {"pct", ps.fireAbsorbPct}}},
                    {"cold", {{"flat", ps.coldAbsorb}, {"pct", ps.coldAbsorbPct}}},
                    {"lightning", {{"flat", ps.lightAbsorb}, {"pct", ps.lightAbsorbPct}}},
                    {"magic", {{"flat", ps.magicAbsorb}, {"pct", ps.magicAbsorbPct}}}
                }},
                {"damage_reduction", {
                    {"physical", {{"flat", ps.dmgReduction}, {"pct", ps.dmgReductionPct}}},
                    {"magic", {{"flat", ps.magDmgReduction}, {"pct", ps.magDmgReductionPct}}}
                }},
                {"attacker_takes_damage", {{"physical", ps.attackerTakesDmg}, {"lightning", ps.attackerTakesLtng}}},
                {"elemental_mastery", {
                    {"fire", ps.fireMastery}, {"cold", ps.coldMastery},
                    {"lightning", ps.lightMastery}, {"poison", ps.poisonMastery}, {"magic", ps.magicMastery}
                }},
                {"elemental_pierce", {
                    {"fire", ps.firePierce}, {"cold", ps.coldPierce},
                    {"lightning", ps.lightPierce}, {"poison", ps.poisonPierce}, {"magic", ps.magicPierce}
                }},
                {"attack", {
                    {"dex_ar", ps.dexterity * 5}, {"equip_ar", ps.attackRating},
                    {"total_ar", ps.dexterity * 5 + ps.attackRating},
                    {"dex_def", ps.dexterity / 4}, {"equip_def", ps.defense},
                    {"total_def", ps.dexterity / 4 + ps.defense},
                    {"min_dmg_1h", ps.minDmg}, {"max_dmg_1h", ps.maxDmg},
                    {"min_dmg_2h", ps.minDmg2}, {"max_dmg_2h", ps.maxDmg2}
                }},
                {"rates", {
                    {"fcr", ps.fcr}, {"fhr", ps.fhr}, {"fbr", ps.fbr},
                    {"ias", ps.ias}, {"frw", ps.frw}, {"attack_rate", ps.attackRate}
                }},
                {"breakpoints", {
                    {"skill_name", ps.bpSkillName},
                    {"fcr", bpToJson(ps.bpFCR)},
                    {"fhr", bpToJson(ps.bpFHR)}
                }},
                {"combat", {
                    {"crushing_blow", ps.crushingBlow},
                    {"open_wounds", ps.openWounds}, {"deep_wounds", ps.deepWounds},
                    {"deadly_strike", ps.deadlyStrike}, {"max_deadly_strike", 75 + ps.maxDeadlyStrike},
                    {"critical_strike", ps.criticalStrike < 75 ? ps.criticalStrike : 75},
                    {"life_leech", ps.lifeLeech}, {"mana_leech", ps.manaLeech},
                    {"projectile_pierce", ps.piercingAttack + ps.pierce},
                    {"life_per_kill", ps.lifePerKill}, {"mana_per_kill", ps.manaPerKill}
                }},
                {"elemental_damage", {
                    {"added", ps.addedDamage},
                    {"fire", {{"min", applyMastery(ps.minFireDmg, ps.fireMastery)}, {"max", applyMastery(ps.maxFireDmg, ps.fireMastery)}}},
                    {"cold", {{"min", applyMastery(ps.minColdDmg, ps.coldMastery)}, {"max", applyMastery(ps.maxColdDmg, ps.coldMastery)}}},
                    {"lightning", {{"min", applyMastery(ps.minLightDmg, ps.lightMastery)}, {"max", applyMastery(ps.maxLightDmg, ps.lightMastery)}}},
                    {"poison", {{"min", (int)(applyMastery(ps.minPoisonDmg, ps.poisonMastery) / 256.0 * pLen)},
                               {"max", (int)(applyMastery(ps.maxPoisonDmg, ps.poisonMastery) / 256.0 * pLen)},
                               {"duration", pLen / 25.0}}},
                    {"magic", {{"min", applyMastery(ps.minMagicDmg, ps.magicMastery)}, {"max", applyMastery(ps.maxMagicDmg, ps.magicMastery)}}}
                }},
                {"find", {{"magic_find", ps.mf}, {"gold_find", ps.gf}}}
            };

            return {
                {"content", {{
                    {"type", "text"},
                    {"text", info.dump(2)}
                }}}
            };
        }

        if (name == "get_belt_contents") {
            if (!GameState::IsGameReady()) {
                return {
                    {"content", {{
                        {"type", "text"},
                        {"text", "Not in game"}
                    }}},
                    {"isError", true}
                };
            }

            auto belt = GameState::GetBeltState();
            json slots = json::array();
            for (int i = 0; i < belt.columns * belt.rows; ++i) {
                const auto& s = belt.slots[i];
                if (s.occupied) {
                    slots.push_back({
                        {"slot", i},
                        {"column", i % belt.columns},
                        {"row", i / belt.columns},
                        {"name", s.name},
                        {"full_name", s.fullName},
                        {"item_code", s.itemCode}
                    });
                } else {
                    slots.push_back({
                        {"slot", i},
                        {"column", i % belt.columns},
                        {"row", i / belt.columns},
                        {"empty", true}
                    });
                }
            }

            json info = {
                {"columns", belt.columns},
                {"rows", belt.rows},
                {"slots", slots}
            };

            return {
                {"content", {{
                    {"type", "text"},
                    {"text", info.dump(2)}
                }}}
            };
        }

        if (name == "get_nearby_units") {
            if (!GameState::IsGameReady()) {
                return {
                    {"content", {{
                        {"type", "text"},
                        {"text", "Not in game"}
                    }}},
                    {"isError", true}
                };
            }

            int maxDist = arguments.value("max_distance", 40);
            auto units = GameState::GetNearbyUnits(maxDist);

            json unitList = json::array();
            const char* typeNames[] = {"player", "monster", "object", "missile", "item", "tile"};

            for (const auto& u : units) {
                json uj = {
                    {"type", (u.type >= 0 && u.type <= 5) ? typeNames[u.type] : "unknown"},
                    {"class_id", u.classId},
                    {"unit_id", u.unitId},
                    {"position", {{"x", u.x}, {"y", u.y}}},
                    {"distance", u.distance},
                    {"name", u.name}
                };

                if (u.type == 1) { // Monster
                    uj["hp"] = u.maxHp > 0 ? u.hp >> 8 : 0;
                    uj["max_hp"] = u.maxHp >> 8;
                    uj["is_boss"] = u.isBoss;
                    uj["is_champion"] = u.isChampion;
                    uj["is_minion"] = u.isMinion;
                    uj["dead"] = (u.mode == 0 || u.mode == 12);

                    // Immunities (resist >= 100)
                    json immunities = json::array();
                    if (u.fireRes >= 100) immunities.push_back("fire");
                    if (u.coldRes >= 100) immunities.push_back("cold");
                    if (u.lightRes >= 100) immunities.push_back("lightning");
                    if (u.poisonRes >= 100) immunities.push_back("poison");
                    if (u.physRes >= 100) immunities.push_back("physical");
                    if (u.magicRes >= 100) immunities.push_back("magic");
                    if (!immunities.empty()) uj["immunities"] = immunities;

                    uj["resistances"] = {
                        {"fire", u.fireRes}, {"cold", u.coldRes},
                        {"lightning", u.lightRes}, {"poison", u.poisonRes},
                        {"physical", u.physRes}, {"magic", u.magicRes}
                    };
                }

                if (u.type == 0) { // Player
                    uj["hp"] = u.hp >> 8;
                    uj["max_hp"] = u.maxHp >> 8;
                }

                unitList.push_back(uj);
            }

            json info = {
                {"count", unitList.size()},
                {"max_distance", maxDist},
                {"units", unitList}
            };

            return {
                {"content", {{
                    {"type", "text"},
                    {"text", info.dump(2)}
                }}}
            };
        }

        if (name == "get_inventory") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }

            std::string locFilter = arguments.value("location", "all");
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pInventory) {
                return {{"content", {{{"type", "text"}, {"text", "No inventory"}}}}, {"isError", true}};
            }

            const char* nodePageNames[] = {"ground", "storage", "belt", "equipped"};
            json items = json::array();
            UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
            while (pItem) {
                if (pItem->pItemData) {
                    int np = pItem->pItemData->NodePage;
                    const char* loc = (np >= 0 && np <= 3) ? nodePageNames[np] : "unknown";

                    // Filter
                    bool include = (locFilter == "all");
                    if (locFilter == "belt" && np == NODEPAGE_BELTSLOTS) include = true;
                    if (locFilter == "inventory" && np == NODEPAGE_STORAGE) include = true;
                    if (locFilter == "equipped" && np == NODEPAGE_EQUIP) include = true;

                    if (include) {
                        char name[64] = {};
                        SafeGetUnitName(pItem, name, sizeof(name));

                        int qty = D2COMMON_GetUnitStat(pItem, STAT_AMMOQUANTITY, 0);

                        // Get grid position from ItemPath
                        int gridX = 0, gridY = 0;
                        if (pItem->pPath) {
                            ItemPath* ip = (ItemPath*)pItem->pPath;
                            gridX = (int)ip->dwPosX;
                            gridY = (int)ip->dwPosY;
                        }

                        // Get item size (width/height) from item data
                        // Items occupy WxH cells in the grid
                        int itemW = 1, itemH = 1;
                        // We can approximate from the item type
                        // Armor/weapons are typically 2x3 or 2x4, charms 1x1 to 1x3
                        // For now include raw grid pos

                        // Get ItemLocation for storage type distinction
                        int itemLoc = pItem->pItemData->ItemLocation;
                        const char* storageNames[] = {"inventory", "?", "belt", "cube", "stash"};
                        const char* storageName = (itemLoc >= 0 && itemLoc <= 4) ? storageNames[itemLoc] : "?";

                        json item = {
                            {"code", (int)pItem->dwTxtFileNo},
                            {"name", name[0] ? name : "?"},
                            {"location", loc},
                            {"storage", storageName},
                            {"node_page", np},
                            {"item_location", itemLoc},
                            {"unit_id", (int)pItem->dwUnitId},
                            {"grid_x", gridX},
                            {"grid_y", gridY}
                        };
                        if (qty > 0) item["quantity"] = qty;
                        items.push_back(item);
                    }
                }
                pItem = D2COMMON_GetNextItemFromInventory(pItem);
            }

            json info = {{"count", items.size()}, {"items", items}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_auto_potion") {
            auto cfg = AutoPotion::GetConfig();
            json info = {
                {"enabled", cfg.enabled},
                {"hp_threshold", cfg.hpThreshold},
                {"mp_threshold", cfg.mpThreshold},
                {"rejuv_threshold", cfg.rejuvThreshold},
                {"cooldown_ms", cfg.cooldownMs},
                {"skip_in_town", cfg.skipInTown}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "set_auto_potion") {
            auto cfg = AutoPotion::GetConfig();
            if (arguments.contains("enabled")) cfg.enabled = arguments["enabled"].get<bool>();
            if (arguments.contains("hp_threshold")) cfg.hpThreshold = arguments["hp_threshold"].get<int>();
            if (arguments.contains("mp_threshold")) cfg.mpThreshold = arguments["mp_threshold"].get<int>();
            if (arguments.contains("rejuv_threshold")) cfg.rejuvThreshold = arguments["rejuv_threshold"].get<int>();
            if (arguments.contains("cooldown_ms")) cfg.cooldownMs = arguments["cooldown_ms"].get<int>();
            if (arguments.contains("skip_in_town")) cfg.skipInTown = arguments["skip_in_town"].get<bool>();
            AutoPotion::SetConfig(cfg);

            json info = {
                {"status", "updated"},
                {"enabled", cfg.enabled},
                {"hp_threshold", cfg.hpThreshold},
                {"mp_threshold", cfg.mpThreshold},
                {"rejuv_threshold", cfg.rejuvThreshold},
                {"cooldown_ms", cfg.cooldownMs},
                {"skip_in_town", cfg.skipInTown}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_auto_pickup") {
            auto cfg = AutoPickup::GetConfig();
            auto snap = AutoPickup::GetSnapshot();
            json snapJson = json::array();
            for (int i = 0; i < 4; i++) snapJson.push_back(snap.preferredCode[i]);

            json info = {
                {"enabled", cfg.enabled},
                {"max_distance", cfg.maxDistance},
                {"cooldown_ms", cfg.cooldownMs},
                {"pick_tp_scrolls", cfg.pickTpScrolls},
                {"pick_id_scrolls", cfg.pickIdScrolls},
                {"belt_snapshot", snapJson},
                {"snapshot_valid", snap.valid}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "set_auto_pickup") {
            auto cfg = AutoPickup::GetConfig();
            if (arguments.contains("enabled")) cfg.enabled = arguments["enabled"].get<bool>();
            if (arguments.contains("max_distance")) cfg.maxDistance = arguments["max_distance"].get<int>();
            if (arguments.contains("cooldown_ms")) cfg.cooldownMs = arguments["cooldown_ms"].get<int>();
            if (arguments.contains("pick_tp_scrolls")) cfg.pickTpScrolls = arguments["pick_tp_scrolls"].get<bool>();
            if (arguments.contains("pick_id_scrolls")) cfg.pickIdScrolls = arguments["pick_id_scrolls"].get<bool>();
            AutoPickup::SetConfig(cfg);

            if (arguments.contains("resnap") && arguments["resnap"].get<bool>()) {
                AutoPickup::ResnapBelt();
            }

            auto snap = AutoPickup::GetSnapshot();
            json snapJson = json::array();
            for (int i = 0; i < 4; i++) snapJson.push_back(snap.preferredCode[i]);

            json info = {
                {"status", "updated"},
                {"enabled", cfg.enabled},
                {"max_distance", cfg.maxDistance},
                {"pick_tp_scrolls", cfg.pickTpScrolls},
                {"pick_id_scrolls", cfg.pickIdScrolls},
                {"belt_snapshot", snapJson}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "quit_game") {
            // Queue quit: if in-game, saves first, then terminates after 2 seconds
            GameNav::RequestQuitGame();
            return {{"content", {{{"type", "text"}, {"text", "Quit queued — saving and closing Diablo II"}}}}};
        }

        if (name == "exit_game") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}};
            }
            // Queue exit to be executed on the game thread (calling D2CLIENT_ExitGame
            // from the HTTP thread can crash)
            GameNav::RequestExitGame();
            return {{"content", {{{"type", "text"}, {"text", "Exit game queued — will save and return to menu"}}}}};
        }

        if (name == "get_game_state") {
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            Control* pCtrl = *p_D2WIN_FirstControl;

            std::string state;
            if (pPlayer && pPlayer->pPath) {
                state = "in_game";
            } else if (pCtrl) {
                state = "menu";
            } else if (pPlayer && !pPlayer->pPath) {
                state = "loading";
            } else {
                state = "unknown";
            }

            json info = {
                {"state", state},
                {"paused", GamePause::IsPaused()},
                {"frame", GamePause::GetFrameCount()},
                {"mcp_requests", g_requestCount.load()}
            };

            if (state == "in_game" && GameState::IsGameReady()) {
                auto ps = GameState::GetPlayerState();
                info["area"] = ps.area;
                info["area_name"] = ps.areaName;
                info["difficulty"] = ps.difficulty;
            }

            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_controls") {
            Control* pCtrl = *p_D2WIN_FirstControl;
            json controls = json::array();
            int idx = 0;
            while (pCtrl) {
                const char* typeNames[] = {"unknown", "editbox", "image", "unknown3", "textbox", "scrollbar", "button", "list"};
                const char* typeName = (pCtrl->dwType <= 7) ? typeNames[pCtrl->dwType] : "unknown";
                json ctrl = {
                    {"index", idx},
                    {"type", typeName},
                    {"type_id", (int)pCtrl->dwType},
                    {"state", (int)pCtrl->dwState},
                    {"x", (int)pCtrl->dwPosX},
                    {"y", (int)pCtrl->dwPosY},
                    {"w", (int)pCtrl->dwSizeX},
                    {"h", (int)pCtrl->dwSizeY},
                    {"has_on_press", pCtrl->OnPress != nullptr},
                    {"address", (int)(DWORD)pCtrl}
                };
                if (pCtrl->OnPress) {
                    char buf[16]; snprintf(buf, sizeof(buf), "0x%08X", (DWORD)pCtrl->OnPress);
                    ctrl["on_press_addr"] = buf;
                }

                // Read text content for TextBox controls (type 4)
                if (pCtrl->dwType == 4) {
                    struct TextReader {
                        char lines[20][256];
                        int count;
                        static void Read(Control* p, TextReader& out) {
                            out.count = 0;
                            __try {
                                TextBox* tb = (TextBox*)p;
                                ControlText* ct = tb->pFirstText;
                                while (ct && out.count < 20) {
                                    for (int f = 0; f < 5; f++) {
                                        if (ct->wText[f] && out.count < 20) {
                                            WideCharToMultiByte(CP_UTF8, 0, ct->wText[f], -1,
                                                out.lines[out.count], 255, nullptr, nullptr);
                                            if (out.lines[out.count][0]) out.count++;
                                        }
                                    }
                                    ct = ct->pNext;
                                }
                            } __except(EXCEPTION_EXECUTE_HANDLER) {}
                        }
                    };
                    TextReader tr;
                    TextReader::Read(pCtrl, tr);
                    if (tr.count > 0) {
                        json textLines = json::array();
                        for (int t = 0; t < tr.count; t++) textLines.push_back(tr.lines[t]);
                        ctrl["text"] = textLines;
                    }
                }

                controls.push_back(ctrl);
                pCtrl = pCtrl->pNext;
                idx++;
            }
            json info = {{"count", controls.size()}, {"controls", controls}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        // Click a control by index (calls OnPress directly — works at menu, no foreground needed)
        if (name == "click_control") {
            int index = arguments.value("index", -1);
            if (index < 0) {
                return {{"content", {{{"type", "text"}, {"text", "index required"}}}}, {"isError", true}};
            }
            Control* pCtrl = *p_D2WIN_FirstControl;
            int i = 0;
            while (pCtrl && i < index) {
                pCtrl = pCtrl->pNext;
                i++;
            }
            if (!pCtrl) {
                return {{"content", {{{"type", "text"}, {"text", "Control not found at index"}}}}, {"isError", true}};
            }
            if (!pCtrl->OnPress) {
                return {{"content", {{{"type", "text"}, {"text", "Control has no OnPress callback"}}}}, {"isError", true}};
            }
            // Try OnPress first (works for most buttons), mouse flow as fallback
            bool useMouseFlow = arguments.value("mouse_flow", false);

            auto safeClick = [](Control* c, bool mouseFlow) -> int {
                __try {
                    if (mouseFlow && c->pfHandleMouseDown && c->pfHandleMouseUp) {
                        ControlMsg msg = {};
                        msg.pControl = c;
                        msg.uMsg = 0x0201;
                        msg.wParam = 0;
                        c->pfHandleMouseDown(&msg);
                        Sleep(50);
                        msg.uMsg = 0x0202;
                        c->pfHandleMouseUp(&msg);
                        return 1;
                    }
                    if (c->OnPress) {
                        c->OnPress(c);
                        return 2;
                    }
                    return 0;
                } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
            };
            int clickResult = safeClick(pCtrl, useMouseFlow);
            if (clickResult < 0) {
                return {{"content", {{{"type", "text"}, {"text", "Click crashed"}}}}, {"isError", true}};
            }
            json info = {{"status", "clicked"}, {"index", index},
                         {"type", (int)pCtrl->dwType},
                         {"x", (int)pCtrl->dwPosX}, {"y", (int)pCtrl->dwPosY}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "enter_game") {
            std::string charName = arguments.value("character", "");
            int difficulty = arguments.value("difficulty", -1);
            GameNav::EnterGame(charName, difficulty);
            json info = {
                {"status", "navigation_started"},
                {"character", charName.empty() ? "default" : charName},
                {"difficulty", difficulty == -1 ? "highest" : std::to_string(difficulty)}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_nav_status") {
            auto status = GameNav::GetStatus();
            const char* stateNames[] = {"idle", "in_progress", "success", "failed"};
            json info = {
                {"state", stateNames[status.state]},
                {"screen", status.currentScreen},
                {"message", status.message},
                {"step", status.step}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "install_hook") {
            std::string addrStr = arguments.value("address", "");
            if (addrStr.empty()) {
                return {{"content", {{{"type", "text"}, {"text", "address is required"}}}}, {"isError", true}};
            }

            HookManager::HookConfig cfg;
            cfg.address = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            cfg.name = arguments.value("name", addrStr);

            std::string capStr = arguments.value("capture", "light");
            if (capStr == "medium") cfg.capture = HookManager::CAPTURE_MEDIUM;
            else if (capStr == "full") cfg.capture = HookManager::CAPTURE_FULL;
            else cfg.capture = HookManager::CAPTURE_LIGHT;

            cfg.argCount = arguments.value("arg_count", 0);
            if (cfg.argCount > 8) cfg.argCount = 8;

            bool ok = HookManager::InstallHook(cfg);
            if (!ok) {
                return {{"content", {{{"type", "text"}, {"text", "Failed to install hook at " + addrStr + " (already hooked or no free slots)"}}}}, {"isError", true}};
            }

            json info = {
                {"status", "installed"},
                {"address", addrStr},
                {"name", cfg.name},
                {"capture", capStr},
                {"arg_count", cfg.argCount}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "remove_hook") {
            if (arguments.value("all", false)) {
                HookManager::RemoveAllHooks();
                return {{"content", {{{"type", "text"}, {"text", "All hooks removed"}}}}};
            }

            std::string addrStr = arguments.value("address", "");
            if (addrStr.empty()) {
                return {{"content", {{{"type", "text"}, {"text", "address or all=true required"}}}}, {"isError", true}};
            }

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            bool ok = HookManager::RemoveHook(addr);
            if (!ok) {
                return {{"content", {{{"type", "text"}, {"text", "No hook at " + addrStr}}}}, {"isError", true}};
            }

            return {{"content", {{{"type", "text"}, {"text", "Hook removed at " + addrStr}}}}};
        }

        if (name == "list_hooks") {
            auto hooks = HookManager::ListHooks();
            json list = json::array();
            for (auto& h : hooks) {
                char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", h.config.address);
                const char* capNames[] = {"light", "medium", "full"};
                list.push_back({
                    {"address", addrBuf},
                    {"name", h.config.name},
                    {"capture", capNames[h.config.capture]},
                    {"arg_count", h.config.argCount},
                    {"installed", h.installed},
                    {"call_count", h.callCount}
                });
            }
            json info = {{"count", list.size()}, {"hooks", list}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_call_log") {
            int maxEntries = arguments.value("max_entries", 50);
            DWORD filterAddr = 0;
            if (arguments.contains("address")) {
                filterAddr = (DWORD)strtoul(arguments["address"].get<std::string>().c_str(), nullptr, 16);
            }

            auto records = HookManager::GetCallLog(maxEntries, filterAddr);

            if (arguments.value("clear", false)) {
                HookManager::ClearCallLog();
            }

            json entries = json::array();
            for (auto& r : records) {
                char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", r.address);
                json entry = {
                    {"address", addrBuf},
                    {"timestamp", r.timestamp},
                    {"thread_id", r.threadId},
                    {"call_count", r.argCount}
                };
                if (r.argCount > 0) {
                    json args = json::array();
                    for (int i = 0; i < r.argCount; i++) {
                        char argBuf[16]; snprintf(argBuf, sizeof(argBuf), "0x%08X", r.args[i]);
                        args.push_back(argBuf);
                    }
                    entry["args"] = args;
                }
                if (r.hasReturnValue) {
                    char retBuf[16]; snprintf(retBuf, sizeof(retBuf), "0x%08X", r.returnValue);
                    entry["return_value"] = retBuf;
                }
                entries.push_back(entry);
            }

            json info = {
                {"count", entries.size()},
                {"total_logged", HookManager::GetCallLogSize()},
                {"entries", entries}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "pause_game") {
            GamePause::Pause();
            json info = {{"paused", true}, {"frame", GamePause::GetFrameCount()}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "resume_game") {
            GamePause::Resume();
            json info = {{"paused", false}, {"frame", GamePause::GetFrameCount()}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "step_game") {
            if (!GamePause::IsPaused()) {
                GamePause::Pause();
                Sleep(50); // let the game loop reach the pause point
            }
            int frameBefore = GamePause::GetFrameCount();
            GamePause::Step();
            Sleep(50); // let the frame execute
            json info = {
                {"paused", true},
                {"frame_before", frameBefore},
                {"frame_after", GamePause::GetFrameCount()}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_crash_log") {
            auto crashes = CrashCatcher::GetCrashLog();
            if (arguments.value("clear", false)) CrashCatcher::ClearCrashLog();

            json entries = json::array();
            for (auto& c : crashes) {
                char addrBuf[16], faultBuf[16];
                snprintf(addrBuf, sizeof(addrBuf), "0x%08X", c.exceptionAddress);
                snprintf(faultBuf, sizeof(faultBuf), "0x%08X", c.faultAddress);

                json regs = {
                    {"eax", c.eax}, {"ebx", c.ebx}, {"ecx", c.ecx}, {"edx", c.edx},
                    {"esi", c.esi}, {"edi", c.edi}, {"esp", c.esp}, {"ebp", c.ebp},
                    {"eip", c.eip}, {"eflags", c.eflags}
                };

                json stack = json::array();
                for (int i = 0; i < c.stackDepth; i++) {
                    char sBuf[16]; snprintf(sBuf, sizeof(sBuf), "0x%08X", c.stackTrace[i]);
                    stack.push_back(sBuf);
                }

                char offsetBuf[32];
                snprintf(offsetBuf, sizeof(offsetBuf), "%s+0x%X", c.moduleName, c.moduleOffset);

                entries.push_back({
                    {"exception", CrashCatcher::GetExceptionName(c.exceptionCode)},
                    {"code", c.exceptionCode},
                    {"address", addrBuf},
                    {"fault_address", faultBuf},
                    {"module", offsetBuf},
                    {"thread_id", c.threadId},
                    {"timestamp", c.timestamp},
                    {"registers", regs},
                    {"stack_trace", stack}
                });
            }

            json info = {{"count", entries.size()}, {"crashes", entries}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "list_patches") {
            auto patches = PatchManager::ListPatches();
            json list = json::array();
            for (auto& p : patches) {
                char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", p.address);
                list.push_back({
                    {"name", p.name},
                    {"source", p.source},
                    {"address", addrBuf},
                    {"size", p.size},
                    {"active", p.active},
                    {"original", p.originalHex},
                    {"patched", p.patchedHex}
                });
            }
            json info = {{"count", list.size()}, {"patches", list}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "apply_patch") {
            std::string patchName = arguments.value("name", "");
            std::string addrStr = arguments.value("address", "");
            std::string bytesStr = arguments.value("bytes", "");

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);

            // Parse hex bytes
            std::vector<BYTE> bytes;
            for (size_t i = 0; i < bytesStr.size(); i++) {
                if (isspace(bytesStr[i])) continue;
                if (i + 1 < bytesStr.size() && isxdigit(bytesStr[i]) && isxdigit(bytesStr[i + 1])) {
                    char tmp[3] = { bytesStr[i], bytesStr[i + 1], 0 };
                    bytes.push_back((BYTE)strtoul(tmp, nullptr, 16));
                    i++;
                }
            }

            if (!PatchManager::ApplyPatch(patchName, addr, bytes.data(), (int)bytes.size())) {
                return {{"content", {{{"type", "text"}, {"text", "Failed to apply patch (already exists or bad address)"}}}}, {"isError", true}};
            }

            json info = {{"status", "applied"}, {"name", patchName}, {"address", addrStr}, {"size", bytes.size()}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "toggle_patch") {
            std::string patchName = arguments.value("name", "");
            if (!PatchManager::TogglePatch(patchName)) {
                return {{"content", {{{"type", "text"}, {"text", "Patch not found: " + patchName}}}}, {"isError", true}};
            }
            return {{"content", {{{"type", "text"}, {"text", "Patch toggled: " + patchName}}}}};
        }

        if (name == "import_patches") {
            std::string patchesJson = arguments.value("patches", "");
            int count = PatchManager::ImportPatches(patchesJson);
            json info = {{"imported", count}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "export_patches") {
            auto patches = PatchManager::ListPatches();
            json exportData = json::array();

            for (auto& p : patches) {
                char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", p.address);

                // Find which module this address belongs to
                char modName[64] = "unknown";
                DWORD modBase = 0;
                HMODULE hMods[256];
                DWORD cbNeeded;
                if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
                    int cnt = cbNeeded / sizeof(HMODULE);
                    for (int i = 0; i < cnt; i++) {
                        MODULEINFO mi;
                        if (GetModuleInformation(GetCurrentProcess(), hMods[i], &mi, sizeof(mi))) {
                            DWORD base = (DWORD)mi.lpBaseOfDll;
                            if (p.address >= base && p.address < base + mi.SizeOfImage) {
                                GetModuleBaseNameA(GetCurrentProcess(), hMods[i], modName, sizeof(modName));
                                modBase = base;
                                break;
                            }
                        }
                    }
                }

                exportData.push_back({
                    {"name", p.name},
                    {"module", modName},
                    {"offset", p.address - modBase},
                    {"address", addrBuf},
                    {"original", p.originalHex},
                    {"patched", p.patchedHex},
                    {"size", p.size}
                });
            }

            // Write to file
            std::string filePath = arguments.value("file", "");
            if (filePath.empty()) {
                char exePath[MAX_PATH];
                GetModuleFileNameA(nullptr, exePath, MAX_PATH);
                std::string dir(exePath);
                dir = dir.substr(0, dir.find_last_of("\\/") + 1);
                filePath = dir + "patches_export.json";
            }

            std::ofstream fout(filePath);
            if (fout.is_open()) {
                fout << exportData.dump(2) << std::endl;
                fout.close();
            }

            json info = {
                {"exported", exportData.size()},
                {"file", filePath},
                {"patches", exportData}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "add_watch") {
            std::string watchName = arguments.value("name", "");
            std::string addrStr = arguments.value("address", "");
            std::string typeStr = arguments.value("type", "dword");

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            MemWatch::WatchType wt = MemWatch::WATCH_DWORD;
            if (typeStr == "byte") wt = MemWatch::WATCH_BYTE;
            else if (typeStr == "word") wt = MemWatch::WATCH_WORD;
            else if (typeStr == "float") wt = MemWatch::WATCH_FLOAT;

            if (!MemWatch::AddWatch(watchName, addr, wt)) {
                return {{"content", {{{"type", "text"}, {"text", "Failed (already exists or bad address)"}}}}, {"isError", true}};
            }

            char buf[16]; snprintf(buf, sizeof(buf), "0x%08X", addr);
            json info = {{"status", "added"}, {"name", watchName}, {"address", buf}, {"type", typeStr}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "remove_watch") {
            if (arguments.value("all", false)) {
                MemWatch::RemoveAllWatches();
                return {{"content", {{{"type", "text"}, {"text", "All watches removed"}}}}};
            }
            std::string watchName = arguments.value("name", "");
            if (!MemWatch::RemoveWatch(watchName)) {
                return {{"content", {{{"type", "text"}, {"text", "Watch not found: " + watchName}}}}, {"isError", true}};
            }
            return {{"content", {{{"type", "text"}, {"text", "Watch removed: " + watchName}}}}};
        }

        if (name == "get_watches") {
            auto watches = MemWatch::GetWatches();
            json list = json::array();
            for (auto& w : watches) {
                char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", w.address);
                char curBuf[16]; snprintf(curBuf, sizeof(curBuf), "0x%08X", w.currentValue);
                char prevBuf[16]; snprintf(prevBuf, sizeof(prevBuf), "0x%08X", w.previousValue);

                json entry = {
                    {"name", w.name},
                    {"address", addrBuf},
                    {"current", curBuf},
                    {"current_dec", (int)w.currentValue},
                    {"previous", prevBuf},
                    {"changed", w.changed},
                    {"change_count", w.changeCount}
                };

                if (w.type == MemWatch::WATCH_FLOAT) {
                    float f; memcpy(&f, &w.currentValue, 4);
                    entry["current_float"] = f;
                }

                list.push_back(entry);
            }
            json info = {{"count", list.size()}, {"watches", list}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "list_struct_defs") {
            auto names = StructRegistry::ListStructs();
            json list = json::array();
            for (auto& n : names) {
                auto* s = StructRegistry::GetStruct(n);
                if (s) list.push_back({{"name", n}, {"size", s->size}, {"source", s->source}, {"fields", (int)s->fields.size()}});
            }
            json info = {{"count", list.size()}, {"structs", list}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_struct_def") {
            std::string sname = arguments.value("name", "");
            auto* s = StructRegistry::GetStruct(sname);
            if (!s) return {{"content", {{{"type", "text"}, {"text", "Struct not found: " + sname}}}}, {"isError", true}};

            const char* typeNames[] = {"byte","word","dword","int","float","pointer","string","wstring","array","padding"};
            json fields = json::array();
            for (auto& f : s->fields) {
                json jf = {{"name", f.name}, {"offset", f.offset}, {"type", typeNames[f.type]}, {"size", f.size}};
                if (!f.pointsTo.empty()) jf["points_to"] = f.pointsTo;
                if (f.arrayCount > 0) jf["array_count"] = f.arrayCount;
                if (!f.comment.empty()) jf["comment"] = f.comment;
                fields.push_back(jf);
            }
            json info = {{"name", s->name}, {"size", s->size}, {"source", s->source}, {"fields", fields}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "read_struct") {
            std::string addrStr = arguments.value("address", "");
            std::string sname = arguments.value("struct_name", "");
            bool followPtrs = arguments.value("follow_pointers", false);

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            auto* s = StructRegistry::GetStruct(sname);
            if (!s) return {{"content", {{{"type", "text"}, {"text", "Struct not found: " + sname}}}}, {"isError", true}};

            // Read the struct memory
            std::vector<BYTE> buf(s->size, 0);
            if (!SafeMemRead(addr, buf.data(), s->size)) {
                return {{"content", {{{"type", "text"}, {"text", "Cannot read address " + addrStr}}}}, {"isError", true}};
            }

            json fields = json::array();
            for (auto& f : s->fields) {
                if (f.type == StructRegistry::FIELD_PADDING) continue;
                if (f.offset + f.size > s->size) continue;

                json jf = {{"name", f.name}, {"offset", f.offset}};
                char hexBuf[16];

                switch (f.type) {
                    case StructRegistry::FIELD_BYTE:
                        jf["value"] = (int)buf[f.offset];
                        break;
                    case StructRegistry::FIELD_WORD:
                        jf["value"] = (int)*(WORD*)(buf.data() + f.offset);
                        break;
                    case StructRegistry::FIELD_DWORD:
                    case StructRegistry::FIELD_INT: {
                        DWORD v = *(DWORD*)(buf.data() + f.offset);
                        jf["value"] = (int)v;
                        snprintf(hexBuf, sizeof(hexBuf), "0x%08X", v);
                        jf["hex"] = hexBuf;
                        break;
                    }
                    case StructRegistry::FIELD_FLOAT: {
                        float v = *(float*)(buf.data() + f.offset);
                        jf["value"] = v;
                        break;
                    }
                    case StructRegistry::FIELD_POINTER: {
                        DWORD v = *(DWORD*)(buf.data() + f.offset);
                        snprintf(hexBuf, sizeof(hexBuf), "0x%08X", v);
                        jf["value"] = hexBuf;
                        jf["is_null"] = (v == 0);
                        if (!f.pointsTo.empty()) jf["points_to"] = f.pointsTo;

                        // Follow pointer one level if requested
                        if (followPtrs && v != 0 && !f.pointsTo.empty()) {
                            auto* childStruct = StructRegistry::GetStruct(f.pointsTo);
                            if (childStruct) {
                                std::vector<BYTE> childBuf(childStruct->size, 0);
                                if (SafeMemRead(v, childBuf.data(), childStruct->size)) {
                                    json childFields = json::array();
                                    for (auto& cf : childStruct->fields) {
                                        if (cf.type == StructRegistry::FIELD_PADDING) continue;
                                        if (cf.offset + cf.size > childStruct->size) continue;
                                        json cjf = {{"name", cf.name}, {"offset", cf.offset}};
                                        switch (cf.type) {
                                            case StructRegistry::FIELD_BYTE: cjf["value"] = (int)childBuf[cf.offset]; break;
                                            case StructRegistry::FIELD_WORD: cjf["value"] = (int)*(WORD*)(childBuf.data() + cf.offset); break;
                                            case StructRegistry::FIELD_DWORD:
                                            case StructRegistry::FIELD_INT: {
                                                DWORD cv = *(DWORD*)(childBuf.data() + cf.offset);
                                                cjf["value"] = (int)cv;
                                                char chex[16]; snprintf(chex, sizeof(chex), "0x%08X", cv);
                                                cjf["hex"] = chex;
                                                break;
                                            }
                                            case StructRegistry::FIELD_POINTER: {
                                                DWORD cv = *(DWORD*)(childBuf.data() + cf.offset);
                                                char chex[16]; snprintf(chex, sizeof(chex), "0x%08X", cv);
                                                cjf["value"] = chex;
                                                break;
                                            }
                                            case StructRegistry::FIELD_STRING: {
                                                char str[64] = {};
                                                int len = cf.size < 63 ? cf.size : 63;
                                                memcpy(str, childBuf.data() + cf.offset, len);
                                                cjf["value"] = str;
                                                break;
                                            }
                                            default: break;
                                        }
                                        childFields.push_back(cjf);
                                    }
                                    jf["expanded"] = childFields;
                                }
                            }
                        }
                        break;
                    }
                    case StructRegistry::FIELD_STRING: {
                        char str[64] = {};
                        int len = f.size < 63 ? f.size : 63;
                        memcpy(str, buf.data() + f.offset, len);
                        jf["value"] = str;
                        break;
                    }
                    default: break;
                }

                if (!f.comment.empty()) jf["comment"] = f.comment;
                fields.push_back(jf);
            }

            json info = {{"struct", sname}, {"address", addrStr}, {"size", s->size}, {"fields", fields}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "read_region") {
            std::string addrStr = arguments.value("address", "");
            int size = arguments.value("size", 64);
            if (size > 1024) size = 1024;
            if (size < 4) size = 4;
            size = (size + 3) & ~3; // align to 4

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            std::vector<BYTE> buf(size, 0);
            if (!SafeMemRead(addr, buf.data(), size)) {
                return {{"content", {{{"type", "text"}, {"text", "Cannot read address " + addrStr}}}}, {"isError", true}};
            }

            // Classify each DWORD
            json dwords = json::array();
            for (int i = 0; i < size; i += 4) {
                DWORD val = *(DWORD*)(buf.data() + i);
                char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", addr + i);
                char valBuf[16]; snprintf(valBuf, sizeof(valBuf), "0x%08X", val);

                std::string classification = "unknown";
                if (val == 0) {
                    classification = "zero";
                } else if (val >= 0x00400000 && val < 0x7FFFFFFF) {
                    // Looks like a valid user-mode pointer
                    BYTE test;
                    if (SafeMemRead(val, &test, 1)) {
                        classification = "pointer";
                    } else {
                        classification = "integer";
                    }
                } else if (val < 0x10000) {
                    classification = "small_int";
                } else {
                    // Check if it could be ASCII
                    BYTE b0 = buf[i], b1 = buf[i+1], b2 = buf[i+2], b3 = buf[i+3];
                    if (b0 >= 32 && b0 < 127 && b1 >= 32 && b1 < 127) {
                        classification = "possible_string";
                    } else {
                        classification = "integer";
                    }
                }

                json entry = {
                    {"offset", i},
                    {"address", addrBuf},
                    {"hex", valBuf},
                    {"value", (int)val},
                    {"type", classification}
                };
                dwords.push_back(entry);
            }

            json info = {{"address", addrStr}, {"size", size}, {"dwords", dwords}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "save_struct_defs") {
            char exePath[MAX_PATH];
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            std::string dir(exePath);
            dir = dir.substr(0, dir.find_last_of("\\/") + 1);
            std::string path = dir + "structs.json";

            if (StructRegistry::SaveToFile(path)) {
                json info = {{"saved", true}, {"file", path}, {"count", StructRegistry::GetStructCount()}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            }
            return {{"content", {{{"type", "text"}, {"text", "Failed to save"}}}}, {"isError", true}};
        }

        if (name == "click_screen") {
            int x = arguments.value("x", 0);
            int y = arguments.value("y", 0);
            std::string method = arguments.value("method", "sendinput");

            HWND hWnd = FindWindow(nullptr, "Diablo II");
            if (!hWnd) {
                return {{"content", {{{"type", "text"}, {"text", "Game window not found"}}}}, {"isError", true}};
            }

            if (method == "post") {
                // PostMessage approach (doesn't work for PD2 custom UI)
                LPARAM lParam = MAKELPARAM(x, y);
                PostMessage(hWnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
                Sleep(50);
                PostMessage(hWnd, WM_LBUTTONUP, 0, lParam);
            } else {
                // SetCursorPos + SendInput approach
                // SetCursorPos for precise positioning (works with multi-monitor)
                // SendInput for click only (no absolute move which has precision issues)
                POINT pt = { x, y };
                ClientToScreen(hWnd, &pt);

                // Move cursor directly
                SetCursorPos(pt.x, pt.y);
                Sleep(50);

                // Bring window to foreground
                SetForegroundWindow(hWnd);
                Sleep(100);

                // Send click at current cursor position (no MOUSEEVENTF_ABSOLUTE)
                INPUT inputs[2] = {};
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;

                SendInput(1, &inputs[0], sizeof(INPUT));
                Sleep(50);
                SendInput(1, &inputs[1], sizeof(INPUT));

                json info = {{"status", "clicked"}, {"x", x}, {"y", y},
                    {"screen_x", pt.x}, {"screen_y", pt.y}, {"method", "sendinput"}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            }

            json info = {{"status", "clicked"}, {"x", x}, {"y", y}, {"method", method}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_cursor_item") {
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pInventory) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            UnitAny* pCursor = pPlayer->pInventory->pCursorItem;
            if (!pCursor) {
                json info = {{"has_item", false}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            }
            char itemName[64] = {};
            SafeGetUnitName(pCursor, itemName, sizeof(itemName));
            json info = {
                {"has_item", true},
                {"unit_id", (int)pCursor->dwUnitId},
                {"code", (int)pCursor->dwTxtFileNo},
                {"name", itemName[0] ? itemName : "?"}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_stash_grid") {
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pInventory) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }

            std::string container = arguments.value("container", "inventory");
            int targetLoc = 0; // STORAGE_INVENTORY
            int gridW = 10, gridH = 4;
            if (container == "stash") { targetLoc = 4; gridW = 10; gridH = 16; }
            else if (container == "cube") { targetLoc = 3; gridW = 3; gridH = 4; }

            // Build occupancy grid
            std::vector<int> grid(gridW * gridH, 0);

            UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
            while (pItem) {
                if (pItem->pItemData && pItem->pItemData->ItemLocation == targetLoc) {
                    int x = 0, y = 0;
                    if (pItem->pPath) {
                        ItemPath* ip = (ItemPath*)pItem->pPath;
                        x = (int)ip->dwPosX;
                        y = (int)ip->dwPosY;
                    }

                    // Get item size
                    int w = 1, h = 1;
                    SafeGetItemSize(pItem->dwTxtFileNo, &w, &h);

                    // Mark occupied cells
                    for (int iy = y; iy < y + h && iy < gridH; iy++) {
                        for (int ix = x; ix < x + w && ix < gridW; ix++) {
                            grid[iy * gridW + ix] = pItem->dwUnitId;
                        }
                    }
                }

                pItem = D2COMMON_GetNextItemFromInventory(pItem);
            }

            // Build response
            json rows = json::array();
            int freeCount = 0;
            for (int y = 0; y < gridH; y++) {
                json row = json::array();
                for (int x = 0; x < gridW; x++) {
                    int uid = grid[y * gridW + x];
                    row.push_back(uid);
                    if (uid == 0) freeCount++;
                }
                rows.push_back(row);
            }

            json info = {
                {"container", container},
                {"width", gridW},
                {"height", gridH},
                {"total_cells", gridW * gridH},
                {"free_cells", freeCount},
                {"grid", rows}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_nearby_objects") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            int maxDist = arguments.value("max_distance", 50);
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pPath) {
                return {{"content", {{{"type", "text"}, {"text", "No player"}}}}, {"isError", true}};
            }
            int px = pPlayer->pPath->xPos, py = pPlayer->pPath->yPos;

            // Scan objects via unit hash table (type 2, 128 buckets)
            json objects = json::array();
            UnitAny** pTable = (UnitAny**)*Var_D2CLIENT_pUnitTable();
            if (pTable) {
                for (int bucket = 0; bucket < 128; bucket++) {
                    UnitAny* u = pTable[256 + bucket]; // type 2 starts at 2*128
                    while (u) {
                        if (u->dwType == 2 && u->pPath) {
                            // Objects use ObjectPath: pos at +0x0C and +0x10 (DWORD, not WORD)
                            ObjectPath* op = (ObjectPath*)u->pPath;
                            int ux = (int)op->dwPosX, uy = (int)op->dwPosY;
                            int dx = ux - px, dy = uy - py;
                            int dist = (int)sqrt((double)(dx*dx + dy*dy));

                            if (maxDist <= 0 || dist <= maxDist) {
                                char objName[64] = {};
                                SafeGetUnitName(u, objName, sizeof(objName));
                                objects.push_back({
                                    {"unit_id", (int)u->dwUnitId},
                                    {"class_id", (int)u->dwTxtFileNo},
                                    {"name", objName[0] ? objName : "?"},
                                    {"distance", dist},
                                    {"position", {{"x", ux}, {"y", uy}}},
                                    {"mode", (int)u->dwMode}
                                });
                            }
                        }
                        u = u->pListNext;
                    }
                }
            }
            std::sort(objects.begin(), objects.end(),
                [](const json& a, const json& b) { return a["distance"] < b["distance"]; });

            json info = {{"count", objects.size()}, {"objects", objects}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "walk_to") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            int x = arguments.value("x", 0);
            int y = arguments.value("y", 0);
            bool run = arguments.value("run", true);

            BYTE packet[5] = {};
            packet[0] = run ? 0x03 : 0x01; // 0x03=run, 0x01=walk
            *(WORD*)&packet[1] = (WORD)x;
            *(WORD*)&packet[3] = (WORD)y;
            D2NET_SendPacket(5, 1, packet);

            // Get current position for response
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            int cx = 0, cy = 0;
            if (pPlayer && pPlayer->pPath) {
                cx = pPlayer->pPath->xPos;
                cy = pPlayer->pPath->yPos;
            }
            int dx = x - cx, dy = y - cy;
            int dist = (int)sqrt((double)(dx*dx + dy*dy));

            json info = {
                {"status", run ? "running" : "walking"},
                {"target", {{"x", x}, {"y", y}}},
                {"current", {{"x", cx}, {"y", cy}}},
                {"distance", dist}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_waypoints") {
            // Read waypoint destination table at 0x6FBACD8C
            struct WpEntry { int slot; int areaId; };
            struct WpReader {
                WpEntry entries[40];
                int count;
                static void Read(WpReader& out) {
                    out.count = 0;
                    __try {
                        for (int s = 0; s < 39 && out.count < 40; s++) {
                            DWORD a = *(DWORD*)(0x6FBACD8C + s * 20);
                            if (a > 0 && a < 200) {
                                out.entries[out.count++] = {s, (int)a};
                            }
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                }
            };
            WpReader wr;
            WpReader::Read(wr);

            json waypoints = json::array();
            for (int i = 0; i < wr.count; i++) {
                waypoints.push_back({{"slot", wr.entries[i].slot}, {"area_id", wr.entries[i].areaId}});
            }

            DWORD wpFlag = *(DWORD*)0x6FBAADD0;
            json info = {{"count", (int)waypoints.size()}, {"panel_open", wpFlag != 0}, {"waypoints", waypoints}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "interact_entity") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            DWORD unitId = arguments.value("unit_id", (DWORD)0);
            DWORD unitType = arguments.value("unit_type", (DWORD)2);
            std::string expectedPanel = arguments.value("expected_panel", "any");
            int timeoutMs = arguments.value("timeout_ms", 10000);
            if (timeoutMs < 1000) timeoutMs = 1000;
            if (timeoutMs > 30000) timeoutMs = 30000;

            // Helper: get panel state
            auto getPanelState = []() -> int {
                HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
                if (!hPD2) return -1;
                DWORD* pPtr = (DWORD*)((DWORD)hPD2 + 0x00410688);
                if (!*pPtr) return -1;
                return (int)*((DWORD*)*pPtr);
            };

            // Helper: check if expected panel is open
            auto panelMatches = [&](int state) -> bool {
                if (expectedPanel == "stash") return state == 0x0C;
                if (expectedPanel == "trade") return state == 0x0D;
                if (expectedPanel == "waypoint") {
                    // Waypoint panel uses g_dwData_add0 at D2CLIENT+0xFAADD0 offset
                    // Address: 0x6FBAADD0
                    DWORD* pWpFlag = (DWORD*)0x6FBAADD0;
                    return *pWpFlag != 0;
                }
                if (expectedPanel == "any") {
                    if (state > 0) return true;
                    // Also check waypoint panel
                    DWORD* pWpFlag = (DWORD*)0x6FBAADD0;
                    return *pWpFlag != 0;
                }
                return state > 0;
            };

            // If panel already open, return immediately
            int panelBefore = getPanelState();
            if (panelMatches(panelBefore)) {
                json info = {{"status", "already_open"}, {"panel_state", panelBefore}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            }

            // Step 1: Send Walk to Entity (0x02) to approach
            {
                BYTE pkt[9] = {};
                pkt[0] = 0x02;
                *(DWORD*)&pkt[1] = unitType;
                *(DWORD*)&pkt[5] = unitId;
                D2NET_SendPacket(9, 0, pkt);
            }

            // Step 2: Wait for panel to open, retrying interact periodically
            DWORD start = GetTickCount();
            bool opened = false;
            int attempts = 0;

            while ((int)(GetTickCount() - start) < timeoutMs) {
                Sleep(300);

                // Check if panel opened
                int state = getPanelState();
                if (panelMatches(state)) {
                    opened = true;
                    break;
                }

                // Every 2 seconds, send another interact packet
                if (attempts % 7 == 3) {
                    BYTE pkt[9] = {};
                    pkt[0] = 0x13;
                    *(DWORD*)&pkt[1] = unitType;
                    *(DWORD*)&pkt[5] = unitId;
                    D2NET_SendPacket(9, 1, pkt);
                }

                // Every 3 seconds, try Walk to Entity again
                if (attempts % 10 == 6) {
                    BYTE pkt[9] = {};
                    pkt[0] = 0x02;
                    *(DWORD*)&pkt[1] = unitType;
                    *(DWORD*)&pkt[5] = unitId;
                    D2NET_SendPacket(9, 0, pkt);
                }

                attempts++;
            }

            int elapsed = (int)(GetTickCount() - start);
            int finalState = getPanelState();

            json info = {
                {"status", opened ? "opened" : "timeout"},
                {"unit_id", (int)unitId},
                {"panel_state", finalState},
                {"elapsed_ms", elapsed},
                {"attempts", attempts}
            };
            if (!opened) {
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}, {"isError", !opened}};
            }
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "wait_until") {
            std::string condition = arguments.value("condition", "");
            int timeoutMs = arguments.value("timeout_ms", 10000);
            if (timeoutMs < 500) timeoutMs = 500;
            if (timeoutMs > 30000) timeoutMs = 30000;

            DWORD start = GetTickCount();
            bool met = false;
            std::string detail;

            while ((int)(GetTickCount() - start) < timeoutMs) {
                Sleep(200);

                if (condition == "panel_open") {
                    HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
                    if (hPD2) {
                        DWORD* pPtr = (DWORD*)((DWORD)hPD2 + 0x00410688);
                        if (*pPtr && *((DWORD*)*pPtr) > 0) {
                            int s = *((DWORD*)*pPtr);
                            met = true;
                            detail = (s == 0xC) ? "stash" : (s == 0xD) ? "trade" : "other";
                            break;
                        }
                    }
                }
                else if (condition == "panel_closed") {
                    HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
                    if (hPD2) {
                        DWORD* pPtr = (DWORD*)((DWORD)hPD2 + 0x00410688);
                        if (!*pPtr || *((DWORD*)*pPtr) == 0) {
                            met = true; detail = "closed"; break;
                        }
                    }
                }
                else if (condition == "area_changed") {
                    int targetArea = arguments.value("area_id", 0);
                    UnitAny* p = D2CLIENT_GetPlayerUnit();
                    if (p && p->pPath) {
                        // Get current area from path->room->room2->level
                        // Simpler: just check if area changed from initial
                        if (targetArea > 0) {
                            // Read area from game state
                            auto gs = GameState::GetPlayerState();
                            // Can't easily get area here, use position change as proxy
                        }
                    }
                    // Fallback: check via game frame advancement
                    met = GameState::IsGameReady();
                    if (met) { detail = "in_game"; break; }
                }
                else if (condition == "position_reached") {
                    int tx = arguments.value("x", 0);
                    int ty = arguments.value("y", 0);
                    int maxDist = arguments.value("distance", 5);
                    UnitAny* p = D2CLIENT_GetPlayerUnit();
                    if (p && p->pPath) {
                        int dx = p->pPath->xPos - tx;
                        int dy = p->pPath->yPos - ty;
                        int dist = (int)sqrt((double)(dx*dx + dy*dy));
                        if (dist <= maxDist) {
                            char buf[32]; snprintf(buf, sizeof(buf), "dist=%d", dist);
                            met = true; detail = buf; break;
                        }
                    }
                }
                else if (condition == "in_game") {
                    if (GameState::IsGameReady()) {
                        met = true; detail = "in_game"; break;
                    }
                }
                else if (condition == "cursor_empty") {
                    UnitAny* p = D2CLIENT_GetPlayerUnit();
                    if (p && p->pInventory && !p->pInventory->pCursorItem) {
                        met = true; detail = "empty"; break;
                    }
                }
            }

            int elapsed = (int)(GetTickCount() - start);
            json info = {
                {"condition", condition},
                {"met", met},
                {"elapsed_ms", elapsed},
                {"detail", detail}
            };
            if (!met) {
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}, {"isError", true}};
            }
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "cast_skill") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            int x = arguments.value("x", 0);
            int y = arguments.value("y", 0);
            int unitId = arguments.value("unit_id", -1);
            int unitType = arguments.value("unit_type", 1);
            bool left = arguments.value("left", false);

            if (unitId >= 0) {
                // Cast on unit: 0x06 (left) or 0x0D (right) = 9 bytes
                BYTE packet[9] = {};
                packet[0] = left ? 0x06 : 0x0D;
                *(DWORD*)&packet[1] = unitType;
                *(DWORD*)&packet[5] = unitId;
                D2NET_SendPacket(9, 1, packet);
                json info = {{"status", "cast_on_unit"}, {"unit_id", unitId},
                             {"skill_side", left ? "left" : "right"}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            } else {
                // Cast at location: 0x05 (left) or 0x0C (right) = 5 bytes
                BYTE packet[5] = {};
                packet[0] = left ? 0x05 : 0x0C;
                *(WORD*)&packet[1] = (WORD)x;
                *(WORD*)&packet[3] = (WORD)y;
                D2NET_SendPacket(5, 1, packet);
                json info = {{"status", "cast_at_location"}, {"x", x}, {"y", y},
                             {"skill_side", left ? "left" : "right"}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            }
        }

        if (name == "use_waypoint") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            DWORD wpId = arguments.value("waypoint_id", (DWORD)0);
            int dest = arguments.value("destination", 0);

            // Step 1: Interact with waypoint (0x13) — character auto-walks to it
            {
                BYTE pkt[9] = {};
                pkt[0] = 0x13;
                *(DWORD*)&pkt[1] = 2;  // entity type = object
                *(DWORD*)&pkt[5] = wpId;
                D2NET_SendPacket(9, 1, pkt);
            }
            Sleep(500); // brief wait for interact to process

            // Step 2: Send waypoint destination packet
            // Format: {0x49, DWORD wp_data, DWORD area_id} = 9 bytes
            // wp_data = waypoint unit ID, area_id = destination area number
            {
                BYTE pkt[9] = {};
                pkt[0] = 0x49;
                *(DWORD*)&pkt[1] = wpId;
                *(DWORD*)&pkt[5] = dest; // dest is the AREA ID (1=Rogue, 109=Harrogath, etc.)
                D2NET_SendPacket(9, 1, pkt);
            }

            // Waypoint destination names
            static const char* wpNames[] = {
                "Rogue Encampment", "Cold Plains", "Stony Field", "Dark Wood",
                "Black Marsh", "Outer Cloister", "Jail Level 1", "Inner Cloister",
                "Catacombs Level 2",
                "Lut Gholein", "Sewers Level 2", "Dry Hills", "Halls of the Dead Level 2",
                "Far Oasis", "Lost City", "Palace Cellar Level 1", "Arcane Sanctuary",
                "Canyon of the Magi",
                "Kurast Docks", "Spider Forest", "Great Marsh", "Flayer Jungle",
                "Lower Kurast", "Kurast Bazaar", "Upper Kurast", "Travincal",
                "Durance of Hate Level 2",
                "Pandemonium Fortress", "City of the Damned", "River of Flame",
                "Harrogath", "Frigid Highlands", "Arreat Plateau", "Crystalline Passage",
                "Halls of Pain", "Glacial Trail", "Frozen Tundra",
                "The Ancients' Way", "Worldstone Keep Level 2"
            };
            const char* destName = (dest >= 0 && dest < 39) ? wpNames[dest] : "Unknown";

            json info = {{"status", "waypoint_used"}, {"waypoint_id", (int)wpId},
                         {"destination", dest}, {"destination_name", destName}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "sell_item") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            DWORD itemId = arguments.value("item_id", (DWORD)0);
            DWORD npcId = arguments.value("npc_id", (DWORD)0);

            // Sell packet: 0x33, npc_id(4), item_id(4), tab(4), cost(4) = 17 bytes
            BYTE packet[17] = {};
            packet[0] = 0x33;
            *(DWORD*)&packet[1] = npcId;
            *(DWORD*)&packet[5] = itemId;
            *(DWORD*)&packet[9] = 0;  // tab (0 = default)
            *(DWORD*)&packet[13] = 0; // cost (0 = server determines)
            D2NET_SendPacket(17, 1, packet);

            json info = {{"status", "sell_sent"}, {"item_id", itemId}, {"npc_id", npcId}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "interact_object") {
            DWORD unitId = arguments.value("unit_id", 0);
            DWORD unitType = arguments.value("unit_type", 2); // default: object

            BYTE packet[9] = {};
            packet[0] = 0x13;
            *(DWORD*)&packet[1] = unitType;
            *(DWORD*)&packet[5] = unitId;
            D2NET_SendPacket(9, 1, packet);

            json info = {{"status", "interact_sent"}, {"unit_id", unitId}, {"unit_type", unitType}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "switch_stash_tab") {
            int tab = arguments.value("tab", 0);
            if (tab < 0 || tab > 10) {
                return {{"content", {{{"type", "text"}, {"text", "Tab must be 0-10 (0=Personal, 1-9=Shared I-IX, 10=Materials)"}}}}, {"isError", true}};
            }

            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }

            HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
            if (!hPD2) {
                return {{"content", {{{"type", "text"}, {"text", "ProjectDiablo.dll not loaded"}}}}, {"isError", true}};
            }

            // PD2 tab click handler functions in ProjectDiablo.dll
            // Each is void(void) with built-in guards (stash open check, already-on-tab check)
            // RVAs from Ghidra analysis of InitStashTabs / g_nActiveStashTab xrefs
            // Internally they send a 2-byte packet via the dispatcher:
            //   byte[0] = 0x55 (stash tab switch command)
            //   byte[1] = 1-based tab number
            static const DWORD tabHandlerRVA[] = {
                0x001906c0, // Tab 0 (Personal)  -> sends 0x55,0x01
                0x00190700, // Tab 1 (Shared I)   -> sends 0x55,0x02
                0x00190740, // Tab 2 (Shared II)  -> sends 0x55,0x03
                0x00190780, // Tab 3 (Shared III) -> sends 0x55,0x04
                0x001907c0, // Tab 4 (Shared IV)  -> sends 0x55,0x05
                0x00190800, // Tab 5 (Shared V)   -> sends 0x55,0x06
                0x00190840, // Tab 6 (Shared VI)  -> sends 0x55,0x07
                0x00190880, // Tab 7 (Shared VII) -> sends 0x55,0x08
                0x001908c0, // Tab 8 (Shared VIII)-> sends 0x55,0x09
                0x00190900, // Tab 9 (Shared IX)  -> sends 0x55,0x0A
            };

            GameCallQueue::PendingCall call = {};

            if (tab <= 9) {
                call.address = (DWORD)hPD2 + tabHandlerRVA[tab];
                call.argCount = 0;
                call.convention = 1; // cdecl
            } else {
                // Tab 10 (Materials): no dedicated handler in PD2.
                // Tab handlers send a 2-byte command {0x55, 1-based_tab} via FUN_1023ead0
                // which uses a non-standard calling convention: ECX=packet_ptr, EDX=length, EBX=length
                // We replicate this with inline asm via a static helper called on the game thread.
                struct MaterialsTabHelper {
                    static DWORD __cdecl Switch() {
                        HMODULE pd2 = GetModuleHandle("ProjectDiablo.dll");
                        if (!pd2) return 0;
                        // Check guards: stash must be open, not already on tab 10, no pending op
                        DWORD* pStashState = (DWORD*)(*(DWORD*)((DWORD)pd2 + 0x00410688));
                        if (*pStashState != 0x0C) return 0; // stash not open
                        DWORD* pActiveTab = (DWORD*)((DWORD)pd2 + 0x0040edd4);
                        if (*pActiveTab == 10) return 0; // already on Materials
                        DWORD* pPending = (DWORD*)((DWORD)pd2 + 0x0030de48);
                        if (*pPending != 0) return 0; // pending operation
                        // Call FUN_1023ead0 the same way tab handlers do:
                        // EDX = 2 (packet length), ECX = ptr to packet {0x55, 0x0B}
                        DWORD dispatchAddr = (DWORD)pd2 + 0x0023ead0;
                        WORD packet = 0x0B55; // little-endian: byte[0]=0x55, byte[1]=0x0B
                        __asm {
                            lea ecx, packet
                            mov edx, 2
                            call dispatchAddr
                        }
                        return 1;
                    }
                };
                call.address = (DWORD)&MaterialsTabHelper::Switch;
                call.argCount = 0;
                call.convention = 1; // cdecl
            }

            bool ok = GameCallQueue::CallOnGameThread(call, 3000);
            if (!ok) {
                json err = {{"status", "failed"}, {"tab", tab}, {"crashed", call.crashed}};
                return {{"content", {{{"type", "text"}, {"text", err.dump(2)}}}}, {"isError", true}};
            }

            Sleep(300); // wait for stash data to load

            json info = {{"status", "switched"}, {"tab", tab}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "open_stash") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pPath || !pPlayer->pAct) {
                return {{"content", {{{"type", "text"}, {"text", "No player"}}}}, {"isError", true}};
            }
            int px = pPlayer->pPath->xPos, py = pPlayer->pPath->yPos;

            // Find the stash object via D2CLIENT unit hash table (type 2 = objects)
            // The unit table has 128 buckets per type, objects start at bucket index 256
            UnitAny* stash = nullptr;
            int bestDist = 999;
            UnitAny** pTable = (UnitAny**)*Var_D2CLIENT_pUnitTable();
            if (pTable) {
                for (int bucket = 0; bucket < 128; bucket++) {
                    UnitAny* u = pTable[256 + bucket]; // type 2 (object) starts at 2*128
                    while (u) {
                        if (u->dwType == 2) {
                            // Check for stash by txtFileNo or name
                            bool isStash = (u->dwTxtFileNo == 267 || u->dwTxtFileNo == 580 || u->dwTxtFileNo == 581);
                            if (!isStash) {
                                char objName[64] = {};
                                SafeGetUnitName(u, objName, sizeof(objName));
                                isStash = (strstr(objName, "tash") || strstr(objName, "Bank") || strstr(objName, "bank"));
                            }
                            if (isStash && u->pPath) {
                                ObjectPath* op = (ObjectPath*)u->pPath;
                                int ux = (int)op->dwPosX, uy = (int)op->dwPosY;
                                int dx = ux - px, dy = uy - py;
                                int dist = (int)sqrt((double)(dx*dx + dy*dy));
                                if (dist < bestDist) {
                                    bestDist = dist;
                                    stash = u;
                                }
                            }
                        }
                        u = u->pListNext;
                    }
                }
            }

            if (!stash) {
                return {{"content", {{{"type", "text"}, {"text", "No stash found nearby. Are you in town?"}}}}, {"isError", true}};
            }

            // Helper: check if stash panel is open
            auto isStashOpen = []() -> bool {
                HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
                if (!hPD2) return false;
                DWORD* pPtr = (DWORD*)((DWORD)hPD2 + 0x00410688);
                if (!*pPtr) return false;
                return *((DWORD*)*pPtr) == 0x0C;
            };

            // If already open, just return success
            if (isStashOpen()) {
                char objName[64] = {};
                SafeGetUnitName(stash, objName, sizeof(objName));
                json info = {{"status", "already_open"}, {"stash_id", (int)stash->dwUnitId},
                             {"stash_name", objName}};
                return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
            }

            // Send interact packet — auto-walks and opens
            BYTE packet[9] = {};
            packet[0] = 0x13;
            *(DWORD*)&packet[1] = 2; // UNIT_OBJECT
            *(DWORD*)&packet[5] = stash->dwUnitId;
            D2NET_SendPacket(9, 1, packet);

            // Wait for stash to actually open (up to 5 seconds)
            bool opened = false;
            for (int wait = 0; wait < 25; wait++) {
                Sleep(200);
                if (isStashOpen()) { opened = true; break; }
            }

            char objName[64] = {};
            SafeGetUnitName(stash, objName, sizeof(objName));

            json info = {
                {"status", opened ? "opened" : "interact_sent"},
                {"stash_id", (int)stash->dwUnitId},
                {"stash_class", (int)stash->dwTxtFileNo},
                {"stash_name", objName},
                {"distance", bestDist},
                {"verified", opened}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "open_cube") {
            // Toggle cube UI panel via SetUIVar
            D2CLIENT_SetUIVar(UI_CUBE, 1, 0);
            return {{"content", {{{"type", "text"}, {"text", "Cube panel toggled"}}}}};
        }

        if (name == "is_panel_open") {
            HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
            int panelState = 0;
            if (hPD2) {
                DWORD* pPtr = (DWORD*)((DWORD)hPD2 + 0x00410688);
                if (*pPtr) panelState = *((DWORD*)*pPtr);
            }
            const char* panelName = "none";
            if (panelState == 0x0C) panelName = "stash";
            else if (panelState == 0x0D) panelName = "trade";
            else if (panelState == 0x01) panelName = "inventory";
            else if (panelState > 0) panelName = "other";

            json info = {
                {"panel", panelName},
                {"panel_state", panelState},
                {"stash_open", panelState == 0x0C},
                {"trade_open", panelState == 0x0D}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "close_panels") {
            D2CLIENT_CloseInteract();
            return {{"content", {{{"type", "text"}, {"text", "Panels closed"}}}}};
        }

        if (name == "use_item") {
            DWORD itemId = arguments.value("item_id", 0);
            if (itemId == 0) return {{"content", {{{"type", "text"}, {"text", "item_id required"}}}}, {"isError", true}};

            // Find the item to determine its location
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pInventory) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }

            // Determine location by scanning inventory
            int nodeP = -1;
            UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
            while (pItem) {
                if (pItem->dwUnitId == itemId && pItem->pItemData) {
                    nodeP = pItem->pItemData->NodePage;
                    break;
                }
                pItem = D2COMMON_GetNextItemFromInventory(pItem);
            }

            BYTE packet[13] = {};
            if (nodeP == NODEPAGE_BELTSLOTS) {
                packet[0] = 0x26; // use belt item
            } else {
                packet[0] = 0x20; // use inventory/stash/cube item
                if (pPlayer->pPath) {
                    *(DWORD*)&packet[5] = pPlayer->pPath->xPos;
                    *(DWORD*)&packet[9] = pPlayer->pPath->yPos;
                }
            }
            *(DWORD*)&packet[1] = itemId;
            D2NET_SendPacket(13, 1, packet);

            json info = {{"status", "used"}, {"item_id", itemId}, {"location", nodeP}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "drop_item") {
            DWORD itemId = arguments.value("item_id", 0);
            BYTE packet[5] = {};
            packet[0] = 0x17;
            *(DWORD*)&packet[1] = itemId;
            D2NET_SendPacket(5, 1, packet);

            json info = {{"status", "dropped"}, {"item_id", itemId}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "pickup_item") {
            DWORD itemId = arguments.value("item_id", 0);
            BYTE packet[13] = {};
            packet[0] = 0x16;
            *(DWORD*)&packet[1] = 0x04; // UNIT_ITEM
            *(DWORD*)&packet[5] = itemId;
            D2NET_SendPacket(13, 0, packet);

            json info = {{"status", "pickup_sent"}, {"item_id", itemId}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "item_to_cursor") {
            DWORD itemId = arguments.value("item_id", 0);
            BYTE packet[5] = {};
            packet[0] = 0x19;
            *(DWORD*)&packet[1] = itemId;
            D2NET_SendPacket(5, 1, packet);

            json info = {{"status", "to_cursor"}, {"item_id", itemId}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "cursor_to_container") {
            DWORD itemId = arguments.value("item_id", 0);
            int x = arguments.value("x", 0);
            int y = arguments.value("y", 0);
            std::string container = arguments.value("container", "inventory");

            int dest = 0; // inventory
            if (container == "stash") dest = 4;
            else if (container == "cube") dest = 3;
            else if (container == "trade") dest = 2;

            BYTE packet[17] = {};
            packet[0] = 0x18;
            *(DWORD*)&packet[1] = itemId;
            *(DWORD*)&packet[5] = x;
            *(DWORD*)&packet[9] = y;
            *(DWORD*)&packet[13] = dest;
            D2NET_SendPacket(17, 1, packet);

            json info = {{"status", "placed"}, {"item_id", itemId}, {"x", x}, {"y", y}, {"container", container}};
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "get_skills") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pInfo) {
                return {{"content", {{{"type", "text"}, {"text", "No player/skill info"}}}}, {"isError", true}};
            }

            json result;
            result["class_id"] = (int)pPlayer->dwTxtFileNo;
            static const char* classNames[] = {"Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin"};
            int classId = pPlayer->dwTxtFileNo;
            result["class"] = (classId >= 0 && classId <= 6) ? classNames[classId] : "Unknown";

            // Read skills via SEH-safe helper
            struct SkillEntry { int skillId; int baseLevel; int totalLevel; char tree[32]; };
            struct SkillReadResult {
                SkillEntry entries[120];
                int count;
                bool error;
            };

            auto readSkills = [](UnitAny* pUnit, int cls, SkillReadResult& out) {
                out.count = 0;
                out.error = false;
                __try {
                    Skill* pSkill = pUnit->pInfo->pFirstSkill;
                    while (pSkill && out.count < 120) {
                        if (pSkill->pSkillInfo && pSkill->skillLevel > 0) {
                            auto& e = out.entries[out.count];
                            DWORD sid = pSkill->pSkillInfo->wSkillId;
                            e.skillId = (int)sid;
                            e.baseLevel = pSkill->skillLevel;
                            e.totalLevel = D2COMMON_GetSkillLevel(pUnit, pSkill, TRUE);

                            const char* tree = "Unknown";
                            if (cls == 0) {
                                if (sid <= 11) tree = "Bow and Crossbow";
                                else if (sid <= 21) tree = "Passive and Magic";
                                else if (sid <= 31) tree = "Javelin and Spear";
                            } else if (cls == 1) {
                                if (sid >= 36 && sid <= 46) tree = "Fire";
                                else if (sid >= 47 && sid <= 57) tree = "Lightning";
                                else if (sid >= 58 && sid <= 65) tree = "Cold";
                            } else if (cls == 2) {
                                if (sid >= 66 && sid <= 76) tree = "Curses";
                                else if (sid >= 77 && sid <= 86) tree = "Poison and Bone";
                                else if (sid >= 87 && sid <= 95) tree = "Summoning";
                            } else if (cls == 3) {
                                if (sid >= 96 && sid <= 106) tree = "Combat";
                                else if (sid >= 107 && sid <= 116) tree = "Offensive Auras";
                                else if (sid >= 117 && sid <= 125) tree = "Defensive Auras";
                            } else if (cls == 4) {
                                if (sid >= 126 && sid <= 135) tree = "Warcries";
                                else if (sid >= 136 && sid <= 145) tree = "Combat Masteries";
                                else if (sid >= 146 && sid <= 155) tree = "Combat";
                            } else if (cls == 5) {
                                if (sid >= 221 && sid <= 230) tree = "Summoning";
                                else if (sid >= 231 && sid <= 240) tree = "Shape Shifting";
                                else if (sid >= 241 && sid <= 250) tree = "Elemental";
                            } else if (cls == 6) {
                                if (sid >= 251 && sid <= 260) tree = "Traps";
                                else if (sid >= 261 && sid <= 270) tree = "Shadow Disciplines";
                                else if (sid >= 271 && sid <= 280) tree = "Martial Arts";
                            }
                            strncpy_s(e.tree, tree, sizeof(e.tree));
                            out.count++;
                        }
                        pSkill = pSkill->pNextSkill;
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    out.error = true;
                }
            };

            SkillReadResult sr;
            readSkills(pPlayer, classId, sr);

            json skillList = json::array();
            std::map<std::string, int> treeTotals;
            int totalPoints = 0;

            for (int i = 0; i < sr.count; i++) {
                auto& e = sr.entries[i];
                json sk;
                sk["skill_id"] = e.skillId;
                sk["base_level"] = e.baseLevel;
                sk["total_level"] = e.totalLevel;
                sk["tree"] = e.tree;
                skillList.push_back(sk);
                totalPoints += e.baseLevel;
                treeTotals[e.tree] += e.baseLevel;
            }

            json treeSummary = json::object();
            std::string primaryTree = "Unknown";
            int maxPoints = 0;
            for (auto it = treeTotals.begin(); it != treeTotals.end(); ++it) {
                treeSummary[it->first] = it->second;
                if (it->second > maxPoints) {
                    maxPoints = it->second;
                    primaryTree = it->first;
                }
            }

            if (sr.error) result["_error"] = "access violation reading skills";

            result["skills"] = skillList;
            result["skill_count"] = (int)skillList.size();
            result["total_points"] = totalPoints;
            result["tree_summary"] = treeSummary;
            result["primary_tree"] = primaryTree;
            result["build_name"] = std::string(result["class"].get<std::string>()) + " (" + primaryTree + ")";

            return {{"content", {{{"type", "text"}, {"text", result.dump(2)}}}}};
        }

        if (name == "get_item_stats") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            DWORD itemId = arguments.value("item_id", (DWORD)0);

            // Find the item unit via hash table
            UnitAny* pItem = D2CLIENT_FindServerSideUnit(itemId, 4); // type 4 = item
            if (!pItem) {
                pItem = D2CLIENT_FindClientSideUnit(itemId, 4);
            }
            if (!pItem) {
                return {{"content", {{{"type", "text"}, {"text", "Item not found"}}}}, {"isError", true}};
            }

            json result;

            // Basic item info
            char itemName[64] = {};
            SafeGetUnitName(pItem, itemName, sizeof(itemName));
            result["name"] = itemName[0] ? itemName : "?";
            result["unit_id"] = (int)pItem->dwUnitId;
            result["code"] = (int)pItem->dwTxtFileNo;

            // Item data (quality, level, flags, etc.)
            if (pItem->pItemData) {
                ItemData* id = pItem->pItemData;
                static const char* qualityNames[] = {
                    "none", "inferior", "normal", "superior",
                    "magic", "set", "rare", "unique", "craft"
                };
                int q = id->dwQuality;
                result["quality"] = (q >= 0 && q <= 8) ? qualityNames[q] : "unknown";
                result["item_level"] = (int)id->dwItemLevel;
                result["location"] = (int)id->ItemLocation;
                result["body_location"] = (int)id->BodyLocation;

                // Flags from dwFlags (ItemData+0x18)
                DWORD flags = id->dwFlags;
                result["ethereal"] = (flags & 0x00400000) != 0;
                result["identified"] = (flags & 0x00000010) != 0;
                result["socketed"] = (flags & 0x00000800) != 0;
                result["runeword"] = (flags & 0x04000000) != 0;
            }

            // Item size
            int w = 1, h = 1;
            SafeGetItemSize(pItem->dwTxtFileNo, &w, &h);
            result["size_x"] = w;
            result["size_y"] = h;

            // Read all stats via stat list (SEH in separate helper)
            struct ItemStatData {
                struct Entry { int id; int subIndex; int value; };
                Entry entries[256];
                int count;
                int sockets;
                int defense;
                int minDmg;
                int maxDmg;
                bool error;
            };

            auto readStats = [](UnitAny* pUnit, ItemStatData& out) {
                out.count = 0;
                out.sockets = 0;
                out.defense = 0;
                out.minDmg = 0;
                out.maxDmg = 0;
                out.error = false;
                __try {
                    StatList* pSL = D2COMMON_GetStatList(pUnit, 0, 0x40);
                    if (pSL) {
                        Stat buf[256];
                        memset(buf, 0, sizeof(buf));
                        DWORD n = D2COMMON_CopyStatList(pSL, buf, 256);
                        for (DWORD i = 0; i < n && i < 256; i++) {
                            out.entries[out.count] = { buf[i].wStatIndex, buf[i].wSubIndex, (int)buf[i].dwStatValue };
                            out.count++;
                        }
                    }
                    out.sockets = D2COMMON_GetUnitStat(pUnit, 214, 0);
                    out.defense = D2COMMON_GetUnitStat(pUnit, 31, 0);
                    out.minDmg = D2COMMON_GetUnitStat(pUnit, 21, 0);
                    out.maxDmg = D2COMMON_GetUnitStat(pUnit, 22, 0);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    out.error = true;
                }
            };

            ItemStatData sd;
            readStats(pItem, sd);

            json statsArray = json::array();
            for (int i = 0; i < sd.count; i++) {
                auto& e = sd.entries[i];
                json stat;
                stat["id"] = e.id;
                stat["value"] = e.value;
                if (e.subIndex != 0) stat["sub_index"] = e.subIndex;

                const char* sname = GetStatName(e.id);
                if (sname) {
                    stat["name"] = sname;
                } else {
                    char buf[32]; snprintf(buf, sizeof(buf), "stat_%d", e.id);
                    stat["name"] = buf;
                }

                if (e.id == 214) result["sockets"] = e.value;
                if (e.id == 6 || e.id == 7 || e.id == 8 || e.id == 9 ||
                    e.id == 10 || e.id == 11) {
                    stat["display_value"] = e.value >> 8;
                }
                if (e.id == 57 || e.id == 58) stat["display_value"] = e.value / 256;
                if (e.id == 59) stat["display_value_sec"] = e.value / 25.0;

                statsArray.push_back(stat);
            }

            if (sd.sockets > 0) result["sockets"] = sd.sockets;
            if (sd.defense > 0) result["defense"] = sd.defense;
            if (sd.maxDmg > 0) {
                result["min_damage"] = sd.minDmg;
                result["max_damage"] = sd.maxDmg;
            }
            if (sd.error) result["_stat_error"] = "access violation reading stats";

            result["stats"] = statsArray;
            result["stat_count"] = (int)statsArray.size();

            return {{"content", {{{"type", "text"}, {"text", result.dump(2)}}}}};
        }

        if (name == "move_item") {
            if (!GameState::IsGameReady()) {
                return {{"content", {{{"type", "text"}, {"text", "Not in game"}}}}, {"isError", true}};
            }
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pInventory) {
                return {{"content", {{{"type", "text"}, {"text", "No player/inventory"}}}}, {"isError", true}};
            }

            DWORD itemId = arguments.value("item_id", (DWORD)0);
            std::string destContainer = arguments.value("dest_container", "stash");
            int destX = arguments.value("dest_x", 0);
            int destY = arguments.value("dest_y", 0);
            int destTab = arguments.value("dest_tab", -1);
            int pickWait = arguments.value("pick_wait_ms", 200);
            int placeWait = arguments.value("place_wait_ms", 200);

            if (pickWait < 50) pickWait = 50;
            if (pickWait > 2000) pickWait = 2000;
            if (placeWait < 50) placeWait = 50;
            if (placeWait > 2000) placeWait = 2000;

            int dest = 0;
            if (destContainer == "stash") dest = 4;
            else if (destContainer == "cube") dest = 3;
            else if (destContainer == "trade") dest = 2;

            // Step 1: Check if cursor is empty (abort if something already on cursor)
            UnitAny* pCursorBefore = pPlayer->pInventory->pCursorItem;
            if (pCursorBefore) {
                json err = {{"status", "cursor_occupied"}, {"cursor_item_id", (int)pCursorBefore->dwUnitId}};
                return {{"content", {{{"type", "text"}, {"text", err.dump(2)}}}}, {"isError", true}};
            }

            // Step 2: Pick item to cursor (packet 0x19)
            {
                BYTE pkt[5] = {};
                pkt[0] = 0x19;
                *(DWORD*)&pkt[1] = itemId;
                D2NET_SendPacket(5, 1, pkt);
            }
            Sleep(pickWait);

            // Step 3: Verify item is on cursor
            UnitAny* pCursorAfterPick = pPlayer->pInventory->pCursorItem;
            if (!pCursorAfterPick || pCursorAfterPick->dwUnitId != itemId) {
                DWORD actualId = pCursorAfterPick ? pCursorAfterPick->dwUnitId : 0;
                json err = {{"status", "pick_failed"}, {"item_id", (int)itemId},
                            {"cursor_item_id", (int)actualId}};
                return {{"content", {{{"type", "text"}, {"text", err.dump(2)}}}}, {"isError", true}};
            }

            // Step 4: Tab switch if needed
            if (destTab >= 0 && destTab <= 10) {
                bool tabOk = SwitchStashTab(destTab);
                if (!tabOk) {
                    // Tab switch failed — item is still on cursor, caller must handle
                    json err = {{"status", "tab_switch_failed"}, {"item_id", (int)itemId},
                                {"dest_tab", destTab}, {"cursor_has_item", true}};
                    return {{"content", {{{"type", "text"}, {"text", err.dump(2)}}}}, {"isError", true}};
                }
            }

            // Step 5: Place item (packet 0x18)
            {
                BYTE pkt[17] = {};
                pkt[0] = 0x18;
                *(DWORD*)&pkt[1] = itemId;
                *(DWORD*)&pkt[5] = destX;
                *(DWORD*)&pkt[9] = destY;
                *(DWORD*)&pkt[13] = dest;
                D2NET_SendPacket(17, 1, pkt);
            }
            Sleep(placeWait);

            // Step 6: Check result — cursor should be empty (or hold swapped item)
            UnitAny* pCursorAfterPlace = pPlayer->pInventory->pCursorItem;
            json result = {
                {"status", "moved"},
                {"item_id", (int)itemId},
                {"dest_container", destContainer},
                {"dest_x", destX},
                {"dest_y", destY},
            };
            if (destTab >= 0) result["dest_tab"] = destTab;

            if (pCursorAfterPlace) {
                // Item swap occurred — another item was displaced
                char swapName[64] = {};
                SafeGetUnitName(pCursorAfterPlace, swapName, sizeof(swapName));
                result["status"] = "swapped";
                result["swapped_item"] = {
                    {"unit_id", (int)pCursorAfterPlace->dwUnitId},
                    {"code", (int)pCursorAfterPlace->dwTxtFileNo},
                    {"name", swapName[0] ? swapName : "?"}
                };
            }

            return {{"content", {{{"type", "text"}, {"text", result.dump(2)}}}}};
        }

        if (name == "resolve_function") {
            std::string dll = arguments.value("dll", "");
            int ordinal = arguments.value("ordinal", 0);
            std::string funcName = arguments.value("name", "");

            HMODULE hMod = GetModuleHandleA(dll.c_str());
            if (!hMod) {
                return {{"content", {{{"type", "text"}, {"text", "DLL not loaded: " + dll}}}}, {"isError", true}};
            }

            DWORD addr = 0;
            if (ordinal > 0) {
                addr = (DWORD)GetProcAddress(hMod, (LPCSTR)(DWORD_PTR)ordinal);
            } else if (!funcName.empty()) {
                addr = (DWORD)GetProcAddress(hMod, funcName.c_str());
            }

            if (addr == 0) {
                return {{"content", {{{"type", "text"}, {"text", "Function not found"}}}}, {"isError", true}};
            }

            char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", addr);
            char baseBuf[16]; snprintf(baseBuf, sizeof(baseBuf), "0x%08X", (DWORD)hMod);

            json info = {
                {"dll", dll},
                {"base", baseBuf},
                {"address", addrBuf},
                {"offset", addr - (DWORD)hMod}
            };
            if (ordinal > 0) info["ordinal"] = ordinal;
            if (!funcName.empty()) info["name"] = funcName;

            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "call_function") {
            std::string addrStr = arguments.value("address", "");
            std::string convention = arguments.value("convention", "stdcall");
            DWORD funcAddr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);

            if (funcAddr == 0) {
                return {{"content", {{{"type", "text"}, {"text", "Invalid function address"}}}}, {"isError", true}};
            }

            // Parse arguments
            DWORD args[8] = {};
            int argCount = 0;
            if (arguments.contains("args") && arguments["args"].is_array()) {
                for (auto& a : arguments["args"]) {
                    if (argCount >= 8) break;
                    if (a.is_string()) {
                        // Auto-detect: "0x..." = hex, otherwise decimal
                        std::string sv = a.get<std::string>();
                        if (sv.size() > 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X'))
                            args[argCount] = (DWORD)strtoul(sv.c_str(), nullptr, 16);
                        else
                            args[argCount] = (DWORD)strtoul(sv.c_str(), nullptr, 10);
                    } else {
                        args[argCount] = a.get<DWORD>();
                    }
                    argCount++;
                }
            }

            // Execute on game thread for safety (game functions expect game thread context)
            GameCallQueue::PendingCall call = {};
            call.address = funcAddr;
            for (int i = 0; i < argCount && i < 8; i++) call.args[i] = args[i];
            call.argCount = argCount;
            call.convention = 0;
            if (convention == "cdecl") call.convention = 1;
            else if (convention == "fastcall") call.convention = 2;

            bool success = GameCallQueue::CallOnGameThread(call, 5000);
            DWORD result = call.result;

            if (!success) {
                std::string errMsg;
                if (call.crashed) {
                    errMsg = "Function call crashed at " + addrStr;
                    auto crashes = CrashCatcher::GetCrashLog();
                    if (!crashes.empty()) {
                        auto& last = crashes.back();
                        char detail[256];
                        snprintf(detail, sizeof(detail),
                            ". %s at 0x%08X (%s+0x%X)",
                            CrashCatcher::GetExceptionName(last.exceptionCode),
                            last.exceptionAddress, last.moduleName, last.moduleOffset);
                        errMsg += detail;
                    }
                } else {
                    errMsg = "Function call timed out (game not running or paused?)";
                }
                return {{"content", {{{"type", "text"}, {"text", errMsg}}}}, {"isError", true}};
            }

            char retBuf[16]; snprintf(retBuf, sizeof(retBuf), "0x%08X", result);
            json argList = json::array();
            for (int i = 0; i < argCount; i++) {
                char ab[16]; snprintf(ab, sizeof(ab), "0x%08X", args[i]);
                argList.push_back(std::string(ab));
            }

            json info = {
                {"address", addrStr},
                {"convention", convention},
                {"args", argList},
                {"return_value", retBuf},
                {"return_decimal", (int)result}
            };
            return {{"content", {{{"type", "text"}, {"text", info.dump(2)}}}}};
        }

        if (name == "read_memory") {
            std::string addrStr = arguments.value("address", "");
            int size = arguments.value("size", 64);
            std::string format = arguments.value("format", "all");

            if (addrStr.empty()) {
                return {{"content", {{{"type", "text"}, {"text", "address is required"}}}}, {"isError", true}};
            }
            if (size < 1) size = 1;
            if (size > 4096) size = 4096;

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            if (addr == 0) {
                return {{"content", {{{"type", "text"}, {"text", "Invalid address: " + addrStr}}}}, {"isError", true}};
            }

            // Read memory with SEH protection
            std::vector<BYTE> buffer(size, 0);
            bool readOk = SafeMemRead(addr, buffer.data(), size);

            if (!readOk) {
                return {{"content", {{{"type", "text"}, {"text", "Access violation reading address " + addrStr}}}}, {"isError", true}};
            }

            json result;
            result["address"] = addrStr;
            result["size"] = size;

            // Hex dump
            if (format == "hex" || format == "all") {
                std::string hexStr;
                char hexBuf[4];
                for (int i = 0; i < size; i++) {
                    if (i > 0 && i % 16 == 0) hexStr += "\n";
                    else if (i > 0) hexStr += " ";
                    snprintf(hexBuf, sizeof(hexBuf), "%02X", buffer[i]);
                    hexStr += hexBuf;
                }
                result["hex"] = hexStr;
            }

            // DWORD values
            if (format == "dwords" || format == "all") {
                json dwords = json::array();
                for (int i = 0; i + 3 < size; i += 4) {
                    DWORD val = *(DWORD*)(buffer.data() + i);
                    char addrBuf[16];
                    snprintf(addrBuf, sizeof(addrBuf), "0x%08X", addr + i);
                    dwords.push_back({{"offset", i}, {"address", addrBuf}, {"value", val},
                        {"hex", (std::stringstream() << "0x" << std::hex << std::uppercase << val).str()}});
                }
                result["dwords"] = dwords;
            }

            // ASCII
            if (format == "ascii" || format == "all") {
                std::string ascii;
                for (int i = 0; i < size; i++) {
                    ascii += (buffer[i] >= 32 && buffer[i] < 127) ? (char)buffer[i] : '.';
                }
                result["ascii"] = ascii;
            }

            return {{"content", {{{"type", "text"}, {"text", result.dump(2)}}}}};
        }

        if (name == "write_memory") {
            std::string addrStr = arguments.value("address", "");
            if (addrStr.empty()) {
                return {{"content", {{{"type", "text"}, {"text", "address is required"}}}}, {"isError", true}};
            }

            DWORD addr = (DWORD)strtoul(addrStr.c_str(), nullptr, 16);
            if (addr == 0) {
                return {{"content", {{{"type", "text"}, {"text", "Invalid address: " + addrStr}}}}, {"isError", true}};
            }

            std::vector<BYTE> bytes;

            if (arguments.contains("dword")) {
                DWORD val = arguments["dword"].get<DWORD>();
                bytes.resize(4);
                *(DWORD*)bytes.data() = val;
            } else if (arguments.contains("bytes")) {
                std::string hexStr = arguments["bytes"].get<std::string>();
                // Parse hex string (supports "90 90 90" or "909090")
                for (size_t i = 0; i < hexStr.size(); i++) {
                    if (isspace(hexStr[i])) continue;
                    if (i + 1 < hexStr.size() && isxdigit(hexStr[i]) && isxdigit(hexStr[i+1])) {
                        char tmp[3] = { hexStr[i], hexStr[i+1], 0 };
                        bytes.push_back((BYTE)strtoul(tmp, nullptr, 16));
                        i++; // skip second nibble
                    }
                }
            } else {
                return {{"content", {{{"type", "text"}, {"text", "Either 'bytes' or 'dword' parameter required"}}}}, {"isError", true}};
            }

            if (bytes.empty()) {
                return {{"content", {{{"type", "text"}, {"text", "No bytes to write"}}}}, {"isError", true}};
            }

            // Read original bytes first (for undo/reporting)
            std::vector<BYTE> original(bytes.size(), 0);
            if (!SafeMemRead(addr, original.data(), bytes.size())) {
                return {{"content", {{{"type", "text"}, {"text", "Access violation reading original bytes at " + addrStr}}}}, {"isError", true}};
            }

            // Make memory writable, write, restore protection
            DWORD oldProtect;
            if (!VirtualProtect((void*)addr, bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                return {{"content", {{{"type", "text"}, {"text", "VirtualProtect failed at " + addrStr}}}}, {"isError", true}};
            }

            bool writeOk = SafeMemWrite(addr, bytes.data(), bytes.size());

            VirtualProtect((void*)addr, bytes.size(), oldProtect, &oldProtect);

            if (!writeOk) {
                return {{"content", {{{"type", "text"}, {"text", "Access violation writing to " + addrStr}}}}, {"isError", true}};
            }

            // Build response with original and new values
            std::string origHex, newHex;
            char hexBuf[4];
            for (size_t i = 0; i < bytes.size(); i++) {
                if (i > 0) { origHex += " "; newHex += " "; }
                snprintf(hexBuf, sizeof(hexBuf), "%02X", original[i]); origHex += hexBuf;
                snprintf(hexBuf, sizeof(hexBuf), "%02X", bytes[i]); newHex += hexBuf;
            }

            json result = {
                {"address", addrStr},
                {"bytes_written", bytes.size()},
                {"original", origHex},
                {"written", newHex}
            };

            return {{"content", {{{"type", "text"}, {"text", result.dump(2)}}}}};
        }

        return {
            {"content", {{
                {"type", "text"},
                {"text", "Unknown tool: " + name}
            }}},
            {"isError", true}
        };
    }

    // Process a JSON-RPC request and return a response
    json ProcessRequest(const json& request) {
        std::string method = request.value("method", "");
        auto id = request.value("id", json(nullptr));
        json params = request.value("params", json::object());

        json result;

        if (method == "initialize") {
            result = {
                {"protocolVersion", MCP_VERSION},
                {"capabilities", {
                    {"tools", {{"listChanged", false}}}
                }},
                {"serverInfo", ServerInfo()}
            };
        }
        else if (method == "notifications/initialized") {
            // Client notification — no response needed
            return json(nullptr);
        }
        else if (method == "tools/list") {
            result = {
                {"tools", ToolsList()}
            };
        }
        else if (method == "tools/call") {
            std::string toolName = params.value("name", "");
            json arguments = params.value("arguments", json::object());
            result = HandleToolCall(toolName, arguments);
        }
        else {
            // Unknown method
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {
                    {"code", -32601},
                    {"message", "Method not found: " + method}
                }}
            };
        }

        if (result.is_null()) {
            return json(nullptr);
        }

        return {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", result}
        };
    }

    std::atomic<bool> g_shouldRun{false};

    // Structured exception filter for the HTTP server thread
    static LONG WINAPI ServerExceptionFilter(PEXCEPTION_POINTERS) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void ServerThread() {
        // Translate SEH exceptions to C++ exceptions so we can catch them
        _set_se_translator([](unsigned int code, EXCEPTION_POINTERS*) {
            throw std::runtime_error("SEH exception in MCP server");
        });

        while (g_shouldRun) {
        // Create server (re-created on each restart)
        g_server = new httplib::Server();

        // CORS headers for browser/tool access
        g_server->set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
            {"Access-Control-Allow-Headers", "Content-Type"}
        });

        // OPTIONS preflight
        g_server->Options("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
        });

        // POST /mcp — JSON-RPC endpoint
        // Accepts an optional ?sessionId= query param to route response through SSE
        g_server->Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
            g_requestCount++;

            // Check for session ID to route response through SSE
            int sessionId = 0;
            if (req.has_param("sessionId")) {
                sessionId = std::atoi(req.get_param_value("sessionId").c_str());
            }

            try {
                json request = json::parse(req.body);
                json response = ProcessRequest(request);

                if (response.is_null()) {
                    // Notification — no response body
                    res.status = 202;
                    return;
                }

                if (sessionId > 0) {
                    // Route response through the SSE stream
                    SendSseMessage(sessionId, "message", response.dump());
                    res.status = 202;
                    res.set_content("", "text/plain");
                } else {
                    // Direct HTTP response (for non-SSE clients like curl)
                    res.set_content(response.dump(), "application/json");
                }
            }
            catch (const json::parse_error& e) {
                json error = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {
                        {"code", -32700},
                        {"message", std::string("Parse error: ") + e.what()}
                    }}
                };
                if (sessionId > 0) {
                    SendSseMessage(sessionId, "message", error.dump());
                    res.status = 202;
                } else {
                    res.set_content(error.dump(), "application/json");
                }
            }
            catch (...) {
                json error = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", {
                        {"code", -32603},
                        {"message", "Internal error"}
                    }}
                };
                if (sessionId > 0) {
                    SendSseMessage(sessionId, "message", error.dump());
                    res.status = 202;
                } else {
                    res.set_content(error.dump(), "application/json");
                }
            }
        });

        // GET /mcp/sse — Server-Sent Events endpoint
        // Keeps the connection open. Sends an initial 'endpoint' event with
        // the POST URL (including sessionId), then streams responses back.
        g_server->Get("/mcp/sse", [](const httplib::Request&, httplib::Response& res) {
            int sessionId = g_nextSessionId++;

            // Create session
            auto session = std::make_shared<SseSession>();
            {
                std::lock_guard<std::mutex> lock(g_sessionsMutex);
                g_sessions[sessionId] = session;
            }

            // Build the endpoint URL with session ID
            std::string postUrl = "/mcp?sessionId=" + std::to_string(sessionId);

            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            // Track whether we've sent the endpoint event for this connection
            auto sentEndpoint = std::make_shared<bool>(false);

            // Use chunked content provider to stream SSE events
            res.set_chunked_content_provider(
                "text/event-stream",
                [sessionId, session, postUrl, sentEndpoint](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    // Send the initial endpoint event
                    if (!*sentEndpoint) {
                        std::string endpointEvent = "event: endpoint\ndata: " + postUrl + "\n\n";
                        sink.write(endpointEvent.c_str(), endpointEvent.size());
                        *sentEndpoint = true;
                    }

                    // Wait for messages or shutdown
                    std::unique_lock<std::mutex> lock(session->mutex);
                    session->cv.wait_for(lock, std::chrono::seconds(15), [&session]() {
                        return !session->messages.empty() || session->closed;
                    });

                    if (session->closed) {
                        return false; // Close the stream
                    }

                    // Send all queued messages
                    while (!session->messages.empty()) {
                        std::string msg = session->messages.front();
                        session->messages.pop();
                        if (!sink.write(msg.c_str(), msg.size())) {
                            return false;
                        }
                    }

                    // If no messages, send a keep-alive comment
                    if (session->messages.empty()) {
                        std::string keepalive = ": keepalive\n\n";
                        sink.write(keepalive.c_str(), keepalive.size());
                    }

                    return true; // Keep connection open
                },
                [sessionId](bool /*success*/) {
                    // Cleanup session on disconnect
                    std::lock_guard<std::mutex> lock(g_sessionsMutex);
                    g_sessions.erase(sessionId);
                }
            );
        });

        // GET /health — simple health check
        g_server->Get("/health", [](const httplib::Request&, httplib::Response& res) {
            json health = {
                {"status", "ok"},
                {"server", "d2-mod-toolkit"},
                {"port", g_port},
                {"requests", g_requestCount.load()}
            };
            res.set_content(health.dump(), "application/json");
        });

        g_running = true;
        try {
            g_server->listen("127.0.0.1", g_port);
        } catch (...) {
            // Server crashed — will restart via loop
        }
        g_running = false;

        // Clean up server
        delete g_server;
        g_server = nullptr;

        // If we should still be running, wait before restarting
        if (g_shouldRun) {
            Sleep(1000); // brief delay before restart
        }
        } // end while(g_shouldRun)
    }
}

namespace McpServer {
    void Init(int port) {
        if (g_running) return;
        g_port = port;
        g_requestCount = 0;
        g_shouldRun = true;
        g_thread = std::thread(ServerThread);
        g_thread.detach();
    }

    void Shutdown() {
        g_shouldRun = false;  // stop restart loop
        if (g_server) {
            g_server->stop();
        }
        // Wait for thread to exit
        for (int i = 0; i < 30 && g_running; ++i) {
            Sleep(100);
        }
        if (g_server) {
            delete g_server;
            g_server = nullptr;
        }
    }

    bool IsRunning() {
        return g_running;
    }

    int GetPort() {
        return g_port;
    }

    int GetRequestCount() {
        return g_requestCount;
    }
}
