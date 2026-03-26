---
name: task-queue
description: Manage the Twitch audience task queue — submit requests, vote, prioritize, pick next task
---

# Task Queue

A prioritized backlog of feature requests from Twitch viewers. The agent pulls the highest-priority task when ready for new work.

## Priority scoring

Tasks are ranked by: pinned status > vote weight sum > donation boost > submitter tier bonus.

## Commands

### Submit a request
```bash
python scripts/task_queue.py submit <username> "<description>" --tier subscriber --boost 0
```

### Vote for a task
```bash
python scripts/task_queue.py vote <username> <task_id> --weight 3
```
Weight should come from the voter's tier (use `PermissionManager.get_vote_weight()`).

### Boost a task (from donations/bits)
```bash
python scripts/task_queue.py boost <task_id> <amount>
```

### Pin a task to top (owner only)
```bash
python scripts/task_queue.py pin <task_id>
```

### Remove a task (owner only)
```bash
python scripts/task_queue.py remove <task_id>
```

### See the queue
```bash
python scripts/task_queue.py list
```

### Get the next task to work on
```bash
python scripts/task_queue.py next
```

### Start working on a task
```bash
python scripts/task_queue.py start <task_id> --branch feature/monster-avoidance
```

### Complete a task
```bash
python scripts/task_queue.py complete <task_id> --result "Implemented and tested"
```

## Workflow for the agent

1. Check `task_queue.py next` to see the highest priority task
2. Create a git feature branch: `git checkout -b feature/<slug>`
3. Mark the task as started: `task_queue.py start <id> --branch feature/<slug>`
4. Implement the feature
5. Compile and test: `build_and_deploy.py --json`
6. If successful: merge branch and `task_queue.py complete <id>`
7. If failed 3 times: `task_queue.py fail <id>` (requeues it) and move to next task

## Twitch chat integration

Map chat commands to queue operations:
- `!request <description>` → submit (requires "request" capability)
- `!vote <id>` → vote with tier-appropriate weight (requires "vote" capability)
- `!queue` → list top 5 tasks (requires "info" capability)
- `!pin <id>` → pin task (requires "override_queue" capability, owner only)
- `!remove <id>` → remove task (requires "override_queue" capability, owner only)

## State

Persists in `scripts/task_queue.json`. Completed tasks are archived in the `completed` array for history.
