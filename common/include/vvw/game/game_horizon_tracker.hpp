#pragma once

#include "types.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace vvw::game {

// Converts the observed alternating TCP protocol into a per-side game horizon.
// The server does not send ply IDs. Its observed behavior is:
// - a first mover receives an empty first UPD before either side has moved;
// - a second mover's first UPD contains the first player's opening movement;
// - each later UPD for this client follows one opponent movement.
// Therefore only the first UPD's changed-cell count is special; subsequent
// UPDs each advance the opponent counter by one. This is based on the verified
// server trace, not an attempt to infer individual opponent movement records.
// The server does not transmit its game-start timestamp, so elapsed time starts
// at this client's first UPD. For the second mover that slightly undercounts
// the opening ply's duration rather than guessing an unobserved timestamp.
class GameHorizonTracker {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void observeUpdate(std::size_t changedCellCount, TimePoint receivedAt) noexcept {
        if (!hasReceivedUpdate_) {
            hasReceivedUpdate_ = true;
            horizon_.known = true;
            gameStartedAt_ = receivedAt;
            if (changedCellCount > 0) {
                increment(horizon_.opponentPliesUsed);
            }
            return;
        }

        increment(horizon_.opponentPliesUsed);
        updateElapsed(receivedAt);
    }

    void recordOurMoveSent() noexcept {
        if (horizon_.known) {
            increment(horizon_.ourPliesUsed);
        }
    }

    GameHorizon snapshot(TimePoint now) const noexcept {
        GameHorizon result = horizon_;
        if (hasReceivedUpdate_) {
            const double elapsed =
                std::chrono::duration<double>(now - gameStartedAt_).count();
            result.elapsedSeconds = std::max(0.0, elapsed);
        }
        return result;
    }

private:
    GameHorizon horizon_{};
    bool hasReceivedUpdate_{false};
    TimePoint gameStartedAt_{};

    static void increment(std::uint16_t& value) noexcept {
        if (value < GameHorizon::kMaxPliesPerSide) {
            ++value;
        }
    }

    void updateElapsed(TimePoint now) noexcept {
        const double elapsed =
            std::chrono::duration<double>(now - gameStartedAt_).count();
        horizon_.elapsedSeconds = std::max(0.0, elapsed);
    }
};

} // namespace vvw::game
