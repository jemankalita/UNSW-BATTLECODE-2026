#pragma once

#include <cstdint>

// Pure rules shared by the bot and the unit tests.
// Directions are 0 north, 1 east, 2 south, 3 west. Y grows south.
namespace logic {

static constexpr int DIR_N = 0;
static constexpr int DIR_E = 1;
static constexpr int DIR_S = 2;
static constexpr int DIR_W = 3;

static constexpr int KIND_PEARL = 1;
static constexpr int KIND_ENEMY = 2;
static constexpr int KIND_CLAIM = 3;
static constexpr int KIND_SECTOR = 4;
static constexpr int KIND_SCORER = 5;

// Tuned to the bundled ladder maps:
//   Snowball   area <= 520   Arena, Colosseum, Default Small, Devil
//   MidContest area <= 1100  Trophy, Queen Of Spades, Default
//   FarmLength area >  1100  Trauma, Stronghold, Schooltime, Big Empty
enum class Macro {
    Snowball = 0,
    MidContest = 1,
    FarmLength = 2,
};

static constexpr std::uint64_t SONAR_KEY = 0xA31B57C9E2046DF1ull;
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

inline std::uint64_t pack_sonar(int kind, int x, int y, int aux, int id, int round) {
    std::uint64_t body = 0;
    body |= static_cast<std::uint64_t>(kind & 15);
    body |= static_cast<std::uint64_t>(x & 255) << 4;
    body |= static_cast<std::uint64_t>(y & 255) << 12;
    body |= static_cast<std::uint64_t>(aux & 255) << 20;
    body |= static_cast<std::uint64_t>(id & 4095) << 28;
    body |= static_cast<std::uint64_t>(round & 255) << 40;
    std::uint64_t sum = (body * SONAR_MUL) & 0xFFFFull;
    return ((body << 16) | sum) ^ SONAR_KEY;
}

inline SonarMsg unpack_sonar(std::uint64_t word) {
    SonarMsg message;
    message.ok = false;
    message.kind = 0;
    message.x = 0;
    message.y = 0;
    message.aux = 0;
    message.id = 0;
    message.round_mod = 0;
    std::uint64_t raw = word ^ SONAR_KEY;
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

// Only a short dragon rams a clearly longer enemy. The last dragon on our team never does:
// dying with it is a draw at best, and a loss if they still have dragons.
// Snowball farmers may ram a little more freely (self <= 5, enemy at least self+2).
inline bool worth_trade(int self_len, int enemy_len, int allies, bool snowball) {
    if (allies <= 1) {
        return false;
    }
    if (snowball) {
        if (self_len > 5) {
            return false;
        }
        return enemy_len >= self_len + 2;
    }
    if (self_len > 6) {
        return false;
    }
    return enemy_len >= self_len + 3;
}

// No legal step remains. Ramming their head can draw the game or trade up.
inline bool trapped_ram(int self_len, int enemy_len, int allies) {
    if (allies <= 1) {
        return true;
    }
    return enemy_len + 1 >= self_len;
}

// Sprint costs one segment per extra step. Take that trade for two pearls,
// a contested pearl, or an escape. Otherwise walk.
inline int plan_score(int pearls, int steps, int dist_before, int dist_after, bool contested, bool escape) {
    if (steps == 2) {
        bool allow = escape || pearls >= 2 || (pearls == 1 && contested);
        if (!allow) {
            return -1000000000;
        }
    }
    if (steps < 1) {
        return -1000000000;
    }
    int score = pearls * 5000 - (steps - 1) * 3500;
    score += (dist_before - dist_after) * 100;
    return score;
}

inline bool edge_blocked(bool seen, bool kelp) {
    return seen && kelp;
}

inline bool refuse_eat(int exits) {
    return exits < 1;
}

// Length >= 8 needs two exits after the bite. Shorter dragons only refuse a dead end.
inline bool long_body_refuse_eat(int exits, int length) {
    if (length >= 8) {
        return exits < 2;
    }
    return exits < 1;
}

// One-exit corridor with an enemy head within chebyshev 3. Eating is allowed
// only when the landing still has two exits (a one-exit landing never qualifies).
inline bool corridor_refuse(int exits, int enemy_dist, bool eating_pearl) {
    if (exits != 1 || enemy_dist > 3) {
        return false;
    }
    if (eating_pearl && exits >= 2) {
        return false;
    }
    return true;
}

// Both sprint edges must already have been seen and must not be kelp.
inline bool sprint_edges_open(bool seen_first, bool kelp_first, bool seen_second, bool kelp_second) {
    return seen_first && !kelp_first && seen_second && !kelp_second;
}

// Longer dragons must not step next to a shorter enemy head: that enemy will ram.
inline bool avoid_enemy_head(int self_len, int enemy_len, int allies) {
    if (allies <= 1) {
        return true;
    }
    return !worth_trade(self_len, enemy_len, allies, false);
}

// Near-equal long bodies should step apart instead of offering a head-to-head.
inline bool peer_threat(int self_len, int enemy_len) {
    if (self_len < 8) {
        return false;
    }
    int delta = self_len - enemy_len;
    if (delta < 0) {
        delta = -delta;
    }
    return delta <= 2;
}

inline int avoid_radius(int self_len, bool scorer) {
    if (scorer && self_len >= 12) {
        return 2;
    }
    return 1;
}

inline Macro classify_macro(int width, int height) {
    int area = width * height;
    if (area <= 520) {
        return Macro::Snowball;
    }
    if (area <= 1100) {
        return Macro::MidContest;
    }
    return Macro::FarmLength;
}

struct MapPolicy {
    Macro macro;
    int split_min;
    int unit_cap;
    int scorer_min_length;
    int scorer_stop_round;
    int pearl_horizon;
    int sector_prefer;
    bool portal_bfs;
    bool outer_first;
};

inline int split_min_length(int width, int height, int round) {
    Macro macro = classify_macro(width, height);
    if (macro == Macro::Snowball) {
        return 3;
    }
    if (macro == Macro::FarmLength && round >= 200) {
        return 6;
    }
    return 4;
}

inline int unit_cap(int width, int height) {
    Macro macro = classify_macro(width, height);
    if (macro == Macro::Snowball) {
        return 12;
    }
    if (macro == Macro::MidContest) {
        return 16;
    }
    return 28;
}

inline MapPolicy make_policy(int width, int height, int portal_pairs, int round) {
    MapPolicy policy;
    policy.macro = classify_macro(width, height);
    policy.split_min = split_min_length(width, height, round);
    policy.unit_cap = unit_cap(width, height);
    policy.scorer_min_length = 0;
    policy.scorer_stop_round = 100000;
    policy.pearl_horizon = 12;
    policy.sector_prefer = 0;
    policy.portal_bfs = portal_pairs > 0;
    policy.outer_first = false;
    if (policy.macro == Macro::Snowball) {
        policy.pearl_horizon = 8;
    } else if (policy.macro == Macro::MidContest) {
        policy.scorer_min_length = 10;
        policy.scorer_stop_round = 300;
        policy.pearl_horizon = 12;
        policy.sector_prefer = 1;
        policy.outer_first = true;
        policy.portal_bfs = true;
    } else {
        policy.scorer_min_length = 12;
        policy.scorer_stop_round = 250;
        policy.pearl_horizon = 20;
        policy.sector_prefer = 2;
        policy.portal_bfs = portal_pairs > 0;
    }
    return policy;
}

struct AllySnap {
    int id;
    int length;
};

// Longest ally at or above the length gate. Ties go to the lowest id.
// Snowball passes min_length <= 0 and never designates a scorer.
inline int designated_scorer(AllySnap const* allies, int count, int round, int min_length) {
    if (min_length <= 0 || round < 80) {
        return -1;
    }
    int best_id = -1;
    int best_len = -1;
    for (int i = 0; i < count; ++i) {
        int len = allies[i].length;
        int id = allies[i].id;
        if (len < min_length) {
            continue;
        }
        if (best_id < 0 || len > best_len || (len == best_len && id < best_id)) {
            best_id = id;
            best_len = len;
        }
    }
    return best_id;
}

inline bool want_split(int length, int allies, int round, bool pearl_adjacent, bool threatened,
                       bool is_scorer, bool child_room, int width, int height) {
    if (pearl_adjacent || threatened || is_scorer || !child_room) {
        return false;
    }
    MapPolicy policy = make_policy(width, height, 0, round);
    if (allies >= policy.unit_cap) {
        return false;
    }
    int need = policy.split_min;
    if (policy.macro != Macro::Snowball && round < 60 && allies < 6 && need > 3) {
        need = 3;
    }
    if (length < need) {
        return false;
    }
    if (policy.macro == Macro::Snowball && round > 100 && allies >= 8) {
        return false;
    }
    return true;
}

// Snowball: escape or a contested pearl. Mid: also a double pearl.
// Farm: double pearl, or any single pearl once the body is length 5+.
inline bool sprint_allowed(int macro, int pearls, bool contested, bool escape, int length) {
    if (escape) {
        return true;
    }
    if (macro == static_cast<int>(Macro::Snowball)) {
        return pearls >= 1 && contested;
    }
    if (pearls >= 2) {
        return true;
    }
    if (macro == static_cast<int>(Macro::FarmLength)) {
        return pearls >= 1 && length >= 5;
    }
    return pearls == 1 && contested;
}

inline bool claim_live(int claim_round, int round, bool tile_empty) {
    if (claim_round < 0 || tile_empty) {
        return false;
    }
    return round - claim_round <= 40;
}

inline int sector_axis(int size) {
    int count = size / 8;
    if (count < 2) {
        count = 2;
    }
    if (count > 4) {
        count = 4;
    }
    return count;
}

inline int sector_index(int x, int y, int width, int height, int cols, int rows) {
    if (width <= 0 || height <= 0 || cols <= 0 || rows <= 0) {
        return 0;
    }
    int sx = x * cols / width;
    int sy = y * rows / height;
    if (sx < 0) {
        sx = 0;
    }
    if (sy < 0) {
        sy = 0;
    }
    if (sx >= cols) {
        sx = cols - 1;
    }
    if (sy >= rows) {
        sy = rows - 1;
    }
    return sy * cols + sx;
}

inline bool sector_is_outer(int x, int y, int width, int height, int cols, int rows) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    int sx = x * cols / width;
    int sy = y * rows / height;
    if (sx >= cols) {
        sx = cols - 1;
    }
    if (sy >= rows) {
        sy = rows - 1;
    }
    if (sx < 0) {
        sx = 0;
    }
    if (sy < 0) {
        sy = 0;
    }
    return sx == 0 || sy == 0 || sx == cols - 1 || sy == rows - 1;
}

// Take a portal when the long way around is at least three steps longer.
inline bool portal_shortcut(int around_steps, int portal_steps) {
    return portal_steps >= 0 && around_steps >= portal_steps + 3;
}

// Child is born at the tail, facing away from the parent. It needs two free
// neighbours besides the parent segment or it dies on its first turn.
inline bool child_has_room(int open_from_tail) {
    return open_from_tail >= 2;
}

}  // namespace logic
