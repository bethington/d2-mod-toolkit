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

    static bool SafeMemWrite(DWORD addr, const void* src, size_t size) {
        __try {
            memcpy((void*)addr, src, size);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
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
            {"name", "get_controls"},
            {"description", "Dump all UI controls on the current screen (buttons, editboxes, etc.) with positions and states. Useful for debugging menu navigation."},
            {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}
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
                {"required", {"address"}}
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
                {"required", {"name", "address", "bytes"}}
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
                {"required", {"name"}}
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
                {"required", {"patches"}}
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
                {"required", {"address"}}
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
                {"required", {"address"}}
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

                        json item = {
                            {"code", (int)pItem->dwTxtFileNo},
                            {"name", name[0] ? name : "?"},
                            {"location", loc},
                            {"node_page", np},
                            {"unit_id", (int)pItem->dwUnitId}
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

        if (name == "get_controls") {
            Control* pCtrl = *p_D2WIN_FirstControl;
            json controls = json::array();
            while (pCtrl) {
                const char* typeNames[] = {"unknown", "editbox", "image", "unknown3", "unknown4", "unknown5", "button", "list"};
                const char* typeName = (pCtrl->dwType <= 7) ? typeNames[pCtrl->dwType] : "unknown";
                json ctrl = {
                    {"type", typeName},
                    {"type_id", (int)pCtrl->dwType},
                    {"state", (int)pCtrl->dwState},
                    {"x", (int)pCtrl->dwPosX},
                    {"y", (int)pCtrl->dwPosY},
                    {"w", (int)pCtrl->dwSizeX},
                    {"h", (int)pCtrl->dwSizeY},
                    {"has_on_press", pCtrl->OnPress != nullptr}
                };
                controls.push_back(ctrl);
                pCtrl = pCtrl->pNext;
            }
            json info = {{"count", controls.size()}, {"controls", controls}};
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

    void ServerThread() {
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
        g_server->listen("127.0.0.1", g_port);
        g_running = false;
    }
}

namespace McpServer {
    void Init(int port) {
        if (g_running) return;
        g_port = port;
        g_requestCount = 0;
        g_thread = std::thread(ServerThread);
        g_thread.detach();
    }

    void Shutdown() {
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
