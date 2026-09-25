#pragma once

// Strategy knobs, gathered so a variant bot is a copy of this folder with a
// few numbers changed, and tools/arena.py can play the two against each other.
namespace tuning {

// Discount per step of distance when summing the pull of pearls, in percent.
static constexpr int GAMMA = 80;
// Chance, in percent, that a tile we have never seen holds a pearl.
static constexpr int FOG_PRIOR = 4;
// On a baked map: chance, in percent, that a pearl on a tile we cannot see
// is grazed away each round. Sets how full a tile stays between visits.
static constexpr int GRAZE = 5;
// How the baked pearl gaps price tiles: 0 not at all (edges only), 1 only
// rules out tiles that never spawn, 2 also prices every tile by its gap.
// Level 2 lost 32-22 to level 0's 39-15 in local games, so it stays off.
static constexpr int KNOWN_VALUE = 0;
// A dragon splits off a farmer once it is at least this long ...
static constexpr int SPLIT_LEN = 4;
// ... and only up to this round.
static constexpr int SPLIT_LAST_ROUND = 380;
// Segments handed to each new farmer.
static constexpr int CHILD_SIZE = 2;
// 1 keeps v4's cautious splitting (no split beside an enemy or instead of a
// pearl); 0 splits whenever the child has room to move.
static constexpr int SPLIT_CAREFUL = 0;
// Free tiles a newborn child needs around it.
static constexpr int CHILD_ROOM = 4;
// Map tiles per dragon when sizing the team.
static constexpr int TILES_PER_DRAGON = 12;
// Worth, in tenths of a pearl, of stepping through a portal whose far end
// we have never seen. Unexplored water behind portals is where the pearls
// pile up on portal-heavy maps.
static constexpr int EXPLORE_PORTAL = 25;
// From this round the dragon that believes it is the king stops splitting,
// so it enters the feeding with a head start: it eats at most one pearl a
// round, and the tiebreak is the longest single dragon.
static constexpr int KING_HOLD_ROUND = 300;
// From this round the team feeds its king (0 turns feeding off) ...
static constexpr int FEED_ROUND = 420;
// ... keeping this many dragons (king included) out of it.
static constexpr int FEED_KEEP = 3;

}  // namespace tuning
