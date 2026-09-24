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
    expect(logic::worth_trade(5, 9, 2, false), "short dragon trades up");
    expect(!logic::worth_trade(10, 14, 2, false), "queen does not ram");
    expect(logic::worth_trade(3, 8, 2, false), "short dragon rams a long one");
    expect(!logic::worth_trade(3, 8, 1, false), "last dragon does not ram");
    expect(logic::worth_trade(4, 6, 3, true), "snowball farmer rams a longer peer");
    expect(!logic::worth_trade(6, 9, 3, true), "snowball queen does not ram");
    expect(logic::trapped_ram(10, 3, 1), "last dragon rams when trapped");
    expect(!logic::trapped_ram(10, 3, 2), "queen does not ram a tiny dragon");
    expect(logic::trapped_ram(10, 9, 3), "trapped dragon rams a peer");

    expect(logic::plan_score(1, 2, 5, 3, false, false) < -100000, "uncontested sprint rejected");
    expect(logic::plan_score(0, 2, 5, 1, false, false) < -100000, "fog sprint rejected");
    expect(logic::plan_score(1, 2, 5, 3, true, false) == 1700, "contested sprint score");
    expect(logic::plan_score(2, 2, 4, 2, false, false) == 6700, "double pearl sprint");
    expect(logic::plan_score(1, 1, 3, 2, false, false) == 5100, "single step eat");
    expect(logic::plan_score(0, 2, 5, 1, false, true) == -3100, "escape sprint");

    int snow = static_cast<int>(logic::Macro::Snowball);
    int mid = static_cast<int>(logic::Macro::MidContest);
    int farm = static_cast<int>(logic::Macro::FarmLength);
    expect(!logic::sprint_allowed(snow, 2, false, false, 6), "snowball skips a quiet double pearl");
    expect(logic::sprint_allowed(snow, 1, true, false, 4), "snowball takes a contested pearl");
    expect(logic::sprint_allowed(mid, 2, false, false, 4), "mid map takes two pearls");
    expect(!logic::sprint_allowed(mid, 1, false, false, 8), "mid map skips an uncontested sprint");
    expect(!logic::sprint_allowed(farm, 1, true, false, 4), "short farm dragon skips a single pearl sprint");
    expect(logic::sprint_allowed(farm, 1, false, false, 5), "farm dragon sprints a long gap");
    expect(logic::sprint_allowed(farm, 0, false, true, 3), "escape sprint stays legal");

    expect(logic::unit_cap(11, 11) == 12, "snowball cap");
    expect(logic::unit_cap(32, 32) == 16, "mid cap");
    expect(logic::unit_cap(64, 64) == 28, "farm cap");
    expect(logic::split_min_length(16, 16, 100) == 3, "snowball split length");
    expect(logic::split_min_length(32, 32, 100) == 4, "mid split length");
    expect(logic::split_min_length(64, 64, 100) == 4, "farm early split length");
    expect(logic::split_min_length(64, 64, 220) == 6, "farm late split length");
    expect(logic::want_split(4, 2, 100, false, false, false, true, 32, 32), "mid map splits at 4");
    expect(!logic::want_split(3, 2, 100, false, false, false, true, 32, 32), "mid map waits until 4");
    expect(logic::want_split(3, 2, 40, false, false, false, true, 32, 32), "early game splits at 3");
    expect(!logic::want_split(3, 6, 40, false, false, false, true, 32, 32), "early split stops once six dragons exist");
    expect(logic::want_split(3, 2, 20, false, false, false, true, 16, 16), "snowball splits at 3");
    expect(!logic::want_split(2, 2, 20, false, false, false, true, 16, 16), "length 2 never splits");
    expect(!logic::want_split(8, 8, 120, false, false, false, true, 32, 16), "snowball stops oversplitting");
    expect(!logic::want_split(10, 28, 100, false, false, false, true, 64, 64), "farm cap stops splits");
    expect(logic::want_split(8, 20, 100, false, false, false, true, 64, 64), "farm map still splits under the cap");
    expect(!logic::want_split(20, 4, 100, false, false, true, true, 64, 64), "scorer never splits");
    expect(!logic::want_split(8, 4, 100, true, false, false, true, 32, 32), "eat instead of split");
    expect(!logic::want_split(8, 4, 100, false, true, false, true, 32, 32), "no split beside an enemy");
    expect(!logic::want_split(8, 4, 100, false, false, false, false, 32, 32), "no split without room for the child");

    expect(!logic::edge_blocked(false, false), "unseen open edge is walkable");
    expect(!logic::edge_blocked(false, true), "unseen kelp is assumed open");
    expect(logic::edge_blocked(true, true), "seen kelp is a wall");
    expect(!logic::edge_blocked(true, false), "seen empty is open");
    expect(logic::refuse_eat(0), "never eat into a dead end");
    expect(!logic::refuse_eat(1), "eat when an exit remains");
    expect(logic::long_body_refuse_eat(1, 10), "long body refuses a one-exit bite");
    expect(!logic::long_body_refuse_eat(2, 10), "long body eats with two exits");
    expect(!logic::long_body_refuse_eat(1, 4), "short body may eat with one exit");
    expect(logic::corridor_refuse(1, 3, false), "corridor with a nearby head is refused");
    expect(!logic::corridor_refuse(1, 4, false), "far head does not close the corridor");
    expect(logic::corridor_refuse(1, 2, true), "eating a one-exit pearl is still refused");
    expect(!logic::corridor_refuse(2, 1, true), "two exits stay open");
    expect(!logic::sprint_edges_open(false, false, true, false), "unseen sprint edge is illegal");
    expect(!logic::sprint_edges_open(true, true, true, false), "kelp sprint edge is illegal");
    expect(logic::sprint_edges_open(true, false, true, false), "seen open sprint edges are legal");
    expect(logic::avoid_enemy_head(10, 4, 2), "queen stays off a short enemy");
    expect(!logic::avoid_enemy_head(3, 8, 2), "short dragon may close in");
    expect(logic::avoid_enemy_head(10, 4, 1), "last dragon never invites a ram");
    expect(logic::peer_threat(10, 11), "near-equal long bodies are a peer threat");
    expect(!logic::peer_threat(10, 14), "a much longer enemy is not a peer");
    expect(logic::avoid_radius(14, true) == 2, "scorer keeps a wider berth");
    expect(logic::avoid_radius(14, false) == 1, "farmer avoid radius stays adjacent");
    expect(logic::child_has_room(2), "child lives with two exits");
    expect(!logic::child_has_room(1), "child dies in a one-tile pocket");
}

