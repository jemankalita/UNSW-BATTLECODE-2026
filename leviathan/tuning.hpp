#pragma once

// Strategy knobs, gathered so a variant bot is a copy of this folder with a
// few numbers changed, and tools/arena.py can play the two against each other.
namespace tuning {

// Discount per step of distance when summing the pull of pearls, in percent.
static constexpr int GAMMA = 80;
// Chance, in percent, that a tile we have never seen holds a pearl.
static constexpr int FOG_PRIOR = 4;
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
// From this round the team feeds its king (0 turns feeding off) ...
static constexpr int FEED_ROUND = 420;
// ... keeping this many dragons (king included) out of it.
static constexpr int FEED_KEEP = 3;

}  // namespace tuning
