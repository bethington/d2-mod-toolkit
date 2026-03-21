#include "GameState.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

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

        // Helper macro to reduce boilerplate
        #define GS(stat) D2COMMON_GetUnitStat(pPlayer, stat, 0)

        // Core
        ps.level = GS(STAT_LEVEL);
        ps.hp = GS(STAT_HP);
        ps.maxHp = GS(STAT_MAXHP);
        ps.mana = GS(STAT_MANA);
        ps.maxMana = GS(STAT_MAXMANA);
        ps.stamina = GS(STAT_STAMINA);
        ps.maxStamina = GS(STAT_MAXSTAMINA);
        ps.gold = GS(STAT_GOLD);
        ps.goldStash = GS(STAT_GOLDBANK);
        ps.addXp = GS(STAT_ADDEXPERIENCE);

        // Resistances (raw — penalty applied in display)
        ps.fireRes = GS(STAT_FIRERESIST);
        ps.maxFireRes = GS(STAT_MAXFIRERESIST) + 75;
        ps.coldRes = GS(STAT_COLDRESIST);
        ps.maxColdRes = GS(STAT_MAXCOLDRESIST) + 75;
        ps.lightRes = GS(STAT_LIGHTNINGRESIST);
        ps.maxLightRes = GS(STAT_MAXLIGHTNINGRESIST) + 75;
        ps.poisonRes = GS(STAT_POISONRESIST);
        ps.maxPoisonRes = GS(STAT_MAXPOISONRESIST) + 75;
        ps.curseRes = GS(STAT_CURSE_EFFECTIVENESS);
        ps.curseLenReduce = GS(STAT_CURSERESISTANCE);
        ps.poisonLenReduce = GS(STAT_POISONLENGTHREDUCTION);
        ps.halfFreeze = GS(STAT_HALFFREEZEDURATION);
        ps.cannotBeFrozen = GS(STAT_CANNOTBEFROZEN);

        // Cap max resistances
        if (ps.maxFireRes > MAX_PLAYER_RESISTANCE) ps.maxFireRes = MAX_PLAYER_RESISTANCE;
        if (ps.maxColdRes > MAX_PLAYER_RESISTANCE) ps.maxColdRes = MAX_PLAYER_RESISTANCE;
        if (ps.maxLightRes > MAX_PLAYER_RESISTANCE) ps.maxLightRes = MAX_PLAYER_RESISTANCE;
        if (ps.maxPoisonRes > MAX_PLAYER_RESISTANCE) ps.maxPoisonRes = MAX_PLAYER_RESISTANCE;

        // Absorption
        ps.fireAbsorb = GS(STAT_FIREABSORB);        ps.fireAbsorbPct = GS(STAT_FIREABSORBPERCENT);
        ps.coldAbsorb = GS(STAT_COLDABSORB);        ps.coldAbsorbPct = GS(STAT_COLDABSORBPERCENT);
        ps.lightAbsorb = GS(STAT_LIGHTNINGABSORB);  ps.lightAbsorbPct = GS(STAT_LIGHTNINGABSORBPERCENT);
        ps.magicAbsorb = GS(STAT_MAGICABSORB);      ps.magicAbsorbPct = GS(STAT_MAGICABSORBPERCENT);

        // Damage reduction
        ps.dmgReduction = GS(STAT_DMGREDUCTION);        ps.dmgReductionPct = GS(STAT_DMGREDUCTIONPCT);
        ps.magDmgReduction = GS(STAT_MAGICDMGREDUCTION); ps.magDmgReductionPct = GS(STAT_MAGICDMGREDUCTIONPCT);
        ps.attackerTakesDmg = GS(STAT_ATTACKERTAKESDAMAGE);
        ps.attackerTakesLtng = GS(STAT_ATTACKERTAKESLTNGDMG);

        // Elemental mastery & pierce
        ps.fireMastery = GS(STAT_FIREMASTERY);      ps.firePierce = GS(STAT_PSENEMYFIRERESREDUC);
        ps.coldMastery = GS(STAT_COLDMASTERY);      ps.coldPierce = GS(STAT_PSENEMYCOLDRESREDUC);
        ps.lightMastery = GS(STAT_LIGHTNINGMASTERY); ps.lightPierce = GS(STAT_PSENEMYLIGHTNRESREDUC);
        ps.poisonMastery = GS(STAT_POISONMASTERY);  ps.poisonPierce = GS(STAT_PSENEMYPSNRESREDUC);
        ps.magicMastery = GS(STAT_PASSIVEMAGICDMGMASTERY); ps.magicPierce = GS(STAT_PASSIVEMAGICRESREDUC);

        // Attack / defense
        ps.dexterity = GS(STAT_DEXTERITY);
        ps.attackRating = GS(STAT_ATTACKRATING);
        ps.defense = GS(STAT_DEFENSE);
        ps.minDmg = GS(STAT_MINIMUMDAMAGE);         ps.maxDmg = GS(STAT_MAXIMUMDAMAGE);
        ps.minDmg2 = GS(STAT_SECONDARYMINIMUMDAMAGE); ps.maxDmg2 = GS(STAT_SECONDARYMAXIMUMDAMAGE);

        // Rates
        ps.fcr = GS(STAT_FASTERCAST);    ps.fhr = GS(STAT_FASTERHITRECOVERY);
        ps.fbr = GS(STAT_FASTERBLOCK);   ps.ias = GS(STAT_IAS);
        ps.frw = GS(STAT_FASTERRUNWALK); ps.attackRate = GS(STAT_ATTACKRATE);

        // Combat
        ps.crushingBlow = GS(STAT_CRUSHINGBLOW);
        ps.openWounds = GS(STAT_OPENWOUNDS);     ps.deepWounds = GS(STAT_DEEP_WOUNDS);
        ps.deadlyStrike = GS(STAT_DEADLYSTRIKE);  ps.maxDeadlyStrike = GS(STAT_MAXDEADLYSTRIKE);
        ps.criticalStrike = GS(STAT_CRITICALSTRIKE);
        ps.lifeLeech = GS(STAT_LIFELEECH);        ps.manaLeech = GS(STAT_MANALEECH);
        ps.piercingAttack = GS(STAT_PIERCINGATTACK); ps.pierce = GS(STAT_PIERCE);
        ps.lifePerKill = GS(STAT_LIFEAFTEREACHKILL); ps.manaPerKill = GS(STAT_MANAAFTEREACHKILL);

        // Elemental damage
        ps.minFireDmg = GS(STAT_MINIMUMFIREDAMAGE);     ps.maxFireDmg = GS(STAT_MAXIMUMFIREDAMAGE);
        ps.minColdDmg = GS(STAT_MINIMUMCOLDDAMAGE);     ps.maxColdDmg = GS(STAT_MAXIMUMCOLDDAMAGE);
        ps.minLightDmg = GS(STAT_MINIMUMLIGHTNINGDAMAGE); ps.maxLightDmg = GS(STAT_MAXIMUMLIGHTNINGDAMAGE);
        ps.minPoisonDmg = GS(STAT_MINIMUMPOISONDAMAGE); ps.maxPoisonDmg = GS(STAT_MAXIMUMPOISONDAMAGE);
        ps.poisonLength = GS(STAT_POISONDAMAGELENGTH);
        ps.poisonLenOverride = GS(STAT_SKILLPOISONOVERRIDELEN);
        ps.minMagicDmg = GS(STAT_MINIMUMMAGICALDAMAGE); ps.maxMagicDmg = GS(STAT_MAXIMUMMAGICALDAMAGE);
        ps.addedDamage = GS(STAT_ADDSDAMAGE);

        // Breakpoints — FCR
        {
            // FCR table per class (matching StatsDisplay.cpp)
            static const std::map<int, std::vector<int>> fcrTables = {
                { CLASS_AMA, { 7, 14, 22, 32, 48, 68, 99, 152 } },
                { CLASS_SOR, { 9, 20, 37, 63, 105, 200 } },
                { CLASS_BAR, { 9, 20, 37, 63, 105, 200 } },
                { CLASS_NEC, { 9, 18, 30, 48, 75, 125 } },
                { CLASS_PAL, { 9, 18, 30, 48, 75, 125 } },
                { CLASS_ASN, { 8, 16, 27, 42, 65, 102, 174 } },
                { CLASS_DRU, { 4, 10, 19, 30, 46, 68, 99, 163 } },
            };

            int fcrKey = pPlayer->dwTxtFileNo;
            // Check for lightning skills (Chain Lightning, Frozen Orb) which use different FCR
            try {
                if (pPlayer->pInfo && pPlayer->pInfo->pRightSkill &&
                    pPlayer->pInfo->pRightSkill->pSkillInfo) {
                    int skillId = pPlayer->pInfo->pRightSkill->pSkillInfo->wSkillId;
                    if (skillId == 53 || skillId == 64) fcrKey = 143;
                }
                if (D2COMMON_GetUnitState(pPlayer, 139)) fcrKey = 139;
                if (D2COMMON_GetUnitState(pPlayer, 140)) fcrKey = 140;
            } catch (...) {}

            auto it = fcrTables.find(fcrKey);
            // Fall back to class if alias not found
            if (it == fcrTables.end()) it = fcrTables.find(pPlayer->dwTxtFileNo);

            if (it != fcrTables.end()) {
                const auto& bps = it->second;
                ps.bpFCR.currentValue = ps.fcr;
                strncpy_s(ps.bpFCR.label, "FCR", sizeof(ps.bpFCR.label));
                ps.bpFCR.count = (int)bps.size();
                if (ps.bpFCR.count > 16) ps.bpFCR.count = 16;
                ps.bpFCR.activeIndex = -1;
                for (int i = 0; i < ps.bpFCR.count; i++) {
                    ps.bpFCR.values[i] = bps[i];
                    if (ps.fcr >= bps[i]) ps.bpFCR.activeIndex = i;
                }
            }
        }

        // Breakpoints — FHR
        {
            static const std::map<int, std::vector<int>> fhrTables = {
                { CLASS_AMA, { 6, 13, 20, 32, 52, 86, 174, 600 } },
                { CLASS_SOR, { 5, 9, 14, 20, 30, 42, 60, 86, 142, 280 } },
                { CLASS_NEC, { 5, 10, 16, 26, 39, 56, 86, 152, 377 } },
                { CLASS_DRU, { 5, 10, 16, 26, 39, 56, 86, 152, 377 } },
                { CLASS_BAR, { 7, 15, 27, 48, 86, 200 } },
                { CLASS_ASN, { 7, 15, 27, 48, 86, 200 } },
                { CLASS_PAL, { 7, 15, 27, 48, 86, 200 } },
            };

            int fhrKey = pPlayer->dwTxtFileNo;
            try {
                if (D2COMMON_GetUnitState(pPlayer, 139)) fhrKey = 139;
                if (D2COMMON_GetUnitState(pPlayer, 140)) fhrKey = 140;
            } catch (...) {}

            auto it = fhrTables.find(fhrKey);
            if (it == fhrTables.end()) it = fhrTables.find(pPlayer->dwTxtFileNo);

            if (it != fhrTables.end()) {
                const auto& bps = it->second;
                ps.bpFHR.currentValue = ps.fhr;
                strncpy_s(ps.bpFHR.label, "FHR", sizeof(ps.bpFHR.label));
                ps.bpFHR.count = (int)bps.size();
                if (ps.bpFHR.count > 16) ps.bpFHR.count = 16;
                ps.bpFHR.activeIndex = -1;
                for (int i = 0; i < ps.bpFHR.count; i++) {
                    ps.bpFHR.values[i] = bps[i];
                    if (ps.fhr >= bps[i]) ps.bpFHR.activeIndex = i;
                }
            }
        }

        // Breakpoint skill name — wrapped in safety checks since data tables
        // may not be initialized during loading screens
        try {
            if (pPlayer->pInfo && pPlayer->pInfo->pRightSkill &&
                pPlayer->pInfo->pRightSkill->pSkillInfo) {
                int skillDesc = pPlayer->pInfo->pRightSkill->pSkillInfo->wSkillDesc;
                sgptDataTable* pDataTbl = *p_D2COMMON_sgptDataTable;
                if (skillDesc > 0 && pDataTbl && pDataTbl->pSkillDescTxt) {
                    SkillDescTxt* pSkillDescTxt = &pDataTbl->pSkillDescTxt[skillDesc];
                    if (pSkillDescTxt && pSkillDescTxt->wStrName > 0) {
                        wchar_t* wName = GetTblEntryByIndex(pSkillDescTxt->wStrName, TBLOFFSET_STRING);
                        if (wName) {
                            WideCharToMultiByte(CP_UTF8, 0, wName, -1, ps.bpSkillName, sizeof(ps.bpSkillName) - 1, nullptr, nullptr);
                        }
                    }
                }
            }
        } catch (...) {
            // Data tables not ready yet — skip skill name
        }

        // Find
        ps.mf = GS(STAT_MAGICFIND);
        ps.gf = GS(STAT_GOLDFIND);

        // XP — use hardcoded XP table since STAT_LASTEXPERIENCE/STAT_NEXTEXPERIENCE return 0
        static const unsigned int xpTable[100] = {
            0, 0, 500, 1500, 3750, 7875, 14175, 22680, 32886, 44396,          // 0-9
            57715, 72144, 90180, 112725, 140906, 176132, 220165, 275207,       // 10-17
            344008, 430010, 537513, 671891, 839864, 1049830, 1312287,          // 18-24
            1640359, 2050449, 2563061, 3203826, 3902260, 4663553, 5493363,    // 25-31
            6397855, 7383752, 8458379, 9629723, 10906488, 12298162,           // 32-37
            13815086, 15468534, 17270791, 19235252, 21376515, 23710491,       // 38-43
            26254525, 29027522, 32050088, 35344686, 38935798, 42850109,       // 44-49
            47116709, 51767302, 56836449, 62361819, 68384473, 74949165,       // 50-55
            82104680, 89904191, 98405658, 107672256, 117772849, 128782495,    // 56-61
            140783010, 153863570, 168121381, 183662396, 200602101, 219066380, // 62-67
            239192444, 261129853, 285041630, 311105466, 339515048, 370481492, // 68-73
            404234327, 441026474, 481136442, 524872354, 572575690, 624623834, // 74-79
            681432048, 743456075, 811194541, 885201999, 966088518,            // 80-84
            1054524008, 1151248897, 1257084257, 1372942669, 1499831554,      // 85-89
            1638858934, 1791246578, 1958345166, 2141652315, 2342826386,      // 90-94
            2563823926U, 2806940593U, 3074784946U, 3370426858U, 3696930036U  // 95-99
        };

        // PD2 may have a modified XP curve, so use ratio-based approach:
        // Find the two table entries that bracket our current XP
        ps.currentXp = (unsigned int)GS(STAT_EXP);
        if (ps.level >= 1 && ps.level <= 98) {
            // Use actual level boundaries from table
            ps.lastLevelXp = xpTable[ps.level];
            ps.nextLevelXp = xpTable[ps.level + 1];

            // If XP table doesn't match PD2's curve, find where current XP falls
            // by scanning the table for the correct bracket
            if (ps.currentXp < ps.lastLevelXp) {
                // PD2 modified XP curve — find correct bracket
                for (int i = 1; i < 99; i++) {
                    if (ps.currentXp >= xpTable[i] && ps.currentXp < xpTable[i + 1]) {
                        ps.lastLevelXp = xpTable[i];
                        ps.nextLevelXp = xpTable[i + 1];
                        break;
                    }
                }
            }

            if (ps.nextLevelXp > ps.lastLevelXp && ps.currentXp >= ps.lastLevelXp) {
                double progress = (double)(ps.currentXp - ps.lastLevelXp);
                double range = (double)(ps.nextLevelXp - ps.lastLevelXp);
                ps.xpPctToNext = (float)(progress / range * 100.0);
            }
        } else if (ps.level == 99) {
            ps.lastLevelXp = xpTable[99];
            ps.nextLevelXp = xpTable[99];
            ps.xpPctToNext = 100.0f;
        }

        #undef GS

        // Difficulty & expansion
        ps.difficulty = D2CLIENT_GetDifficulty();
        int xPacMultiplier = 1;
        BnetData* pBnData = (*p_D2LAUNCH_BnData);
        if (pBnData) {
            ps.isExpansion = (pBnData->nCharFlags & PLAYER_TYPE_EXPANSION) != 0;
            xPacMultiplier = ps.isExpansion ? 2 : 1;
            ps.charClassNum = pBnData->nCharClass;
        }

        // Resistance penalty based on difficulty
        int resPenalties[3] = { RES_PENALTY_CLS_NORM, RES_PENALTY_CLS_NM, RES_PENALTY_CLS_HELL };
        ps.resPenalty = resPenalties[ps.difficulty] * xPacMultiplier;

        // Player count (walk roster list)
        ps.playerCount = 0;
        RosterUnit* pRoster = *p_D2CLIENT_PlayerUnitList;
        while (pRoster) {
            ps.playerCount++;
            pRoster = pRoster->pNext;
        }
        if (ps.playerCount < 1) ps.playerCount = 1;

        // Position
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

        // Area name from level data
        try {
            if (ps.area > 0) {
                wchar_t* wName = D2CLIENT_GetLevelName(ps.area);
                if (wName) {
                    WideCharToMultiByte(CP_UTF8, 0, wName, -1, ps.areaName, sizeof(ps.areaName) - 1, nullptr, nullptr);
                }
            }
        } catch (...) {}

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

                    // Get the real item name from the game's data tables
                    char fullName[64] = {};
                    try {
                        wchar_t* wName = D2CLIENT_GetUnitName(pItem);
                        if (wName) {
                            WideCharToMultiByte(CP_UTF8, 0, wName, -1, fullName, sizeof(fullName) - 1, nullptr, nullptr);
                        }
                    } catch (...) {}

                    // Store full name for MCP/tooltip
                    if (fullName[0]) {
                        strncpy_s(slot.fullName, fullName, sizeof(slot.fullName) - 1);
                    }

                    // Derive short label from full name
                    if (fullName[0]) {
                        // Map known full names to short labels
                        // Health potions
                        if (strstr(fullName, "Minor Heal"))       strncpy_s(slot.name, "HP1", sizeof(slot.name));
                        else if (strstr(fullName, "Light Heal"))  strncpy_s(slot.name, "HP2", sizeof(slot.name));
                        else if (strstr(fullName, "Greater Heal"))strncpy_s(slot.name, "HP4", sizeof(slot.name));
                        else if (strstr(fullName, "Super Heal"))  strncpy_s(slot.name, "HP5", sizeof(slot.name));
                        else if (strstr(fullName, "Heal"))        strncpy_s(slot.name, "HP3", sizeof(slot.name));
                        // Mana potions
                        else if (strstr(fullName, "Minor Mana"))  strncpy_s(slot.name, "MP1", sizeof(slot.name));
                        else if (strstr(fullName, "Light Mana"))  strncpy_s(slot.name, "MP2", sizeof(slot.name));
                        else if (strstr(fullName, "Greater Mana"))strncpy_s(slot.name, "MP4", sizeof(slot.name));
                        else if (strstr(fullName, "Super Mana"))  strncpy_s(slot.name, "MP5", sizeof(slot.name));
                        else if (strstr(fullName, "Mana"))        strncpy_s(slot.name, "MP3", sizeof(slot.name));
                        // Rejuv
                        else if (strstr(fullName, "Full Rejuv"))  strncpy_s(slot.name, "FRJ", sizeof(slot.name));
                        else if (strstr(fullName, "Rejuv"))       strncpy_s(slot.name, "RJ", sizeof(slot.name));
                        // Scrolls
                        else if (strstr(fullName, "Town Portal")) strncpy_s(slot.name, "TP", sizeof(slot.name));
                        else if (strstr(fullName, "Identify"))    strncpy_s(slot.name, "ID", sizeof(slot.name));
                        // Utility
                        else if (strstr(fullName, "Antidote"))    strncpy_s(slot.name, "Antd", sizeof(slot.name));
                        else if (strstr(fullName, "Thawing"))     strncpy_s(slot.name, "Thaw", sizeof(slot.name));
                        else if (strstr(fullName, "Stamina"))     strncpy_s(slot.name, "Stam", sizeof(slot.name));
                        else if (strstr(fullName, "Key"))         strncpy_s(slot.name, "Key", sizeof(slot.name));
                        // Unknown — use abbreviated full name
                        else {
                            strncpy_s(slot.name, fullName, sizeof(slot.name) - 1);
                            // Truncate to fit button
                            if (strlen(slot.name) > 8) slot.name[8] = '\0';
                        }
                    } else {
                        snprintf(slot.name, sizeof(slot.name), "#%d", pItem->dwTxtFileNo);
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

                // Get unit name from game data tables
                try {
                    wchar_t* wName = D2CLIENT_GetUnitName(pUnit);
                    if (wName) {
                        WideCharToMultiByte(CP_UTF8, 0, wName, -1, nu.name, sizeof(nu.name) - 1, nullptr, nullptr);
                    }
                } catch (...) {}

                if (pUnit->dwType == 1) { // Monster
                    nu.hp = D2COMMON_GetUnitStat(pUnit, STAT_HP, 0);
                    nu.maxHp = D2COMMON_GetUnitStat(pUnit, STAT_MAXHP, 0);

                    if (pUnit->pMonsterData) {
                        BYTE flags = pUnit->pMonsterData->fBoss;
                        nu.isBoss = (flags & 1) != 0;
                        nu.isChampion = (flags & 2) != 0;
                        nu.isMinion = (flags & 4) != 0;
                    }
                    // Fallback if name lookup failed
                    if (!nu.name[0]) snprintf(nu.name, sizeof(nu.name), "Monster#%d", pUnit->dwTxtFileNo);
                } else if (pUnit->dwType == 0) { // Player
                    nu.hp = D2COMMON_GetUnitStat(pUnit, STAT_HP, 0);
                    nu.maxHp = D2COMMON_GetUnitStat(pUnit, STAT_MAXHP, 0);
                    // Players: prefer pPlayerData->szName (ASCII, no lookup needed)
                    if (pUnit->pPlayerData && !nu.name[0]) {
                        strncpy_s(nu.name, pUnit->pPlayerData->szName, sizeof(nu.name) - 1);
                    }
                } else if (pUnit->dwType == 4) { // Item
                    if (!nu.name[0]) snprintf(nu.name, sizeof(nu.name), "Item#%d", pUnit->dwTxtFileNo);
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

        __try {
            UpdatePlayerState();
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            UpdateBeltState();
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            UpdateNearbyUnits();
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
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
