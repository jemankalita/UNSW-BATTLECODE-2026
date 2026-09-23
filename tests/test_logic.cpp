#include "../leviathan/logic.hpp"

#include <cstdio>
#include <cstdint>

namespace {

int g_failed = 0;

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
    std::uint64_t word = logic::pack_sonar(logic::KIND_PEARL, 14, 9, 3, 42, 260);
    logic::SonarMsg message = logic::unpack_sonar(word);
    expect(message.ok, "sonar checksum accepts");
    expect(message.kind == logic::KIND_PEARL && message.x == 14 && message.y == 9, "sonar position");
    expect(message.aux == 3 && message.id == 42 && message.round_mod == (260 & 255), "sonar payload");
    expect(!logic::unpack_sonar(word ^ 1ull).ok, "tampered sonar rejected");
    expect(logic::sonar_age(260, 250) == 10, "sonar age across mod 256");
    expect(logic::sonar_age(40, 40) == 0, "sonar age same round");
}

void test_combat_and_split() {
    expect(logic::worth_trade(5, 9, 2), "short dragon trades up");
    expect(!logic::worth_trade(10, 14, 2), "queen does not ram");
    expect(logic::worth_trade(3, 8, 2), "short dragon rams a long one");
    expect(!logic::worth_trade(3, 8, 1), "last dragon does not ram");
    expect(logic::trapped_ram(10, 3, 1), "last dragon rams when trapped");
    expect(!logic::trapped_ram(10, 3, 2), "queen does not ram a tiny dragon");
    expect(logic::trapped_ram(10, 9, 3), "trapped dragon rams a peer");

    expect(logic::plan_score(1, 2, 5, 3, false, false) < -100000, "uncontested sprint rejected");
    expect(logic::plan_score(1, 2, 5, 3, true, false) == 1700, "contested sprint score");
    expect(logic::plan_score(2, 2, 4, 2, false, false) == 6700, "double pearl sprint");
    expect(logic::plan_score(1, 1, 3, 2, false, false) == 5100, "single step eat");
    expect(logic::plan_score(0, 2, 5, 1, false, true) == -3100, "escape sprint");

    expect(logic::unit_cap(11, 11) == 6, "small map cap");
    expect(logic::unit_cap(32, 32) == 10, "medium map cap");
    expect(logic::unit_cap(64, 64) == 16, "large map cap");
    expect(logic::want_split(12, 2, 32, 32, 100, false, false), "split when long");
    expect(!logic::want_split(11, 2, 32, 32, 100, false, false), "no split before length 12");
    expect(!logic::want_split(12, 10, 32, 32, 100, false, false), "cap stops splits");
    expect(!logic::want_split(20, 2, 32, 32, 460, false, false), "no late splits");
    expect(!logic::want_split(20, 2, 32, 32, 100, true, false), "eat instead of split");
    expect(!logic::want_split(20, 2, 32, 32, 100, false, true), "no split beside an enemy");
}

}  // namespace

int main() {
    test_directions();
    test_wrap();
    test_portal();
    test_sonar();
    test_combat_and_split();
    if (g_failed != 0) {
        std::printf("%d failed\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