void test_policy_and_roles() {
    expect(logic::classify_macro(16, 16) == logic::Macro::Snowball, "default small is snowball");
    expect(logic::classify_macro(32, 16) == logic::Macro::Snowball, "devil is snowball");
    expect(logic::classify_macro(11, 11) == logic::Macro::Snowball, "arena is snowball");
    expect(logic::classify_macro(32, 32) == logic::Macro::MidContest, "default is mid");
    expect(logic::classify_macro(25, 25) == logic::Macro::MidContest, "trophy is mid");
    expect(logic::classify_macro(25, 35) == logic::Macro::MidContest, "queen of spades is mid");
    expect(logic::classify_macro(48, 24) == logic::Macro::FarmLength, "trauma is farm");
    expect(logic::classify_macro(60, 40) == logic::Macro::FarmLength, "schooltime is farm");
    expect(logic::classify_macro(64, 64) == logic::Macro::FarmLength, "big empty is farm");

    logic::MapPolicy farm = logic::make_policy(64, 64, 0, 100);
    expect(farm.scorer_min_length == 12, "farm scorer length");
    expect(farm.pearl_horizon == 20, "farm pearl horizon");
    expect(!farm.portal_bfs, "big empty skips portal search");
    expect(farm.sector_prefer == 2, "farm sectors are on");
    logic::MapPolicy school = logic::make_policy(60, 40, 12, 100);
    expect(school.portal_bfs, "schooltime uses portals");
    logic::MapPolicy mid = logic::make_policy(32, 32, 0, 100);
    expect(mid.outer_first, "default farms the outside first");
    expect(mid.scorer_stop_round == 300, "mid scorer stops splitting at 300");
    logic::MapPolicy snow = logic::make_policy(16, 16, 0, 50);
    expect(snow.scorer_min_length == 0, "snowball has no scorer");

    logic::AllySnap allies[3];
    allies[0].id = 4;
    allies[0].length = 4;
    allies[1].id = 9;
    allies[1].length = 4;
    allies[2].id = 2;
    allies[2].length = 18;
    expect(logic::designated_scorer(allies, 3, 100, 12) == 2, "longest ally is the scorer");
    allies[2].length = 12;
    allies[2].id = 7;
    allies[0].id = 3;
    allies[0].length = 12;
    expect(logic::designated_scorer(allies, 3, 100, 12) == 3, "tie goes to the lowest id");
    expect(logic::designated_scorer(allies, 3, 40, 12) == -1, "no scorer before round 80");
    expect(logic::designated_scorer(allies, 3, 100, 0) == -1, "snowball never designates a scorer");

    std::uint64_t claim = logic::pack_sonar(logic::KIND_CLAIM, 4, 5, 9, 12, 10);
    logic::SonarMsg claim_msg = logic::unpack_sonar(claim);
    expect(claim_msg.ok && claim_msg.kind == logic::KIND_CLAIM && claim_msg.x == 4 && claim_msg.y == 5, "claim kind round trips");
    std::uint64_t sector = logic::pack_sonar(logic::KIND_SECTOR, 1, 2, 3, 8, 10);
    logic::SonarMsg sector_msg = logic::unpack_sonar(sector);
    expect(sector_msg.ok && sector_msg.kind == logic::KIND_SECTOR, "sector kind round trips");
    std::uint64_t scorer = logic::pack_sonar(logic::KIND_SCORER, 6, 7, 20, 2, 90);
    logic::SonarMsg scorer_msg = logic::unpack_sonar(scorer);
    expect(scorer_msg.ok && scorer_msg.kind == logic::KIND_SCORER && scorer_msg.aux == 20 && scorer_msg.id == 2, "scorer kind round trips");

    expect(logic::claim_live(10, 50, false), "claim is live inside 40 rounds");
    expect(!logic::claim_live(10, 51, false), "claim expires after 40 rounds");
    expect(!logic::claim_live(10, 20, true), "empty tile drops the claim");
    expect(logic::sector_axis(64) == 4, "large map clamps to 4 sectors");
    expect(logic::sector_axis(11) == 2, "small map uses 2 sectors");
    expect(logic::sector_is_outer(0, 10, 32, 32, 4, 4), "corner sector is outer");
    expect(!logic::sector_is_outer(16, 16, 32, 32, 4, 4), "center sector is inner");
    expect(logic::portal_shortcut(6, 1), "portal that saves three steps is worth it");
    expect(!logic::portal_shortcut(3, 1), "portal that saves only two steps is not");
}

}  // namespace

int main() {
    test_directions();
    test_wrap();
    test_portal();
    test_sonar();
    test_combat_and_split();
    test_policy_and_roles();
    if (g_failed != 0) {
        std::printf("%d failed\n", g_failed);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
