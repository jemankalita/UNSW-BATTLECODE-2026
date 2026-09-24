#include "helper.hpp"
#include "logic.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Cell {
    bool seen = false;
    bool pearl = false;
    int seen_round = -1;
    int spawn_round = -1;
    bool kelp[4] = {};
    bool edge_seen[4] = {};
    int portal[4] = {-1, -1, -1, -1};
    int claimed_by = -1;
    int claim_round = -1;
};

struct Head {
    int x = 0;
    int y = 0;
    int id = 0;
    int len = 0;
    bool enemy = false;
};

struct Step {
    int dir = 0;
    logic::Point dest{};
    bool pearl = false;
};

class Brain {
  public:
    explicit Brain(int width, int height)
        : width_(width), height_(height), cells_(static_cast<std::size_t>(width * height)) {
        cols_ = logic::sector_axis(width_);
        rows_ = logic::sector_axis(height_);
        int sectors = cols_ * rows_;
        sector_owner_.assign(static_cast<std::size_t>(sectors), -1);
        sector_round_.assign(static_cast<std::size_t>(sectors), -1);
        sector_visit_.assign(static_cast<std::size_t>(sectors), -1000);
    }

    void act(unswbc::Controller& ct, unswbc::Game& game) {
        round_ = game.get_round_num();
        length_ = ct.get_length();
        allies_ = ct.get_unit_count();
        my_id_ = ct.get_id();
        facing_ = dir_index(ct.get_dir());
        here_ = ct.get_position();
        enemy_heads_.clear();
        ally_heads_.clear();

        observe(ct);
        hear(ct);
        policy_ = logic::make_policy(width_, height_, portal_pairs_, round_);
        refresh_book();
        own_sector(here_.x, here_.y);
        scorer_ = compute_scorer();
        speak(ct);

        std::vector<logic::Point> body = trace_body(ct);
        tail_known_ = static_cast<int>(body.size()) == length_ && length_ >= 2;
        if (tail_known_) {
            tail_ = body.back();
            if (body.size() >= 2) {
                tail_neck_ = body[body.size() - 2];
                tail_neck_ok_ = true;
            } else {
                tail_neck_ok_ = false;
            }
        } else {
            tail_neck_ok_ = false;
        }

        std::vector<Step> raw = safe_steps(ct, here_.x, here_.y, facing_, true);
        std::vector<Step> safe = filter_steps(ct, raw, true, true);
        if (safe.empty()) {
            safe = filter_steps(ct, raw, true, false);
        }
        if (safe.empty()) {
            safe = filter_steps(ct, raw, false, false);
        }
        if (safe.empty()) {
            safe = raw;
        }

        Step pearl = best_pearl_step(ct, safe);
        if (pearl.dir >= 0 && !bad_bite(ct, pearl) && !scorer_skips_pearl(pearl)) {
            commit(ct, pearl.dir, "eat");
            return;
        }

        int kill = ram_dir(ct, false);
        if (kill >= 0 && !scorer_) {
            commit(ct, kill, "ram");
            return;
        }

        Step sprint = best_sprint(ct, safe, safe.empty());
        if (sprint.dir >= 0) {
            commit_sprint(ct, sprint);
            return;
        }

        bool threatened = nearest_enemy_dist() <= 2;
        bool pearl_here = pearl.dir >= 0;
        bool room = split_is_safe(ct, raw);
        bool scorer_lock = scorer_ || protect_grower();
        if (logic::want_split(length_, allies_, round_, pearl_here, threatened, scorer_lock, room, width_, height_) &&
            ct.can_split(2)) {
            ct.do_split(2);
            ct.set_indicator_string("split");
            remember(here_.x, here_.y);
            return;
        }

        int bfs_dir = bfs_dir_toward_goal(ct);
        if (bfs_dir >= 0 && step_is_safe(safe, bfs_dir) && !grower_walks_into_enemy(bfs_dir)) {
            commit(ct, bfs_dir, scorer_ ? "score" : "farm");
            return;
        }

        int roam = pick_roam(safe, length_ >= 8 || nearest_enemy_dist() <= 3);
        if (roam >= 0) {
            commit(ct, roam, scorer_ ? "score" : "farm");
            return;
        }

        if (sprint.dir < 0) {
            sprint = best_sprint(ct, safe, true);
        }
        if (sprint.dir >= 0) {
            commit_sprint(ct, sprint);
            return;
        }

        int ram = ram_dir(ct, true);
        if (ram >= 0) {
            commit(ct, ram, "ram");
            return;
        }
        int last = desperate_dir(ct);
        ct.make_move(from_index(last));
        ct.set_indicator_string("stuck");
    }

  private:
    int width_;
    int height_;
    int cols_ = 2;
    int rows_ = 2;
    int round_ = 1;
    int length_ = 2;
    int allies_ = 1;
    int my_id_ = 0;
    int facing_ = logic::DIR_N;
    unswbc::Position here_{};
    bool tail_known_ = false;
    bool tail_neck_ok_ = false;
    logic::Point tail_{};
    logic::Point tail_neck_{};
    std::vector<Cell> cells_;
    std::unordered_map<int, std::vector<logic::PortalSide>> portals_;
    std::vector<Head> enemy_heads_;
    std::vector<Head> ally_heads_;
    std::vector<int> sector_owner_;
    std::vector<int> sector_round_;
    std::vector<int> sector_visit_;
    logic::MapPolicy policy_{};
    bool scorer_ = false;
    bool known_pearl_ = false;
    bool outer_ready_ = false;
    int portal_pairs_ = 0;
    int hint_x_ = -1;
    int hint_y_ = -1;
    int hint_len_ = 0;
    int hint_round_ = -1;
    int scorer_hint_id_ = -1;
    int scorer_hint_len_ = 0;
    int scorer_hint_round_ = -100;
    int last_claim_x_ = -1;
    int last_claim_y_ = -1;
    int last_claim_round_ = -100;
    int history_[8] = {};
    int history_n_ = 0;

