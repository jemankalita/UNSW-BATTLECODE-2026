"""Generate random symmetric maps for local testing, beyond the eleven bundled ones.

    python tools/genmap.py OUT_DIR --count 20 --seed 1

Maps mirror kelp, pearl timings and spawns the way real maps do (x, y or
180-degree symmetry), vary size, kelp density and pearl richness, and are
rejected unless most of the board is reachable from the spawns.
"""

from __future__ import annotations

import argparse
import pathlib
import random
from collections import deque


def mirror(kind: str, x: int, y: int, w: int, h: int) -> tuple[int, int]:
    """The engine names a symmetry by its mirror line: "y" reflects x, "x" reflects y."""
    if kind == "y":
        return w - 1 - x, y
    if kind == "x":
        return x, h - 1 - y
    return w - 1 - x, h - 1 - y


def edge_id(w: int, x: int, y: int, north: bool) -> int:
    row = 2 * y if north else 2 * y + 1
    return row * (w + 1) + x


class Board:
    def __init__(self, w: int, h: int, kind: str) -> None:
        self.w, self.h, self.kind = w, h, kind
        self.kelp_n: set[tuple[int, int]] = set()  # north side of (x, y)
        self.kelp_w: set[tuple[int, int]] = set()  # west side of (x, y)

    def add_kelp(self, x: int, y: int, side: str) -> None:
        """Kelp on one side of a tile, and on the mirrored side of its mirror."""
        w, h = self.w, self.h
        for kx, ky, s in ((x, y, side), self.mirror_side(x, y, side)):
            if s == "N":
                self.kelp_n.add((kx, ky))
            elif s == "S":
                self.kelp_n.add((kx, (ky + 1) % h))
            elif s == "W":
                self.kelp_w.add((kx, ky))
            else:
                self.kelp_w.add(((kx + 1) % w, ky))

    def mirror_side(self, x: int, y: int, side: str) -> tuple[int, int, str]:
        mx, my = mirror(self.kind, x, y, self.w, self.h)
        flip_x = self.kind in ("y", "xy")
        flip_y = self.kind in ("x", "xy")
        swap = {"E": "W", "W": "E"} if flip_x else {}
        swap.update({"N": "S", "S": "N"} if flip_y else {})
        return mx, my, swap.get(side, side)

    def blocked(self, x: int, y: int, side: str) -> bool:
        w, h = self.w, self.h
        if side == "N":
            return (x, y) in self.kelp_n
        if side == "S":
            return (x, (y + 1) % h) in self.kelp_n
        if side == "W":
            return (x, y) in self.kelp_w
        return ((x + 1) % w, y) in self.kelp_w

    def neighbours(self, x: int, y: int):
        steps = {"N": (0, -1), "S": (0, 1), "E": (1, 0), "W": (-1, 0)}
        for side, (dx, dy) in steps.items():
            if not self.blocked(x, y, side):
                yield (x + dx) % self.w, (y + dy) % self.h

    def reachable(self, start: tuple[int, int]) -> int:
        seen = {start}
        queue = deque([start])
        while queue:
            cell = queue.popleft()
            for nxt in self.neighbours(*cell):
                if nxt not in seen:
                    seen.add(nxt)
                    queue.append(nxt)
        return len(seen)


def pearl_profile(rng: random.Random) -> list[tuple[int, int]]:
    """A menu of (min, max) spawn gaps; a map draws its tiles from one menu."""
    menus = [
        [(1, 1000)] * 8 + [(1, 50)],
        [(1, 250), (1, 100), (1, 400)],
        [(8, 20), (8, 40)],
        [(1, 750)],
        [(1, 200)] * 12 + [(1, 1)],
        [(20, 200), (20, 80), (1, 20)],
    ]
    return rng.choice(menus)


def generate(rng: random.Random, name: str) -> str | None:
    w = rng.randrange(14, 49)
    h = rng.randrange(14, 41)
    kind = rng.choice(["x", "y", "xy"])
    board = Board(w, h, kind)
    density = rng.choice([0.0, 0.05, 0.12, 0.2, 0.28])
    for _ in range(int(density * w * h)):
        board.add_kelp(rng.randrange(w), rng.randrange(h), rng.choice("NSEW"))
    if rng.random() < 0.4:  # a walled border: no wrap-around
        for x in range(w):
            board.add_kelp(x, 0, "N")
        for y in range(h):
            board.add_kelp(0, y, "W")

    menu = pearl_profile(rng)
    gaps: dict[tuple[int, int], tuple[int, int]] = {}
    for y in range(h):
        for x in range(w):
            if (x, y) in gaps:
                continue
            gap = (0, 0) if rng.random() < 0.15 else rng.choice(menu)
            gaps[(x, y)] = gap
            gaps[mirror(kind, x, y, w, h)] = gap

    team_size = rng.choice([1, 2, 2, 3])
    length = rng.choice([3, 4, 4, 5])
    dragons: list[list[tuple[int, int]]] = []
    used: set[tuple[int, int]] = set()
    for _ in range(team_size):
        body = place_body(rng, board, length, used)
        if body is None:
            return None
        twin = [mirror(kind, x, y, w, h) for x, y in body]
        if used & set(twin) or set(body) & set(twin):
            return None
        used |= set(body) | set(twin)
        dragons.append(body)
        dragons.append(twin)

    if board.reachable(dragons[0][0]) < 0.6 * w * h:
        return None

    lines = [f"MAP {w} {h}", f"SYMMETRY {kind}", f"MAP_NAME {name}", f"TILE_COUNT {w * h}"]
    for y in range(h):
        for x in range(w):
            lo, hi = gaps[(x, y)]
            lines.append(f"TILE {x} {y} {lo} {hi}")
    edges = [edge_id(w, x, y, True) for x, y in sorted(board.kelp_n)]
    edges += [edge_id(w, x, y, False) for x, y in sorted(board.kelp_w)]
    lines.append(f"EDGE_COUNT {len(edges)}")
    lines += [f"EDGE {e} 1 -1" for e in sorted(edges)]
    lines.append(f"DRAGON_COUNT {len(dragons)}")
    for index, body in enumerate(dragons):
        coords = " ".join(f"{x} {y}" for x, y in body)
        lines.append(f"DRAGON {index % 2} {len(body)} {coords}")
    return "\n".join(lines) + "\n"


def place_body(rng: random.Random, board: Board, length: int, used: set) -> list[tuple[int, int]] | None:
    """A straight body with no kelp between its segments, head first."""
    steps = {"N": (0, -1), "S": (0, 1), "E": (1, 0), "W": (-1, 0)}
    back = {"N": "S", "S": "N", "E": "W", "W": "E"}
    for _ in range(200):
        x, y = rng.randrange(board.w), rng.randrange(board.h)
        facing = rng.choice("NSEW")
        dx, dy = steps[back[facing]]
        body = [(x, y)]
        ok = True
        for _seg in range(length - 1):
            px, py = body[-1]
            if board.blocked(px, py, back[facing]):
                ok = False
                break
            body.append(((px + dx) % board.w, (py + dy) % board.h))
        if ok and len(set(body)) == length and not (set(body) & used):
            return body
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    made = 0
    while made < args.count:
        name = f"gen{args.seed}_{made:02d}"
        text = generate(rng, name)
        if text is None:
            continue
        (out / f"{name}.map").write_text(text)
        made += 1
    print(f"wrote {made} maps to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
