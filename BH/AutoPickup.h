#pragma once

// AutoPickup - Automatically pick up potions to fill empty belt slots.
// Only picks up potions when belt has empty slots of matching column type.

namespace AutoPickup {
    struct Config {
        bool enabled = false;
        int maxDistance = 15;       // max pickup range (game units)
        int cooldownMs = 250;       // minimum ms between pickups
        bool pickHpPotions = true;
        bool pickMpPotions = true;
        bool pickRejuvs = true;
    };

    // Call from game loop
    void Update();

    // Get/set config (thread-safe)
    Config GetConfig();
    void SetConfig(const Config& cfg);
}
