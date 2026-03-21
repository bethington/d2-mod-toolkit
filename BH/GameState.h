#pragma once

// GameState - Reads live game state for the debug panel and MCP server.
// Thread-safe snapshot model: game thread populates, UI/MCP threads read.

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace GameState {

    struct PlayerState {
        bool valid = false;
        char name[16] = {};
        int classId = 0;        // 0=Amazon, 1=Sorc, 2=Necro, 3=Pala, 4=Barb, 5=Druid, 6=Assassin
        int level = 0;
        int hp = 0;             // current (divide by 256 for display)
        int maxHp = 0;          // max (divide by 256 for display)
        int mana = 0;           // current (divide by 256 for display)
        int maxMana = 0;        // max (divide by 256 for display)
        int stamina = 0;
        int maxStamina = 0;
        int x = 0;              // world position
        int y = 0;
        int area = 0;           // level number
        int act = 0;            // 0-4
        int gold = 0;           // gold on person
        int goldStash = 0;      // gold in stash
        int fcr = 0;            // faster cast rate
        int fhr = 0;            // faster hit recovery
        int fbr = 0;            // faster block rate
        int ias = 0;            // increased attack speed
        int frw = 0;            // faster run/walk
        int mf = 0;             // magic find
        int fireRes = 0;
        int coldRes = 0;
        int lightRes = 0;
        int poisonRes = 0;
    };

    struct BeltSlot {
        bool occupied = false;
        int itemCode = 0;       // TxtFileNo
        char name[32] = {};     // item name/code
        int quantity = 0;       // for stackable items
    };

    struct BeltState {
        int columns = 4;
        int rows = 1;           // 1-4 depending on belt type
        BeltSlot slots[16];     // 4 columns x 4 rows max
    };

    struct NearbyUnit {
        int type = 0;           // UNIT_PLAYER=0, UNIT_MONSTER=1, UNIT_OBJECT=2, UNIT_ITEM=4
        int classId = 0;
        int unitId = 0;
        int x = 0;
        int y = 0;
        int hp = 0;             // current (divide by 256)
        int maxHp = 0;          // max (divide by 256)
        int mode = 0;           // 0=dead, etc.
        int distance = 0;       // from player
        char name[64] = {};
        bool isBoss = false;
        bool isChampion = false;
        bool isMinion = false;
    };

    // Update game state snapshot (call from game thread)
    void Update();

    // Thread-safe accessors (call from any thread)
    PlayerState GetPlayerState();
    BeltState GetBeltState();
    std::vector<NearbyUnit> GetNearbyUnits(int maxDistance = 40);

    // Is the game in a state where we can read data?
    bool IsGameReady();
}
