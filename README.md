# UNSW Battlecode 2026 — Leviathan

A C++ bot for [UNSW Battlecode](https://game.battlecode.au), the 2026 dragon
game. Each dragon runs its own copy of one program with no shared memory and
only ~25 ms (or 100 million CPU points) per turn, so the bot is built around
cheap local reasoning plus a checksummed sonar protocol for teammate
communication.

The win goes to the team that wipes the other side, or — after 500 rounds —
to the **longest single dragon**, then the most total length. Leviathan v3
plays that in two phases. Early, it splits into length-3–4 farmers so the
map is covered. On maps that run long, one dragon becomes the scorer and
stops splitting so the team banks a single long body.

## Repository layout

```
leviathan/        the bot — main.cpp, logic.hpp, helper.hpp, bot.toml
starter/           the stock `unswbc init` bot, kept as a reference opponent
maps/              bundled match maps shipped by the toolkit
tests/test_logic.cpp   unit tests for the pure rules in logic.hpp
Battlecode_Postmortem_2026.pdf   design notes / postmortem
```

`helper.hpp` is the toolkit's engine reader; `logic.hpp` holds the pure,
testable rules (torus geometry, portal traversal, sonar packing, combat and
split policy); `main.cpp` is the per-turn decision loop.

## Strategy

Map class comes from area, tuned to the bundled ladder maps:

| Class | Maps | Early split | Cap | Late game |
|-------|------|-------------|-----|-----------|
| Snowball | Arena, Colosseum, Default Small, Devil | length 3 | 12 | no scorer; stop splitting once 8 dragons exist after round 100 |
| Mid | Default, Trophy, Queen Of Spades | length 4 (3 before round 60 if fewer than 6 dragons) | 16 | light scorer from length 10; outer sectors before the center |
| Farm | Trauma, Stronghold, Schooltime, Big Empty | length 4, then 6 after round 200 | 28 | one scorer from length 12; that dragon never splits |

- **Eat first.** Step onto a visible pearl, and remember tile countdowns so
  the bot returns just before a pearl spawns. A body of length 8 or more
  needs two exits after the bite. A one-exit corridor next to an enemy head
  is refused.
- **Stay alive.** Never reverse into its own neck, never cross kelp, and
  never enter a one-tile pocket. A sprint is legal only when both edges have
  already been seen. Unseen edges are walls for real steps.
- **Roles.** The scorer is the longest dragon (lowest id on a tie) once it
  reaches the map's length gate and round 80. It does not split, does not
  pick offensive rams, and keeps a wider gap from enemy heads. Farmers keep
  splitting up to the cap, yield pearls to a longer ally, and spread across
  sectors.
- **Sprint with purpose.** A sprint costs one segment per extra step. Snowball
  maps sprint only to escape or to steal a contested pearl. Mid maps may also
  take two pearls. Farm maps may cross a gap to a known pearl once length is
  at least 5. Fog sprints are never taken.
- **Portals.** Recorded portal pairs are one-cost edges in the search. An
  exit sitting on an enemy head is refused unless the dragon is escaping a
  trap. Big Empty has no portals, so that search stays off.
- **Trade up, never down.** A short dragon will ram a clearly longer enemy
  head. On snowball maps the trade is a little looser. Longer dragons step
  away from a near-equal head. The last dragon never invites a head-to-head.
- **Talk by sonar.** Teammates broadcast pearls, claims, sector owners,
  the scorer, and enemy heads. Messages are a 64-bit body with the same
  multiplicative checksum, so random enemy sonar is ignored. Kinds 1 and 2
  stay compatible with v2. Claims expire after 40 rounds.

## Quickstart

Install the toolkit (Python 3.11+ or `uv`):

```
uv tool install unswbc          # or: pip install unswbc
```

Run a match against the stock starter:

```
unswbc run maps\arena.map leviathan starter
```

Open the generated `.replay` in VS Code / Cursor, or run `unswbc vscode` to
install the viewer. The same command works with any map in `maps/`.

## Tests

The pure rules in `logic.hpp` are unit-tested independently of the engine:

```
g++ -std=c++14 -Wall -Wextra -o tests/test_logic.exe tests/test_logic.cpp
./tests/test_logic.exe
```

## Submitting

Create an API key on your team page (it starts with `bc_`), then:

```
unswbc auth set bc_...
unswbc submit leviathan -n "leviathan v3" -d "v3 map-policy swarm+scorer, portals, claims"
```

## Status

Against the stock starter in the judge sandbox, Leviathan wins both colours on
Arena, Colosseum, Default, Trophy, Queen Of Spades, Trauma, Schooltime, and
Big Empty. It also wins Default Small, Devil, and Stronghold as red. The three
blue losses are Default Small, Devil, and Stronghold. v2 lost Devil and
Stronghold as blue as well, and lost Default Small as red, which v3 now wins.
Turns use about 4–9 million of the 100 million CPU-point budget. Default and
Trauma run to round 500 and win on the longest dragon.

On snowball maps a one-exit tile is usually the lane, not a pocket, so a long
body may eat there when the next tile still has a way forward. Those maps also
keep the plain pearl sonar from v2, because a single claim sends the other
dragon off the lane and into kelp.
