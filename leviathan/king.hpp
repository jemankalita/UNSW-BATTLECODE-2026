#pragma once

#include "logic.hpp"

#include <cstdint>

// Which ally is the team's king, as far as this dragon knows. News comes
// from our own body, allies in view, and KING sonar messages that allies
// relay every turn, so the whole team settles on the same dragon.
namespace king {

// Rounds a report stays usable once nobody has refreshed it. 10 went 42-38
// against 20 on the ranked pool but 37-45 on unseen maps; 40 lost 32-48.
static constexpr int STALE = 20;

struct Report {
    bool valid = false;
    int id = 0;
    int x = 0;
    int y = 0;
    int len = 0;
    int round = 0;
};

class Tracker {
  public:
    // Offer one report; keep it if it names a better king, or newer news of
    // the same one.
    void offer(int id, int x, int y, int len, int round) {
        if (best_.valid && best_.id == id) {
            if (round >= best_.round) {
                // Allies in view may be partly outside it, so a sighting is
                // only a lower bound on length: keep the largest.
                best_.len = len > best_.len ? len : best_.len;
                best_.x = x;
                best_.y = y;
                best_.round = round;
            }
            return;
        }
        if (!best_.valid || logic::better_king(len, id, best_.len, best_.id)) {
            best_ = Report{true, id, x, y, len, round};
        }
    }

    // Each team signs with its own key, so a sonar that reaches the other
    // team's dragons (it stops at the first body of either team) is noise.
    void set_team(bool team_b) {
        key_ = team_b ? logic::SONAR_KEY ^ logic::SONAR_TEAM_B : logic::SONAR_KEY;
    }

    void hear(std::uint64_t word, int now) {
        logic::SonarMsg msg = logic::unpack_sonar(word, key_);
        if (!msg.ok || msg.kind != logic::KIND_KING) {
            return;
        }
        int age = logic::sonar_age(now, msg.round_mod);
        if (age > STALE) {
            return;
        }
        offer(msg.id, msg.x, msg.y, msg.aux, now - age);
    }

    // A report nobody has refreshed for a while is dropped: that king has
    // probably died.
    void expire(int now) {
        if (best_.valid && now - best_.round > STALE) {
            best_.valid = false;
        }
    }

    void forget() { best_.valid = false; }

    Report const& best() const { return best_; }

    std::uint64_t key() const { return key_; }

    std::uint64_t message() const {
        int len = best_.len > 255 ? 255 : best_.len;
        return logic::pack_sonar(logic::KIND_KING, best_.x, best_.y, len, best_.id, best_.round, key_);
    }

  private:
    Report best_;
    std::uint64_t key_ = logic::SONAR_KEY;
};

}  // namespace king
