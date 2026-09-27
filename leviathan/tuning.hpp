#pragma once

// Strategy knobs, gathered so a variant bot is a copy of this folder with a
// few numbers changed, and tools/arena.py can play the two against each other.
namespace tuning {

// 0 plays every map blind, as on a map the bot has never seen (the sprint
// tournament uses a new one): the test for everything that must work
// without mapdb.hpp.
static constexpr int USE_BAKED_MAPS = 1;

// Discount per step of distance when summing the pull of pearls, in percent.
static constexpr int GAMMA = 80;
// Chance, in percent, that a tile we have never seen holds a pearl.
static constexpr int FOG_PRIOR = 4;
// On a baked map: chance, in percent, that a pearl on a tile we cannot see
// is grazed away each round. Sets how full a tile stays between visits.
static constexpr int GRAZE = 5;
// How the baked pearl gaps price tiles: 0 not at all (edges only), 1 only
// rules out tiles that never spawn, 2 also prices every tile by its gap.
// Level 2 lost 32-22 to level 0's 39-15 in local games against our own
// bots. In unranked ladder games against the same teams on the same maps
// it ate more by round 100 (Queen of Spades 180 vs 66, Trophy 141 vs 98)
// but won no more (4/40 with the feeding cap vs 6/40). On with the v8
// aggressive settings: 5/12 on Default Small/Trophy/Autarky, same as off.
static constexpr int KNOWN_VALUE = 2;
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
static constexpr int CHILD_ROOM = 2;
// 1 splits even where an enemy head could reach us next turn.
// v8 copies the top teams: split at every chance, even under threat.
static constexpr int SPLIT_UNDER_THREAT = 1;
// Map tiles per dragon when sizing the team.
static constexpr int TILES_PER_DRAGON = 4;
// On maps with at least BIG_MAP_TILES tiles the team stops splitting at
// BIG_MAP_CAP dragons, so growth goes into longer dragons (64: no change).
static constexpr int BIG_MAP_TILES = 1500;
static constexpr int BIG_MAP_CAP = 64;
// A dragon at least this long with no safe step turns round by splitting off
// all but a two-segment head (0: never), when its tail has this much room.
static constexpr int REVERSE_LEN = 0;
static constexpr int REVERSE_ROOM = 4;
// A dragon rams an enemy head at least this much longer (both die). 0 trades
// evenly: the top-20 teams win by attrition, 30 head-on trades a game in the
// first 200 rounds, most of them started by the winner.
static constexpr int RAM_MARGIN = 0;
// Percent of a tile's pearl we still count when an enemy head is closer.
static constexpr int ENEMY_CONTEST = 80;
// Worth, in tenths of a pearl, of stepping through a portal whose far end
// we have never seen. Unexplored water behind portals is where the pearls
// pile up on portal-heavy maps.
static constexpr int EXPLORE_PORTAL = 25;
// Portals against friendly fire: 1 makes each crossing one-way (team B by
// mirrored tile order) on even rounds; 0 opens both ways in 4-round slots.
static constexpr int PORTAL_ONE_WAY = 0;
// ... except on this baked map, which always runs one-way.
static constexpr char const* ONE_WAY_MAP = "Trophy";
// 1 runs that map's portal into its centre instead of out of it.
static constexpr int ONE_WAY_INTO_CENTRE = 1;
// More than this many steps from the nearest rich tile of a baked map, ...
// Only in regions with no rich tile on foot (world.hpp starving_region):
// pulling Trophy's dragons into its contested centre lost 1/8 vs 5/8.
static constexpr int RICH_NEAR = 3;
// ... each step closer is worth this many tenths of a pearl.
static constexpr int RICH_PULL = 5;
// A baked region of at least 40 tiles whose tiles spawn at least this many
// pearls per 1000 rounds in total is a field: dragons in a starving region
// head for it as they would for a fast tile (0: fast tiles only). Trauma's
// middle field makes ~1090; its spawn region ~100.
static constexpr int FIELD_RATE = 500;
// Zone play. Until this round every dragon farther than RUSH_NEAR steps
// from a rich tile is pulled toward one, ...
static constexpr int RUSH_ROUNDS = 0;  // the rush lost 39-49 to v13
static constexpr int RUSH_NEAR = 2;
// ... at this many tenths of a pearl per step.
static constexpr int RUSH_PULL = 6;
// Within this many steps of a rich tile a dragon holds ground: crowding is
// ignored and ram risk counts this percent.
static constexpr int HOLD_RADIUS = 3;
static constexpr int HOLD_THREAT = 35;
// On a map we do not know, a tile is rich once no countdown read there
// exceeded this; the field is rebuilt every few rounds.
static constexpr int FAST_SEEN = 20;
// Short countdown reads, on different rounds, before a tile counts as fast
// on a map we do not know; 1 lets holding work there too.
static constexpr int BLIND_SHORT_READS = 2;
static constexpr int BLIND_HOLD = 0;
// Ram risk counts this percent on a move that eats a pearl now.
static constexpr int EAT_THREAT = 100;
// Nodes for the exact survival look-ahead (turns use 20-30M of 100M points).
static constexpr int SURVIVE_NODES = 4000;
static constexpr int RICH_FIELD_EVERY = 4;
// With no safe move, a single step that the look-ahead keeps alive this
// many moves beats paying a segment for a two-step escape.
static constexpr int SQUEEZE_LASTS = 8;
// ... on maps we do not know (8 there went 17-19 against 2's results; on
// baked maps 2 cost Stronghold 2/8 against 8's 6/8).
static constexpr int SQUEEZE_LASTS_BLIND = 2;
// Dragons at least this long never end a move where an enemy head could
// reach them next turn unless nothing else lives.
static constexpr int KING_GUARD_LEN = 8;
// Room counts and the survival search treat tiles another head could reach
// within this many moves as taken, for the first moves only ...
static constexpr int HEAD_REACH = 2;
// ... counting allied heads too (1) or only enemy heads (0). In our own
// swarm nearly every tile is within reach of an ally, so no single step
// counted as safe: we sprinted 5-20 times per 1000 dragon-turns (a segment
// each) where top-10 teams sprint 1-4, and split less (splits need a safe step).
static constexpr int HEAD_REACH_ALLIES = 0;
// With no safe move, the cost in pearls of a portal crossing we cannot see
// through: off our slot (allies cross the other way), or onto a landing a
// dragon stood near lately. AS_UNSAFE or more ranks it below every other move.
static constexpr int AS_UNSAFE = 1000000;
static constexpr int OFF_SLOT_PEARLS = AS_UNSAFE;
static constexpr int BLIND_LANDING_PEARLS = AS_UNSAFE;
// ...except from an on-foot region this small (baked maps only), where an
// off-slot crossing is the only way out of a portal room.
static constexpr int TRAPPED_ROOM = 24;
static constexpr int OFF_SLOT_TRAPPED_PEARLS = 20;
// Nook diving: only dragons at most this long dive (a long dragon is worth
// more alive), ...
static constexpr int DIVE_MAX_LEN = 6;
// ... through at most this many one-wide tiles, ...
static constexpr int DIVE_DEPTH = 8;
// ... and pockets of any shape up to this many tiles.
static constexpr int POCKET_MAX = 12;
// ... pricing each child it splits out at this many pearls.
static constexpr int DIVE_CHILD = 2;
// From this round the dragon that believes it is the king stops splitting,
// so it enters the feeding with a head start: it eats at most one pearl a
// round, and the tiebreak is the longest single dragon.
static constexpr int KING_HOLD_ROUND = 300;
// Feeding only on maps at least this many tiles (0: every map).
static constexpr int FEED_MIN_TILES = 0;
// From this round the team feeds its king (0 turns feeding off) ...
static constexpr int FEED_ROUND = 420;
// ... or this round on maps smaller than FEED_CAP_TILES, leaving the enemy
// less time to pick off a swarm that fed itself down. Full feeding from 450
// beat capping at half from 420: 56-32 vs 53-35 against v5 locally, and 8/12
// vs 6/12 in the same unranked ladder games (Ravenus 3/4 vs 1/4).
static constexpr int FEED_ROUND_SMALL = 450;
// ... keeping this many dragons (king included) out of it ...
static constexpr int FEED_KEEP = 3;
// ... and this share, in percent, of the team it began with (0: no cap).
// Feeding all but three got v7 eliminated from 19 dragons against 9 on
// Default Small, but a 50% cap lost 6-12 to v5 locally, where every
// opponent feeds and games end on the longest dragon; left off for now.
static constexpr int FEED_SHARE = 0;
// The cap applies only on maps smaller than this. On big maps games go the
// distance and the longest dragon decides: capped feeding lost Stronghold
// 0/8 against 6/8 uncapped. On small maps ladder teams eliminate a team
// that fed itself down (Default Small, Autarky replays).
static constexpr int FEED_CAP_TILES = 1100;

// Per-map overrides, chosen from local per-map results (tools: eval4.sh).
// -1 keeps the default. Maps not listed, and maps the bot does not
// recognise (the tournament's new maps), play with the defaults above.
struct MapProfile {
    char const* name;
    int width;            // > 0: an unbaked map, matched by its size
    int height;
    int ram_margin;
    int feed_round;       // replaces FEED_ROUND and FEED_ROUND_SMALL
    int king_hold_round;
    int hold = -1;        // 0: never hold ground near jackpots
    int rich_pull = -1;   // replaces RICH_PULL
};
// Wins for the override vs the default, summed over every local run:
static constexpr MapProfile MAP_PROFILES[] = {
    {"Queen Of Spades", 0, 0, 1, -1, -1},     // careful rams: 31/52
    {"Autarky", 0, 0, -1, 410, -1},           // feed earlier: 26/36
    {"Slithery Fight", 63, 27, -1, -1, 100},  // king stops splitting at 100: 23/36
};

}  // namespace tuning
