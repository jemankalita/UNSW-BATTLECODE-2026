#pragma once

#include "helper.hpp"
#include "logic.hpp"
#include "mapdb.hpp"

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
// Stand-in id for a portal only the baked map told us about.
static constexpr int KNOWN_PORTAL = 1 << 21;
// Slack on a countdown read against the baked map's max gap.
static constexpr int COUNTDOWN_SLACK = 1;

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
    Map(int width, int height) : width_(width), height_(height), cells_(static_cast<std::size_t>(width * height)) {
        for (int i = 0; i < mapdb::MAP_COUNT; ++i) {
            if (mapdb::MAPS[i].width == width && mapdb::MAPS[i].height == height) {
                candidates_.push_back(i);
            }
        }
    }

    // The baked ranked map we are on, once what we have seen rules out
    // every other one of this size; null on any other map.
    mapdb::Known const* known() const { return known_; }

    // The pearl gap of a tile on the baked map. False when the map is unknown.
    bool gap(int cell, int& lo, int& hi) const {
        if (known_ == nullptr) {
            return false;
        }
        mapdb::Gap const& g = known_->gaps[known_->tiles[cell] - 'a'];
        lo = g.lo;
        hi = g.hi;
        return true;
    }

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
        settle_known();
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
        if (edge == EDGE_UNKNOWN && known_ != nullptr) {
            edge = known_edge(x, y, dir);
        }
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
        mark_mouth(index(x, y), index(ex, ey));
    }

    // Portals are one-way for our team, from the lower tile index to the
    // higher, and only on even rounds. Two allies queued at opposite mouths
    // would otherwise cross into each other's heads, which neither can see:
    // on the ladder that killed more of our dragons than the enemy did.
    // A dragon that arrives on an even round leaves on the odd one, so the
    // exit mouth is clear by the next crossing.
    static bool crossing_allowed(int from, int landing, int round) {
        return from < landing && round % 2 == 0;
    }

    // A tile our own dragons land on when they cross a portal.
    bool exit_mouth(int cell) const {
        return !mouths_.empty() && mouths_[static_cast<std::size_t>(cell)] != 0;
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
    std::vector<int> candidates_;       // baked maps still consistent with our view
    mapdb::Known const* known_ = nullptr;
    std::vector<int> known_exits_;      // exit_key -> landing tile, from the baked map
    std::vector<char> mouths_;          // tiles our crossings land on

    void mark_mouth(int from, int landing) {
        if (from >= landing) {
            return;
        }
        if (mouths_.empty()) {
            mouths_.assign(static_cast<std::size_t>(size()), 0);
        }
        mouths_[static_cast<std::size_t>(landing)] = 1;
    }

    int exit_key(int x, int y, int dir) const { return index(x, y) * 4 + dir; }

    // ---- the baked map --------------------------------------------------------
    // Edge rows alternate: row 2y holds the north sides of tile row y, row
    // 2y+1 the west sides, each width + 1 long. The board wraps, so the
    // bottom row's south sides are the top row's north sides (the file's
    // last row and column are unused padding), and likewise east and west.

    int edge_index(int x, int y, int dir) const {
        if (dir == logic::DIR_S) {
            return edge_index(x, logic::wrap_coord(y + 1, height_), logic::DIR_N);
        }
        if (dir == logic::DIR_E) {
            return edge_index(logic::wrap_coord(x + 1, width_), y, logic::DIR_W);
        }
        int row = dir == logic::DIR_N ? 2 * y : 2 * y + 1;
        return row * (width_ + 1) + x;
    }

    static int edge_value(char kind) {
        return kind == '#' ? EDGE_KELP : kind == 'P' ? KNOWN_PORTAL : EDGE_OPEN;
    }

    int known_edge(int x, int y, int dir) const {
        return edge_value(known_->edges[edge_index(x, y, dir)]);
    }

    static bool edge_agrees(int seen, int baked) {
        return (seen >= 0 && baked >= 0) || seen == baked;
    }

    // Rule out every baked map that disagrees with a tile we can see.
    void check_candidates(unswbc::Tile const& tile, int x, int y) {
        if (candidates_.empty()) {
            return;
        }
        int countdown = tile.get_pearl_time();
        std::vector<int> keep;
        for (int id : candidates_) {
            mapdb::Known const& m = mapdb::MAPS[id];
            mapdb::Gap const& g = m.gaps[m.tiles[index(x, y)] - 'a'];
            bool ok = (countdown < 0) == (g.hi <= 0) && countdown <= g.hi + COUNTDOWN_SLACK;
            for (int dir = 0; dir < 4 && ok; ++dir) {
                ok = edge_agrees(at(x, y).edge[dir], edge_value(m.edges[edge_index(x, y, dir)]));
            }
            if (ok) {
                keep.push_back(id);
            }
        }
        candidates_.swap(keep);
        if (candidates_.empty()) {
            known_ = nullptr;
            known_exits_.clear();
        }
    }

    // Once a single baked map is left, learn where each of its portals leads
    // and take its symmetry.
    void settle_known() {
        if (known_ != nullptr || candidates_.size() != 1) {
            return;
        }
        known_ = &mapdb::MAPS[candidates_.front()];
        known_exits_.assign(static_cast<std::size_t>(size() * 4), -1);
        for (int p = 0; p < known_->portal_count; ++p) {
            int a = known_->portals[2 * p];
            int b = known_->portals[2 * p + 1];
            link_portal(a, b);
            link_portal(b, a);
        }
        if (symmetry_ < 0 && !broken_ && known_->symmetry >= 0 && possible_[known_->symmetry]) {
            for (int kind = 0; kind < logic::SYM_KINDS; ++kind) {
                possible_[kind] = kind == known_->symmetry;
            }
            settle_symmetry();
        }
    }

    // Record, for both tiles beside edge 'from', where crossing it lands:
    // the tile on the same side of edge 'to' that the step points at.
    void link_portal(int from, int to) {
        int stride = width_ + 1;
        int fr = from / stride;
        int fc = from % stride;
        int tr = to / stride;
        int tc = to % stride;
        if (fr % 2 == 0) {
            int y = fr / 2;
            int ty = tr / 2;
            add_exit(fc, y - 1, logic::DIR_S, tc, ty);
            add_exit(fc, y, logic::DIR_N, tc, ty - 1);
        } else {
            int y = (fr - 1) / 2;
            int ty = (tr - 1) / 2;
            add_exit(fc - 1, y, logic::DIR_E, tc, ty);
            add_exit(fc, y, logic::DIR_W, tc - 1, ty);
        }
    }

    void add_exit(int x, int y, int dir, int lx, int ly) {
        x = logic::wrap_coord(x, width_);
        y = logic::wrap_coord(y, height_);
        lx = logic::wrap_coord(lx, width_);
        ly = logic::wrap_coord(ly, height_);
        known_exits_[static_cast<std::size_t>(exit_key(x, y, dir))] = index(lx, ly);
        mark_mouth(index(x, y), index(lx, ly));
    }

    Landing through_portal(int x, int y, int dir, int portal_id) const {
        Landing out;
        auto learned = exits_.find(exit_key(x, y, dir));
        int landing = learned != exits_.end() ? learned->second : -1;
        if (landing < 0 && !known_exits_.empty()) {
            landing = known_exits_[static_cast<std::size_t>(exit_key(x, y, dir))];
        }
        if (landing >= 0) {
            out.x = landing % width_;
            out.y = landing / width_;
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
        check_candidates(tile, pos.x, pos.y);
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
