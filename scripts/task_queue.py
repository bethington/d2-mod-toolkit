"""Task Queue for Twitch audience-driven development.

Manages a prioritized backlog of feature requests from viewers.
Supports voting, donation boosting, and owner overrides.
Persists state to task_queue.json in the scripts directory.

Usage:
    from task_queue import TaskQueue
    q = TaskQueue()
    q.submit("viewer123", "Add monster avoidance AI", tier="subscriber")
    q.vote("viewer456", task_id=1, weight=3)
    task = q.next()  # Returns highest priority task
"""

import json
import os
import time

STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "task_queue.json")


def _default_state():
    return {
        "next_id": 1,
        "tasks": [],
        "completed": [],
    }


class TaskQueue:
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

    def submit(self, username, description, tier="viewer", boost=0):
        """Submit a new task request. Returns the task."""
        task = {
            "id": self.state["next_id"],
            "description": description,
            "submitted_by": username.lower(),
            "submitted_at": time.time(),
            "tier": tier,
            "votes": {},           # username -> weight
            "boost": boost,        # Donation/bits boost
            "pinned": False,       # Owner-pinned to top
            "status": "queued",    # queued, in_progress, completed, rejected
            "branch": None,        # Git branch name once work starts
            "result": None,        # Outcome description
        }
        self.state["next_id"] += 1
        self.state["tasks"].append(task)
        self._save()
        return task

    def vote(self, username, task_id, weight=1):
        """Vote for a task. Weight is determined by voter's tier."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        if task["status"] != "queued":
            return False, f"Task is {task['status']}, not voteable"
        task["votes"][username.lower()] = weight
        self._save()
        return True, f"Vote recorded (weight={weight})"

    def boost(self, task_id, amount):
        """Boost a task's priority (from donations/bits)."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        task["boost"] += amount
        self._save()
        return True, f"Boosted by {amount} (total={task['boost']})"

    def pin(self, task_id):
        """Pin a task to the top of the queue (owner action)."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        task["pinned"] = True
        self._save()
        return True, "Pinned"

    def remove(self, task_id, reason="Removed by owner"):
        """Remove a task from the queue."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        task["status"] = "rejected"
        task["result"] = reason
        self.state["tasks"] = [t for t in self.state["tasks"] if t["id"] != task_id]
        self.state["completed"].append(task)
        self._save()
        return True, "Removed"

    def next(self):
        """Get the highest priority queued task. Does NOT change its status."""
        queued = [t for t in self.state["tasks"] if t["status"] == "queued"]
        if not queued:
            return None
        return sorted(queued, key=self._priority_score, reverse=True)[0]

    def start(self, task_id, branch=None):
        """Mark a task as in progress."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        task["status"] = "in_progress"
        task["branch"] = branch
        self._save()
        return True, f"Started (branch={branch})"

    def complete(self, task_id, result="Success"):
        """Mark a task as completed and move to history."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        task["status"] = "completed"
        task["result"] = result
        task["completed_at"] = time.time()
        self.state["tasks"] = [t for t in self.state["tasks"] if t["id"] != task_id]
        self.state["completed"].append(task)
        self._save()
        return True, "Completed"

    def fail(self, task_id, reason="Failed after 3 attempts"):
        """Mark a task as failed and requeue or archive."""
        task = self._find(task_id)
        if not task:
            return False, "Task not found"
        task["status"] = "queued"
        task["result"] = reason
        task["branch"] = None
        self._save()
        return True, "Requeued after failure"

    def list_queued(self):
        """List all queued tasks sorted by priority."""
        queued = [t for t in self.state["tasks"] if t["status"] == "queued"]
        return sorted(queued, key=self._priority_score, reverse=True)

    def list_in_progress(self):
        """List tasks currently being worked on."""
        return [t for t in self.state["tasks"] if t["status"] == "in_progress"]

    def list_completed(self, limit=10):
        """List recently completed tasks."""
        return self.state["completed"][-limit:]

    def _find(self, task_id):
        for t in self.state["tasks"]:
            if t["id"] == task_id:
                return t
        return None

    def _priority_score(self, task):
        """Calculate priority score for sorting. Higher = more urgent."""
        score = 0

        # Pinned tasks always on top
        if task.get("pinned"):
            score += 10000

        # Sum of weighted votes
        score += sum(task.get("votes", {}).values())

        # Donation boost (1 point per unit of boost)
        score += task.get("boost", 0)

        # Submitter tier bonus
        tier_bonus = {
            "owner": 50,
            "trusted": 20,
            "subscriber": 5,
            "viewer": 1,
        }
        score += tier_bonus.get(task.get("tier", "viewer"), 0)

        return score

    def format_queue(self, limit=5):
        """Format the queue for display in chat."""
        queued = self.list_queued()[:limit]
        if not queued:
            return "Queue is empty!"
        lines = []
        for i, t in enumerate(queued):
            votes = sum(t.get("votes", {}).values())
            pin = "[PIN] " if t.get("pinned") else ""
            boost = f" +{t['boost']}b" if t.get("boost", 0) > 0 else ""
            lines.append(f"#{t['id']} {pin}{t['description']} ({votes}v{boost}) by {t['submitted_by']}")
        return " | ".join(lines)


def main():
    """CLI for managing the task queue."""
    import argparse
    parser = argparse.ArgumentParser(description="Task Queue Manager")
    sub = parser.add_subparsers(dest="action")

    s = sub.add_parser("submit")
    s.add_argument("username")
    s.add_argument("description")
    s.add_argument("--tier", default="viewer")
    s.add_argument("--boost", type=int, default=0)

    s = sub.add_parser("vote")
    s.add_argument("username")
    s.add_argument("task_id", type=int)
    s.add_argument("--weight", type=int, default=1)

    s = sub.add_parser("boost")
    s.add_argument("task_id", type=int)
    s.add_argument("amount", type=int)

    s = sub.add_parser("pin")
    s.add_argument("task_id", type=int)

    s = sub.add_parser("remove")
    s.add_argument("task_id", type=int)

    s = sub.add_parser("start")
    s.add_argument("task_id", type=int)
    s.add_argument("--branch", default=None)

    s = sub.add_parser("complete")
    s.add_argument("task_id", type=int)
    s.add_argument("--result", default="Success")

    sub.add_parser("list")
    sub.add_parser("next")

    args = parser.parse_args()
    q = TaskQueue()

    if args.action == "submit":
        task = q.submit(args.username, args.description, tier=args.tier, boost=args.boost)
        print(f"Task #{task['id']}: {task['description']}")

    elif args.action == "vote":
        ok, msg = q.vote(args.username, args.task_id, weight=args.weight)
        print(msg)

    elif args.action == "boost":
        ok, msg = q.boost(args.task_id, args.amount)
        print(msg)

    elif args.action == "pin":
        ok, msg = q.pin(args.task_id)
        print(msg)

    elif args.action == "remove":
        ok, msg = q.remove(args.task_id)
        print(msg)

    elif args.action == "start":
        ok, msg = q.start(args.task_id, branch=args.branch)
        print(msg)

    elif args.action == "complete":
        ok, msg = q.complete(args.task_id, result=args.result)
        print(msg)

    elif args.action == "list":
        print(q.format_queue(limit=10))

    elif args.action == "next":
        task = q.next()
        if task:
            print(json.dumps(task, indent=2))
        else:
            print("Queue is empty")

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
