#include "AutoPickup.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"

#include <mutex>
#include <cmath>

namespace {
    std::mutex g_mutex;
    AutoPickup::Config g_config;
    DWORD g_lastPickTime = 0;

    // Known potion TxtFileNo codes (PD2-S12 + older PD2)
    bool IsHpPotion(DWORD code) {
        return (code >= 602 && code <= 606) || (code >= 589 && code <= 593);
    }
    bool IsMpPotion(DWORD code) {
        return (code >= 607 && code <= 611) || (code >= 594 && code <= 598);
    }
    bool IsRejuv(DWORD code) {
        return code == 517 || code == 518 || code == 531;
    }

    // Scroll codes (vanilla + PD2)
    bool IsTpScroll(DWORD code) { return code == 529; }
    bool IsIdScroll(DWORD code) { return code == 530; }

    // Tome codes (vanilla + PD2)
    // Vanilla: tbk=518, ibk=519. PD2: TP Tome=520, ID Tome=521 (may vary)
    bool IsTpTome(DWORD code) { return code == 518 || code == 520; }
    bool IsIdTome(DWORD code) { return code == 519 || code == 521; }

    static const int MAX_TOME_CHARGES = 20;

    // Find a tome in inventory and return its current charge count
    // Returns -1 if no tome found
    int GetTomeCharges(UnitAny* pPlayer, bool isTp) {
        if (!pPlayer || !pPlayer->pInventory) return -1;

        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData) {
                // Tomes are in inventory (NodePage == NODEPAGE_STORAGE)
                DWORD code = pItem->dwTxtFileNo;
                if ((isTp && IsTpTome(code)) || (!isTp && IsIdTome(code))) {
                    int charges = D2COMMON_GetUnitStat(pItem, STAT_AMMOQUANTITY, 0);
                    return charges;
                }
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }
        return -1; // no tome found
    }

    // Track whether tomes need scrolls (cached per frame)
    bool g_needTpScrolls = false;
    bool g_needIdScrolls = false;

    // Check if belt has any empty slots
    bool BeltHasEmptySlots(UnitAny* pPlayer) {
        if (!pPlayer || !pPlayer->pInventory) return false;

        // Count occupied belt slots
        int occupied = 0;
        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                occupied++;
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }

        // A 4-column belt has 4-16 slots depending on belt type
        // If we have fewer items than 4 (first row), there are definitely empty slots
        // For simplicity, assume at least 4 slots and check if any are empty
        return occupied < 16; // conservative — always try if not completely full
    }

    // Send pickup packet (0x16) for a ground item
    void PickupItem(DWORD itemId) {
        BYTE packet[13] = {};
        packet[0] = 0x16;
        *(DWORD*)&packet[1] = 0x04; // UNIT_ITEM type
        *(DWORD*)&packet[5] = itemId;
        D2NET_SendPacket(13, 0, packet);
    }

    // Check if item should be picked up based on config
    bool ShouldPickup(DWORD code, const AutoPickup::Config& cfg) {
        if (cfg.pickHpPotions && IsHpPotion(code)) return true;
        if (cfg.pickMpPotions && IsMpPotion(code)) return true;
        if (cfg.pickRejuvs && IsRejuv(code)) return true;
        if (cfg.pickTpScrolls && IsTpScroll(code) && g_needTpScrolls) return true;
        if (cfg.pickIdScrolls && IsIdScroll(code) && g_needIdScrolls) return true;
        return false;
    }
}

namespace AutoPickup {

    void Update() {
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            cfg = g_config;
        }

        if (!cfg.enabled || !IsGameReady()) return;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pPath || !pPlayer->pAct || !pPlayer->pInventory) return;

        // Don't pickup if cursor is holding an item
        if (pPlayer->pInventory->pCursorItem) return;

        // Cooldown
        DWORD now = GetTickCount();
        if (now - g_lastPickTime < (DWORD)cfg.cooldownMs) return;

        // Check tome charges for scroll pickup
        g_needTpScrolls = false;
        g_needIdScrolls = false;
        if (cfg.pickTpScrolls) {
            int tpCharges = GetTomeCharges(pPlayer, true);
            g_needTpScrolls = (tpCharges >= 0 && tpCharges < MAX_TOME_CHARGES);
        }
        if (cfg.pickIdScrolls) {
            int idCharges = GetTomeCharges(pPlayer, false);
            g_needIdScrolls = (idCharges >= 0 && idCharges < MAX_TOME_CHARGES);
        }

        // Check if belt has room (for potions) or tomes need scrolls
        bool beltHasRoom = BeltHasEmptySlots(pPlayer);
        bool needScrolls = g_needTpScrolls || g_needIdScrolls;
        if (!beltHasRoom && !needScrolls) return;

        int playerX = pPlayer->pPath->xPos;
        int playerY = pPlayer->pPath->yPos;

        // Scan nearby ground items for potions
        UnitAny* bestItem = nullptr;
        int bestDist = cfg.maxDistance + 1;

        for (Room1* room1 = pPlayer->pAct->pRoom1; room1; room1 = room1->pRoomNext) {
            for (UnitAny* pUnit = room1->pUnitFirst; pUnit; pUnit = pUnit->pListNext) {
                if (pUnit->dwType != 4) continue; // UNIT_ITEM only
                if (!pUnit->pItemData) continue;

                // Ground items have mode 3 or 5 (on ground)
                if (pUnit->dwMode != 3 && pUnit->dwMode != 5) continue;

                DWORD code = pUnit->dwTxtFileNo;
                if (!ShouldPickup(code, cfg)) continue;

                // Calculate distance
                int ix = 0, iy = 0;
                if (pUnit->pPath) {
                    ItemPath* ip = (ItemPath*)pUnit->pPath;
                    ix = ip->dwPosX;
                    iy = ip->dwPosY;
                }

                int dx = ix - playerX;
                int dy = iy - playerY;
                int dist = (int)sqrt((double)(dx * dx + dy * dy));

                if (dist < bestDist) {
                    bestDist = dist;
                    bestItem = pUnit;
                }
            }
        }

        if (bestItem && bestDist <= cfg.maxDistance) {
            PickupItem(bestItem->dwUnitId);
            g_lastPickTime = now;
        }
    }

    Config GetConfig() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_config;
    }

    void SetConfig(const Config& cfg) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = cfg;
    }
}
