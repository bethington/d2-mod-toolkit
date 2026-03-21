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
    bool IsPotion(DWORD code) {
        return IsHpPotion(code) || IsMpPotion(code) || IsRejuv(code);
    }

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

        // Check if belt has room
        if (!BeltHasEmptySlots(pPlayer)) return;

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
