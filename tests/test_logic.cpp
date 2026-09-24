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

    expect(logic::unit_cap(11, 11) == 10, "small map cap");
    expect(logic::unit_cap(32, 32) == 24, "medium map cap");
    expect(logic::unit_cap(64, 64) == 48, "large map cap");
    expect(logic::split_min_length(11, 11) == 10, "small split length");
    expect(logic::split_min_length(32, 32) == 10, "medium split length");
    expect(logic::split_min_length(64, 64) == 6, "large split length");
    expect(!logic::want_split(9, 2, 32, 32, 100, false, false), "medium map waits until 10");
    expect(logic::want_split(10, 2, 32, 32, 100, false, false), "medium map splits at 10");
    expect(logic::want_split(6, 2, 64, 64, 100, false, false), "large map splits at 6");
    expect(!logic::want_split(9, 2, 16, 16, 100, false, false), "small map waits until 10");
    expect(!logic::want_split(20, 48, 64, 64, 100, false, false), "cap stops splits");
    expect(!logic::want_split(20, 2, 32, 32, 460, false, false), "no late splits");
    expect(!logic::want_split(20, 2, 32, 32, 100, true, false), "eat instead of split");
    expect(!logic::want_split(20, 2, 32, 32, 100, false, true), "no split beside an enemy");

    expect(!logic::edge_blocked(false, false), "unseen open edge is walkable");
    expect(!logic::edge_blocked(false, true), "unseen kelp is assumed open");
    expect(logic::edge_blocked(true, true), "seen kelp is a wall");
    expect(!logic::edge_blocked(true, false), "seen empty is open");
    expect(logic::refuse_eat(0), "never eat into a dead end");
    expect(!logic::refuse_eat(1), "eat when an exit remains");
    expect(logic::avoid_enemy_head(10, 4, 2), "queen stays off a short enemy");
    expect(!logic::avoid_enemy_head(3, 8, 2), "short dragon may close in");
    expect(logic::avoid_enemy_head(10, 4, 1), "last dragon never invites a ram");
    expect(logic::child_has_room(2), "child lives with two exits");
    expect(!logic::child_has_room(1), "child dies in a one-tile pocket");

    int safe_move = logic::move_risk_score(8, 4, 2, 3, 1);
    int risky_move = logic::move_risk_score(8, 10, 1, 1, 0);
    expect(safe_move > risky_move, "safer moves outrank exposed head contacts");
    expect(logic::move_risk_score(12, 9, 2, 2, 0) < 0, "risk score penalizes stepping beside a nearby stronger enemy");
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
