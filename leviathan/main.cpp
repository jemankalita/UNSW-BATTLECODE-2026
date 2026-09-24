#include "helper.hpp"
#include "king.hpp"
#include "logic.hpp"
#include "tuning.hpp"
#include "world.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using logic::NEVER;

// Pull of one sure pearl on the tile a move lands on. Everything else in a
// plan's score is measured against this.
constexpr long long PEARL = 1000000;
constexpr long long THREAT_SCALE = PEARL / 50;
constexpr long long ROOM_STEP = PEARL / 50;
constexpr long long STRAIGHT = PEARL / 20;
constexpr long long PORTAL_RISK = PEARL / 2;
constexpr long long UNSAFE = 1000000000000LL;
constexpr int VALUE_DEPTH = 60;
constexpr int KAMIKAZE_LEN = 5;
constexpr int SURVIVE_DEPTH = 40;
constexpr int SURVIVE_BUDGET = 4000;
constexpr int CHILD_ROOM = 16;
constexpr int HEAD_REACH = 2;
constexpr int FEED_RANGE = 3;
constexpr long long CROWD = PEARL / 8;

// Another dragon's head seen this turn.
struct Other {
    int id = 0;
    bool enemy = false;
    int x = 0;
    int y = 0;
    int len = 0;  // segments in view: a lower bound on its length
};

// One candidate action: a walk of one or more steps.
struct Plan {
    std::vector<int> dirs;
    std::vector<int> cells;  // tile after each step
    int length = 0;          // our length once it is done
    int eaten = 0;
    int room = 0;
    int lasts = 0;           // turns the look-ahead could keep moving
    bool safe = false;
    bool portal = false;
    bool portal_blind = false;  // the far side of the portal is out of view
    int ram_len = 0;         // enemy length if the last step rams a head
    long long score = 0;
};

unswbc::Direction to_direction(int dir) {
    static unswbc::Direction::Value const values[] = {
        unswbc::Direction::NORTH, unswbc::Direction::EAST, unswbc::Direction::SOUTH, unswbc::Direction::WEST};
    return unswbc::Direction(values[dir & 3]);
}

int dir_index(unswbc::Direction direction) {
    switch (direction.value) {
    case unswbc::Direction::NORTH: return logic::DIR_N;
    case unswbc::Direction::EAST: return logic::DIR_E;
    case unswbc::Direction::SOUTH: return logic::DIR_S;
    default: return logic::DIR_W;
    }
}

class Brain {
  public:
    Brain(int width, int height)
        : map_(width, height), w_(width), h_(height), n_(width * height),
          occ_(static_cast<std::size_t>(n_), -1), occ_enemy_(static_cast<std::size_t>(n_), 0),
          threat_(static_cast<std::size_t>(n_), 0), mark_(static_cast<std::size_t>(n_), 0) {
        long long weight = PEARL;
        for (int t = 0; t <= VALUE_DEPTH; ++t) {
            gamma_.push_back(weight);
            weight = weight * tuning::GAMMA / 100;
        }
    }

    void act(unswbc::Controller& ct, unswbc::Game& game) {
        begin_turn(ct, game);
        if (feed_king(ct)) {
            return;
        }
        std::vector<Plan> plans = walks(1);
        for (Plan& plan : plans) {
            judge(plan);
        }
        bool any_safe = best_of(plans, true) != nullptr;

        Plan const* ram = best_ram(plans, !any_safe);
        if (ram != nullptr) {
            commit(ct, *ram, "ram");
            return;
        }
        if (any_safe && try_split(ct, *best_of(plans, true))) {
            return;
        }
        if (!any_safe) {
            std::vector<Plan> sprints = walks(2);
            for (Plan& plan : sprints) {
                judge(plan);
            }
            Plan const* ram2 = best_ram(sprints, true);
            Plan const* escape = best_of(sprints, true);
            if (escape != nullptr) {
                commit(ct, *escape, "escape");
                return;
            }
            if (ram2 != nullptr) {
                commit(ct, *ram2, "last ram");
                return;
            }
        }
        Plan const* pick = best_of(plans, false);
        if (pick != nullptr) {
            commit(ct, *pick, pick->safe ? "farm" : "squeeze");
            return;
        }
        ct.make_move(to_direction(facing_));
        ct.set_indicator_string("stuck");
    }

