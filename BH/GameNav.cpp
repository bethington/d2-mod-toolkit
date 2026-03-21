#include "GameNav.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"

#include <mutex>

// Control type constants
#define CTRL_TYPE_EDITBOX  1
#define CTRL_TYPE_IMAGE    2
#define CTRL_TYPE_BUTTON   6
#define CTRL_TYPE_LIST     7

namespace {
    std::mutex g_mutex;
    GameNav::NavStatus g_status;
    std::string g_targetChar;
    int g_targetDifficulty = -1;
    DWORD g_lastStepTime = 0;
    DWORD g_stepDelay = 500; // ms between steps
    int g_retryCount = 0;
    bool g_exitRequested = false;

    // Count controls by type
    struct ControlCounts {
        int total = 0;
        int buttons = 0;
        int editboxes = 0;
        int lists = 0;
        int images = 0;
    };

    ControlCounts CountControls() {
        ControlCounts cc;
        Control* pCtrl = *p_D2WIN_FirstControl;
        while (pCtrl) {
            cc.total++;
            switch (pCtrl->dwType) {
                case CTRL_TYPE_BUTTON: cc.buttons++; break;
                case CTRL_TYPE_EDITBOX: cc.editboxes++; break;
                case CTRL_TYPE_LIST: cc.lists++; break;
                case CTRL_TYPE_IMAGE: cc.images++; break;
            }
            pCtrl = pCtrl->pNext;
        }
        return cc;
    }

    // Find a button control by approximate position
    Control* FindButton(int minX, int maxX, int minY, int maxY) {
        Control* pCtrl = *p_D2WIN_FirstControl;
        while (pCtrl) {
            if (pCtrl->dwType == CTRL_TYPE_BUTTON && pCtrl->dwState >= 2) {
                int x = (int)pCtrl->dwPosX;
                int y = (int)pCtrl->dwPosY;
                if (x >= minX && x <= maxX && y >= minY && y <= maxY) {
                    return pCtrl;
                }
            }
            pCtrl = pCtrl->pNext;
        }
        return nullptr;
    }

    // Click a control via its OnPress callback
    bool ClickControl(Control* pCtrl) {
        if (!pCtrl || !pCtrl->OnPress) return false;
        // Simulate a click by calling OnPress
        pCtrl->OnPress(pCtrl);
        return true;
    }

    // Identify current OOG screen by control patterns
    // Based on d2_auto.py heuristics (800x600 coordinates)
    enum OogScreen {
        OOG_UNKNOWN,
        OOG_MAIN_MENU,
        OOG_CHAR_SELECT,
        OOG_DIFFICULTY,
        OOG_IN_GAME,
        OOG_LOADING,
        OOG_OTHER
    };

    OogScreen IdentifyScreen() {
        // If player unit exists, we're in-game
        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (pPlayer && pPlayer->pPath) {
            return OOG_IN_GAME;
        }

        Control* pCtrl = *p_D2WIN_FirstControl;
        if (!pCtrl) return OOG_LOADING;

        ControlCounts cc = CountControls();

        // Screen identification by button count and layout:
        // Main Menu: 7 buttons, all at x=264
        // Character Select: 5 buttons, OK at (627,572), Create/Convert/Delete at y=528
        // Difficulty: 3-4 buttons stacked vertically around x=264

        // Check for Character Select: has an OK button at bottom-right (627, 572)
        // and character slots (type 4) in the middle
        if (cc.buttons >= 4 && cc.buttons <= 6) {
            // Look for OK button at bottom-right
            Control* pOK = *p_D2WIN_FirstControl;
            bool hasOKButton = false;
            int type4Count = 0;
            while (pOK) {
                if (pOK->dwType == CTRL_TYPE_BUTTON && pOK->dwPosX > 600 && pOK->dwPosY > 550) {
                    hasOKButton = true;
                }
                if (pOK->dwType == 4) type4Count++;
                pOK = pOK->pNext;
            }
            if (hasOKButton && type4Count >= 4) {
                return OOG_CHAR_SELECT;
            }
        }

        if (cc.buttons >= 6) {
            return OOG_MAIN_MENU;
        }

        if (cc.buttons >= 2 && cc.buttons <= 4 && cc.total <= 10) {
            return OOG_DIFFICULTY;
        }

        return OOG_UNKNOWN;
    }
}

