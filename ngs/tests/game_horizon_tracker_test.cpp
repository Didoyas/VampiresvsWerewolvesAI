#include "vvw/game/game_horizon_tracker.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using vvw::game::GameHorizon;
using vvw::game::GameHorizonTracker;

int main() {
    using Clock = GameHorizonTracker::Clock;
    const auto start = Clock::time_point{};

    // First mover: the first UPD is empty, then the opponent moves before
    // the next UPD received by this client.
    GameHorizonTracker firstMover;
    firstMover.observeUpdate(0, start);
    auto horizon = firstMover.snapshot(start);
    assert(horizon.known);
    assert(horizon.ourPliesUsed == 0);
    assert(horizon.opponentPliesUsed == 0);

    firstMover.recordOurMoveSent();
    firstMover.observeUpdate(2, start + std::chrono::seconds(3));
    horizon = firstMover.snapshot(start + std::chrono::seconds(3));
    assert(horizon.ourPliesUsed == 1);
    assert(horizon.opponentPliesUsed == 1);
    assert(horizon.elapsedSeconds == 3.0);

    // Second mover: the first UPD already includes the first player's move.
    GameHorizonTracker secondMover;
    secondMover.observeUpdate(2, start);
    horizon = secondMover.snapshot(start);
    assert(horizon.known);
    assert(horizon.ourPliesUsed == 0);
    assert(horizon.opponentPliesUsed == 1);

    secondMover.recordOurMoveSent();
    secondMover.observeUpdate(4, start + std::chrono::seconds(2));
    horizon = secondMover.snapshot(start + std::chrono::seconds(2));
    assert(horizon.ourPliesUsed == 1);
    assert(horizon.opponentPliesUsed == 2);
    assert(horizon.elapsedSeconds == 2.0);

    // Counter saturation must not overflow after the configured limit.
    for (int i = 0; i < 150; ++i) {
        secondMover.recordOurMoveSent();
    }
    horizon = secondMover.snapshot(start + std::chrono::seconds(2));
    assert(horizon.ourPliesUsed == GameHorizon::kMaxPliesPerSide);

    std::cout << "All game horizon tracker tests passed.\n";
    return 0;
}
