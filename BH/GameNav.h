#pragma once

// GameNav - Out-of-game menu navigation.
// Automates character selection and game entry.

#include <string>

namespace GameNav {
    enum NavState {
        NAV_IDLE = 0,
        NAV_IN_PROGRESS,
        NAV_SUCCESS,
        NAV_FAILED
    };

    struct NavStatus {
        NavState state = NAV_IDLE;
        std::string currentScreen;
        std::string message;
        int step = 0;
    };

    // Start navigating to enter a game.
    // charName: empty = use whatever character is selected
    // difficulty: -1 = last played, 0 = normal, 1 = nightmare, 2 = hell
    void EnterGame(const std::string& charName = "", int difficulty = -1);

    // Get current navigation status
    NavStatus GetStatus();

    // Call from game loop to advance navigation state machine
    void Update();

    // Cancel ongoing navigation
    void Cancel();

    // Request a graceful game exit (executed on game thread)
    void RequestExitGame();

    // Check and execute pending exit (call from game loop)
    void CheckPendingExit();
}
