#include "../leviathan/logic.hpp"
#include "../leviathan/mapdb.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

int g_failed = 0;

// Edge rows alternate north sides (row 2y) and west sides (row 2y+1); the
// board wraps, so south and east sides are the next tile's north and west.
int baked_edge(int x, int y, int dir, int w, int h) {
    if (dir == logic::DIR_S) {
        return baked_edge(x, logic::wrap_coord(y + 1, h), logic::DIR_N, w, h);
    }
    if (dir == logic::DIR_E) {
        return baked_edge(logic::wrap_coord(x + 1, w), y, logic::DIR_W, w, h);
    }
    return (dir == logic::DIR_N ? 2 * y : 2 * y + 1) * (w + 1) + x;
}

void expect(bool condition, char const* name) {
    if (condition) {
        return;
    }
    std::printf("FAIL %s\n", name);
    ++g_failed;
}

void test_directions() {
    expect(logic::opposite_dir(logic::DIR_N) == logic::DIR_S, "opposite north");
    expect(logic::opposite_dir(logic::DIR_E) == logic::DIR_W, "opposite east");
    expect(logic::opposite_dir(logic::opposite_dir(logic::DIR_S)) == logic::DIR_S, "opposite twice");
    expect(logic::right_of(logic::DIR_N) == logic::DIR_E, "right of north");
    expect(logic::left_of(logic::DIR_N) == logic::DIR_W, "left of north");
    expect(logic::right_of(logic::right_of(logic::DIR_E)) == logic::DIR_W, "two rights");
}

void test_wrap() {
    expect(logic::wrap_coord(-1, 11) == 10, "wrap negative");
    expect(logic::wrap_coord(11, 11) == 0, "wrap width");
    expect(logic::chebyshev(0, 0, 10, 0, 11, 11) == 1, "torus chebyshev");
    expect(logic::chebyshev(1, 1, 4, 5, 32, 32) == 4, "plain chebyshev");
    logic::Point stepped = logic::step_wrap(0, 0, logic::DIR_W, 16, 16);
    expect(stepped.ok && stepped.x == 15 && stepped.y == 0, "step west wraps");
}

void test_portal() {
    logic::PortalSide sides[4];
    sides[0].x = 1;
    sides[0].y = 1;
    sides[0].dir = logic::DIR_E;
    sides[1].x = 2;
    sides[1].y = 1;
    sides[1].dir = logic::DIR_W;
    sides[2].x = 5;
    sides[2].y = 3;
    sides[2].dir = logic::DIR_E;
    sides[3].x = 6;
    sides[3].y = 3;
    sides[3].dir = logic::DIR_W;

    logic::Point east = logic::emerge(1, 1, logic::DIR_E, 16, 16, sides, 4);
    expect(east.ok && east.x == 6 && east.y == 3, "portal east emerges on the east side");

    logic::Point west = logic::emerge(2, 1, logic::DIR_W, 16, 16, sides, 4);
    expect(west.ok && west.x == 5 && west.y == 3, "portal west emerges on the west side");

    logic::PortalSide horizontal[2];
    horizontal[0].x = 4;
    horizontal[0].y = 4;
    horizontal[0].dir = logic::DIR_N;
    horizontal[1].x = 8;
    horizontal[1].y = 8;
    horizontal[1].dir = logic::DIR_N;
    logic::Point north = logic::emerge(4, 4, logic::DIR_N, 20, 20, horizontal, 2);
    expect(north.ok && north.x == 8 && north.y == 7, "horizontal portal keeps direction");
}

void test_sonar() {
    std::uint64_t word = logic::pack_sonar(logic::KIND_KING, 14, 9, 3, 42, 260);
    logic::SonarMsg message = logic::unpack_sonar(word);
    expect(message.ok, "sonar checksum accepts");
    expect(message.kind == logic::KIND_KING && message.x == 14 && message.y == 9, "sonar position");
    expect(message.aux == 3 && message.id == 42 && message.round_mod == (260 & 255), "sonar payload");
    expect(!logic::unpack_sonar(word ^ 1ull).ok, "tampered sonar rejected");
    expect(logic::sonar_age(260, 250) == 10, "sonar age across mod 256");
    expect(logic::sonar_age(40, 40) == 0, "sonar age same round");
}

