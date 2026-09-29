#pragma once

#include "helper.hpp"
#include "logic.hpp"
#include "mapdb.hpp"
#include "tuning.hpp"

#include <cstdint>
#include <string>
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
    // Rounds on which we read a short countdown here, and the last one:
    // two short reads on different rounds mark a fast tile on a map we do
    // not know.
    int short_reads = 0;
    int short_round = -1;
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
        for (int i = 0; i < mapdb::MAP_COUNT && tuning::USE_BAKED_MAPS; ++i) {
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
        mark_slot(index(x, y), index(ex, ey));
    }

    // Two allies at opposite ends of a portal crossing into each other's
    // heads, which neither can see, killed more of our dragons on the ladder
    // than the enemy did. Each crossing therefore has a slot in a four-round
    // cycle: from the lower tile index to the higher on rounds 0 mod 4, back
    // on rounds 2 mod 4. The tile a crossing starts from is the tile the
    // reverse crossing lands on, so standing there is only risky around the
    // reverse slot; the rest of the time it is an ordinary tile.
    // Both directions stay open, so no spawn is ever walled in.
    static int crossing_slot(int from, int landing) {
        return from < landing ? 0 : 2;
    }

    // Our team, so team B can order tiles by their mirror images: the maps
    // are symmetric, and a raw index rule would give the two spawns
    // different portal networks (on Trauma it walled team B's spawn in).
    void set_team_b(bool team_b) { team_b_ = team_b; }

    // Trophy's portal is the back door into its central fountain: two-way
    // slots lost 0/16 there against one-way's 10/16 over 176-game tests.
    bool one_way() const {
        if (known_ != nullptr && std::string(known_->name) == tuning::ONE_WAY_MAP) {
            return true;
        }
        return tuning::PORTAL_ONE_WAY != 0;
    }

    bool crossing_allowed(int from, int landing, int round) const {
        if (one_way()) {
            return forward(from, landing) && round % 2 == 0;
        }
        return round % 4 == crossing_slot(from, landing);
    }

    // True when walking onto 'cell' this round risks an ally popping out of
    // a portal onto our head.
    bool mouth_risk(int cell, int round) const {
        if (one_way()) {
            return !exit_mouths_.empty() && exit_mouths_[static_cast<std::size_t>(cell)] != 0;
        }
        if (slots_.empty()) {
            return false;
        }
        int slot = slots_[static_cast<std::size_t>(cell)];
        if (slot == SLOT_NONE) {
            return false;
        }
        // Allies land here only on the reverse crossing's slot, slot + 2. A
        // head that ends its turn here on round r sits here until its turn
        // in round r + 1, so rounds slot + 1 and slot + 2 are the risky ones.
        int phase = ((round - slot) % 4 + 4) % 4;
        return slot == SLOT_MIXED || phase == 1 || phase == 2;
    }

    // Steps from 'cell' to the nearest rich tile on the baked map, walking
    // and crossing portals; -1 when unknown or unreachable.
    int rich_dist(int cell) const {
        return rich_dist_.empty() ? -1 : rich_dist_[static_cast<std::size_t>(cell)];
    }

    // Steps to the nearest jackpot (a tile refilling within a few rounds);
    // -1 when unknown. Holding ground pays only around these: around
    // Schooltime's 80 moderately fast tiles it cost 4/8 -> 0/8.
    int jackpot_dist(int cell) const {
        return jackpot_dist_.empty() ? -1 : jackpot_dist_[static_cast<std::size_t>(cell)];
    }

    // True when the tiles reachable from 'cell' on foot, without portals,
    // hold no rich tile: Stronghold's and Trauma's spawns and the part of
    // Queen of Spades outside its fountain room. Only there does it pay to
    // head for food far away; elsewhere it pulls dragons into a crowd.
    // Tiles in the on-foot region of 'cell'; 0 when the map is not baked.
    int region_size(int cell) const {
        return region_size_.empty() ? 0 : region_size_[static_cast<std::size_t>(cell)];
    }

    // Room to live in around 'cell': its on-foot region, plus, for a small
    // region, every other small region its portals chain to without passing
    // through a big one. Queen of Spades' fountain rooms are single 16-tile
    // traps; Default's nine 16-tile rooms link to each other by portal into a
    // 194-tile network that ladder teams farm.
    // Steps to the nearest rich tile in a room too small to live in, one
    // entered only by portal (Queen of Spades' two 16-tile fountain rooms);
    // 0 inside one, -1 when unknown or the map has none.
    int room_dist(int cell) const {
        return room_dist_.empty() ? -1 : room_dist_[static_cast<std::size_t>(cell)];
    }

    int living_room(int cell) const {
        return network_size_.empty() ? 0 : network_size_[static_cast<std::size_t>(cell)];
    }

    bool same_region(int a, int b) const {
        return !region_id_.empty() && region_id_[static_cast<std::size_t>(a)] == region_id_[static_cast<std::size_t>(b)];
    }

    bool starving_region(int cell) const {
        return !region_rich_.empty() && region_rich_[static_cast<std::size_t>(cell)] == 0;
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
    std::vector<signed char> slots_;    // crossing slot of each tile a crossing starts from
    std::vector<char> exit_mouths_;     // one-way mode: tiles our crossings land on
    bool team_b_ = false;
    std::vector<int> rich_dist_;        // steps to the nearest rich tile, baked maps only
    std::vector<int> jackpot_dist_;     // steps to the nearest jackpot tile, baked maps only
    std::vector<char> region_rich_;     // whether each tile's on-foot region holds a rich tile
    std::vector<char> region_field_;    // whether it is a field: many slow tiles, a high total rate
    std::vector<int> region_size_;      // tiles in each tile's on-foot region
    std::vector<int> region_id_;        // lowest tile index of each tile's on-foot region
    std::vector<int> network_size_;     // see living_room()
    std::vector<int> room_dist_;        // see room_dist()

    static constexpr signed char SLOT_NONE = -1;
    static constexpr signed char SLOT_MIXED = -2;

    // The direction a one-way portal runs. On Trophy it runs into the
    // central fountain, (12,22) -> (12,5): the top teams take that route
    // and reach the fountain by round 40, and the bowl's open bottom lets
    // dragons walk back out.
    bool forward(int from, int landing) const {
        bool into = known_ != nullptr && std::string(known_->name) == tuning::ONE_WAY_MAP && tuning::ONE_WAY_INTO_CENTRE;
        return into ? order(from) > order(landing) : order(from) < order(landing);
    }

    // Team B compares tiles by their mirror images under the map's symmetry.
    int order(int cell) const {
        int sym = known_ != nullptr ? known_->symmetry : symmetry_;
        if (!team_b_ || sym < 0) {
            return cell;
        }
        logic::Point m = logic::mirror_tile(sym, cell % width_, cell / width_, width_, height_);
        return index(m.x, m.y);
    }

    void mark_slot(int from, int landing) {
        if (forward(from, landing)) {
            if (exit_mouths_.empty()) {
                exit_mouths_.assign(static_cast<std::size_t>(size()), 0);
            }
            exit_mouths_[static_cast<std::size_t>(landing)] = 1;
        }
        if (slots_.empty()) {
            slots_.assign(static_cast<std::size_t>(size()), SLOT_NONE);
        }
        signed char slot = static_cast<signed char>(crossing_slot(from, landing));
        signed char& cur = slots_[static_cast<std::size_t>(from)];
        cur = cur == SLOT_NONE || cur == slot ? slot : SLOT_MIXED;
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
        compute_rich_dist();
        jackpot_dist_ = distance_field([this](int i) {
            mapdb::Gap const& g = known_->gaps[known_->tiles[i] - 'a'];
            return g.hi > 0 && g.hi <= JACKPOT_GAP && network_size_[static_cast<std::size_t>(i)] >= RICH_REGION_MIN;
        });
        room_dist_ = distance_field([this](int i) {
            return is_rich(i) && network_size_[static_cast<std::size_t>(i)] < RICH_REGION_MIN;
        });
        if (symmetry_ < 0 && !broken_ && known_->symmetry >= 0 && possible_[known_->symmetry]) {
            for (int kind = 0; kind < logic::SYM_KINDS; ++kind) {
                possible_[kind] = kind == known_->symmetry;
            }
            settle_symmetry();
        }
    }

    // Breadth-first search back from every rich tile (one that is full most
    // of the time) over the baked map's edges and portals. On Trauma the
    // every-round tiles sit behind portals 20 to 37 steps from the spawns,
    // too far for the discounted pearl value to ever pull a dragon there.
    void compute_rich_dist() {
        rich_dist_.assign(static_cast<std::size_t>(size()), -1);
        std::vector<std::vector<int>> into(static_cast<std::size_t>(size()));
        for (int key = 0; key < static_cast<int>(known_exits_.size()); ++key) {
            int landing = known_exits_[static_cast<std::size_t>(key)];
            if (landing >= 0) {
                into[static_cast<std::size_t>(landing)].push_back(key / 4);
            }
        }
        label_regions();
        std::vector<int> queue;
        for (int i = 0; i < size(); ++i) {
            // A rich tile in a pocket too small to live in (Queen of Spades'
            // 16-tile fountain rooms, entered only by portal) is a trap, not
            // a destination: pulling toward them lost QoS 0/16 vs 7/16.
            bool field_tile = region_field_[static_cast<std::size_t>(i)] != 0 &&
                              known_->gaps[known_->tiles[i] - 'a'].hi > 0;
            if ((is_rich(i) && network_size_[static_cast<std::size_t>(i)] >= RICH_REGION_MIN) || field_tile) {
                rich_dist_[static_cast<std::size_t>(i)] = 0;
                queue.push_back(i);
            }
        }
        for (std::size_t q = 0; q < queue.size(); ++q) {
            int cell = queue[q];
            int next = rich_dist_[static_cast<std::size_t>(cell)] + 1;
            int x = cell % width_;
            int y = cell / width_;
            for (int dir = 0; dir < 4; ++dir) {
                if (known_edge(x, y, dir) != EDGE_OPEN) {
                    continue;
                }
                logic::Point p = logic::step_wrap(x, y, dir, width_, height_);
                int from = index(p.x, p.y);
                if (rich_dist_[static_cast<std::size_t>(from)] < 0) {
                    rich_dist_[static_cast<std::size_t>(from)] = next;
                    queue.push_back(from);
                }
            }
            for (int from : into[static_cast<std::size_t>(cell)]) {
                if (rich_dist_[static_cast<std::size_t>(from)] < 0) {
                    rich_dist_[static_cast<std::size_t>(from)] = next;
                    queue.push_back(from);
                }
            }
        }
    }

    // Breadth-first steps from every tile matching 'source', over the baked
    // edges and portals.
    template <class Source>
    std::vector<int> distance_field(Source source) const {
        std::vector<int> dist(static_cast<std::size_t>(size()), -1);
        std::vector<std::vector<int>> into(static_cast<std::size_t>(size()));
        for (int key = 0; key < static_cast<int>(known_exits_.size()); ++key) {
            int landing = known_exits_[static_cast<std::size_t>(key)];
            if (landing >= 0) {
                into[static_cast<std::size_t>(landing)].push_back(key / 4);
            }
        }
        std::vector<int> queue;
        for (int i = 0; i < size(); ++i) {
            if (source(i)) {
                dist[static_cast<std::size_t>(i)] = 0;
                queue.push_back(i);
            }
        }
        for (std::size_t q = 0; q < queue.size(); ++q) {
            int cell = queue[q];
            int next = dist[static_cast<std::size_t>(cell)] + 1;
            for (int dir = 0; dir < 4; ++dir) {
                if (known_edge(cell % width_, cell / width_, dir) != EDGE_OPEN) {
                    continue;
                }
                logic::Point p = logic::step_wrap(cell % width_, cell / width_, dir, width_, height_);
                int from = index(p.x, p.y);
                if (dist[static_cast<std::size_t>(from)] < 0) {
                    dist[static_cast<std::size_t>(from)] = next;
                    queue.push_back(from);
                }
            }
            for (int from : into[static_cast<std::size_t>(cell)]) {
                if (dist[static_cast<std::size_t>(from)] < 0) {
                    dist[static_cast<std::size_t>(from)] = next;
                    queue.push_back(from);
                }
            }
        }
        return dist;
    }

    static constexpr int JACKPOT_GAP = 10;

    bool is_rich(int cell) const {
        mapdb::Gap const& g = known_->gaps[known_->tiles[cell] - 'a'];
        return logic::steady_chance(g.lo, g.hi, RICH_GRAZE) >= RICH_MIN;
    }

    // Flood each on-foot region of the baked map and note whether it holds
    // a rich tile.
    void label_regions() {
        region_rich_.assign(static_cast<std::size_t>(size()), 0);
        region_field_.assign(static_cast<std::size_t>(size()), 0);
        region_size_.assign(static_cast<std::size_t>(size()), 0);
        region_id_.assign(static_cast<std::size_t>(size()), -1);
        std::vector<int> region(static_cast<std::size_t>(size()), -1);
        for (int start = 0; start < size(); ++start) {
            if (region[static_cast<std::size_t>(start)] >= 0) {
                continue;
            }
            std::vector<int> members{start};
            region[static_cast<std::size_t>(start)] = start;
            bool rich = false;
            for (std::size_t q = 0; q < members.size(); ++q) {
                int cell = members[q];
                rich = rich || is_rich(cell);
                for (int dir = 0; dir < 4; ++dir) {
                    if (known_edge(cell % width_, cell / width_, dir) != EDGE_OPEN) {
                        continue;
                    }
                    logic::Point p = logic::step_wrap(cell % width_, cell / width_, dir, width_, height_);
                    int to = index(p.x, p.y);
                    if (region[static_cast<std::size_t>(to)] < 0) {
                        region[static_cast<std::size_t>(to)] = start;
                        members.push_back(to);
                    }
                }
            }
            // A field of slow tiles feeds a swarm as well as a fast tile does
            // (Trauma's middle: 110 tiles, about a pearl a round). Per mille
            // pearls a round: one tile spawns every (lo + hi) / 2 rounds.
            int rate = 0;
            for (int cell : members) {
                mapdb::Gap const& g = known_->gaps[known_->tiles[cell] - 'a'];
                if (g.hi > 0) {
                    rate += 2000 / (g.lo + g.hi);
                }
            }
            bool field = tuning::FIELD_RATE > 0 && rate >= tuning::FIELD_RATE &&
                         static_cast<int>(members.size()) >= RICH_REGION_MIN;
            for (int cell : members) {
                region_rich_[static_cast<std::size_t>(cell)] = rich || field ? 1 : 0;
                region_field_[static_cast<std::size_t>(cell)] = field ? 1 : 0;
                region_size_[static_cast<std::size_t>(cell)] = static_cast<int>(members.size());
                region_id_[static_cast<std::size_t>(cell)] = start;
            }
        }
        link_networks();
    }

    // Union small regions joined by portals; big regions are not crossed.
    void link_networks() {
        network_size_ = region_size_;
        std::unordered_map<int, std::vector<int>> links;
        for (int key = 0; key < static_cast<int>(known_exits_.size()); ++key) {
            int landing = known_exits_[static_cast<std::size_t>(key)];
            if (landing < 0) {
                continue;
            }
            int from = key / 4;
            int a = region_id_[static_cast<std::size_t>(from)];
            int b = region_id_[static_cast<std::size_t>(landing)];
            if (a != b && region_size_[static_cast<std::size_t>(from)] < RICH_REGION_MIN &&
                region_size_[static_cast<std::size_t>(landing)] < RICH_REGION_MIN) {
                links[a].push_back(b);
                links[b].push_back(a);
            }
        }
        std::unordered_map<int, int> total;
        for (int i = 0; i < size(); ++i) {
            int r = region_id_[static_cast<std::size_t>(i)];
            if (region_size_[static_cast<std::size_t>(i)] >= RICH_REGION_MIN || total.count(r) != 0) {
                continue;
            }
            std::vector<int> group{r};
            std::unordered_map<int, bool> seen{{r, true}};
            int sum = 0;
            for (std::size_t q = 0; q < group.size(); ++q) {
                sum += region_size_[static_cast<std::size_t>(group[q])];
                for (int n : links[group[q]]) {
                    if (!seen[n]) {
                        seen[n] = true;
                        group.push_back(n);
                    }
                }
            }
            for (int g : group) {
                total[g] = sum;
            }
        }
        for (int i = 0; i < size(); ++i) {
            auto it = total.find(region_id_[static_cast<std::size_t>(i)]);
            if (it != total.end()) {
                network_size_[static_cast<std::size_t>(i)] = it->second;
            }
        }
    }

    static constexpr int RICH_GRAZE = 5;
  public:
    static constexpr int RICH_REGION_MIN = 40;

  private:
    static constexpr int RICH_MIN = 30;

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
        mark_slot(index(x, y), index(lx, ly));
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
        if (countdown >= 0 && countdown <= tuning::FAST_SEEN && round != c.short_round) {
            c.short_reads += 1;
            c.short_round = round;
        }
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
