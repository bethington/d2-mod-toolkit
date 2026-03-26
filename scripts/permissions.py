"""Permission Tier System for Twitch-driven development.

Manages user tiers, rate limiting, and command filtering.
Persists state to permissions.json in the scripts directory.

Usage:
    from permissions import PermissionManager
    pm = PermissionManager()
    pm.check("bethington", "!request add monster avoidance")  # -> allowed
    pm.check("random_viewer", "!request do something")        # -> denied (not whitelisted)
"""

import json
import os
import time

STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "permissions.json")

# Tier levels (higher = more access)
TIERS = {
    "owner": 100,
    "trusted": 80,
    "subscriber": 60,
    "viewer": 40,
    "default": 0,
    "banned": -1,
}

# What each tier can do
TIER_CAPABILITIES = {
    "owner": {
        "direct_command",    # Direct instructions to the agent
        "override_queue",    # Reorder, pin, remove queue items
        "manage_users",      # Ban, whitelist, promote users
        "request",           # Submit feature requests
        "vote",              # Vote on queue items
        "info",              # Info commands (!stats, !area, etc.)
        "halt",              # Stop the agent
    },
    "trusted": {
        "request",
        "vote",
        "info",
        "code_review",       # Request specific code changes
    },
    "subscriber": {
        "request",
        "vote",
        "info",
    },
    "viewer": {
        "vote",
        "info",
    },
    "default": set(),        # Nothing — must be whitelisted
    "banned": set(),         # Nothing — all messages ignored
}

# Vote weight multipliers per tier
VOTE_WEIGHTS = {
    "owner": 10,
    "trusted": 5,
    "subscriber": 3,
    "viewer": 1,
    "default": 0,
    "banned": 0,
}

# Rate limits: (max_actions, window_seconds)
RATE_LIMITS = {
    "owner": (999, 1),       # Effectively unlimited
    "trusted": (20, 60),
    "subscriber": (10, 60),
    "viewer": (5, 60),
    "default": (0, 60),
    "banned": (0, 60),
}


def _default_state():
    return {
        "users": {
            "bethington": {"tier": "owner"},
        },
        "rate_log": {},  # user -> [timestamp, ...]
    }


class PermissionManager:
    def __init__(self, state_file=STATE_FILE):
        self.state_file = state_file
        self.state = self._load()

    def _load(self):
        if os.path.exists(self.state_file):
            try:
                with open(self.state_file, "r") as f:
                    return json.load(f)
            except:
                pass
        return _default_state()

    def _save(self):
        with open(self.state_file, "w") as f:
            json.dump(self.state, f, indent=2)

    def get_tier(self, username):
        """Get a user's tier. Returns 'default' for unknown users."""
        username = username.lower()
        user = self.state["users"].get(username)
        if user:
            return user.get("tier", "default")
        return "default"

    def set_tier(self, username, tier):
        """Set a user's tier. Only owner can do this."""
        username = username.lower()
        if tier not in TIERS:
            return False, f"Unknown tier: {tier}"
        if username not in self.state["users"]:
            self.state["users"][username] = {}
        self.state["users"][username]["tier"] = tier
        self._save()
        return True, f"{username} set to {tier}"

    def ban(self, username):
        return self.set_tier(username, "banned")

    def whitelist(self, username, tier="viewer"):
        return self.set_tier(username, tier)

    def get_capabilities(self, username):
        """Get what a user is allowed to do."""
        tier = self.get_tier(username)
        return TIER_CAPABILITIES.get(tier, set())

    def get_vote_weight(self, username):
        """Get a user's vote weight."""
        tier = self.get_tier(username)
        return VOTE_WEIGHTS.get(tier, 0)

    def _check_rate_limit(self, username):
        """Check if user is within rate limit. Returns (allowed, reason)."""
        tier = self.get_tier(username)
        max_actions, window = RATE_LIMITS.get(tier, (0, 60))

        if max_actions == 0:
            return False, "no access"

        now = time.time()
        key = username.lower()

        # Clean old entries
        if key not in self.state["rate_log"]:
            self.state["rate_log"][key] = []
        self.state["rate_log"][key] = [
            t for t in self.state["rate_log"][key] if now - t < window
        ]

        if len(self.state["rate_log"][key]) >= max_actions:
            return False, f"rate limited ({max_actions}/{window}s)"

        self.state["rate_log"][key].append(now)
        return True, "ok"

    def classify_command(self, message):
        """Classify a chat message into a capability requirement."""
        msg = message.strip().lower()

        if msg.startswith("!ban ") or msg.startswith("!whitelist ") or msg.startswith("!promote "):
            return "manage_users"
        if msg.startswith("!halt") or msg.startswith("!stop"):
            return "halt"
        if msg.startswith("!override") or msg.startswith("!pin ") or msg.startswith("!remove "):
            return "override_queue"
        if msg.startswith("!request ") or msg.startswith("!feature "):
            return "request"
        if msg.startswith("!vote "):
            return "vote"
        if msg.startswith("!stats") or msg.startswith("!area") or msg.startswith("!build") or \
           msg.startswith("!hp") or msg.startswith("!deaths") or msg.startswith("!help") or \
           msg.startswith("!queue") or msg.startswith("!commands"):
            return "info"
        if not msg.startswith("!"):
            # Free-form message directed at the agent — requires direct_command
            return "direct_command"
        return "info"  # Unknown commands treated as info

    def check(self, username, message):
        """Check if a user can execute a message. Returns (allowed, reason, tier, capability)."""
        username = username.lower()
        tier = self.get_tier(username)
        capability = self.classify_command(message)
        capabilities = self.get_capabilities(username)

        if tier == "banned":
            return False, "banned", tier, capability

        if capability not in capabilities:
            return False, f"tier '{tier}' cannot '{capability}'", tier, capability

        allowed, reason = self._check_rate_limit(username)
        if not allowed:
            return False, reason, tier, capability

        self._save()
        return True, "ok", tier, capability

    def list_users(self):
        """List all users and their tiers."""
        return {u: d.get("tier", "default") for u, d in self.state["users"].items()}


def main():
    """CLI for managing permissions."""
    import argparse
    parser = argparse.ArgumentParser(description="Permission Manager")
    sub = parser.add_subparsers(dest="action")

    sub.add_parser("list").add_argument("--tier", help="Filter by tier")

    s = sub.add_parser("set")
    s.add_argument("username")
    s.add_argument("tier", choices=TIERS.keys())

    s = sub.add_parser("check")
    s.add_argument("username")
    s.add_argument("message")

    s = sub.add_parser("ban")
    s.add_argument("username")

    s = sub.add_parser("whitelist")
    s.add_argument("username")
    s.add_argument("--tier", default="viewer")

    args = parser.parse_args()
    pm = PermissionManager()

    if args.action == "list":
        users = pm.list_users()
        if args.tier:
            users = {u: t for u, t in users.items() if t == args.tier}
        for user, tier in sorted(users.items()):
            print(f"  {user}: {tier}")

    elif args.action == "set":
        ok, msg = pm.set_tier(args.username, args.tier)
        print(msg)

    elif args.action == "check":
        allowed, reason, tier, cap = pm.check(args.username, args.message)
        status = "ALLOWED" if allowed else "DENIED"
        print(f"  [{status}] {args.username} (tier={tier}) -> {cap}: {reason}")

    elif args.action == "ban":
        ok, msg = pm.ban(args.username)
        print(msg)

    elif args.action == "whitelist":
        ok, msg = pm.whitelist(args.username, args.tier)
        print(msg)

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
