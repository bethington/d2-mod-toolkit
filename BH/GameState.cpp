#include "GameState.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
    std::mutex g_mutex;
    GameState::PlayerState g_player;
    GameState::BeltState g_belt;
    std::vector<GameState::NearbyUnit> g_units;
    bool g_ready = false;

    const char* GetClassName(int classId) {
        switch (classId) {
            case 0: return "Amazon";
            case 1: return "Sorceress";
            case 2: return "Necromancer";
            case 3: return "Paladin";
            case 4: return "Barbarian";
            case 5: return "Druid";
            case 6: return "Assassin";
            default: return "Unknown";
        }
    }

    void UpdatePlayerState() {
        GameState::PlayerState ps;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_player = ps;
            return;
        }

        ps.valid = true;
        ps.classId = pPlayer->dwTxtFileNo;

        if (pPlayer->pPlayerData) {
            strncpy_s(ps.name, pPlayer->pPlayerData->szName, sizeof(ps.name) - 1);
        }

        ps.level = D2COMMON_GetUnitStat(pPlayer, STAT_LEVEL, 0);
        ps.hp = D2COMMON_GetUnitStat(pPlayer, STAT_HP, 0);
        ps.maxHp = D2COMMON_GetUnitStat(pPlayer, STAT_MAXHP, 0);
        ps.mana = D2COMMON_GetUnitStat(pPlayer, STAT_MANA, 0);
        ps.maxMana = D2COMMON_GetUnitStat(pPlayer, STAT_MAXMANA, 0);
        ps.stamina = D2COMMON_GetUnitStat(pPlayer, STAT_STAMINA, 0);
        ps.maxStamina = D2COMMON_GetUnitStat(pPlayer, STAT_MAXSTAMINA, 0);
        ps.gold = D2COMMON_GetUnitStat(pPlayer, STAT_GOLD, 0);
        ps.goldStash = D2COMMON_GetUnitStat(pPlayer, STAT_GOLDBANK, 0);

        ps.fcr = D2COMMON_GetUnitStat(pPlayer, STAT_FASTERCAST, 0);
        ps.fhr = D2COMMON_GetUnitStat(pPlayer, STAT_FASTERHITRECOVERY, 0);
        ps.fbr = D2COMMON_GetUnitStat(pPlayer, STAT_FASTERBLOCK, 0);
        ps.ias = D2COMMON_GetUnitStat(pPlayer, STAT_IAS, 0);
        ps.frw = D2COMMON_GetUnitStat(pPlayer, STAT_FASTERRUNWALK, 0);
        ps.mf = D2COMMON_GetUnitStat(pPlayer, STAT_MAGICFIND, 0);

        ps.fireRes = D2COMMON_GetUnitStat(pPlayer, STAT_FIRERESIST, 0);
        ps.coldRes = D2COMMON_GetUnitStat(pPlayer, STAT_COLDRESIST, 0);
        ps.lightRes = D2COMMON_GetUnitStat(pPlayer, STAT_LIGHTNINGRESIST, 0);
        ps.poisonRes = D2COMMON_GetUnitStat(pPlayer, STAT_POISONRESIST, 0);

        if (pPlayer->pPath) {
            ps.x = pPlayer->pPath->xPos;
            ps.y = pPlayer->pPath->yPos;

            if (pPlayer->pPath->pRoom1 &&
                pPlayer->pPath->pRoom1->pRoom2 &&
                pPlayer->pPath->pRoom1->pRoom2->pLevel) {
                ps.area = pPlayer->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
            }
        }

        ps.act = pPlayer->dwAct;

        std::lock_guard<std::mutex> lock(g_mutex);
        g_player = ps;
    }

    void UpdateBeltState() {
        GameState::BeltState bs;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pInventory) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_belt = bs;
            return;
        }

        bs.rows = 4; // assume max belt size, show all 16 slots
        bs.columns = 4;

        // Walk inventory items — belt items have NodePage == NODEPAGE_BELTSLOTS
        // ItemPath.dwPosX contains the flat belt slot index (0-15)
        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                int idx = 0;
                if (pItem->pPath) {
                    ItemPath* ip = (ItemPath*)pItem->pPath;
                    idx = (int)ip->dwPosX;
                }

                if (idx >= 0 && idx < 16) {
                    GameState::BeltSlot& slot = bs.slots[idx];
                    slot.occupied = true;
                    slot.itemCode = pItem->dwTxtFileNo;

                    // Potion short names: HP1-HP5, MP1-MP5, RJ/FRJ
                    // PD2-S12 TxtFileNo codes (confirmed in-game)
                    int code = pItem->dwTxtFileNo;
                    const char* label = nullptr;
                    switch (code) {
                        // Health potions (PD2-S12 codes)
                        case 602: label = "HP1"; break;  // Minor Healing
                        case 603: label = "HP2"; break;  // Light Healing
                        case 604: label = "HP3"; break;  // Healing
                        case 605: label = "HP4"; break;  // Greater Healing
                        case 606: label = "HP5"; break;  // Super Healing
                        // Mana potions (PD2-S12 codes)
                        case 607: label = "MP1"; break;  // Minor Mana
                        case 608: label = "MP2"; break;  // Light Mana
                        case 609: label = "MP3"; break;  // Mana
                        case 610: label = "MP4"; break;  // Greater Mana
                        case 611: label = "MP5"; break;  // Super Mana
                        // Rejuvenation potions
                        case 517: label = "RJ";  break;  // Rejuvenation
                        case 518: label = "FRJ"; break;  // Full Rejuvenation
                        // Utility potions
                        case 515: label = "Antd"; break; // Antidote
                        case 516: label = "Thaw"; break; // Thawing
                        case 519: label = "Stam"; break; // Stamina
                        // Scrolls
                        case 529: label = "TP";  break;  // Town Portal
                        case 530: label = "ID";  break;  // Identify
                        // Alternate PD2 codes (older seasons / funmixxed)
                        case 589: label = "HP1"; break;
                        case 590: label = "HP2"; break;
                        case 591: label = "HP3"; break;
                        case 592: label = "HP4"; break;
                        case 593: label = "HP5"; break;
                        case 594: label = "MP1"; break;
                        case 595: label = "MP2"; break;
                        case 596: label = "MP3"; break;
                        case 597: label = "MP4"; break;
                        case 598: label = "MP5"; break;
                        case 531: label = "FRJ"; break;  // PD2 Full Rejuv (confirmed)
                        default: break;
                    }
                    if (label) {
                        strncpy_s(slot.name, label, sizeof(slot.name) - 1);
                    } else {
                        snprintf(slot.name, sizeof(slot.name), "#%d", code);
                    }
                }
            }

            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_belt = bs;
    }

    void UpdateNearbyUnits() {
        std::vector<GameState::NearbyUnit> units;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pPath || !pPlayer->pAct) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_units = units;
            return;
        }

        int playerX = pPlayer->pPath->xPos;
        int playerY = pPlayer->pPath->yPos;

        for (Room1* room1 = pPlayer->pAct->pRoom1; room1; room1 = room1->pRoomNext) {
            for (UnitAny* pUnit = room1->pUnitFirst; pUnit; pUnit = pUnit->pListNext) {
                if (pUnit == pPlayer) continue;
                if (pUnit->dwType != 0 && pUnit->dwType != 1 && pUnit->dwType != 4) continue;

                GameState::NearbyUnit nu;
                nu.type = pUnit->dwType;
                nu.classId = pUnit->dwTxtFileNo;
                nu.unitId = pUnit->dwUnitId;
                nu.mode = pUnit->dwMode;

                if (pUnit->pPath) {
                    nu.x = pUnit->pPath->xPos;
                    nu.y = pUnit->pPath->yPos;
                } else {
                    nu.x = pUnit->wX;
                    nu.y = pUnit->wY;
                }

                int dx = nu.x - playerX;
                int dy = nu.y - playerY;
                nu.distance = (int)sqrt((double)(dx * dx + dy * dy));

                if (pUnit->dwType == 1) { // Monster
                    nu.hp = D2COMMON_GetUnitStat(pUnit, STAT_HP, 0);
                    nu.maxHp = D2COMMON_GetUnitStat(pUnit, STAT_MAXHP, 0);

                    if (pUnit->pMonsterData) {
                        // Check monster flags for boss/champion/minion
                        // Flag 0x0E = boss, champion, etc.
                        BYTE flags = pUnit->pMonsterData->fBoss;
                        nu.isBoss = (flags & 1) != 0;
                        nu.isChampion = (flags & 2) != 0;
                        nu.isMinion = (flags & 4) != 0;
                    }
                    snprintf(nu.name, sizeof(nu.name), "Monster#%d", pUnit->dwTxtFileNo);
                } else if (pUnit->dwType == 0) { // Player
                    nu.hp = D2COMMON_GetUnitStat(pUnit, STAT_HP, 0);
                    nu.maxHp = D2COMMON_GetUnitStat(pUnit, STAT_MAXHP, 0);
                    if (pUnit->pPlayerData) {
                        strncpy_s(nu.name, pUnit->pPlayerData->szName, sizeof(nu.name) - 1);
                    }
                } else if (pUnit->dwType == 4) { // Item
                    snprintf(nu.name, sizeof(nu.name), "Item#%d", pUnit->dwTxtFileNo);
                }

                units.push_back(nu);
            }
        }

        // Sort by distance
        std::sort(units.begin(), units.end(),
            [](const GameState::NearbyUnit& a, const GameState::NearbyUnit& b) {
                return a.distance < b.distance;
            });

        std::lock_guard<std::mutex> lock(g_mutex);
        g_units = std::move(units);
    }
}

namespace GameState {

    void Update() {
        g_ready = IsGameReady();
        if (!g_ready) return;

        UpdatePlayerState();
        UpdateBeltState();
        UpdateNearbyUnits();
    }

    PlayerState GetPlayerState() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_player;
    }

    BeltState GetBeltState() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_belt;
    }

    std::vector<NearbyUnit> GetNearbyUnits(int maxDistance) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (maxDistance <= 0) return g_units;

        std::vector<NearbyUnit> filtered;
        for (const auto& u : g_units) {
            if (u.distance <= maxDistance) filtered.push_back(u);
        }
        return filtered;
    }

    bool IsGameReady() {
        return ::IsGameReady();
    }
}
