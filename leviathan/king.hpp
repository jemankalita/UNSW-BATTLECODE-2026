#pragma once

#include "logic.hpp"

#include <cstdint>

// Which ally is the team's king, as far as this dragon knows. News comes
// from our own body, allies in view, and KING sonar messages that allies
// relay every turn, so the whole team settles on the same dragon.
namespace king {

// Rounds a report stays usable once nobody has refreshed it.
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

    void hear(std::uint64_t word, int now) {
        logic::SonarMsg msg = logic::unpack_sonar(word);
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

    std::uint64_t message() const {
        int len = best_.len > 255 ? 255 : best_.len;
        return logic::pack_sonar(logic::KIND_KING, best_.x, best_.y, len, best_.id, best_.round);
    }

  private:
    Report best_;
};

}  // namespace king