    Cell& at(int x, int y) {
        return cells_[static_cast<std::size_t>(y * width_ + x)];
    }

    Cell const& at(int x, int y) const {
        return cells_[static_cast<std::size_t>(y * width_ + x)];
    }

    int index_of(int x, int y) const {
        return y * width_ + x;
    }

    bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    static int dir_index(unswbc::Direction direction) {
        switch (direction.value) {
        case unswbc::Direction::NORTH:
            return logic::DIR_N;
        case unswbc::Direction::EAST:
            return logic::DIR_E;
        case unswbc::Direction::SOUTH:
            return logic::DIR_S;
        default:
            return logic::DIR_W;
        }
    }

    static unswbc::Direction from_index(int dir) {
        static unswbc::Direction::Value const values[] = {
            unswbc::Direction::NORTH,
            unswbc::Direction::EAST,
            unswbc::Direction::SOUTH,
            unswbc::Direction::WEST,
        };
        return unswbc::Direction(values[dir & 3]);
    }

    void remember_portal(int id, int x, int y, int dir) {
        std::vector<logic::PortalSide>& sides = portals_[id];
        for (logic::PortalSide const& side : sides) {
            if (side.x == x && side.y == y && side.dir == dir) {
                return;
            }
        }
        bool had_pair = sides.size() >= 2;
        if (sides.size() < 8) {
            logic::PortalSide side;
            side.x = x;
            side.y = y;
            side.dir = dir;
            sides.push_back(side);
        }
        if (!had_pair && sides.size() >= 2) {
            ++portal_pairs_;
        }
    }