  private:
    world::Map map_;
    int w_;
    int h_;
    int n_;
    int round_ = 1;
    int len_ = 2;
    int id_ = 0;
    int units_ = 1;
    int limit_ = 64;
    int facing_ = logic::DIR_N;
    int head_ = 0;
    std::vector<int> occ_;        // dragon id on each tile in view, -1 if none
    std::vector<char> occ_enemy_;
    std::vector<int> occ_list_;
    std::vector<int> body_;       // our tiles, head first
    std::vector<int> trail_;      // tiles our head has stood on, oldest first
    std::vector<Other> others_;
    std::vector<int> threat_;     // cost of ending a move on each tile
    std::vector<int> threat_list_;
    std::vector<int> mark_;
    int stamp_ = 0;
    std::vector<long long> gamma_;
    std::vector<int> survive_index_;
    std::vector<int> entered_;    // look-ahead step that entered each tile, -1 if none
    int budget_ = 0;
    int deepest_ = 0;
    king::Tracker king_;
    int feed_target_ = -1;        // king's head while we walk over to feed it
    bool pending_portal_ = false;
    int portal_from_ = 0;
    int portal_dir_ = 0;

    int cx(int cell) const { return cell % w_; }
    int cy(int cell) const { return cell / w_; }

    // ---- reading the turn ----------------------------------------------

    void begin_turn(unswbc::Controller& ct, unswbc::Game& game) {
        round_ = game.get_round_num();
        len_ = ct.get_length();
        id_ = ct.get_id();
        units_ = ct.get_unit_count();
        limit_ = game.get_unit_limit();
        facing_ = dir_index(ct.get_dir());
        unswbc::Position here = ct.get_position();
        head_ = map_.index(here.x, here.y);
        map_.observe(ct, round_);
        if (pending_portal_) {
            map_.learn_exit(cx(portal_from_), cy(portal_from_), portal_dir_, here.x, here.y);
            pending_portal_ = false;
        }
        scan_dragons(ct);
        track_body(ct);
        map_threats();
        follow_king(ct);
    }

    // ---- the king ------------------------------------------------------------

    void follow_king(unswbc::Controller& ct) {
        for (std::uint64_t word : ct.get_sonar_messages()) {
            king_.hear(word, round_);
        }
        king_.offer(id_, cx(head_), cy(head_), len_, round_);
        for (Other const& other : others_) {
            if (!other.enemy) {
                king_.offer(other.id, other.x, other.y, other.len, round_);
            }
        }
        king_.expire(round_);
        king::Report const& crown = king_.best();
        if (crown.valid && crown.id != id_ && ct.get_tile(unswbc::Position(crown.x, crown.y)) != nullptr &&
            !ally_head_seen(crown.id)) {
            king_.forget();
            king_.offer(id_, cx(head_), cy(head_), len_, round_);
        }
        for (int dir = 0; dir < 4; ++dir) {
            ct.send_sonar(to_direction(dir), king_.message());
        }
        feed_target_ = -1;
        if (!logic::feeding_time(round_, tuning::FEED_ROUND, units_, tuning::FEED_KEEP)) {
            return;
        }
        king::Report const& target = king_.best();
        if (!target.valid || target.id == id_) {
            return;
        }
        int gap = logic::manhattan(cx(head_), cy(head_), target.x, target.y, w_, h_);
        if (logic::can_feed_in_time(gap, len_, round_, unswbc::Constants::MAX_ROUNDS)) {
            feed_target_ = map_.index(target.x, target.y);
        }
    }

    bool ally_head_seen(int id) const {
        for (Other const& other : others_) {
            if (!other.enemy && other.id == id) {
                return true;
            }
        }
        return false;
    }

