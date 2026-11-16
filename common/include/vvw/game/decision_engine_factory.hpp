#pragma once

#include "vvw/game/decision_engine.hpp"

#include <memory>

namespace vvw::game {

std::unique_ptr<DecisionEngine> makeDecisionEngine(Species ourSpecies);

} // namespace vvw::game