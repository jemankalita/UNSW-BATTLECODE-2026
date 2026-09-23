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
        : width_(width), height_(height), cells_(static_cast<std::size_t>(width * height)) {}

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
        speak(ct);

        std::vector<logic::Point> body = trace_body(ct);
        tail_known_ = static_cast<int>(body.size()) == length_ && length_ >= 2;
        if (tail_known_) {
            tail_ = body.back();
        }

        int kill = ram_dir(ct, false);
        if (kill >= 0) {
            commit(ct, kill, "ram");
            return;
        }

        std::vector<Step> safe = safe_steps(ct, here_.x, here_.y, facing_, true);
        int here_open = future_exits(ct, Step{facing_, logic::Point{here_.x, here_.y, true}, true});
        std::vector<Step> roomy;
        for (Step const& step : safe) {
            int exits = future_exits(ct, step);
            if (exits >= 2 || (exits >= 1 && here_open <= 1)) {
                roomy.push_back(step);
            }
        }
        if (roomy.empty()) {
            for (Step const& step : safe) {
                if (future_exits(ct, step) >= 1) {
                    roomy.push_back(step);
                }
            }
        }
        if (!roomy.empty()) {
            safe = std::move(roomy);
        }
        Step pearl = best_pearl_step(safe);
        if (pearl.dir >= 0) {
            commit(ct, pearl.dir, "eat");
            return;
        }

        Step sprint = best_sprint(ct, safe, safe.empty());
        if (sprint.dir >= 0) {
            commit_sprint(ct, sprint);
            return;
        }

        bool threatened = nearest_enemy_dist() <= 2;
        if (logic::want_split(length_, allies_, width_, height_, round_, false, threatened) &&
            ct.can_split(2)) {
            ct.do_split(2);
            ct.set_indicator_string("split");
            remember(here_.x, here_.y);
            return;
        }

        int bfs_dir = bfs_dir_toward_goal(ct);
        if (bfs_dir >= 0 && step_is_safe(safe, bfs_dir) && !grower_walks_into_enemy(bfs_dir)) {
            commit(ct, bfs_dir, "go");
            return;
        }

        int roam = pick_roam(safe, length_ >= 8);
        if (roam >= 0) {
            commit(ct, roam, "roam");
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
    int round_ = 1;
    int length_ = 2;
    int allies_ = 1;
    int my_id_ = 0;
    int facing_ = logic::DIR_N;
    unswbc::Position here_{};
    bool tail_known_ = false;
    logic::Point tail_{};
    std::vector<Cell> cells_;
    std::unordered_map<int, std::vector<logic::PortalSide>> portals_;
    std::vector<Head> enemy_heads_;
    std::vector<Head> ally_heads_;
    int hint_x_ = -1;
    int hint_y_ = -1;
    int hint_len_ = 0;
    int hint_round_ = -1;
    int history_[8] = {};
    int history_n_ = 0;

    Cell& at(int x, int y) {
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
        if (sides.size() < 8) {
            logic::PortalSide side;
            side.x = x;
            side.y = y;
            side.dir = dir;
            sides.push_back(side);
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
            if (!message.ok || !inside(message.x, message.y)) {
                continue;
            }
            int age = logic::sonar_age(round_, message.round_mod);
            if (age > 25) {
                continue;
            }
            if (message.kind == logic::KIND_PEARL) {
                Cell& cell = at(message.x, message.y);
                if (cell.seen_round != round_) {
                    cell.pearl = true;
                }
            } else if (message.kind == logic::KIND_ENEMY && age <= 15) {
                hint_x_ = message.x;
                hint_y_ = message.y;
                hint_len_ = message.aux;
                hint_round_ = round_ - age;
            }
        }
    }

    void speak(unswbc::Controller& ct) {
        std::array<int, 4> sent = {};
        int slot = 0;
        for (unswbc::Tile const& tile : ct.get_tiles()) {
            if (slot >= 3 || !tile.has_pearl()) {
                continue;
            }
            unswbc::Position position = tile.get_position();
            send(ct, slot, logic::KIND_PEARL, position.x, position.y, 0);
            sent[slot] = 1;
            ++slot;
        }
        if (!enemy_heads_.empty()) {
            Head const& head = enemy_heads_.front();
            send(ct, 3, logic::KIND_ENEMY, head.x, head.y, head.len);
            sent[3] = 1;
        } else if (hint_x_ >= 0 && round_ - hint_round_ <= 15) {
            send(ct, 3, logic::KIND_ENEMY, hint_x_, hint_y_, hint_len_);
        }
        (void)sent;
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

    logic::Point destination(int x, int y, int dir) {
        Cell& cell = at(x, y);
        // Unseen edges stay closed so a path cannot run through kelp we have not looked at.
        if (!cell.edge_seen[dir] || cell.kelp[dir]) {
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
                logic::Point exit = destination(x, y, dir);
                if (!exit.ok) {
                    continue;
                }
                unswbc::Tile const* tile = ct.get_tile(unswbc::Position(exit.x, exit.y));
                if (tile == nullptr || tile->get_dragon() != nullptr) {
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

    Step best_pearl_step(std::vector<Step> const& safe) const {
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

    Step best_sprint(unswbc::Controller& ct, std::vector<Step> const& first, bool escape) {
        Step best;
        best.dir = -1;
        int best_score = -1000000000;
        if (length_ < 3) {
            return best;
        }
        for (Step const& step : first) {
            if (at(here_.x, here_.y).portal[step.dir] >= 0) {
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
                if (at(step.dest.x, step.dest.y).portal[dir2] >= 0) {
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
                int pearls = (ate ? 1 : 0) + (tile->has_pearl() ? 1 : 0);
                if (!escape && length_ < 4 && pearls < 2) {
                    continue;
                }
                bool fight = pearls == 1 && contested(ct, second);
                int score = logic::plan_score(pearls, 2, 2, 0, fight, escape);
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
        logic::Point end = logic::step_wrap(here_.x, here_.y, sprint.dir, width_, height_);
        end = logic::step_wrap(end.x, end.y, sprint.dest.x, width_, height_);
        remember(end.x, end.y);
    }

    int nearest_enemy_dist() const {
        int best = 1000000;
        for (Head const& head : enemy_heads_) {
            int dist = logic::chebyshev(here_.x, here_.y, head.x, head.y, width_, height_);
            if (dist < best) {
                best = dist;
            }
        }
        return best;
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
        int after = 1000000;
        for (Head const& head : enemy_heads_) {
            int dist = logic::chebyshev(next.x, next.y, head.x, head.y, width_, height_);
            if (dist < after) {
                after = dist;
            }
        }
        return after < now;
    }

    int goal_value(int x, int y, int dist) const {
        if (dist == 0) {
            return -1;
        }
        Cell const& cell = cells_[static_cast<std::size_t>(index_of(x, y))];
        int noise = (x * 3 + y * 5 + my_id_) & 7;
        if (cell.pearl) {
            return 1000000 - dist * 10 + noise;
        }
        if (cell.seen && cell.spawn_round > round_ && cell.spawn_round <= round_ + 12) {
            int wait = cell.spawn_round - round_;
            return 500000 - wait * 400 - dist * 10 + noise;
        }
        if (length_ <= 6) {
            for (Head const& head : enemy_heads_) {
                if (!logic::worth_trade(length_, head.len, allies_)) {
                    continue;
                }
                if (logic::chebyshev(x, y, head.x, head.y, width_, height_) == 1) {
                    return 700000 - dist * 10;
                }
            }
            if (hint_x_ == x && hint_y_ == y && hint_len_ >= 8 && round_ - hint_round_ <= 15 &&
                logic::worth_trade(length_, hint_len_, allies_)) {
                return 450000 - dist * 10;
            }
        }
        if (!cell.seen) {
            return 10000 - dist * 10 + noise;
        }
        return -1;
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
                logic::Point next = destination(x, y, dir);
                if (next.ok && current_dist == 0) {
                    Cell const& next_cell = at(next.x, next.y);
                    if (!next_cell.pearl && next_cell.spawn_round > round_) {
                        continue;
                    }
                }
                if (!next.ok) {
                    continue;
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
            if (flee) {
                for (Head const& head : enemy_heads_) {
                    score += logic::chebyshev(step.dest.x, step.dest.y, head.x, head.y, width_, height_) * 40;
                }
            }
            int wait = at(step.dest.x, step.dest.y).spawn_round - round_;
            if (wait > 0 && wait <= 8) {
                score -= 300;
            }
            if (score > best_score) {
                best_score = score;
                best_dir = step.dir;
            }
        }
        return best_dir;
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
            bool take = trapped ? logic::trapped_ram(length_, enemy_len, allies_)
                                : logic::worth_trade(length_, enemy_len, allies_);
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