void test_body_timing() {
    // A body of 5 that does not eat: the tail (index 4) is still a wall on
    // the next move and gone on the one after; the neck frees last.
    expect(logic::unlock_time(5, 4) == 2, "tail frees on the second move");
    expect(logic::unlock_time(5, 1) == 5, "neck frees last");
    expect(logic::unlock_time(5, 0) >= logic::NEVER, "head tile never frees");
    expect(logic::room_needed(3) == logic::MIN_ROOM, "tiny dragon still wants open water");
    expect(logic::room_needed(30) == 31, "long dragon needs its length plus one");
}

void test_combat() {
    expect(logic::worth_ram(3, 8, 2, false), "short dragon trades up");
    expect(!logic::worth_ram(3, 5, 2, false), "no ram for a small margin");
    expect(!logic::worth_ram(3, 20, 1, false), "last dragon never rams by choice");
    expect(logic::worth_ram(10, 2, 1, true), "cornered dragon takes a head with it");

    expect(logic::ram_threat(3, 9, 2, 1) == 0, "a ram by a much longer enemy is welcome");
    int near = logic::ram_threat(10, 4, 3, 1);
    int far = logic::ram_threat(10, 4, 3, 2);
    expect(near > far && far > 0, "a sprint ram is less likely than a step ram");
    expect(logic::ram_threat(10, 4, 1, 1) > near, "the last dragon guards its head hardest");
    expect(logic::ram_threat(10, 12, 3, 1) < near, "a longer enemy is less keen to ram");
}

void test_pearl_chance() {
    expect(logic::pearl_chance(false, false, 0, -1, 10, 4) == 4, "fog uses the prior");
    expect(logic::pearl_chance(true, true, 0, -1, 10, 4) == 100, "a pearl in view is certain");
    expect(logic::pearl_chance(true, true, 40, -1, 50, 4) < 90, "remembered pearls go stale");
    expect(logic::pearl_chance(true, false, 0, -1, 10, 4) == 0, "barren tile never pays");
    expect(logic::pearl_chance(true, false, 0, 20, 10, 4) == 0, "not spawned by arrival");
    expect(logic::pearl_chance(true, false, 0, 20, 25, 4) > 50, "spawned before arrival");
}

void test_team() {
    expect(logic::team_cap(11, 11, 64, 12) == 10, "small map keeps a small team");
    expect(logic::team_cap(64, 64, 64, 12) == 64, "big map fills the unit limit");
    expect(logic::team_cap(10, 10, 64, 1000) == 4, "cap never drops below a handful");
    expect(logic::want_split(6, 3, 10, 100, 6, 380), "long enough splits");
    expect(!logic::want_split(5, 3, 10, 100, 6, 380), "too short to split");
    expect(!logic::want_split(20, 10, 10, 100, 6, 380), "team full");
    expect(!logic::want_split(20, 3, 10, 400, 6, 380), "no late splits");
}

void test_feeding() {
    expect(logic::better_king(9, 5, 8, 1), "longer dragon is king");
    expect(logic::better_king(9, 2, 9, 5), "ties go to the lower id");
    expect(!logic::better_king(9, 5, 9, 2), "the higher id loses a tie");
    expect(!logic::feeding_time(300, 420, 10, 3), "no feeding before the late game");
    expect(logic::feeding_time(430, 420, 10, 3), "late game feeds");
    expect(!logic::feeding_time(430, 420, 3, 3), "a few dragons stay back");
    expect(logic::can_feed_in_time(20, 6, 460, 500), "close enough to feed");
    expect(!logic::can_feed_in_time(40, 6, 460, 500), "too far to matter");
}

