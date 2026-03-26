---
name: permissions
description: Manage Twitch viewer permission tiers — whitelist, ban, promote users, check access
---

# Permission Management

Control who can interact with the agent and what they can do, based on a tier system.

## Tiers (highest to lowest)

| Tier | Who | Can do |
|------|-----|--------|
| owner | bethington | Everything — direct commands, halt, manage users, override queue |
| trusted | Mods, whitelisted regulars | Submit requests, vote (5x weight), request code changes |
| subscriber | Twitch subs | Submit requests, vote (3x weight), info commands |
| viewer | Approved viewers | Vote (1x weight), info commands (!stats, !area) |
| default | Unknown users | Nothing — blocked until whitelisted |
| banned | Blocked users | Nothing — all messages silently ignored |

## Commands

### Check if a user can do something
```bash
python scripts/permissions.py check <username> "<message>"
```

### Whitelist a user
```bash
python scripts/permissions.py whitelist <username> --tier viewer
```

### Promote a user
```bash
python scripts/permissions.py set <username> subscriber
```

### Ban a user
```bash
python scripts/permissions.py ban <username>
```

### List all users
```bash
python scripts/permissions.py list
python scripts/permissions.py list --tier banned
```

## Processing incoming Twitch messages

For every incoming Twitch chat message, check permissions BEFORE acting:

```python
from permissions import PermissionManager
pm = PermissionManager()

allowed, reason, tier, capability = pm.check(username, message)
if not allowed:
    # Silently ignore, or respond with denial for whitelisted users
    pass
```

## Important

- `bethington` is hardcoded as owner and cannot be demoted
- Default tier is blocked — new users must be explicitly whitelisted
- Rate limits are enforced per-tier (owner=unlimited, viewer=5/minute)
- State persists in `scripts/permissions.json`
- Vote weights: owner=10x, trusted=5x, subscriber=3x, viewer=1x
