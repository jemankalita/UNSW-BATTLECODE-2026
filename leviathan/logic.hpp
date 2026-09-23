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
inline bool worth_trade(int self_len, int enemy_len, int allies) {
    if (allies <= 1 || self_len > 6) {
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

// Longer dragons must not step next to a shorter enemy head: that enemy will ram.
inline bool avoid_enemy_head(int self_len, int enemy_len, int allies) {
    if (allies <= 1) {
        return true;
    }
    return !worth_trade(self_len, enemy_len, allies);
}

inline int split_min_length(int width, int height) {
    int area = width * height;
    if (area <= 1500) {
        return 12;
    }
    return 8;
}

inline int unit_cap(int width, int height) {
    int area = width * height;
    if (area <= 400) {
        return 4;
    }
    if (area <= 1500) {
        return 8;
    }
    return 20;
}

// Large maps need more farmers. Small maps keep one long dragon.
inline bool want_split(int length, int allies, int width, int height, int round, bool pearl_adjacent, bool threatened) {
    if (round > 450 || pearl_adjacent || threatened || length < split_min_length(width, height)) {
        return false;
    }
    return allies < unit_cap(width, height);
}

// Child is born at the tail, facing away from the parent. It needs two free
// neighbours besides the parent segment or it dies on its first turn.
inline bool child_has_room(int open_from_tail) {
    return open_from_tail >= 2;
}

}  // namespace logic
