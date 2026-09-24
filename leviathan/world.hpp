#pragma once

#include "helper.hpp"
#include "logic.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

// Everything one dragon remembers about the map. Each dragon runs its own
// process, so this is per dragon, and a split child starts empty.
namespace world {

static constexpr int EDGE_UNKNOWN = -2;
static constexpr int EDGE_OPEN = -1;
static constexpr int EDGE_KELP = -3;
// Stand-in portal ids for mirrored pairs start here (real ids are small).
static constexpr int MIRROR_PORTAL = 1 << 20;

struct Cell {
    int edge[4] = {EDGE_UNKNOWN, EDGE_UNKNOWN, EDGE_UNKNOWN, EDGE_UNKNOWN};
    // Seen with our own eyes, as opposed to inferred from the map's symmetry.
    bool edge_observed[4] = {false, false, false, false};
    bool seen = false;
    // Spawn timing copied from the mirror tile; spawn_round is then valid
    // even for a tile we have never seen.
    bool timed = false;
    int timed_round = -1;
    bool pearl = false;
    int seen_round = -1;
    // Round a pearl next tries to spawn; -1 if the tile never spawns.
    int spawn_round = -1;
    // Largest countdown ever read here: a cheap estimate of the tile's max gap.
    int max_countdown = 0;
    // Last round a dragon segment stood here, as far as we saw.
    int occupied_round = -1000;
};

// Where a step lands, and whether it went through a portal.
struct Landing {
    int x = 0;
    int y = 0;
    bool ok = false;
    bool portal = false;
    // A portal whose far end we have never seen: the step is legal and lands
    // somewhere real, we just cannot say where until we cross it.
    bool unknown_exit = false;
};

class Map {
  public:
    Map(int width, int height) : width_(width), height_(height), cells_(static_cast<std::size_t>(width * height)) {}

    int width() const { return width_; }
    int height() const { return height_; }
    int size() const { return width_ * height_; }
    int index(int x, int y) const { return y * width_ + x; }
    Cell& at(int x, int y) { return cells_[static_cast<std::size_t>(index(x, y))]; }
    Cell const& at(int x, int y) const { return cells_[static_cast<std::size_t>(index(x, y))]; }
    Cell const& cell(int idx) const { return cells_[static_cast<std::size_t>(idx)]; }

