"""Replay one match and draw what a dragon saw in its last turns before a death.

    & "$env:APPDATA\\uv\\tools\\unswbc\\Scripts\\python.exe" tools/stuck_dump.py maps/stronghold.map leviathan starter --team A --count 3

Each turn is drawn as the 7x7 window: '#' between cells is kelp, 'p' on an
edge is a portal, '*' a pearl, lowercase letters are body segments and
uppercase their heads (a letter per dragon id), '@' is the dying dragon's head.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import string
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from arena import build  # noqa: E402

KEEP_TURNS = 4


def parse_block(block: bytes) -> dict:
    lines = [l.split("#")[0].split() for l in block.decode(errors="replace").splitlines()]
    lines = [l for l in lines if l]
    out = {"tiles": [], "parts": [], "h": [], "v": []}
    i = 0
    while i < len(lines):
        head = lines[i][0]
        if head == "ROUND":
            out["round"] = int(lines[i][1])
        elif head == "LENGTH":
            out["length"] = int(lines[i][1])
        elif head == "DIR":
            out["dir"] = lines[i][1]
        elif head == "NUM_MSGS":
            i += int(lines[i][1])
        elif head == "DRAGON_BODIES":
            count = int(lines[i][1])
            out["parts"] = lines[i + 1 : i + 1 + count]
            rest = lines[i + 1 + count :]
            out["h"] = rest[:8]
            out["v"] = rest[8:15]
            break
        elif len(lines[i]) == 4 and head.lstrip("-").isdigit():
            out["tiles"].append([int(v) for v in lines[i]])
        i += 1
    return out


def edge_char(edge: str) -> str:
    return "#" if edge == "w" else (" " if edge == "." else "p")


def render(block: dict, me: int) -> list[str]:
    tiles = block["tiles"]
    cells = {}
    for index, (_x, _y, pearl, _count) in enumerate(tiles):
        cells[(index % 7, index // 7)] = "*" if pearl else "."
    letters: dict[int, str] = {}
    for _team, dragon, x, y, _facing, is_head in block["parts"]:
        pos = next((k % 7, k // 7) for k, t in enumerate(tiles) if t[0] == int(x) and t[1] == int(y))
        dragon = int(dragon)
        letter = letters.setdefault(dragon, string.ascii_lowercase[len(letters) % 26])
        if dragon == me and is_head == "1":
            cells[pos] = "@"
        else:
            cells[pos] = letter.upper() if is_head == "1" else letter
    rows = []
    for y in range(7):
        rows.append("".join("+" + edge_char(block["h"][y][x]) for x in range(7)) + "+")
        line = "".join(edge_char(block["v"][y][x]) + cells[(x, y)] for x in range(7))
        rows.append(line + edge_char(block["v"][y][7]))
    rows.append("".join("+" + edge_char(block["h"][7][x]) for x in range(7)) + "+")
    return rows + [" ".join(f"{letters[d]}={d}" for d in sorted(letters))]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("map")
    parser.add_argument("bot_a")
    parser.add_argument("bot_b")
    parser.add_argument("--team", default="A")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--label", default="stuck", help="only deaths whose last indicator is this ('' for all)")
    args = parser.parse_args()

    from unswbc.engine import EngineModule
    from unswbc.sandbox import SandboxBot, WasmPool

    pools = {"A": WasmPool([build(args.bot_a)], team="a"), "B": WasmPool([build(args.bot_b)], team="b")}
    live: dict = {}
    teams: dict = {}
    history: dict = collections.defaultdict(lambda: collections.deque(maxlen=KEEP_TURNS))
    shown = [0]

    def spawn(dragon_id: int, init: bytes) -> None:
        team = next((l.split()[1] for l in init.decode().splitlines() if l.startswith("TEAM")), "A")
        teams[dragon_id] = team
        live[dragon_id] = SandboxBot(pools[team], init=init, name=str(dragon_id))

    def reply(dragon_id: int, block: bytes) -> bytes:
        out = live[dragon_id].ask(block)
        history[dragon_id].append((block, out or b""))
        return out

    def death(dragon_id: int, round_num: int, reason: str) -> None:
        bot = live.pop(dragon_id, None)
        if bot is not None:
            bot.stop()
        turns = history.pop(dragon_id, [])
        if teams.get(dragon_id) != args.team or shown[0] >= args.count or not turns:
            return
        last = turns[-1][1].decode(errors="replace")
        if args.label and f"INDICATOR {args.label}" not in last:
            return
        shown[0] += 1
        print(f"==== dragon {dragon_id} died round {round_num} reason {reason}")
        for block, out in turns:
            parsed = parse_block(block)
            said = " | ".join(l for l in out.decode(errors="replace").splitlines()
                              if l.startswith(("MOVE", "SPLIT", "INDICATOR")))
            print(f"-- round {parsed.get('round')} len {parsed.get('length')} dir {parsed.get('dir')}: {said}")
            for row in render(parsed, dragon_id):
                print("   " + row)

    engine = EngineModule()
    try:
        engine.run(pathlib.Path(args.map).read_bytes(), reply, death, spawn, lambda _l: None, 0)
    finally:
        for bot in live.values():
            bot.stop()
        for pool in pools.values():
            pool.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
