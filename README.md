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
  never enter a one-tile pocket. Unseen edges are treated as closed. A sprint
  never treats its own tail as vacated mid-turn — the engine checks every
  step of a sprint against the board as it stood at the start of the turn, so
  the tail is a wall for the whole turn even on the step after it would
  normally have retracted.
- **Sprint with purpose.** A sprint costs one segment per extra step, so it is
  only used to grab two pearls at once or to steal a contested pearl from an
  enemy.
- **Split hard toward the real cap.** A length-2 child splits off once the
  parent reaches length 10 (6 on large maps), with the head kept to three
  exits. Caps are 10 / 24 / 48 dragons (against a true team limit of 64):
  head-to-head contact always kills both dragons regardless of length, so a
  longer enemy has no free way to punish a short farmer, and there is no
  starvation mechanic — more mouths is strictly more aggregate growth. A
  child born in a pocket dies on its first turn.
- **Trade up, never down.** A short dragon will ram a clearly longer enemy
  head — a mutual kill, per the rules, that trades a cheap dragon for an
  expensive one and drops half of each dragon's length as pearls at the
  collision site. Longer dragons stay a tile away so they are not rammed.
  The last dragon never invites a head-to-head. On a kill, the whole sonar
  budget for that turn broadcasts the loot's location to any ally in range.
- **Talk by sonar.** Teammates broadcast pearl positions and enemy heads using
  a 64-bit packed message with a multiplicative checksum, so random enemy
  sonar is ignored. Passive sonar echoes (free information from the same
  four pings, no separate cost) add a soft warning when an enemy head is
  somewhere on the map but not yet seen.

## Known limitation

On very kelp-dense maps the bot can still walk itself into a shrinking
pocket over many turns and end up with no legal move (visible in replays as
a run of `roam`/`go`/`eat` indicators followed by `stuck`). Every step —
eating, BFS goal-seeking, and roaming alike — is now filtered by a bounded
flood fill of reachable space before it is offered as a candidate, with
graceful fallback if that empties the candidate set. This is a real
improvement (see below) but not a full fix: a flood fill only sees a region
as fixed obstacles *right now*, and cannot know that the dragon's own body
will keep consuming that region's capacity as it keeps farming inside it.
A robust fix needs entrance/cut-vertex detection (is the tile ahead the
only way out of a region smaller than the body will eventually be?), which
is future work.

Local sanity check — `unswbc run <map> leviathan starter --sandbox`
(the judge's own compiler) across all 11 bundled maps, before vs. after
this round of changes:

| | before | after |
|---|---|---|
| wins vs. the stock starter | 6 / 11 | 9 / 11 |

`queen_of_spades` (the exact map of the team's worst ranked loss to date)
and `trauma` still lose to the pocket trap even after this fix.

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
unswbc submit leviathan -n "leviathan v2" -d "Sprint-tail fix, higher split caps, flood-fill pocket avoidance"
```

## Status

Turns use roughly 4–7 million of the 100 million CPU-point budget. Beats
the stock starter on 9 of the 11 bundled maps under `unswbc run --sandbox`
(the judge's own compiler); see Known limitation above for the two that
still lose to a self-trap on very kelp-dense terrain. Real ladder results
as of the last submission (`leviathan v1`, before this round of changes):
13 wins / 20 losses.