    void observe(unswbc::Controller const& ct, int round) {
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            observe_tile(tile, round);
        }
        if (symmetry_ >= 0 || broken_) {
            mirror_view(ct);
            return;
        }
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            test_symmetry(tile.get_position().x, tile.get_position().y, round);
        }
        settle_symmetry();
    }

    // -1 until only one symmetry fits what we have seen.
    int symmetry() const { return symmetry_; }

    // One step from (x, y). A kelp edge fails. An edge we have never seen is
    // open only when 'optimistic' (planning through fog); the head's own
    // edges are always in vision, so real moves never rely on that.
    Landing step(int x, int y, int dir, bool optimistic) const {
        Landing out;
        int edge = at(x, y).edge[dir];
        if (edge == EDGE_KELP || (edge == EDGE_UNKNOWN && !optimistic)) {
            return out;
        }
        if (edge >= 0) {
            return through_portal(x, y, dir, edge);
        }
        logic::Point next = logic::step_wrap(x, y, dir, width_, height_);
        out.x = next.x;
        out.y = next.y;
        out.ok = true;
        return out;
    }

    // After crossing a portal we see where the head really came out; keep
    // that, so a wrong guess about a portal is made at most once.
    void learn_exit(int x, int y, int dir, int ex, int ey) {
        exits_[exit_key(x, y, dir)] = index(ex, ey);
    }

  private:
    int width_;
    int height_;
    std::vector<Cell> cells_;
    bool possible_[logic::SYM_KINDS] = {true, true, true};
    int symmetry_ = -1;
    bool broken_ = false;  // nothing fits: stop inferring rather than guess
    std::unordered_map<int, std::vector<logic::PortalSide>> portals_;
    std::unordered_map<int, int> exits_;

    int exit_key(int x, int y, int dir) const { return index(x, y) * 4 + dir; }

    Landing through_portal(int x, int y, int dir, int portal_id) const {
        Landing out;
        auto learned = exits_.find(exit_key(x, y, dir));
        if (learned != exits_.end()) {
            out.x = learned->second % width_;
            out.y = learned->second / width_;
            out.ok = true;
            out.portal = true;
            return out;
        }
        auto found = portals_.find(portal_id);
        if (found == portals_.end()) {
            out.unknown_exit = true;
            return out;
        }
        std::vector<logic::PortalSide> const& sides = found->second;
        logic::Point p = logic::emerge(x, y, dir, width_, height_, sides.data(), static_cast<int>(sides.size()));
        out.x = p.x;
        out.y = p.y;
        out.ok = p.ok;
        out.portal = true;
        out.unknown_exit = !p.ok;
        return out;
    }

    void remember_portal(int id, int x, int y, int dir) {
        std::vector<logic::PortalSide>& sides = portals_[id];
        for (logic::PortalSide const& side : sides) {
            if (side.x == x && side.y == y && side.dir == dir) {
                return;
            }
        }
        if (sides.size() < 8) {
            sides.push_back(logic::PortalSide{x, y, dir});
        }
    }

    static unswbc::Direction dir_of(int dir) {
        static unswbc::Direction::Value const values[] = {
            unswbc::Direction::NORTH, unswbc::Direction::EAST, unswbc::Direction::SOUTH, unswbc::Direction::WEST};
        return unswbc::Direction(values[dir & 3]);
    }

    void observe_tile(unswbc::Tile const& tile, int round) {
        unswbc::Position pos = tile.get_position();
        if (pos.x < 0 || pos.y < 0 || pos.x >= width_ || pos.y >= height_) {
            return;
        }
        Cell& c = at(pos.x, pos.y);
        c.seen = true;
        c.seen_round = round;
        c.pearl = tile.has_pearl();
        int countdown = tile.get_pearl_time();
        c.spawn_round = countdown < 0 ? -1 : round + countdown;
        if (countdown > c.max_countdown) {
            c.max_countdown = countdown;
        }
        if (tile.get_dragon() != nullptr) {
            c.occupied_round = round;
        }
        for (int dir = 0; dir < 4; ++dir) {
            unswbc::Edge const& edge = tile.get_edge(dir_of(dir));
            int value = EDGE_OPEN;
            if (edge.get_edge_type() == unswbc::EdgeType::KELP) {
                value = EDGE_KELP;
            } else if (edge.is_portal()) {
                value = edge.get_portal_id();
            }
            c.edge[dir] = value;
            c.edge_observed[dir] = true;
            logic::Point next = logic::step_wrap(pos.x, pos.y, dir, width_, height_);
            at(next.x, next.y).edge[logic::opposite_dir(dir)] = value;
            at(next.x, next.y).edge_observed[logic::opposite_dir(dir)] = true;
            if (value >= 0) {
                remember_portal(value, pos.x, pos.y, dir);
                remember_portal(value, next.x, next.y, logic::opposite_dir(dir));
            }
        }
    }

    // ---- symmetry -----------------------------------------------------------
    // Mirror tiles share their kelp and their pearl countdown, so a single
    // disagreement rules a symmetry out, and once one symmetry is left every
    // tile we see also tells us about a tile we have not.

    static bool same_kind(int a, int b) {
        return (a >= 0 && b >= 0) || a == b;
    }

    void test_symmetry(int x, int y, int round) {
        Cell const& a = at(x, y);
        for (int kind = 0; kind < logic::SYM_KINDS; ++kind) {
            if (!possible_[kind]) {
                continue;
            }
            logic::Point m = logic::mirror_tile(kind, x, y, width_, height_);
            Cell const& b = at(m.x, m.y);
            for (int dir = 0; dir < 4 && possible_[kind]; ++dir) {
                int mdir = logic::mirror_dir(kind, dir);
                if (a.edge_observed[dir] && b.edge_observed[mdir] && !same_kind(a.edge[dir], b.edge[mdir])) {
                    possible_[kind] = false;
                }
            }
            if (possible_[kind] && a.seen_round == round && b.seen_round == round && a.spawn_round != b.spawn_round) {
                possible_[kind] = false;
            }
        }
    }

    void settle_symmetry() {
        int left = 0;
        int last = -1;
        for (int kind = 0; kind < logic::SYM_KINDS; ++kind) {
            if (possible_[kind]) {
                ++left;
                last = kind;
            }
        }
        if (left == 0) {
            broken_ = true;
            return;
        }
        if (left > 1) {
            return;
        }
        symmetry_ = last;
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                mirror_cell(x, y);
            }
        }
    }

    void mirror_view(unswbc::Controller const& ct) {
        if (symmetry_ < 0) {
            return;
        }
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            mirror_cell(tile.get_position().x, tile.get_position().y);
        }
    }

    // Copy what we observed at (x, y) onto its mirror, where we have not
    // looked ourselves. The mirror of a portal pair is another pair with an
    // id we cannot know, so it gets a stand-in id of its own; once both ends
    // of the real pair are known, both ends of the mirrored pair are too.
    void mirror_cell(int x, int y) {
        Cell const& a = at(x, y);
        logic::Point m = logic::mirror_tile(symmetry_, x, y, width_, height_);
        Cell& b = at(m.x, m.y);
        for (int dir = 0; dir < 4; ++dir) {
            int mdir = logic::mirror_dir(symmetry_, dir);
            if (!a.edge_observed[dir] || b.edge_observed[mdir]) {
                continue;
            }
            int value = a.edge[dir] >= 0 ? a.edge[dir] + MIRROR_PORTAL : a.edge[dir];
            b.edge[mdir] = value;
            logic::Point next = logic::step_wrap(m.x, m.y, mdir, width_, height_);
            Cell& beyond = at(next.x, next.y);
            if (!beyond.edge_observed[logic::opposite_dir(mdir)]) {
                beyond.edge[logic::opposite_dir(mdir)] = value;
            }
            if (value >= 0) {
                remember_portal(value, m.x, m.y, mdir);
                remember_portal(value, next.x, next.y, logic::opposite_dir(mdir));
            }
        }
        int fresh = b.seen_round > b.timed_round ? b.seen_round : b.timed_round;
        if (a.seen && a.seen_round > fresh) {
            b.timed = true;
            b.timed_round = a.seen_round;
            b.spawn_round = a.spawn_round;
            if (a.max_countdown > b.max_countdown) {
                b.max_countdown = a.max_countdown;
            }
        }
    }
};

}  // namespace world
