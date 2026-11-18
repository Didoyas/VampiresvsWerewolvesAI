#pragma once

#include <chrono>

namespace vvw::ngs {

using SearchClock = std::chrono::steady_clock;
using SearchDeadline = SearchClock::time_point;

// All search stages share one absolute deadline. Passing max() preserves the
// original unbounded behaviour for callers that do not supply a deadline.
inline bool deadlineReached(SearchDeadline deadline) noexcept {
    return SearchClock::now() >= deadline;
}

} // namespace vvw::ngs
