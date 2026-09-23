# UNSW Battlecode 2026 — Leviathan

A C++ bot for [UNSW Battlecode](https://game.battlecode.au), the 2026 dragon
game. Each dragon runs its own copy of one program with no shared memory and
only ~25 ms (or 100 million CPU points) per turn, so the bot is built around
cheap local reasoning plus a checksummed sonar protocol for teammate
communication.

The win goes to the team that wipes the other side, or — after 500 rounds —
to the **longest single dragon**, then the most total length. Leviathan is
tuned for that scoring: it grows first, splits late, and only sacrifices a
short dragon into a clearly longer enemy.

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

- **Eat first.** Step onto a visible pearl, and remember tile countdowns so
  the bot returns just before a pearl spawns.
- **Stay alive.** Never reverse into its own neck, never cross kelp, and
  never enter a one-tile pocket. Unseen edges are treated as closed.
- **Sprint with purpose.** A sprint costs one segment per extra step, so it is
  only used to grab two pearls at once or to steal a contested pearl from an
  enemy.
- **Split late, and only with room.** A length-2 child is split off at length
  12 on small and medium maps (length 8 on large ones), and only if the head
  has three exits. Caps are 4 / 8 / 20 dragons. A child born in a pocket dies
  on its first turn.
- **Trade up, never down.** A short dragon will ram a clearly longer enemy
  head. Longer dragons stay a tile away so they are not rammed. The last
  dragon never invites a head-to-head.
- **Talk by sonar.** Teammates broadcast pearl positions and enemy heads using
  a 64-bit packed message with a multiplicative checksum, so random enemy
  sonar is ignored.

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
unswbc submit leviathan -n "leviathan v1" -d "Pearl chase, pocket avoidance, late splits"
```

## Status

Beats the stock starter on every bundled map, playing as either colour, under
the judge's sandbox. Turns use roughly 4–7 million of the 100 million CPU-point
budget.
