#pragma once

#include <cstdint>

// Pure rules shared by the bot and the unit tests.
// Directions are 0 north, 1 east, 2 south, 3 west. Y grows south.
namespace logic {

static constexpr int DIR_N = 0;
static constexpr int DIR_E = 1;
static constexpr int DIR_S = 2;
static constexpr int DIR_W = 3;

// Sonar message kinds. A KING message names the longest ally the sender
// knows of: where its head was, its length, its id, and when.
static constexpr int KIND_KING = 3;

static constexpr std::uint64_t SONAR_KEY = 0xA31B57C9E2046DF1ull;
// Team B's messages use SONAR_KEY ^ SONAR_TEAM_B. A sonar reaches either
// team, so in a local game against ourselves one key let each side crown the
// other's king (on the ladder the other team's words fail the checksum).
static constexpr std::uint64_t SONAR_TEAM_B = 0x5D2C8E71B4F90A63ull;
static constexpr std::uint64_t SONAR_MUL = 0x9E3779B97F4A7C15ull;

inline int opposite_dir(int dir) {
    return dir ^ 2;
}

inline int right_of(int dir) {
    return (dir + 1) & 3;
}

inline int left_of(int dir) {
    return (dir + 3) & 3;
}

inline int wrap_coord(int value, int size) {
    if (size <= 0) {
        return 0;
    }
    int wrapped = value % size;
    if (wrapped < 0) {
        wrapped += size;
    }
    return wrapped;
}

struct Point {
    int x;
    int y;
    bool ok;
};

inline Point step_wrap(int x, int y, int dir, int width, int height) {
    int dx = 0;
    int dy = 0;
    if (dir == DIR_N) {
        dy = -1;
    } else if (dir == DIR_E) {
        dx = 1;
    } else if (dir == DIR_S) {
        dy = 1;
    } else {
        dx = -1;
    }
    Point point;
    point.x = wrap_coord(x + dx, width);
    point.y = wrap_coord(y + dy, height);
    point.ok = true;
    return point;
}

inline int torus_span(int a, int b, int size) {
    int delta = a - b;
    if (delta < 0) {
        delta = -delta;
    }
    int around = size - delta;
    return delta < around ? delta : around;
}

inline int chebyshev(int x1, int y1, int x2, int y2, int width, int height) {
    int dx = torus_span(x1, x2, width);
    int dy = torus_span(y1, y2, height);
    return dx > dy ? dx : dy;
}

struct PortalSide {
    int x;
    int y;
    int dir;
};

inline bool same_edge(int x, int y, int dir, PortalSide side, int width, int height) {
    if (side.x == x && side.y == y && side.dir == dir) {
        return true;
    }
    Point next = step_wrap(x, y, dir, width, height);
    return side.x == next.x && side.y == next.y && side.dir == opposite_dir(dir);
}

// Exit tile after crossing a portal from (x, y) in dir.
// sides lists both ends. Entering one side emerges on the matching side of the other.
inline Point emerge(int x, int y, int dir, int width, int height, PortalSide const* sides, int count) {
    Point missing;
    missing.x = 0;
    missing.y = 0;
    missing.ok = false;
    for (int i = 0; i < count; ++i) {
        if (same_edge(x, y, dir, sides[i], width, height)) {
            continue;
        }
        if (sides[i].dir == dir) {
            return step_wrap(sides[i].x, sides[i].y, dir, width, height);
        }
        if (sides[i].dir == opposite_dir(dir)) {
            Point point;
            point.x = sides[i].x;
            point.y = sides[i].y;
            point.ok = true;
            return point;
        }
    }
    return missing;
}

struct SonarMsg {
    bool ok;
    int kind;
    int x;
    int y;
    int aux;
    int id;
    int round_mod;
};

inline std::uint64_t pack_sonar(int kind, int x, int y, int aux, int id, int round, std::uint64_t key = SONAR_KEY) {
    std::uint64_t body = 0;
    body |= static_cast<std::uint64_t>(kind & 15);
    body |= static_cast<std::uint64_t>(x & 255) << 4;
    body |= static_cast<std::uint64_t>(y & 255) << 12;
    body |= static_cast<std::uint64_t>(aux & 255) << 20;
    body |= static_cast<std::uint64_t>(id & 4095) << 28;
    body |= static_cast<std::uint64_t>(round & 255) << 40;
    std::uint64_t sum = (body * SONAR_MUL) & 0xFFFFull;
    return ((body << 16) | sum) ^ key;
}

inline SonarMsg unpack_sonar(std::uint64_t word, std::uint64_t key = SONAR_KEY) {
    SonarMsg message;
    message.ok = false;
    message.kind = 0;
    message.x = 0;
    message.y = 0;
    message.aux = 0;
    message.id = 0;
    message.round_mod = 0;
    std::uint64_t raw = word ^ key;
    std::uint64_t sum = raw & 0xFFFFull;
    std::uint64_t body = raw >> 16;
    if (((body * SONAR_MUL) & 0xFFFFull) != sum) {
        return message;
    }
    message.ok = true;
    message.kind = static_cast<int>(body & 15);
    message.x = static_cast<int>((body >> 4) & 255);
    message.y = static_cast<int>((body >> 12) & 255);
    message.aux = static_cast<int>((body >> 20) & 255);
    message.id = static_cast<int>((body >> 28) & 4095);
    message.round_mod = static_cast<int>((body >> 40) & 255);
    return message;
}

inline int sonar_age(int current_round, int message_round_mod) {
    int age = (current_round & 255) - (message_round_mod & 255);
    if (age < 0) {
        age += 256;
    }
    return age;
}

// Every map is symmetric one of three ways; which one is for us to find out.
static constexpr int SYM_FLIP_X = 0;  // (x, y) <-> (w-1-x, y)
static constexpr int SYM_FLIP_Y = 1;  // (x, y) <-> (x, h-1-y)
static constexpr int SYM_ROTATE = 2;  // (x, y) <-> (w-1-x, h-1-y)
static constexpr int SYM_KINDS = 3;

inline Point mirror_tile(int kind, int x, int y, int width, int height) {
    Point p;
    p.x = kind == SYM_FLIP_Y ? x : width - 1 - x;
    p.y = kind == SYM_FLIP_X ? y : height - 1 - y;
    p.ok = true;
    return p;
}

// The side of the mirrored tile that the mirror of side 'dir' lands on.
inline int mirror_dir(int kind, int dir) {
    bool horizontal = dir == DIR_E || dir == DIR_W;
    bool flips = kind == SYM_ROTATE || (kind == SYM_FLIP_X && horizontal) || (kind == SYM_FLIP_Y && !horizontal);
    return flips ? opposite_dir(dir) : dir;
}

inline int manhattan(int x1, int y1, int x2, int y2, int width, int height) {
    return torus_span(x1, x2, width) + torus_span(y1, y2, height);
}

static constexpr int NEVER = 1 << 28;

// Moves after this one until our own segment 'index' (0 is the head) has
// left its tile, for a body of 'length' that does not eat on the way. At the
// t-th move the engine checks the board before the tail steps, so the
// current tail is still a wall: segment i is gone only once t > length - i.
inline int unlock_time(int length, int index) {
    if (index <= 0) {
        return NEVER;
    }
    return length - index + 1;
}

// Reachable room a head needs to count a move as safe: enough tiles for the
// whole body to follow it, plus one to keep moving. Other dragons keep
// moving too, so even a tiny dragon wants a little open water.
static constexpr int MIN_ROOM = 10;

inline int room_needed(int length) {
    return length + 1 > MIN_ROOM ? length + 1 : MIN_ROOM;
}

// Moving into an enemy head kills both dragons. Worth it when trading up by a
// clear margin, never for our last dragon (that is at best a draw), and
// always when every other move is death anyway.
inline bool worth_ram(int self_len, int enemy_len, int allies, bool cornered, int margin = 3) {
    if (cornered) {
        return true;
    }
    if (allies <= 1) {
        return false;
    }
    return enemy_len >= self_len + margin;
}

// Expected cost of ending a move where an enemy head can reach ours next
// turn: 'reach' 1 is a plain step, 2 needs a sprint. A ram costs our whole
// length, and a rational enemy rams when it is not the bigger loser.
inline int ram_threat(int self_len, int enemy_len, int allies, int reach, int margin = 3) {
    if (allies > 1 && enemy_len >= self_len + margin) {
        return 0;
    }
    int stake = 40 + 12 * self_len;
    if (allies <= 1) {
        stake *= 3;
    }
    if (enemy_len > self_len) {
        stake /= 2;
    }
    if (reach >= 2) {
        stake /= 4;
    }
    return stake;
}

// Chance, in percent, that a remembered tile holds a pearl when we arrive.
// Pearls stay until eaten, so a tile whose countdown has run out is likely
// full; memory goes stale as other dragons graze.
inline int pearl_chance(bool seen, bool pearl, int age, int spawn_round, int arrive_round, int prior) {
    if (!seen) {
        return prior;
    }
    int keep = 100 - age / 2;
    if (keep < 25) {
        keep = 25;
    }
    if (pearl) {
        return age == 0 ? 100 : 90 * keep / 100;
    }
    if (spawn_round < 0 || arrive_round < spawn_round) {
        return 0;
    }
    int since = arrive_round - spawn_round;
    int fresh = since < age ? since : age;
    int chance = 85 - fresh / 3;
    return chance < 20 ? 20 : chance;
}

// Chance, in percent, that a tile holds a pearl 'rounds' after its countdown
// was last drawn from [lo, hi], when we know the gap from the baked map. The
// first countdown is drawn the same way, and a pearl stays until it is eaten,
// so the chance climbs to 'keep' (others graze too) once the widest gap has
// passed. A tile with hi 0 never spawns.
inline int gap_chance(int lo, int hi, int rounds, int keep) {
    if (hi <= 0 || rounds < lo) {
        return 0;
    }
    int span = hi - lo + 1;
    int passed = rounds - lo + 1;
    if (passed >= span) {
        return keep;
    }
    int chance = keep * passed / span;
    return chance < 1 ? 1 : chance;
}

// Long-run share of rounds, in percent, that a tile holds a pearl when it
// spawns one every (lo + hi) / 2 rounds on average and anything on it is
// grazed away with 'graze' percent chance each round. Fast tiles stay full
// most of the time; slow ones are almost always bare, however long unseen.
inline int steady_chance(int lo, int hi, int graze) {
    if (hi <= 0) {
        return 0;
    }
    int chance = 20000 / (200 + graze * (lo + hi));
    return chance < 1 ? 1 : chance;
}

// Largest team a map can feed: one dragon per 'tiles_each' tiles, at least
// a handful, never past the engine's unit limit. More heads graze more
// pearls a round, and more bodies make the team hard to wipe out.
inline int team_cap(int width, int height, int unit_limit, int tiles_each) {
    int cap = width * height / (tiles_each > 0 ? tiles_each : 1);
    if (cap < 4) {
        cap = 4;
    }
    return cap < unit_limit ? cap : unit_limit;
}

// Split off a two-segment farmer while the team is small and the game young.
inline bool want_split(int length, int units, int cap, int round, int split_len, int last_round) {
    if (round > last_round || units >= cap) {
        return false;
    }
    return length >= split_len;
}

// The team's king is its longest dragon, ties to the lower id, so every
// dragon that hears the same news crowns the same one.
inline bool better_king(int len_a, int id_a, int len_b, int id_b) {
    return len_a != len_b ? len_a > len_b : id_a < id_b;
}

// The tiebreak at the round limit is the longest single dragon. Late in the
// game the rest of the team dies beside the king, and every second segment
// of a dead dragon crystallises into a pearl for the king to eat. A few
// dragons are kept back so the team cannot be wiped out.
inline bool feeding_time(int round, int feed_round, int units, int keep) {
    return round >= feed_round && units > keep;
}

// The king eats at most a pearl a round, so only so many feeders can help,
// and every dragon fed is one fewer to stop an elimination. Feeding stops
// once the team is down to 'share' percent of its size when it began.
inline int feed_floor(int units, int keep, int share) {
    int floor = units * share / 100;
    return floor > keep ? floor : keep;
}

// A feeder only helps if it arrives with time for the king to eat what it
// leaves: the walk there, plus about a move for each pearl its body drops.
inline bool can_feed_in_time(int distance, int length, int round, int last_round) {
    return round + distance + (length + 1) / 2 + 2 <= last_round;
}

}  // namespace logic
