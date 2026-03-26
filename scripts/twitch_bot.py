"""Twitch chat bot for D2 farming stream.
Reads chat commands, posts status updates.
Runs alongside farming_loop.py."""

import socket, threading, time, json, os, requests
from dotenv import load_dotenv

load_dotenv(os.path.join(os.path.dirname(__file__), '.env'))

TWITCH_CHANNEL = os.getenv("TWITCH_CHANNEL", "bethington")
TWITCH_BOT = os.getenv("TWITCH_BOT_USERNAME", "bethington")
TWITCH_TOKEN = os.getenv("TWITCH_OAUTH_TOKEN", "")
MCP = "http://127.0.0.1:21337/mcp"

IRC_HOST = "irc.chat.twitch.tv"
IRC_PORT = 6667


class TwitchBot:
    def __init__(self):
        self.sock = None
        self.running = False
        self.last_status_time = 0
        self.status_interval = 120  # post status every 2 min

    def mcp(self, tool, args=None):
        try:
            r = requests.post(MCP, json={
                "jsonrpc": "2.0", "id": 1,
                "method": "tools/call",
                "params": {"name": tool, "arguments": args or {}}
            }, timeout=10)
            return json.loads(r.json()["result"]["content"][0]["text"])
        except:
            return {}

    def connect(self):
        self.sock = socket.socket()
        self.sock.connect((IRC_HOST, IRC_PORT))
        self.sock.send(f"PASS {TWITCH_TOKEN}\r\n".encode())
        self.sock.send(f"NICK {TWITCH_BOT}\r\n".encode())
        self.sock.send(f"JOIN #{TWITCH_CHANNEL}\r\n".encode())
        self.sock.settimeout(1.0)
        print(f"Connected to #{TWITCH_CHANNEL}")

    def send(self, msg):
        try:
            self.sock.send(f"PRIVMSG #{TWITCH_CHANNEL} :{msg}\r\n".encode())
            print(f"[CHAT] {msg}")
        except:
            pass

    def handle_command(self, user, cmd):
        cmd = cmd.strip().lower()

        if cmd == "!stats":
            stats = self.mcp("get_stream_stats")
            kills = stats.get("monsters_killed", 0)
            deaths = stats.get("deaths", 0)
            runs = stats.get("runs_completed", 0)
            items = stats.get("items_picked_up", 0)
            secs = stats.get("session_seconds", 0)
            mins = secs // 60
            self.send(f"Kills: {kills} | Deaths: {deaths} | Runs: {runs} | Items: {items} | Time: {mins}m")

        elif cmd == "!area":
            ps = self.mcp("get_player_state")
            area = ps.get("area_name", "Unknown")
            hp = ps.get("hp", 0)
            max_hp = ps.get("max_hp", 1)
            self.send(f"Area: {area} | HP: {hp}/{max_hp} ({hp*100//max_hp}%)")

        elif cmd == "!build":
            ps = self.mcp("get_player_state")
            name = ps.get("name", "?")
            cls = ps.get("class", "?")
            lvl = ps.get("level", 0)
            mf = ps.get("find", {}).get("magic_find", 0)
            self.send(f"{name} - Level {lvl} {cls} | MF: {mf}%")

        elif cmd == "!hp":
            ps = self.mcp("get_player_state")
            hp = ps.get("hp", 0)
            max_hp = ps.get("max_hp", 1)
            pct = hp * 100 // max_hp if max_hp > 0 else 0
            self.send(f"HP: {hp}/{max_hp} ({pct}%)")

        elif cmd == "!deaths":
            stats = self.mcp("get_stream_stats")
            self.send(f"Deaths: {stats.get('deaths', 0)} | Chickens: {stats.get('chickens', 0)}")

        elif cmd == "!help" or cmd == "!commands":
            self.send("Commands: !stats !area !build !hp !deaths !help")

    def auto_status(self):
        """Post periodic status updates to chat."""
        now = time.time()
        if now - self.last_status_time < self.status_interval:
            return
        self.last_status_time = now

        stats = self.mcp("get_stream_stats")
        status = stats.get("status", "")
        kills = stats.get("monsters_killed", 0)
        if status and kills > 0:
            self.send(f"[BOT] {status} | {kills} kills this session")

    def run(self):
        self.connect()
        self.running = True
        self.send(f"Claude AI bot is live! Type !help for commands.")

        while self.running:
            try:
                data = self.sock.recv(4096).decode("utf-8", errors="ignore")
            except socket.timeout:
                self.auto_status()
                continue
            except:
                break

            for line in data.split("\r\n"):
                if not line:
                    continue

                # Respond to PING to stay connected
                if line.startswith("PING"):
                    self.sock.send(f"PONG {line[5:]}\r\n".encode())
                    continue

                # Parse chat messages
                if "PRIVMSG" in line:
                    try:
                        user = line.split("!")[0][1:]
                        msg = line.split(f"PRIVMSG #{TWITCH_CHANNEL} :")[1]
                        if msg.startswith("!"):
                            self.handle_command(user, msg)
                    except:
                        pass

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.send("Bot going offline. GG!")
                self.sock.close()
            except:
                pass


def main():
    bot = TwitchBot()
    try:
        bot.run()
    except KeyboardInterrupt:
        bot.stop()
    except Exception as e:
        print(f"Error: {e}")
        bot.stop()


if __name__ == "__main__":
    main()
