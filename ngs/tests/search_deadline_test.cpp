#include "../src/agent.hpp"
#include "../src/candidate_generator.hpp"
#include "../src/candidate_ranker.hpp"
#include "../src/multi_move_candidate_generator.hpp"
#include "../src/opponent_search.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

using namespace vvw::ngs;

namespace {

BoardState makeBoard() {
    BoardState board;
    board.rows = 5;
    board.columns = 5;
    board.cells.resize(25);
    board.ourSpecies = Species::Vampires;
    board.previousPly.known = true;
    board.at({1, 1}).vampires = 20;
    return board;
}

TurnPlan makeLegalPlan() {
    TurnPlan plan;
    plan.moves.push_back({{1, 1}, 10, {2, 1}});
    return plan;
}

void testSingleMoveGenerationStopsAtExpiredDeadline() {
    const BoardState board = makeBoard();
    const SearchDeadline deadline =
        SearchClock::now() - std::chrono::milliseconds(1);

    const CandidateGenerationResult result =
        generateSingleMoveCandidates(
            board, Species::Vampires, CandidateGenerationOptions{}, deadline);

    assert(result.valid());
    assert(result.timedOut);
    assert(result.candidates.empty());
}

void testMultiMoveGenerationStopsAtExpiredDeadline() {
    const BoardState board = makeBoard();
    const SearchDeadline deadline =
        SearchClock::now() - std::chrono::milliseconds(1);

    const MultiMoveCandidateResult result =
        generateMultiMoveCandidates(
            board, Species::Vampires, MultiMoveCandidateOptions{}, deadline);

    assert(result.valid());
    assert(result.timedOut);
    assert(result.candidates.empty());
}

void testCandidateRankingStopsAtExpiredDeadline() {
    const BoardState board = makeBoard();
    const std::vector<TurnPlan> candidates{makeLegalPlan()};
    const SearchDeadline deadline =
        SearchClock::now() - std::chrono::milliseconds(1);

    const CandidateRankingResult result =
        rankTurnCandidates(
            board, candidates, Species::Vampires,
            CandidateRankingOptions{}, deadline);

    assert(result.timedOut);
    assert(result.ranked.empty());
}

void testOpponentSearchStopsAtExpiredDeadline() {
    const BoardState board = makeBoard();
    const std::vector<TurnPlan> candidates{makeLegalPlan()};
    const SearchDeadline deadline =
        SearchClock::now() - std::chrono::milliseconds(1);

    const OpponentSearchResult result =
        searchTurnCandidates(
            board, candidates, Species::Vampires,
            OpponentSearchOptions{}, deadline);

    assert(result.timedOut);
    assert(!result.hasBestCandidate);
}

void testAgentReturnsLegalFallbackWhenBudgetIsExhausted() {
    const BoardState board = makeBoard();

    AgentOptions options;
    options.searchBudget = std::chrono::milliseconds(0);

    Agent agent(Species::Vampires, options);
    const AgentDecision decision = agent.chooseMove(board);

    assert(decision.deadlineExceeded);
    assert(decision.hasMove());
    assert(decision.method == AgentDecisionMethod::EmergencyFallback);
    assert(validateTurnPlan(
        board, decision.plan, Species::Vampires,
        ValidationOptions{true}).valid());
}

} // namespace

int main() {
    testSingleMoveGenerationStopsAtExpiredDeadline();
    testMultiMoveGenerationStopsAtExpiredDeadline();
    testCandidateRankingStopsAtExpiredDeadline();
    testOpponentSearchStopsAtExpiredDeadline();
    testAgentReturnsLegalFallbackWhenBudgetIsExhausted();

    std::cout << "All search deadline tests passed.\n";
    return 0;
}