void test_symmetry() {
    logic::Point p = logic::mirror_tile(logic::SYM_FLIP_X, 2, 5, 10, 8);
    expect(p.x == 7 && p.y == 5, "flip x");
    p = logic::mirror_tile(logic::SYM_FLIP_Y, 2, 5, 10, 8);
    expect(p.x == 2 && p.y == 2, "flip y");
    p = logic::mirror_tile(logic::SYM_ROTATE, 2, 5, 10, 8);
    expect(p.x == 7 && p.y == 2, "rotate");
    expect(logic::mirror_dir(logic::SYM_FLIP_X, logic::DIR_E) == logic::DIR_W, "flip x swaps east and west");
    expect(logic::mirror_dir(logic::SYM_FLIP_X, logic::DIR_N) == logic::DIR_N, "flip x keeps north");
    expect(logic::mirror_dir(logic::SYM_FLIP_Y, logic::DIR_N) == logic::DIR_S, "flip y swaps north and south");
    expect(logic::mirror_dir(logic::SYM_ROTATE, logic::DIR_W) == logic::DIR_E, "rotate reverses");
    expect(logic::manhattan(0, 0, 9, 7, 10, 8) == 2, "torus manhattan wraps both ways");
}

void test_gap_chance() {
    expect(logic::gap_chance(0, 0, 400, 80) == 0, "never-spawning tile");
    expect(logic::gap_chance(20, 50, 10, 80) == 0, "too soon after the draw");
    expect(logic::gap_chance(1, 50, 50, 80) == 80, "fast tile full after its widest gap");
    expect(logic::gap_chance(1, 50, 25, 80) == 40, "half way through the gap");
    expect(logic::gap_chance(1, 1000, 2, 80) == 1, "slow tile never rounds to zero");
    expect(logic::steady_chance(0, 0, 5) == 0, "dead tile stays bare");
    expect(logic::steady_chance(1, 50, 5) == 43, "fast tile mostly full");
    expect(logic::steady_chance(1, 1000, 5) == 3, "slow tile mostly bare");
}

// Every baked map is internally consistent, and its kelp is symmetric the
// way the bot will assume: this pins the map file's axis naming.
void test_mapdb() {
    for (int i = 0; i < mapdb::MAP_COUNT; ++i) {
        mapdb::Known const& m = mapdb::MAPS[i];
        int w = m.width;
        int h = m.height;
        expect(static_cast<int>(std::strlen(m.tiles)) == w * h, m.name);
        expect(static_cast<int>(std::strlen(m.edges)) == (2 * h + 1) * (w + 1), m.name);
        for (int p = 0; p < 2 * m.portal_count; ++p) {
            expect(m.edges[m.portals[p]] == 'P', "portal ends are portal edges");
        }
        for (int t = 0; t < w * h; ++t) {
            expect(m.tiles[t] - 'a' < m.gap_count, "tile gap in range");
        }
        if (m.symmetry < 0) {
            continue;
        }
        bool mirrored = true;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                logic::Point q = logic::mirror_tile(m.symmetry, x, y, w, h);
                for (int dir = 0; dir < 4; ++dir) {
                    int mdir = logic::mirror_dir(m.symmetry, dir);
                    char a = m.edges[baked_edge(x, y, dir, w, h)];
                    char b = m.edges[baked_edge(q.x, q.y, mdir, w, h)];
                    mirrored = mirrored && a == b;
                }
                mirrored = mirrored && m.tiles[y * w + x] == m.tiles[q.y * w + q.x];
            }
        }
        expect(mirrored, m.name);
    }
}

}  // namespace

int main() {
    test_directions();
    test_wrap();
    test_portal();
    test_sonar();
    test_body_timing();
    test_combat();
    test_pearl_chance();
    test_team();
    test_feeding();
    test_symmetry();
    test_gap_chance();
    test_mapdb();
    if (g_failed != 0) {
        std::printf("%d failed\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
