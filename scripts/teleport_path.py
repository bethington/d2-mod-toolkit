"""Teleport Pathfinding — A* style path planning using collision map.

Ported from funmixxed/Fortification TeleportPath.cpp (Niren7/Abin algorithm).
Uses the collision map from get_collision_map MCP tool to find walkable paths,
then converts grid coordinates back to game coordinates for teleport hops.

Usage:
    from teleport_path import TeleportPathfinder
    pf = TeleportPathfinder(mcp_client)
    path = pf.find_path(target_x, target_y)
    # path = [(x1,y1), (x2,y2), ...] in game coordinates
"""

import math

TP_RANGE = 35       # Max teleport range in game units
RANGE_INVALID = 10000


class TeleportPathfinder:
    def __init__(self, mcp_client):
        self.mcp = mcp_client
        self._grid = None
        self._grid_w = 0
        self._grid_h = 0
        self._origin_x = 0
        self._origin_y = 0
        self._scale_x = 1
        self._scale_y = 1
        self._size_x = 0
        self._size_y = 0

    def load_collision_map(self):
        """Fetch collision map from MCP and parse it."""
        data = self.mcp.call("get_collision_map", timeout=10)
        if not data or "grid" not in data:
            return False

        self._origin_x = data["origin"]["x"]
        self._origin_y = data["origin"]["y"]
        self._size_x = data["size"]["x"]
        self._size_y = data["size"]["y"]
        self._grid_w = data["grid_size"]["w"]
        self._grid_h = data["grid_size"]["h"]

        # Compute scale factors (same formula as C++ side)
        self._scale_x = max((self._size_x + 199) // 200, 1)
        self._scale_y = max((self._size_y + 199) // 200, 1)

        # Parse grid: '.' = walkable (0), '#' = wall (1)
        rows = data["grid"]
        self._grid = []
        for y in range(self._grid_h):
            row = rows[y] if y < len(rows) else "#" * self._grid_w
            grid_row = []
            for x in range(self._grid_w):
                ch = row[x] if x < len(row) else '#'
                grid_row.append(0 if ch == '.' else 1)
            self._grid.append(grid_row)

        return True

    def game_to_grid(self, gx, gy):
        """Convert game coordinates to grid coordinates."""
        return (
            (gx - self._origin_x) // self._scale_x,
            (gy - self._origin_y) // self._scale_y
        )

    def grid_to_game(self, gx, gy):
        """Convert grid coordinates to game coordinates (center of cell)."""
        return (
            self._origin_x + gx * self._scale_x + self._scale_x // 2,
            self._origin_y + gy * self._scale_y + self._scale_y // 2
        )

    def _is_valid(self, x, y):
        return 0 <= x < self._grid_w and 0 <= y < self._grid_h

    def _is_walkable(self, x, y):
        return self._is_valid(x, y) and self._grid[y][x] == 0

    def _dist(self, x1, y1, x2, y2):
        dx, dy = x2 - x1, y2 - y1
        return math.sqrt(dx * dx + dy * dy)

    def find_path(self, target_x, target_y, max_hops=50):
        """Find teleport path from current position to target (game coords).

        Returns list of (x, y) game coordinate waypoints, or empty list on failure.
        """
        if not self.load_collision_map():
            return []

        # Get current player position
        ps = self.mcp.call("get_player_state")
        if not ps or "position" not in ps:
            return []

        start_gx = ps["position"]["x"]
        start_gy = ps["position"]["y"]

        # Convert to grid coords
        sx, sy = self.game_to_grid(start_gx, start_gy)
        ex, ey = self.game_to_grid(target_x, target_y)

        # Clamp to grid bounds
        sx = max(0, min(sx, self._grid_w - 1))
        sy = max(0, min(sy, self._grid_h - 1))
        ex = max(0, min(ex, self._grid_w - 1))
        ey = max(0, min(ey, self._grid_h - 1))

        # If target is unwalkable, find nearest walkable cell
        if not self._is_walkable(ex, ey):
            ex, ey = self._find_nearest_walkable(ex, ey)
            if ex < 0:
                return []

        # Teleport range in grid units
        tp_range_grid = max(TP_RANGE // self._scale_x, 1)

        # Build distance table (funmixxed algorithm)
        dist_table = self._build_distance_table(ex, ey)

        # Walk path using greedy best-move
        path_grid = [(sx, sy)]
        pos_x, pos_y = sx, sy

        for _ in range(max_hops):
            # Check if we can reach destination directly
            if self._dist(pos_x, pos_y, ex, ey) <= tp_range_grid:
                path_grid.append((ex, ey))
                break

            # Find best move within teleport range
            best_x, best_y = -1, -1
            best_val = RANGE_INVALID

            scan_range = tp_range_grid
            for px in range(pos_x - scan_range, pos_x + scan_range + 1):
                for py in range(pos_y - scan_range, pos_y + scan_range + 1):
                    if not self._is_valid(px, py):
                        continue
                    if dist_table[py][px] >= RANGE_INVALID:
                        continue
                    if self._dist(px, py, pos_x, pos_y) > tp_range_grid:
                        continue
                    if dist_table[py][px] < best_val:
                        best_val = dist_table[py][px]
                        best_x, best_y = px, py

            if best_x < 0:
                break  # No path found

            # Block visited area to prevent backtracking
            block_range = max(tp_range_grid // 4, 1)
            for bx in range(pos_x - block_range, pos_x + block_range + 1):
                for by in range(pos_y - block_range, pos_y + block_range + 1):
                    if self._is_valid(bx, by):
                        dist_table[by][bx] = RANGE_INVALID

            # Redundancy check — skip intermediate hops
            redundant_idx = -1
            for i in range(1, len(path_grid)):
                if self._dist(path_grid[i][0], path_grid[i][1], best_x, best_y) <= tp_range_grid // 2:
                    redundant_idx = i
                    break

            if redundant_idx >= 0:
                path_grid = path_grid[:redundant_idx + 1]

            path_grid.append((best_x, best_y))
            pos_x, pos_y = best_x, best_y
        else:
            # Didn't reach destination in max_hops
            if self._dist(pos_x, pos_y, ex, ey) > tp_range_grid * 2:
                return []  # Too far, path failed

        # Convert grid path to game coordinates
        game_path = []
        for gx, gy in path_grid[1:]:  # skip start position
            game_path.append(self.grid_to_game(gx, gy))

        return game_path

    def _build_distance_table(self, end_x, end_y):
        """Build distance-to-destination table. Walkable cells get euclidean distance,
        walls get RANGE_INVALID."""
        table = []
        for y in range(self._grid_h):
            row = []
            for x in range(self._grid_w):
                if self._grid[y][x] == 0:  # walkable
                    row.append(int(self._dist(x, y, end_x, end_y)))
                else:
                    row.append(RANGE_INVALID)
            table.append(row)

        # Destination gets distance 1 (so it's always the best choice when in range)
        if self._is_valid(end_x, end_y):
            table[end_y][end_x] = 1

        return table

    def _find_nearest_walkable(self, x, y, max_search=20):
        """Find nearest walkable cell to (x, y)."""
        best_dist = 999999
        best_x, best_y = -1, -1
        for r in range(1, max_search):
            for dx in range(-r, r + 1):
                for dy in range(-r, r + 1):
                    nx, ny = x + dx, y + dy
                    if self._is_walkable(nx, ny):
                        d = abs(dx) + abs(dy)
                        if d < best_dist:
                            best_dist = d
                            best_x, best_y = nx, ny
            if best_x >= 0:
                return best_x, best_y
        return -1, -1