    // Beside the king, a feeder turns into its own neck: it dies on the
    // spot and every second segment becomes a pearl within the king's reach.
    bool feed_king(unswbc::Controller& ct) {
        if (feed_target_ < 0) {
            return false;
        }
        king::Report const& target = king_.best();
        if (!ally_head_seen(target.id) ||
            logic::chebyshev(cx(head_), cy(head_), target.x, target.y, w_, h_) > FEED_RANGE) {
            return false;
        }
        ct.make_move(to_direction(logic::opposite_dir(facing_)));
        ct.set_indicator_string("feed");
        return true;
    }

    void scan_dragons(unswbc::Controller& ct) {
        for (int cell : occ_list_) {
            occ_[static_cast<std::size_t>(cell)] = -1;
            occ_enemy_[static_cast<std::size_t>(cell)] = 0;
        }
        occ_list_.clear();
        others_.clear();
        std::vector<std::pair<int, int>> counts;
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            unswbc::DragonPart const* part = tile.get_dragon();
            if (part == nullptr) {
                continue;
            }
            int cell = map_.index(tile.get_position().x, tile.get_position().y);
            occ_[static_cast<std::size_t>(cell)] = part->get_id();
            occ_enemy_[static_cast<std::size_t>(cell)] = part->get_team() == ct.get_team() ? 0 : 1;
            occ_list_.push_back(cell);
            bump(counts, part->get_id());
        }
        for (int cell : occ_list_) {
            int id = occ_[static_cast<std::size_t>(cell)];
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(cx(cell), cy(cell)));
            if (id == id_ || tile == nullptr || !tile->get_dragon()->is_head()) {
                continue;
            }
            Other other;
            other.id = id;
            other.enemy = occ_enemy_[static_cast<std::size_t>(cell)] != 0;
            other.x = cx(cell);
            other.y = cy(cell);
            for (auto const& entry : counts) {
                if (entry.first == id) {
                    other.len = entry.second;
                }
            }
            others_.push_back(other);
        }
    }

    static void bump(std::vector<std::pair<int, int>>& counts, int id) {
        for (auto& entry : counts) {
            if (entry.first == id) {
                ++entry.second;
                return;
            }
        }
        counts.emplace_back(id, 1);
    }

    // Our body is the last len_ tiles the head stood on. Keeping that trail
    // lets us know segments that have left the 7x7 window; anything the
    // window contradicts is rebuilt from what we can see.
    void track_body(unswbc::Controller& ct) {
        if (trail_.empty() || trail_.back() != head_) {
            trail_.push_back(head_);
        }
        body_.clear();
        for (int i = static_cast<int>(trail_.size()) - 1; i >= 0 && static_cast<int>(body_.size()) < len_; --i) {
            body_.push_back(trail_[static_cast<std::size_t>(i)]);
        }
        if (!body_matches_view(ct)) {
            body_ = trace_view(ct);
            trail_.assign(body_.rbegin(), body_.rend());
        }
        if (static_cast<int>(trail_.size()) > len_ + 8) {
            trail_.erase(trail_.begin(), trail_.end() - (len_ + 8));
        }
    }

    bool body_matches_view(unswbc::Controller& ct) const {
        int mine_in_view = 0;
        for (int cell : occ_list_) {
            if (occ_[static_cast<std::size_t>(cell)] == id_) {
                ++mine_in_view;
            }
        }
        int claimed_in_view = 0;
        for (int cell : body_) {
            if (ct.get_tile(unswbc::Position(cx(cell), cy(cell))) == nullptr) {
                continue;
            }
            if (occ_[static_cast<std::size_t>(cell)] != id_) {
                return false;
            }
            ++claimed_in_view;
        }
        return claimed_in_view == mine_in_view;
    }

    std::vector<int> trace_view(unswbc::Controller& ct) const {
        std::vector<int> body{head_};
        logic::Point cursor = logic::step_wrap(cx(head_), cy(head_), logic::opposite_dir(facing_), w_, h_);
        while (static_cast<int>(body.size()) < len_) {
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(cursor.x, cursor.y));
            if (tile == nullptr || tile->get_dragon() == nullptr) {
                break;
            }
            unswbc::DragonPart const* part = tile->get_dragon();
            int cell = map_.index(cursor.x, cursor.y);
            if (part->get_id() != id_ || part->is_head() || std::find(body.begin(), body.end(), cell) != body.end()) {
                break;
            }
            body.push_back(cell);
            cursor = logic::step_wrap(cursor.x, cursor.y, logic::opposite_dir(dir_index(part->get_dir())), w_, h_);
        }
        return body;
    }

    // Tiles an enemy head could reach before our next turn, priced by what a
    // ram there would cost us.
    void map_threats() {
        for (int cell : threat_list_) {
            threat_[static_cast<std::size_t>(cell)] = 0;
        }
        threat_list_.clear();
        for (Other const& other : others_) {
            if (!other.enemy) {
                continue;
            }
            int start = map_.index(other.x, other.y);
            std::vector<int> frontier{start};
            for (int reach = 1; reach <= 2 && !frontier.empty(); ++reach) {
                std::vector<int> next;
                for (int cell : frontier) {
                    for (int dir = 0; dir < 4; ++dir) {
                        world::Landing land = map_.step(cx(cell), cy(cell), dir, true);
                        if (!land.ok) {
                            continue;
                        }
                        int to = map_.index(land.x, land.y);
                        if (occ_[static_cast<std::size_t>(to)] >= 0 && to != head_) {
                            continue;
                        }
                        int cost = logic::ram_threat(len_, other.len, units_, reach);
                        if (cost > threat_[static_cast<std::size_t>(to)]) {
                            if (threat_[static_cast<std::size_t>(to)] == 0) {
                                threat_list_.push_back(to);
                            }
                            threat_[static_cast<std::size_t>(to)] = cost;
                        }
                        next.push_back(to);
                    }
                }
                frontier.swap(next);
            }
        }
    }

    // ---- building and judging plans ---------------------------------------

    bool pearl_now(int cell) const {
        world::Cell const& c = map_.cell(cell);
        return c.pearl && c.seen_round == round_;
    }

    // Every legal walk of exactly 'steps' steps (a sprint when steps > 1).
    // Each step is checked against the board as it stands now: nothing we
    // pass, our own tail included, is treated as vacated during the turn.
    std::vector<Plan> walks(int steps) {
        std::vector<Plan> out;
        if (steps > 1 && len_ <= steps) {
            return out;
        }
        Plan seed;
        extend(seed, head_, steps, out);
        return out;
    }

    void extend(Plan& plan, int from, int steps_left, std::vector<Plan>& out) {
        for (int dir = 0; dir < 4; ++dir) {
            world::Landing land = map_.step(cx(from), cy(from), dir, false);
            if (!land.ok || (land.portal && !plan.dirs.empty())) {
                continue;
            }
            int cell = map_.index(land.x, land.y);
            if (std::find(plan.cells.begin(), plan.cells.end(), cell) != plan.cells.end()) {
                continue;
            }
            int who = occ_[static_cast<std::size_t>(cell)];
            bool ram = who >= 0 && occ_enemy_[static_cast<std::size_t>(cell)] && is_enemy_head(cell);
            if (who >= 0 && !(ram && steps_left == 1)) {
                continue;
            }
            Plan next = plan;
            next.dirs.push_back(dir);
            next.cells.push_back(cell);
            next.portal = next.portal || land.portal;
            if (land.portal && map_.cell(cell).seen_round != round_) {
                next.portal_blind = true;
            }
            if (ram) {
                next.ram_len = enemy_len_at(cell);
            } else if (pearl_now(cell)) {
                ++next.eaten;
            }
            if (steps_left == 1) {
                out.push_back(next);
            } else if (!ram) {
                extend(next, cell, steps_left - 1, out);
            }
        }
    }

    bool is_enemy_head(int cell) const {
        for (Other const& other : others_) {
            if (other.enemy && map_.index(other.x, other.y) == cell) {
                return true;
            }
        }
        return false;
    }

    int enemy_len_at(int cell) const {
        for (Other const& other : others_) {
            if (map_.index(other.x, other.y) == cell) {
                return other.len;
            }
        }
        return 0;
    }

    // Our body after the plan, head first.
    std::vector<int> body_after(Plan const& plan) const {
        std::vector<int> body(plan.cells.rbegin(), plan.cells.rend());
        for (int cell : body_) {
            if (static_cast<int>(body.size()) >= plan.length) {
                break;
            }
            body.push_back(cell);
        }
        return body;
    }

    void judge(Plan& plan) {
        int steps = static_cast<int>(plan.dirs.size());
        plan.length = len_ - (steps - 1) + plan.eaten;
        if (plan.ram_len > 0) {
            plan.safe = false;
            return;
        }
        std::vector<int> body = body_after(plan);
        int need = logic::room_needed(plan.length);
        int pearls = 0;
        plan.room = flood(body, plan.length, need * 2 + 8, pearls);
        bool lives = survives(body, plan.length);
        plan.lasts = lives ? SURVIVE_DEPTH : deepest_;
        // We cannot see through a portal. Crossing one blind can land the
        // head on a dragon, so it is a last resort, never a safe move; the
        // planners may still count portals as exits.
        plan.safe = plan.room >= need + pearls && lives && !plan.portal_blind;
        int end = plan.cells.back();
        long long score = value_from(body, plan.length) - static_cast<long long>(steps - 1) * PEARL;
        for (std::size_t i = 0; i + 1 < plan.cells.size(); ++i) {
            if (pearl_now(plan.cells[i])) {
                score += PEARL;
            }
        }
        score -= static_cast<long long>(threat_[static_cast<std::size_t>(end)]) * THREAT_SCALE;
        score += static_cast<long long>(std::min(plan.room, need * 2 + 8)) * ROOM_STEP;
        if (plan.dirs.front() == facing_) {
            score += STRAIGHT;
        }
        score -= crowding(end);
        if (plan.portal) {
            score -= PORTAL_RISK;
        }
        // With no safe move, buy the most turns: other dragons move on and
        // the tail frees tiles, so a pocket that lasts can still open up.
        if (!plan.safe) {
            score = -UNSAFE + static_cast<long long>(plan.lasts) * PEARL +
                    static_cast<long long>(plan.room) * PEARL / 10 + score / 1000;
        }
        plan.score = score;
    }

    // Dragons packed together wall each other in: keep our head away from
    // allied heads, and out of the narrow water between other bodies.
    long long crowding(int cell) const {
        long long cost = 0;
        for (Other const& other : others_) {
            if (other.enemy) {
                continue;
            }
            int gap = logic::manhattan(cx(cell), cy(cell), other.x, other.y, w_, h_);
            if (gap <= 3) {
                cost += CROWD * (4 - gap);
            }
        }
        for (int part : occ_list_) {
            int who = occ_[static_cast<std::size_t>(part)];
            if (who == id_) {
                continue;
            }
            if (logic::manhattan(cx(cell), cy(cell), cx(part), cy(part), w_, h_) <= 2) {
                cost += CROWD / 4;
            }
        }
        return cost;
    }

    int next_stamp() {
        if (++stamp_ == (1 << 30)) {
            std::fill(mark_.begin(), mark_.end(), 0);
            stamp_ = 1;
        }
        return stamp_;
    }

    // Moves after the plan before a tile is free for our head: other dragons
    // are walls, our own segments free up as the tail passes, and tiles
    // beside another head may be taken before we move again.
    int wait_for(int cell, std::vector<int> const& body_index, int length) const {
        if (occ_[static_cast<std::size_t>(cell)] >= 0 && occ_[static_cast<std::size_t>(cell)] != id_) {
            return NEVER;
        }
        int index = body_index[static_cast<std::size_t>(cell)];
        if (index >= 0) {
            return logic::unlock_time(length, index);
        }
        return 0;
    }

    std::vector<int> index_of(std::vector<int> const& body) const {
        std::vector<int> index(static_cast<std::size_t>(n_), -1);
        for (std::size_t i = 0; i < body.size(); ++i) {
            index[static_cast<std::size_t>(body[i])] = static_cast<int>(i);
        }
        return index;
    }

    // Tiles the head can still reach after the plan, where a tile our own
    // body covers opens once the tail has passed it. Stops counting at 'cap'.
    // 'pearls' returns the known pearls among the counted tiles: grazing
    // them lengthens the body, so the room has to hold those segments too.
    int flood(std::vector<int> const& body, int length, int cap, int& pearls) {
        pearls = 0;
        std::vector<int> body_index = index_of(body);
        int stamp = next_stamp();
        std::vector<int> frontier{body.front()};
        std::vector<std::pair<int, int>> waiting;  // (open time, tile)
        mark_[static_cast<std::size_t>(body.front())] = stamp;
        int count = 1;
        // A dragon cannot stand still, so a tile that opens later is only
        // reachable while the head still has somewhere to go meanwhile.
        for (int t = 1; count < cap && !frontier.empty() && t <= length + 2; ++t) {
            std::vector<int> next;
            for (auto& entry : waiting) {
                if (entry.first <= t && mark_[static_cast<std::size_t>(entry.second)] != stamp) {
                    mark_[static_cast<std::size_t>(entry.second)] = stamp;
                    next.push_back(entry.second);
                    ++count;
                }
            }
            for (int cell : frontier) {
                for (int dir = 0; dir < 4; ++dir) {
                    world::Landing land = map_.step(cx(cell), cy(cell), dir, true);
                    if (!land.ok) {
                        continue;
                    }
                    int to = map_.index(land.x, land.y);
                    if (mark_[static_cast<std::size_t>(to)] == stamp) {
                        continue;
                    }
                    int open = wait_for(to, body_index, length);
                    if (t <= HEAD_REACH && head_can_reach(to, t)) {
                        continue;
                    }
                    if (open >= NEVER) {
                        continue;
                    }
                    if (open > t) {
                        waiting.emplace_back(open, to);
                        continue;
                    }
                    mark_[static_cast<std::size_t>(to)] = stamp;
                    next.push_back(to);
                    ++count;
                    if (map_.cell(to).pearl) {
                        ++pearls;
                    }
                }
            }
            frontier.swap(next);
        }
        return count;
    }

    // Exact look-ahead: can the head keep moving for the next few turns with
    // the body simulated step by step? A tight pocket has few branches, so
    // the search settles it; in open water the budget runs out first and the
    // flood count above decides.
    bool survives(std::vector<int> const& body, int length) {
        survive_index_ = index_of(body);
        if (entered_.empty()) {
            entered_.assign(static_cast<std::size_t>(n_), -1);
        }
        budget_ = SURVIVE_BUDGET;
        deepest_ = 0;
        int depth = std::min(length + 1, SURVIVE_DEPTH);
        bool ok = search(body.front(), 0, depth, length, 0);
        return ok || budget_ <= 0;
    }

    // 'grown' counts pearls eaten along this path: each one holds the tail
    // back a move, so every tile opens that much later.
    bool search(int cell, int t, int depth, int length, int grown) {
        deepest_ = std::max(deepest_, t);
        if (t >= depth || --budget_ <= 0) {
            return true;
        }
        int move = t + 1 - grown;
        for (int dir = 0; dir < 4; ++dir) {
            world::Landing land = map_.step(cx(cell), cy(cell), dir, true);
            if (!land.ok) {
                continue;
            }
            int to = map_.index(land.x, land.y);
            int who = occ_[static_cast<std::size_t>(to)];
            if (who >= 0 && who != id_) {
                continue;
            }
            int index = survive_index_[static_cast<std::size_t>(to)];
            if (index >= 0 && move < length - index + 1) {
                continue;
            }
            int before = entered_[static_cast<std::size_t>(to)];
            if (before >= 0 && move < before + length + 1) {
                continue;
            }
            if (move <= HEAD_REACH && head_can_reach(to, t + 1)) {
                continue;
            }
            entered_[static_cast<std::size_t>(to)] = t + 1;
            int eats = map_.cell(to).pearl && before < 0 ? 1 : 0;
            bool ok = search(to, t + 1, depth, length, grown + eats);
            entered_[static_cast<std::size_t>(to)] = before;
            if (ok) {
                return true;
            }
        }
        return false;
    }

    // Every other dragon moves once before each of ours (lower ids next
    // round, higher ids later this one), so another head, ally or enemy,
    // may already stand on a tile within 'moves' steps of it by the time our
    // head gets there. Only the next couple of moves are judged this way:
    // further out the guess is too loose to rule tiles out.
    bool head_can_reach(int cell, int moves) const {
        for (Other const& other : others_) {
            if (logic::manhattan(cx(cell), cy(cell), other.x, other.y, w_, h_) <= moves) {
                return true;
            }
        }
        return false;
    }

    // Distance-discounted pull of every pearl we expect to find, walking out
    // from where the plan leaves our head.
    long long value_from(std::vector<int> const& body, int length) {
        std::vector<int> body_index = index_of(body);
        int stamp = next_stamp();
        int start = body.front();
        std::vector<int> frontier{start};
        mark_[static_cast<std::size_t>(start)] = stamp;
        long long total = tile_value(start, 0);
        for (int t = 1; t <= VALUE_DEPTH && !frontier.empty(); ++t) {
            std::vector<int> next;
            for (int cell : frontier) {
                for (int dir = 0; dir < 4; ++dir) {
                    world::Landing land = map_.step(cx(cell), cy(cell), dir, true);
                    if (!land.ok) {
                        continue;
                    }
                    int to = map_.index(land.x, land.y);
                    if (mark_[static_cast<std::size_t>(to)] == stamp) {
                        continue;
                    }
                    mark_[static_cast<std::size_t>(to)] = stamp;
                    if (wait_for(to, body_index, length) > t) {
                        total += ram_bait(to, t);
                        continue;
                    }
                    total += tile_value(to, t);
                    next.push_back(to);
                }
            }
            frontier.swap(next);
        }
        return total;
    }

    long long tile_value(int cell, int t) const {
        world::Cell const& c = map_.cell(cell);
        int arrive = round_ + t + 1;
        int chance = 0;
        if (c.seen) {
            chance = logic::pearl_chance(true, c.pearl, round_ - c.seen_round, c.spawn_round, arrive, tuning::FOG_PRIOR);
        } else if (c.timed) {
            // Never seen, but its mirror told us when it spawns.
            chance = logic::pearl_chance(true, false, round_ - c.timed_round, c.spawn_round, arrive, tuning::FOG_PRIOR);
            chance = std::max(chance, tuning::FOG_PRIOR);
        } else {
            chance = tuning::FOG_PRIOR;
        }
        if (feed_target_ >= 0) {
            // A feeder heads for the king and leaves the pearls near it alone.
            int to_king = logic::manhattan(cx(cell), cy(cell), cx(feed_target_), cy(feed_target_), w_, h_);
            if (to_king <= FEED_RANGE) {
                return gamma_[static_cast<std::size_t>(t)] * 3;
            }
            if (to_king <= FEED_RANGE + 3) {
                return 0;
            }
        }
        if (chance == 0) {
            return 0;
        }
        for (Other const& other : others_) {
            int theirs = logic::manhattan(cx(cell), cy(cell), other.x, other.y, w_, h_);
            if (theirs < t + (other.enemy ? 0 : 1) || (theirs == t + 1 && other.id < id_)) {
                chance = chance * (other.enemy ? 50 : 30) / 100;
            }
        }
        return gamma_[static_cast<std::size_t>(t)] * chance / 100;
    }

    // A short dragon is drawn toward a clearly longer enemy head: reaching it
    // is a ram that trades up.
    long long ram_bait(int cell, int t) const {
        if (len_ > KAMIKAZE_LEN || units_ <= 1 || !is_enemy_head(cell)) {
            return 0;
        }
        int theirs = enemy_len_at(cell);
        if (!logic::worth_ram(len_, theirs, units_, false)) {
            return 0;
        }
        return gamma_[static_cast<std::size_t>(t)] * (100 + 10 * theirs) / 100;
    }

    // ---- choosing ----------------------------------------------------------

    static Plan const* best_of(std::vector<Plan> const& plans, bool safe_only) {
        Plan const* best = nullptr;
        for (Plan const& plan : plans) {
            if (plan.ram_len > 0 || (safe_only && !plan.safe)) {
                continue;
            }
            if (best == nullptr || plan.score > best->score) {
                best = &plan;
            }
        }
        return best;
    }

    Plan const* best_ram(std::vector<Plan> const& plans, bool cornered) const {
        Plan const* best = nullptr;
        for (Plan const& plan : plans) {
            if (plan.ram_len <= 0 || !logic::worth_ram(len_, plan.ram_len, units_, cornered)) {
                continue;
            }
            if (best == nullptr || plan.ram_len > best->ram_len) {
                best = &plan;
            }
        }
        return best;
    }

    // Splitting spends the turn standing still, so only do it with a safe
    // move in hand, no enemy head close, and room for the child to move.
    bool try_split(unswbc::Controller& ct, Plan const& safe_move) {
        int cap = logic::team_cap(w_, h_, limit_, tuning::TILES_PER_DRAGON);
        if (!logic::want_split(len_, units_, cap, round_, tuning::SPLIT_LEN, tuning::SPLIT_LAST_ROUND)) {
            return false;
        }
        if (!ct.can_split(tuning::CHILD_SIZE) || static_cast<int>(body_.size()) < len_ || safe_move.eaten > 0) {
            return false;
        }
        if (threat_[static_cast<std::size_t>(head_)] > 0 || enemy_within(3)) {
            return false;
        }
        if (open_room(body_.back(), CHILD_ROOM) < CHILD_ROOM) {
            return false;
        }
        ct.do_split(tuning::CHILD_SIZE);
        ct.set_indicator_string("split");
        trail_.erase(trail_.begin(), trail_.end() - std::min<int>(static_cast<int>(trail_.size()), len_ - tuning::CHILD_SIZE));
        return true;
    }

    // Free tiles reachable from 'start' through tiles no dragon stands on
    // now, counting up to 'cap'. A newborn child must not start in a pocket.
    int open_room(int start, int cap) {
        int stamp = next_stamp();
        mark_[static_cast<std::size_t>(start)] = stamp;
        std::vector<int> queue{start};
        for (std::size_t q = 0; q < queue.size() && static_cast<int>(queue.size()) <= cap; ++q) {
            int cell = queue[q];
            for (int dir = 0; dir < 4; ++dir) {
                world::Landing land = map_.step(cx(cell), cy(cell), dir, true);
                if (!land.ok || land.portal) {
                    continue;
                }
                int to = map_.index(land.x, land.y);
                if (mark_[static_cast<std::size_t>(to)] == stamp || occ_[static_cast<std::size_t>(to)] >= 0) {
                    continue;
                }
                mark_[static_cast<std::size_t>(to)] = stamp;
                queue.push_back(to);
            }
        }
        return static_cast<int>(queue.size()) - 1;
    }

    bool enemy_within(int range) const {
        for (Other const& other : others_) {
            if (other.enemy && logic::chebyshev(cx(head_), cy(head_), other.x, other.y, w_, h_) <= range) {
                return true;
            }
        }
        return false;
    }

    void commit(unswbc::Controller& ct, Plan const& plan, char const* label) {
        std::vector<unswbc::Direction> dirs;
        for (int dir : plan.dirs) {
            dirs.push_back(to_direction(dir));
        }
        if (dirs.size() == 1) {
            ct.make_move(dirs.front());
        } else {
            ct.make_moves(dirs);
        }
        ct.set_indicator_string(label);
        for (int cell : plan.cells) {
            trail_.push_back(cell);
        }
        pending_portal_ = plan.portal && plan.dirs.size() == 1;
        if (pending_portal_) {
            portal_from_ = head_;
            portal_dir_ = plan.dirs.front();
        }
    }
};

}  // namespace

int main() {
    auto [ct, game] = unswbc::init();
    Brain brain(game.width, game.height);

    while (unswbc::update(ct, game)) {
        try {
            brain.act(ct, game);
        } catch (...) {
            ct.make_move(ct.get_dir());
        }
        unswbc::end_turn();
    }
}
