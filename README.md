# UNSW Battlecode 2026 — Leviathan

A C++ bot for [UNSW Battlecode](https://game.battlecode.au), the 2026 dragon
game. Each dragon runs its own copy of one program with no shared memory and
roughly 25 ms (100 million CPU points) per turn.

A game is won by wiping out the other team, or, after 500 rounds, by the
**longest single dragon**, then by total length. Leviathan plays a swarm game
for most of the match and then turns that swarm into one long dragon for the
tiebreak.

## Repository layout

```
leviathan/          the bot
  main.cpp            per-turn decisions (Brain)
  world.hpp           map memory: kelp, portals, pearl timing, symmetry inference
  king.hpp            which ally is the team's king, relayed by sonar
  logic.hpp           pure, unit-tested rules (geometry, sonar, combat, splitting, feeding)
  mapdb.hpp           the ranked maps, baked in by tools/mapdb.py (generated)
  tuning.hpp          strategy knobs
  helper.hpp          the toolkit's engine reader (unchanged)
starter/            the stock `unswbc init` bot, a reference opponent
maps/               bundled match maps
tests/test_logic.cpp  unit tests for logic.hpp
tools/arena.py      batch runner: every map, both sides, in parallel, in the judge sandbox
tools/genmap.py     random symmetric maps for wider testing
tools/stuck_dump.py draws a dragon's last few 7x7 views before it died
tools/mapdb.py      bakes .map files into leviathan/mapdb.hpp
```

## Strategy

- **Swarm early.** Every top-50 team in 587 ladder replays splits a
  two-segment child the moment a dragon reaches length 4, from round 0, and
  keeps 40–64 dragons until about round 400. Leviathan does the same, up to
  one dragon per 12 map tiles, until round 380, and splits whenever the
  child has a few free tiles. Losing children is cheap. Being outnumbered is
  not: on the small maps the team with more heads wipes out the other.
- **Never die by accident.** Every move is checked by a flood fill in which
  our own body frees up as the tail passes, and by an exact bounded look-ahead
  that simulates the body step by step, counting pearls eaten on the way,
  since each pearl holds the tail back one move. Tiles that another head
  could reach before our next move count as possibly taken. Crossing a portal
  whose far side is out of view is a last resort only. When every move is bad,
  the dragon picks the one that keeps it alive longest.
- **Go through portals.** Four of the nine ranked maps wall each spawn in
  behind portals, with the fountain tiles (pearls every round) beyond them.
  A portal whose far end has never been seen is still crossed when the
  current area is running dry. The engine always lands the head on a real
  tile, and the pair's far end is learned on arrival.
- **Know the map.** Maps are mirrored in x, in y, or rotated 180°. Each dragon
  rules out symmetries whose mirror images disagree with what it has seen.
  Once one symmetry is left, it fills in unseen kelp and pearl timings from
  their mirror tiles.
- **Know the ranked maps.** The ladder plays nine fixed maps, and the API
  serves their text. `tools/mapdb.py` bakes their kelp, portals and pearl
  gaps into the bot. A dragon keeps every baked map of its board's size that
  agrees with each tile it sees (edges, and whether the tile spawns at all),
  and once one is left it knows every kelp edge and where every portal
  leads, including edges it has never seen. Any disagreement drops the
  baked map, so custom maps fall back to plain inference. The board is a
  torus: the file's last edge row and column are padding, and the bottom
  row's south sides are the top row's north sides.
- **Farm by expected value.** A BFS from each candidate move sums the
  distance-discounted chance of a pearl on every reachable tile. That uses
  remembered pearls, countdowns that will have run out by the time we arrive,
  and mirrored timings, and it discounts tiles an ally or enemy will reach
  first. Heads keep clear of each other and of narrow water between bodies.
- **Trade up.** A short dragon rams a clearly longer enemy head (both die),
  and a cornered dragon takes an enemy head with it. Longer dragons price the
  risk of ending a move where an enemy head could reach them.
- **Hold the king.** From round 300 the dragon that believes it is the
  longest stops splitting. It can eat at most a pearl a round, so it needs
  that head start before the feeding begins.
- **Feed the king.** Every dragon relays, by sonar each turn, the longest ally
  it knows of. From round 420 the rest of the team, except the last few
  dragons, walk to that king and die beside it. A dead dragon turns every
  second segment into a pearl, so the swarm's total length becomes one long
  dragon for the tiebreak.

## Testing

Unit tests for the pure rules:

```
g++ -std=c++14 -Wall -Wextra -o tests/test_logic.exe tests/test_logic.cpp
./tests/test_logic.exe
```

Batch matches in the judge's own sandbox. Use the unswbc tool's Python, so
its engine imports:

```
& "$env:APPDATA\uv\tools\unswbc\Scripts\python.exe" tools/arena.py leviathan starter --why
& "$env:APPDATA\uv\tools\unswbc\Scripts\python.exe" tools/arena.py leviathan other_bot --maps maps gen_maps
python tools/genmap.py gen_maps --count 24 --seed 7
```

Local results. Each row is both sides of every map; "35 maps" means the 11
bundled plus 24 generated ones, and "33 maps" means the 9 ranked plus 24
generated.

| matchup | result |
|---|---|
| v6 vs v5 / v4 / v3 (9 ranked maps, both sides) | 7-11 / 13-5 / 18-0 (v5's own record: 9-9 / 12-6 / 18-0) |
| v5 (portal exploration, eager splits) vs v4 (33 maps) | 47 W / 19 L |
| v4 vs `main`'s v3 (35 maps) | 70 W / 0 L |
| v4 vs stock starter (35 maps) | 70 W / 0 L |
| king feeding vs none (35 maps) | 45 W / 25 L |

Games are deterministic, so one map played twice is two games, and a small
change reshuffles outcomes; treat differences of a few games as noise.
Pricing tiles by their baked pearl gaps (`KNOWN_VALUE` 2) lost 32-22 against
the same three opponents, so v6 uses the baked maps for edges and portals only.

Settings that tested neutral or worse and were left out: one dragon per 8
tiles (34–32), feeding from round 400 (29–37), keeping 8 dragons out of
feeding (40–30 against no feeding, versus 45–25 when keeping 3).

These are all local games; the ladder is the real test.

Turns use about 7–16M of the 100M CPU-point budget, with a worst case near 60M.

## Known limitations

- Feeding needs the sonar relay to reach allies. On kelp-heavy maps rays stop
  at kelp, so some dragons never hear of the king and farm instead.
- The safety checks see other dragons only as they stand now. A crowd of
  allies can still close around a small dragon in narrow water.

## Submitting

```
unswbc auth set bc_...
unswbc submit leviathan -n "leviathan v3" -d "Swarm, safe look-ahead, symmetry, king feeding"
```