namespace GameNav {

    void EnterGame(const std::string& charName, int difficulty) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_targetChar = charName;
        g_targetDifficulty = difficulty;
        g_status.state = NAV_IN_PROGRESS;
        g_status.step = 0;
        g_status.message = "Starting navigation...";
        g_lastStepTime = 0;
        g_retryCount = 0;
    }

    NavStatus GetStatus() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    void Cancel() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.state = NAV_IDLE;
        g_status.message = "Cancelled";
    }

    void Update() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status.state != NAV_IN_PROGRESS) return;

        DWORD now = GetTickCount();
        if (now - g_lastStepTime < g_stepDelay) return;
        g_lastStepTime = now;

        OogScreen screen = IdentifyScreen();

        switch (screen) {
            case OOG_IN_GAME:
                g_status.state = NAV_SUCCESS;
                g_status.currentScreen = "In Game";
                g_status.message = "Successfully entered game";
                return;

            case OOG_LOADING:
                g_status.currentScreen = "Loading";
                g_status.message = "Waiting for load...";
                return;

            case OOG_MAIN_MENU: {
                g_status.currentScreen = "Main Menu";
                g_status.message = "Clicking Single Player...";
                // Single Player button is typically around x=264, y=324 (800x600)
                Control* btn = FindButton(250, 380, 310, 340);
                if (btn) {
                    ClickControl(btn);
                    g_status.step = 1;
                } else {
                    g_status.message = "Can't find Single Player button";
                    g_retryCount++;
                    if (g_retryCount > 10) {
                        g_status.state = NAV_FAILED;
                        g_status.message = "Failed: couldn't find Single Player button";
                    }
                }
                return;
            }

            case OOG_CHAR_SELECT: {
                g_status.currentScreen = "Character Select";
                // TODO: if charName specified, find and click on that character in the list
                // For now, just click OK to use whatever is selected
                g_status.message = "Clicking OK...";
                // OK button at bottom-right: (627, 572) size 128x35
                Control* btn = FindButton(600, 760, 560, 610);
                if (btn) {
                    ClickControl(btn);
                    g_status.step = 2;
                } else {
                    g_status.message = "Can't find OK button";
                    g_retryCount++;
                    if (g_retryCount > 10) {
                        g_status.state = NAV_FAILED;
                    }
                }
                return;
            }

            case OOG_DIFFICULTY: {
                g_status.currentScreen = "Difficulty Select";
                int diff = g_targetDifficulty;

                // Difficulty buttons stacked vertically
                // Normal ~y=280, Nightmare ~y=330, Hell ~y=380
                Control* btn = nullptr;
                if (diff == 0) {
                    g_status.message = "Selecting Normal...";
                    btn = FindButton(250, 400, 265, 295);
                } else if (diff == 1) {
                    g_status.message = "Selecting Nightmare...";
                    btn = FindButton(250, 400, 315, 345);
                } else if (diff == 2) {
                    g_status.message = "Selecting Hell...";
                    btn = FindButton(250, 400, 365, 395);
                } else {
                    // Default: click the last (highest) enabled button
                    g_status.message = "Selecting highest difficulty...";
                    // Try Hell first, then Nightmare, then Normal
                    btn = FindButton(250, 400, 365, 395);
                    if (!btn) btn = FindButton(250, 400, 315, 345);
                    if (!btn) btn = FindButton(250, 400, 265, 295);
                }

                if (btn) {
                    ClickControl(btn);
                    g_status.step = 3;
                } else {
                    g_status.message = "Can't find difficulty button";
                    g_retryCount++;
                    if (g_retryCount > 10) {
                        g_status.state = NAV_FAILED;
                    }
                }
                return;
            }

            default:
                g_status.currentScreen = "Unknown";
                g_status.message = "Unrecognized screen, waiting...";
                g_retryCount++;
                if (g_retryCount > 20) {
                    g_status.state = NAV_FAILED;
                    g_status.message = "Failed: stuck on unknown screen";
                }
                return;
        }
    }

    void RequestExitGame() {
        g_exitRequested = true;
    }

    void CheckPendingExit() {
        if (g_exitRequested) {
            g_exitRequested = false;
            if (IsGameReady()) {
                D2CLIENT_ExitGame();
            }
        }
    }
}
