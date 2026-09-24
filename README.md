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
  tuning.hpp          strategy knobs
  helper.hpp          the toolkit's engine reader (unchanged)
starter/            the stock `unswbc init` bot, a reference opponent
maps/               bundled match maps
tests/test_logic.cpp  unit tests for logic.hpp
tools/arena.py      batch runner: every map, both sides, in parallel, in the judge sandbox
tools/genmap.py     random symmetric maps for wider testing
tools/stuck_dump.py draws a dragon's last few 7x7 views before it died
```

## Strategy

- **Swarm early.** Replays of strong ladder teams showed they split early and
  often, up to the 64-dragon limit. Leviathan splits a two-segment child
  whenever a dragon reaches length 4, up to one dragon per 12 map tiles,
  until round 380. More heads eat more pearls each round, and a big team is
  hard to wipe out.
- **Never die by accident.** Every move is checked by a flood fill in which
  our own body frees up as the tail passes, and by an exact bounded look-ahead
  that simulates the body step by step, counting pearls eaten on the way,
  since each pearl holds the tail back one move. Tiles that another head
  could reach before our next move count as possibly taken. Crossing a portal
  whose far side is out of view is a last resort only. When every move is bad,
  the dragon picks the one that keeps it alive longest.
- **Know the map.** Maps are mirrored in x, in y, or rotated 180°. Each dragon
  rules out symmetries whose mirror images disagree with what it has seen.
  Once one symmetry is left, it fills in unseen kelp and pearl timings from
  their mirror tiles.
- **Farm by expected value.** A BFS from each candidate move sums the
  distance-discounted chance of a pearl on every reachable tile. That uses
  remembered pearls, countdowns that will have run out by the time we arrive,
  and mirrored timings, and it discounts tiles an ally or enemy will reach
  first. Heads keep clear of each other and of narrow water between bodies.
- **Trade up.** A short dragon rams a clearly longer enemy head (both die),
  and a cornered dragon takes an enemy head with it. Longer dragons price the
  risk of ending a move where an enemy head could reach them.
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

Local results on the 11 bundled maps plus 24 generated maps, both sides (70 games each):

| matchup | result |
|---|---|
| leviathan vs `main`'s v3 (map-policy swarm and scorer) | 70 W / 0 L |
| leviathan vs stock starter | 70 W / 0 L |
| swarm settings vs the same bot with conservative splitting | 65 W / 5 L |
| king feeding vs the same bot without it | 45 W / 25 L |

These are all local games. Real ladder opponents may play differently.

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
