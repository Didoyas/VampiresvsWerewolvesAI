#include "ngs_decision_engine.hpp"
#include "vvw/game/decision_engine_factory.hpp"

#include <memory>

namespace vvw::game {

std::unique_ptr<DecisionEngine> makeDecisionEngine(
    Species ourSpecies
) {
    if (ourSpecies == Species::Unknown) {
        return nullptr;
    }

    return std::make_unique<vvw::ngs::NgsDecisionEngine>(
        ourSpecies
    );
}

} // namespace vvw::game