    void observe(unswbc::Controller& ct) {
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            unswbc::Position position = tile.get_position();
            if (!inside(position.x, position.y)) {
                continue;
            }
            Cell& cell = at(position.x, position.y);
            cell.seen = true;
            cell.seen_round = round_;
            cell.pearl = tile.has_pearl();
            if (!cell.pearl) {
                cell.claimed_by = -1;
                cell.claim_round = -1;
            }
            int pearl_time = tile.get_pearl_time();
            cell.spawn_round = pearl_time < 0 ? -1 : round_ + pearl_time;

            for (int dir = 0; dir < 4; ++dir) {
                unswbc::Edge const& edge = tile.get_edge(from_index(dir));
                cell.edge_seen[dir] = true;
                cell.kelp[dir] = edge.get_edge_type() == unswbc::EdgeType::KELP;
                logic::Point next = logic::step_wrap(position.x, position.y, dir, width_, height_);
                Cell& other = at(next.x, next.y);
                other.kelp[logic::opposite_dir(dir)] = cell.kelp[dir];
                other.edge_seen[logic::opposite_dir(dir)] = true;
                int opposite = logic::opposite_dir(dir);
                if (edge.is_portal()) {
                    cell.portal[dir] = edge.get_portal_id();
                    other.portal[opposite] = edge.get_portal_id();
                    remember_portal(edge.get_portal_id(), position.x, position.y, dir);
                    remember_portal(edge.get_portal_id(), next.x, next.y, opposite);
                } else {
                    cell.portal[dir] = -1;
                    other.portal[opposite] = -1;
                }
            }

            unswbc::DragonPart const* part = tile.get_dragon();
            if (part == nullptr || !part->is_head() || part->get_id() == my_id_) {
                continue;
            }
            Head head;
            head.x = position.x;
            head.y = position.y;
            head.id = part->get_id();
            head.enemy = part->get_team() != ct.get_team();
            head.len = count_segments(ct, head.id);
            if (head.enemy) {
                enemy_heads_.push_back(head);
            } else {
                ally_heads_.push_back(head);
            }
        }
    }

    static int count_segments(unswbc::Controller& ct, int id) {
        int count = 0;
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            unswbc::DragonPart const* part = tile.get_dragon();
            if (part != nullptr && part->get_id() == id) {
                ++count;
            }
        }
        return count;
    }

    void hear(unswbc::Controller& ct) {
        for (std::uint64_t word : ct.get_sonar_messages()) {
            logic::SonarMsg message = logic::unpack_sonar(word);
            if (!message.ok) {
                continue;
            }
            int age = logic::sonar_age(round_, message.round_mod);
            if (age > 25) {
                continue;
            }
            if (message.kind == logic::KIND_SECTOR) {
                if (message.x >= 0 && message.y >= 0 && message.x < cols_ && message.y < rows_) {
                    int index = message.y * cols_ + message.x;
                    int when = round_ - age;
                    if (when >= sector_round_[static_cast<std::size_t>(index)]) {
                        sector_owner_[static_cast<std::size_t>(index)] = message.id;
                        sector_round_[static_cast<std::size_t>(index)] = when;
                    }
                }
                continue;
            }
            if (message.kind == logic::KIND_SCORER) {
                scorer_hint_id_ = message.id;
                scorer_hint_len_ = message.aux;
                scorer_hint_round_ = round_ - age;
                continue;
            }
            if (!inside(message.x, message.y)) {
                continue;
            }
            if (message.kind == logic::KIND_PEARL || message.kind == logic::KIND_CLAIM) {
                Cell& cell = at(message.x, message.y);
                if (cell.seen_round == round_ && !cell.pearl) {
                    continue;
                }
                if (cell.seen_round != round_) {
                    cell.pearl = true;
                }
                if (message.kind == logic::KIND_CLAIM) {
                    int when = round_ - age;
                    if (cell.claim_round < 0 || when >= cell.claim_round) {
                        cell.claimed_by = message.id;
                        cell.claim_round = when;
                    }
                }
            } else if (message.kind == logic::KIND_ENEMY && age <= 15) {
                hint_x_ = message.x;
                hint_y_ = message.y;
                hint_len_ = message.aux;
                hint_round_ = round_ - age;
            }
        }
    }

    bool sonar_duty() const {
        int index = logic::sector_index(here_.x, here_.y, width_, height_, cols_, rows_);
        int allies = allies_ < 1 ? 1 : allies_;
        return (my_id_ % allies) == (index % allies);
    }

    void speak(unswbc::Controller& ct) {
        if (policy_.macro == logic::Macro::Snowball) {
            speak_pearls(ct);
            return;
        }
        if (scorer_) {
            for (int dir = 0; dir < 4; ++dir) {
                send(ct, dir, logic::KIND_SCORER, here_.x, here_.y, length_);
            }
        }
        if (!enemy_heads_.empty() && (sonar_duty() || nearest_enemy_dist() <= 4 || scorer_)) {
            Head const& head = enemy_heads_.front();
            send(ct, 0, logic::KIND_ENEMY, head.x, head.y, head.len);
        } else if (hint_x_ >= 0 && round_ - hint_round_ <= 15 && sonar_duty()) {
            send(ct, 0, logic::KIND_ENEMY, hint_x_, hint_y_, hint_len_);
        }
        if (scorer_ || !sonar_duty()) {
            return;
        }
        int sx = here_.x * cols_ / width_;
        int sy = here_.y * rows_ / height_;
        if (sx >= cols_) {
            sx = cols_ - 1;
        }
        if (sy >= rows_) {
            sy = rows_ - 1;
        }
        if ((round_ & 7) == (my_id_ & 7)) {
            send(ct, 1, logic::KIND_SECTOR, sx, sy, my_id_ & 255);
        }
        int px = -1;
        int py = -1;
        int best = 1000000;
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            if (!tile.has_pearl()) {
                continue;
            }
            unswbc::Position position = tile.get_position();
            Cell const& cell = at(position.x, position.y);
            bool foreign = logic::claim_live(cell.claim_round, round_, false) && cell.claimed_by >= 0 &&
                           cell.claimed_by != my_id_ && round_ - cell.claim_round < 20;
            if (foreign) {
                continue;
            }
            int dist = logic::chebyshev(here_.x, here_.y, position.x, position.y, width_, height_);
            if (dist < best) {
                best = dist;
                px = position.x;
                py = position.y;
            }
        }
        if (px < 0) {
            return;
        }
        if (px == last_claim_x_ && py == last_claim_y_ && round_ - last_claim_round_ < 8) {
            return;
        }
        send(ct, facing_, logic::KIND_CLAIM, px, py, length_ & 255);
        send(ct, logic::right_of(facing_), logic::KIND_CLAIM, px, py, length_ & 255);
        last_claim_x_ = px;
        last_claim_y_ = py;
        last_claim_round_ = round_;
    }

    void speak_pearls(unswbc::Controller& ct) {
        int px = -1;
        int py = -1;
        int best = 1000000;
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            if (!tile.has_pearl()) {
                continue;
            }
            unswbc::Position position = tile.get_position();
            int dist = logic::chebyshev(here_.x, here_.y, position.x, position.y, width_, height_);
            if (dist < best) {
                best = dist;
                px = position.x;
                py = position.y;
            }
        }
        for (int dir = 0; dir < 4; ++dir) {
            if (!enemy_heads_.empty() && dir == 3) {
                Head const& head = enemy_heads_.front();
                send(ct, dir, logic::KIND_ENEMY, head.x, head.y, head.len);
            } else if (px >= 0) {
                send(ct, dir, logic::KIND_PEARL, px, py, 0);
            } else if (hint_x_ >= 0 && round_ - hint_round_ <= 15) {
                send(ct, dir, logic::KIND_ENEMY, hint_x_, hint_y_, hint_len_);
            }
        }
    }

    void send(unswbc::Controller& ct, int dir, int kind, int x, int y, int aux) {
        if (x > 255 || y > 255) {
            return;
        }
        ct.send_sonar(from_index(dir), logic::pack_sonar(kind, x, y, aux, my_id_, round_));
    }

    std::vector<logic::Point> trace_body(unswbc::Controller& ct) const {
        std::vector<logic::Point> body;
        logic::Point head;
        head.x = here_.x;
        head.y = here_.y;
        head.ok = true;
        body.push_back(head);
        logic::Point cursor = logic::step_wrap(here_.x, here_.y, logic::opposite_dir(facing_), width_, height_);
        for (int i = 1; i < length_; ++i) {
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(cursor.x, cursor.y));
            if (tile == nullptr || tile->get_dragon() == nullptr) {
                break;
            }
            unswbc::DragonPart const* part = tile->get_dragon();
            if (part->get_id() != my_id_ || part->is_head()) {
                break;
            }
            body.push_back(cursor);
            cursor = logic::step_wrap(cursor.x, cursor.y, logic::opposite_dir(dir_index(part->get_dir())), width_, height_);
        }
        return body;
    }

    // Strict for real moves: unseen edges are walls. Optimistic for BFS so we
    // can path through fog toward remembered pearls (edge_blocked returns false
    // when the edge has never been seen).
    logic::Point destination(int x, int y, int dir) {
        return destination(x, y, dir, true);
    }

    logic::Point destination(int x, int y, int dir, bool strict) {
        Cell& cell = at(x, y);
        bool blocked_edge = strict ? (!cell.edge_seen[dir] || cell.kelp[dir])
                                   : logic::edge_blocked(cell.edge_seen[dir], cell.kelp[dir]);
        if (blocked_edge) {
            logic::Point blocked;
            blocked.ok = false;
            blocked.x = x;
            blocked.y = y;
            return blocked;
        }
        if (cell.portal[dir] >= 0) {
            std::vector<logic::PortalSide>& sides = portals_[cell.portal[dir]];
            return logic::emerge(x, y, dir, width_, height_, sides.data(), static_cast<int>(sides.size()));
        }
        return logic::step_wrap(x, y, dir, width_, height_);
    }

    bool occupied(unswbc::Controller& ct, logic::Point point) const {
        unswbc::Tile const* tile = ct.get_tile(unswbc::Position(point.x, point.y));
        return tile != nullptr && tile->get_dragon() != nullptr;
    }

    bool has_pearl_at(unswbc::Controller& ct, logic::Point point) const {
        unswbc::Tile const* tile = ct.get_tile(unswbc::Position(point.x, point.y));
        return tile != nullptr && tile->has_pearl();
    }

    std::vector<Step> safe_steps(unswbc::Controller& ct, int x, int y, int facing, bool from_head) {
        std::vector<Step> steps;
        for (int dir = 0; dir < 4; ++dir) {
            if (from_head && dir == logic::opposite_dir(facing)) {
                continue;
            }
            if (!from_head && dir == logic::opposite_dir(facing)) {
                continue;
            }
            Cell const& cell = at(x, y);
            if (cell.portal[dir] >= 0) {
                if (!policy_.portal_bfs && policy_.macro != logic::Macro::MidContest) {
                    continue;
                }
                logic::Point exit = destination(x, y, dir);
                if (!exit.ok) {
                    continue;
                }
                unswbc::Tile const* tile = ct.get_tile(unswbc::Position(exit.x, exit.y));
                if (tile == nullptr || tile->get_dragon() != nullptr) {
                    continue;
                }
                if (portal_exit_risky(exit)) {
                    continue;
                }
                Step step;
                step.dir = dir;
                step.dest = exit;
                step.pearl = tile->has_pearl();
                steps.push_back(step);
                continue;
            }
            logic::Point next = destination(x, y, dir);
            if (!next.ok) {
                continue;
            }
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(next.x, next.y));
            if (tile == nullptr || tile->get_dragon() != nullptr) {
                continue;
            }
            Step step;
            step.dir = dir;
            step.dest = next;
            step.pearl = tile->has_pearl();
            steps.push_back(step);
        }
        return steps;
    }

    bool yield_pearl(int x, int y) const {
        for (Head const& ally : ally_heads_) {
            if (ally.id <= my_id_ || ally.len <= length_) {
                continue;
            }
            if (logic::chebyshev(ally.x, ally.y, x, y, width_, height_) <= 1) {
                return true;
            }
        }
        return false;
    }

    bool near_enemy_head(int x, int y) const {
        int radius = logic::avoid_radius(length_, scorer_);
        for (Head const& head : enemy_heads_) {
            int dist = logic::chebyshev(x, y, head.x, head.y, width_, height_);
            if (dist < 1 || dist > radius) {
                continue;
            }
            if (logic::avoid_enemy_head(length_, head.len, allies_)) {
                return true;
            }
        }
        return false;
    }

    bool portal_exit_risky(logic::Point exit) const {
        for (Head const& head : enemy_heads_) {
            if (logic::chebyshev(exit.x, exit.y, head.x, head.y, width_, height_) <= 1) {
                return true;
            }
        }
        return false;
    }

    bool scorer_skips_pearl(Step const& step) const {
        if (!scorer_ || !step.pearl) {
            return false;
        }
        return enemy_dist_to(step.dest.x, step.dest.y) <= 1;
    }

    bool compute_scorer() const {
        if (policy_.scorer_min_length <= 0 || round_ < 80 || length_ < policy_.scorer_min_length) {
            return false;
        }
        logic::AllySnap snaps[32];
        int count = 0;
        snaps[count].id = my_id_;
        snaps[count].length = length_;
        ++count;
        for (Head const& ally : ally_heads_) {
            if (count >= 32) {
                break;
            }
            snaps[count].id = ally.id;
            snaps[count].length = ally.len;
            ++count;
        }
        if (scorer_hint_id_ >= 0 && scorer_hint_id_ != my_id_ && scorer_hint_len_ > 0 &&
            round_ - scorer_hint_round_ <= 25 && count < 32) {
            snaps[count].id = scorer_hint_id_;
            snaps[count].length = scorer_hint_len_;
            ++count;
        }
        return logic::designated_scorer(snaps, count, round_, policy_.scorer_min_length) == my_id_;
    }

    bool protect_grower() const {
        if (policy_.macro == logic::Macro::Snowball || round_ < 80 || allies_ < 6) {
            return false;
        }
        for (Head const& ally : ally_heads_) {
            if (ally.len > length_) {
                return false;
            }
            if (ally.len == length_ && ally.id < my_id_) {
                return false;
            }
        }
        return true;
    }

    void own_sector(int x, int y) {
        int index = logic::sector_index(x, y, width_, height_, cols_, rows_);
        sector_owner_[static_cast<std::size_t>(index)] = my_id_;
        sector_round_[static_cast<std::size_t>(index)] = round_;
        sector_visit_[static_cast<std::size_t>(index)] = round_;
    }

    void refresh_book() {
        known_pearl_ = false;
        int outer_seen = 0;
        int outer_total = 0;
        int cells = width_ * height_;
        for (int i = 0; i < cells; ++i) {
            Cell& cell = cells_[static_cast<std::size_t>(i)];
            int x = i % width_;
            int y = i / width_;
            bool empty = cell.seen && !cell.pearl;
            if (cell.claim_round >= 0 && !logic::claim_live(cell.claim_round, round_, empty)) {
                cell.claimed_by = -1;
                cell.claim_round = -1;
                if (!cell.seen) {
                    cell.pearl = false;
                }
            }
            if (cell.pearl) {
                known_pearl_ = true;
            }
            if (policy_.outer_first && logic::sector_is_outer(x, y, width_, height_, cols_, rows_)) {
                ++outer_total;
                if (cell.seen) {
                    ++outer_seen;
                }
            }
        }
        outer_ready_ = outer_total > 0 && outer_seen * 2 >= outer_total;
    }

    std::vector<Step> filter_steps(unswbc::Controller& ct, std::vector<Step> const& raw, bool avoid_heads, bool avoid_corridors) {
        std::vector<Step> out;
        for (Step const& step : raw) {
            if (avoid_heads && near_enemy_head(step.dest.x, step.dest.y)) {
                continue;
            }
            int exits = future_exits(ct, step);
            if (exits < 1) {
                continue;
            }
            int threat = enemy_dist_to(step.dest.x, step.dest.y);
            if (policy_.macro == logic::Macro::Snowball && threat > 1) {
                threat = 100;
            }
            if (avoid_corridors && logic::corridor_refuse(exits, threat, step.pearl)) {
                continue;
            }
            if (step.pearl && bad_bite(ct, step)) {
                continue;
            }
            out.push_back(step);
        }
        return out;
    }

    Step best_pearl_step(unswbc::Controller&, std::vector<Step> const& safe) const {
        Step best;
        best.dir = -1;
        int best_dist = 1000000;
        for (Step const& step : safe) {
            if (!step.pearl || yield_pearl(step.dest.x, step.dest.y)) {
                continue;
            }
            int dist = logic::chebyshev(here_.x, here_.y, step.dest.x, step.dest.y, width_, height_);
            if (dist < best_dist) {
                best_dist = dist;
                best = step;
            }
        }
        return best;
    }

    bool contested(unswbc::Controller& ct, logic::Point pearl) const {
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            unswbc::DragonPart const* part = tile.get_dragon();
            if (part == nullptr || part->get_team() == ct.get_team()) {
                continue;
            }
            unswbc::Position position = tile.get_position();
            if (logic::chebyshev(position.x, position.y, pearl.x, pearl.y, width_, height_) <= 2) {
                return true;
            }
        }
        return false;
    }

    int future_exits(unswbc::Controller& ct, Step const& step) {
        int exits = 0;
        for (int dir = 0; dir < 4; ++dir) {
            if (dir == logic::opposite_dir(step.dir)) {
                continue;
            }
            logic::Point next = destination(step.dest.x, step.dest.y, dir);
            if (!next.ok || at(step.dest.x, step.dest.y).portal[dir] >= 0) {
                continue;
            }
            bool tail_leaves = tail_known_ && !step.pearl && next.x == tail_.x && next.y == tail_.y;
            if (occupied(ct, next) && !tail_leaves) {
                continue;
            }
            ++exits;
        }
        return exits;
    }

    // A one-exit landing is a lane when that exit itself has a way onward.
    // It is a pocket when the next tile is closed. Long bodies may eat in the lane.
    bool bad_bite(unswbc::Controller& ct, Step const& step) {
        int exits = future_exits(ct, step);
        if (exits < 1) {
            return true;
        }
        if (policy_.macro != logic::Macro::Snowball) {
            return logic::long_body_refuse_eat(exits, length_);
        }
        if (length_ < 8 || exits >= 2) {
            return false;
        }
        return !lane_continues(ct, step);
    }

    bool lane_continues(unswbc::Controller& ct, Step const& step) {
        int open_dir = -1;
        for (int dir = 0; dir < 4; ++dir) {
            if (dir == logic::opposite_dir(step.dir)) {
                continue;
            }
            logic::Point next = destination(step.dest.x, step.dest.y, dir);
            if (!next.ok || at(step.dest.x, step.dest.y).portal[dir] >= 0) {
                continue;
            }
            bool tail_leaves = tail_known_ && !step.pearl && next.x == tail_.x && next.y == tail_.y;
            if (occupied(ct, next) && !tail_leaves) {
                continue;
            }
            if (open_dir >= 0) {
                return true;
            }
            open_dir = dir;
        }
        if (open_dir < 0) {
            return false;
        }
        logic::Point next = destination(step.dest.x, step.dest.y, open_dir);
        for (int dir = 0; dir < 4; ++dir) {
            if (dir == logic::opposite_dir(open_dir)) {
                continue;
            }
            logic::Point beyond = destination(next.x, next.y, dir);
            if (!beyond.ok || at(next.x, next.y).portal[dir] >= 0) {
                continue;
            }
            if (occupied(ct, beyond)) {
                continue;
            }
            return true;
        }
        return false;
    }

    Step best_sprint(unswbc::Controller& ct, std::vector<Step> const& first, bool escape) {
        Step best;
        best.dir = -1;
        int best_score = -1000000000;
        if (length_ < 3) {
            return best;
        }
        int macro = static_cast<int>(policy_.macro);
        Cell const& start = at(here_.x, here_.y);
        for (Step const& step : first) {
            bool portal_first = start.portal[step.dir] >= 0;
            if (portal_first && !escape) {
                continue;
            }
            if (portal_first && portal_exit_risky(step.dest)) {
                continue;
            }
            bool ate = step.pearl;
            logic::Point freed;
            freed.ok = tail_known_ && !ate;
            if (freed.ok) {
                freed = tail_;
            }
            for (int dir2 = 0; dir2 < 4; ++dir2) {
                if (dir2 == logic::opposite_dir(step.dir)) {
                    continue;
                }
                Cell const& mid = at(step.dest.x, step.dest.y);
                if (mid.portal[dir2] >= 0) {
                    continue;
                }
                if (!logic::sprint_edges_open(start.edge_seen[step.dir], start.kelp[step.dir], mid.edge_seen[dir2],
                                              mid.kelp[dir2])) {
                    continue;
                }
                logic::Point second = destination(step.dest.x, step.dest.y, dir2);
                if (!second.ok || (second.x == step.dest.x && second.y == step.dest.y)) {
                    continue;
                }
                bool freed_here = freed.ok && second.x == freed.x && second.y == freed.y;
                unswbc::Tile const* tile = ct.get_tile(unswbc::Position(second.x, second.y));
                if (tile == nullptr) {
                    continue;
                }
                if (tile->get_dragon() != nullptr && !freed_here) {
                    continue;
                }
                if (!escape && near_enemy_head(second.x, second.y)) {
                    continue;
                }
                int pearls = (ate ? 1 : 0) + (tile->has_pearl() ? 1 : 0);
                if (!escape && length_ < 4 && pearls < 2) {
                    continue;
                }
                // Same dead-end rule as one-step eats: never land on a pearl pocket.
                Step landing;
                landing.dir = dir2;
                landing.dest = second;
                landing.pearl = tile->has_pearl();
                if (pearls > 0 && bad_bite(ct, landing)) {
                    continue;
                }
                if (pearls == 0 && future_exits(ct, landing) < 1 && policy_.macro != logic::Macro::Snowball) {
                    continue;
                }
                bool fight = pearls >= 1 && contested(ct, tile->has_pearl() ? second : step.dest);
                if (!logic::sprint_allowed(macro, pearls, fight, escape, length_)) {
                    continue;
                }
                int score = logic::plan_score(pearls, 2, 2, 0, fight || pearls >= 1, escape);
                if (pearls == 0 && !escape) {
                    continue;
                }
                if (score > best_score) {
                    best_score = score;
                    best.dir = step.dir;
                    best.dest.x = dir2;
                    best.dest.y = pearls;
                    best.pearl = pearls > 0;
                }
            }
        }
        return best;
    }

    void commit_sprint(unswbc::Controller& ct, Step const& sprint) {
        std::vector<unswbc::Direction> dirs;
        dirs.push_back(from_index(sprint.dir));
        dirs.push_back(from_index(sprint.dest.x));
        ct.make_moves(dirs);
        ct.set_indicator_string(sprint.pearl ? "sprint" : "escape");
        logic::Point end = destination(here_.x, here_.y, sprint.dir, true);
        if (!end.ok) {
            return;
        }
        logic::Point land = destination(end.x, end.y, sprint.dest.x, true);
        if (land.ok) {
            remember(land.x, land.y);
        }
    }

    int enemy_dist_to(int x, int y) const {
        int best = 1000000;
        for (Head const& head : enemy_heads_) {
            int dist = logic::chebyshev(x, y, head.x, head.y, width_, height_);
            if (dist < best) {
                best = dist;
            }
        }
        return best;
    }

    int nearest_enemy_dist() const {
        return enemy_dist_to(here_.x, here_.y);
    }

    bool peer_nearby() const {
        for (Head const& head : enemy_heads_) {
            if (logic::peer_threat(length_, head.len) &&
                logic::chebyshev(here_.x, here_.y, head.x, head.y, width_, height_) <= 4) {
                return true;
            }
        }
        return false;
    }

    bool grower_walks_into_enemy(int dir) const {
        if (length_ < 8 || enemy_heads_.empty()) {
            return false;
        }
        logic::Point next = logic::step_wrap(here_.x, here_.y, dir, width_, height_);
        int now = nearest_enemy_dist();
        if (now > 4) {
            return false;
        }
        int after = enemy_dist_to(next.x, next.y);
        if (scorer_ && after > logic::avoid_radius(length_, true)) {
            return false;
        }
        if (peer_nearby() && after <= now) {
            return true;
        }
        return after < now;
    }

    static int open_edges(Cell const& cell) {
        int count = 0;
        for (int dir = 0; dir < 4; ++dir) {
            if (cell.edge_seen[dir] && !cell.kelp[dir]) {
                ++count;
            }
        }
        return count;
    }

    int goal_value(int x, int y, int dist) const {
        if (dist == 0) {
            return -1;
        }
        Cell const& cell = cells_[static_cast<std::size_t>(index_of(x, y))];
        int noise = (x * 3 + y * 5 + my_id_) & 7;
        bool snowball = policy_.macro == logic::Macro::Snowball;
        bool foreign = logic::claim_live(cell.claim_round, round_, false) && cell.claimed_by >= 0 && cell.claimed_by != my_id_;
        bool mine = logic::claim_live(cell.claim_round, round_, false) && cell.claimed_by == my_id_;
        if (!snowball && cell.pearl && foreign && !scorer_) {
            return 250000 - dist * 10 + noise;
        }
        if (cell.pearl) {
            int score = 1000000 - dist * 10 + noise;
            if (mine && scorer_) {
                score += 200000;
            }
            if (scorer_) {
                score += open_edges(cell) >= 2 ? 4000 : -30000;
            }
            if (foreign) {
                score = 300000 - dist * 10;
            }
            return score;
        }
        if (cell.seen && cell.spawn_round > round_ && cell.spawn_round <= round_ + policy_.pearl_horizon) {
            int wait = cell.spawn_round - round_;
            int score = 500000 - wait * 400 - dist * 10 + noise;
            if (!scorer_ && !snowball) {
                score += 80000;
            }
            return score;
        }
        if (!scorer_ && length_ <= 6) {
            for (Head const& head : enemy_heads_) {
                if (!logic::worth_trade(length_, head.len, allies_, snowball)) {
                    continue;
                }
                if (logic::chebyshev(x, y, head.x, head.y, width_, height_) == 1) {
                    return 700000 - dist * 10;
                }
            }
            if (hint_x_ == x && hint_y_ == y && hint_len_ >= 8 && round_ - hint_round_ <= 15 &&
                logic::worth_trade(length_, hint_len_, allies_, snowball)) {
                return 450000 - dist * 10;
            }
        }
        if (scorer_ && known_pearl_) {
            return -1;
        }
        int score = -1;
        if (!cell.seen) {
            score = 40000 - dist * 8 + noise;
        } else if (round_ - cell.seen_round > 20) {
            score = 15000 - dist * 8 + noise;
        } else {
            return -1;
        }
        if (!scorer_ && policy_.sector_prefer > 0 && !sector_owner_.empty()) {
            int index = logic::sector_index(x, y, width_, height_, cols_, rows_);
            int owner = sector_owner_[static_cast<std::size_t>(index)];
            if (owner < 0) {
                score += 25000;
            } else if (owner == my_id_) {
                score += 8000;
            } else {
                score -= 12000;
            }
            if (round_ - sector_visit_[static_cast<std::size_t>(index)] < 25) {
                score -= policy_.sector_prefer == 2 ? 15000 : 5000;
            }
            for (Head const& ally : ally_heads_) {
                if (logic::sector_index(ally.x, ally.y, width_, height_, cols_, rows_) == index) {
                    score -= 18000;
                }
            }
        }
        if (policy_.outer_first && !outer_ready_) {
            if (logic::sector_is_outer(x, y, width_, height_, cols_, rows_)) {
                score += 22000;
            } else {
                score -= 22000;
            }
        }
        return score;
    }

    int bfs_dir_toward_goal(unswbc::Controller& ct) {
        int cells = width_ * height_;
        std::vector<int> dist(static_cast<std::size_t>(cells), -1);
        std::vector<int> first(static_cast<std::size_t>(cells), -1);
        std::vector<char> blocked(static_cast<std::size_t>(cells), 0);
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            unswbc::DragonPart const* part = tile.get_dragon();
            if (part == nullptr) {
                continue;
            }
            unswbc::Position position = tile.get_position();
            if (inside(position.x, position.y)) {
                blocked[static_cast<std::size_t>(index_of(position.x, position.y))] = 1;
            }
        }
        int origin = index_of(here_.x, here_.y);
        blocked[static_cast<std::size_t>(origin)] = 0;
        dist[static_cast<std::size_t>(origin)] = 0;

        std::vector<int> queue;
        queue.push_back(origin);
        int best_value = -1;
        int best_index = -1;
        int order[4] = {facing_, logic::right_of(facing_), logic::left_of(facing_), logic::opposite_dir(facing_)};

        for (std::size_t q = 0; q < queue.size(); ++q) {
            int current = queue[q];
            int x = current % width_;
            int y = current / width_;
            int current_dist = dist[static_cast<std::size_t>(current)];
            for (int k = 0; k < 4; ++k) {
                int dir = current_dist == 0 ? order[k] : ((k + my_id_) & 3);
                if (current_dist == 0 && dir == logic::opposite_dir(facing_)) {
                    continue;
                }
                // First step is a real move (strict). Later steps may cross fog.
                if (at(x, y).portal[dir] >= 0 && !policy_.portal_bfs) {
                    continue;
                }
                logic::Point next = destination(x, y, dir, current_dist == 0);
                if (!next.ok) {
                    continue;
                }
                if (at(x, y).portal[dir] >= 0) {
                    if (portal_exit_risky(next)) {
                        continue;
                    }
                    if (current_dist == 0 && ct.get_tile(unswbc::Position(next.x, next.y)) == nullptr) {
                        continue;
                    }
                }
                int next_index = index_of(next.x, next.y);
                if (blocked[static_cast<std::size_t>(next_index)] || dist[static_cast<std::size_t>(next_index)] >= 0) {
                    continue;
                }
                dist[static_cast<std::size_t>(next_index)] = current_dist + 1;
                first[static_cast<std::size_t>(next_index)] = current_dist == 0 ? dir : first[static_cast<std::size_t>(current)];
                int value = goal_value(next.x, next.y, current_dist + 1);
                if (value > best_value) {
                    best_value = value;
                    best_index = next_index;
                }
                queue.push_back(next_index);
            }
        }
        if (best_index < 0) {
            return -1;
        }
        ct.draw_indicator_dot(unswbc::Position(best_index % width_, best_index / width_), 80, 220, 255);
        return first[static_cast<std::size_t>(best_index)];
    }

    static bool step_is_safe(std::vector<Step> const& safe, int dir) {
        for (Step const& step : safe) {
            if (step.dir == dir) {
                return true;
            }
        }
        return false;
    }

    bool recently(int x, int y) const {
        int key = y * width_ + x;
        int count = history_n_ < 6 ? history_n_ : 6;
        for (int i = 0; i < count; ++i) {
            int slot = (history_n_ - 1 - i) & 7;
            if (history_[slot] == key) {
                return true;
            }
        }
        return false;
    }

    void remember(int x, int y) {
        history_[history_n_ & 7] = y * width_ + x;
        ++history_n_;
        if (inside(x, y)) {
            own_sector(x, y);
        }
    }

    int openness(int x, int y) {
        int score = 0;
        for (int dir = 0; dir < 4; ++dir) {
            logic::Point next = destination(x, y, dir);
            if (next.ok) {
                ++score;
            }
        }
        return score;
    }

    int pick_roam(std::vector<Step> const& safe, bool flee) {
        int best_dir = -1;
        int best_score = -1000000000;
        for (Step const& step : safe) {
            int score = openness(step.dest.x, step.dest.y) * 20;
            if (step.dir == facing_) {
                score += 30;
            }
            if (step.dir == (my_id_ & 3)) {
                score += 10;
            }
            if (recently(step.dest.x, step.dest.y)) {
                score -= 200;
            }
            if (!at(step.dest.x, step.dest.y).seen) {
                score += 80;
            }
            if (policy_.outer_first && !outer_ready_) {
                if (logic::sector_is_outer(step.dest.x, step.dest.y, width_, height_, cols_, rows_)) {
                    score += 50;
                } else {
                    score -= 50;
                }
            }
            if (!scorer_ && policy_.sector_prefer > 0 && !sector_visit_.empty()) {
                int index = logic::sector_index(step.dest.x, step.dest.y, width_, height_, cols_, rows_);
                if (round_ - sector_visit_[static_cast<std::size_t>(index)] < 25) {
                    score -= 80;
                }
            }
            if (flee) {
                for (Head const& head : enemy_heads_) {
                    score += logic::chebyshev(step.dest.x, step.dest.y, head.x, head.y, width_, height_) * 40;
                }
            }
            int wait = at(step.dest.x, step.dest.y).spawn_round - round_;
            if (wait >= 0 && wait <= 8) {
                score += 250 - wait * 20;
            }
            if (score > best_score) {
                best_score = score;
                best_dir = step.dir;
            }
        }
        return best_dir;
    }

    int dir_between(logic::Point from, logic::Point to) const {
        for (int dir = 0; dir < 4; ++dir) {
            logic::Point step = logic::step_wrap(from.x, from.y, dir, width_, height_);
            if (step.x == to.x && step.y == to.y) {
                return dir;
            }
        }
        return -1;
    }

    bool split_is_safe(unswbc::Controller& ct, std::vector<Step> const& head_exits) {
        if (static_cast<int>(head_exits.size()) < 3) {
            return false;
        }
        // Without a traced tail we cannot count the child's neighbours, so skip.
        if (!tail_known_ || !tail_neck_ok_) {
            return false;
        }
        int parent = dir_between(tail_, tail_neck_);
        int open = 0;
        for (int dir = 0; dir < 4; ++dir) {
            if (dir == parent) {
                continue;
            }
            logic::Point next = destination(tail_.x, tail_.y, dir, true);
            if (!next.ok) {
                continue;
            }
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(next.x, next.y));
            if (tile == nullptr || tile->get_dragon() != nullptr) {
                continue;
            }
            ++open;
        }
        return logic::child_has_room(open);
    }

    int ram_dir(unswbc::Controller& ct, bool trapped) {
        int best_dir = -1;
        int best_len = -1;
        for (int dir = 0; dir < 4; ++dir) {
            if (dir == logic::opposite_dir(facing_)) {
                continue;
            }
            logic::Point next = destination(here_.x, here_.y, dir);
            if (!next.ok) {
                continue;
            }
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(next.x, next.y));
            if (tile == nullptr || tile->get_dragon() == nullptr || !tile->get_dragon()->is_head()) {
                continue;
            }
            unswbc::DragonPart const* part = tile->get_dragon();
            if (part->get_team() == ct.get_team()) {
                continue;
            }
            int enemy_len = count_segments(ct, part->get_id());
            bool snowball = policy_.macro == logic::Macro::Snowball;
            bool take = trapped ? logic::trapped_ram(length_, enemy_len, allies_)
                                : logic::worth_trade(length_, enemy_len, allies_, snowball);
            if (take && enemy_len > best_len) {
                best_len = enemy_len;
                best_dir = dir;
            }
        }
        return best_dir;
    }

    // Die alone rather than erase a teammate. A wall, our own body, or an ally body
    // kills only us. An ally head kills both.
    int desperate_dir(unswbc::Controller& ct) {
        int wall = -1;
        int self_body = -1;
        int ally_body = -1;
        int ally_head = -1;
        for (int dir = 0; dir < 4; ++dir) {
            if (dir == logic::opposite_dir(facing_)) {
                continue;
            }
            logic::Point next = destination(here_.x, here_.y, dir);
            if (!next.ok) {
                wall = dir;
                continue;
            }
            unswbc::Tile const* tile = ct.get_tile(unswbc::Position(next.x, next.y));
            unswbc::DragonPart const* part = tile == nullptr ? nullptr : tile->get_dragon();
            if (part == nullptr) {
                return dir;
            }
            if (part->get_id() == my_id_) {
                self_body = dir;
            } else if (part->get_team() == ct.get_team() && part->is_head()) {
                ally_head = dir;
            } else if (part->get_team() == ct.get_team()) {
                ally_body = dir;
            } else if (part->is_head()) {
                return dir;
            } else {
                self_body = dir;
            }
        }
        if (wall >= 0) {
            return wall;
        }
        if (self_body >= 0) {
            return self_body;
        }
        if (ally_body >= 0) {
            return ally_body;
        }
        if (ally_head >= 0) {
            return ally_head;
        }
        return facing_;
    }

    void commit(unswbc::Controller& ct, int dir, char const* label) {
        ct.make_move(from_index(dir));
        ct.set_indicator_string(label);
        logic::Point next = destination(here_.x, here_.y, dir);
        if (next.ok) {
            remember(next.x, next.y);
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
