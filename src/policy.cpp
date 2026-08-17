#include "evnet/policy.hpp"

#include <algorithm>
#include <stdexcept>

namespace evnet {
namespace {

/// Shared selection helper. Ties are broken by node id so that every policy is
/// deterministic -- important because the whole point of the toolkit is
/// comparing policies, and a comparison against a coin flip measures nothing.
template <typename Score>
const Candidate* best(const std::vector<Candidate>& candidates, Score score) {
    const Candidate* winner = nullptr;
    double bestScore = 0.0;
    for (const auto& candidate : candidates) {
        const double value = score(candidate);
        if (winner == nullptr || value < bestScore ||
            (value == bestScore && candidate.node < winner->node)) {
            winner = &candidate;
            bestScore = value;
        }
    }
    return winner;
}

}  // namespace

const Candidate* CheapestEnergyPolicy::choose(const std::vector<Candidate>& candidates) const {
    return best(candidates, [](const Candidate& c) { return c.moneyCost(); });
}

const Candidate* MinWaitPolicy::choose(const std::vector<Candidate>& candidates) const {
    // Queue time only. Charging duration is excluded deliberately: the legacy
    // corridor algorithm this reproduces compared queue lengths against charger
    // counts and knew nothing about charger power.
    return best(candidates, [](const Candidate& c) { return c.waitHours; });
}

const Candidate* FarthestReachablePolicy::choose(const std::vector<Candidate>& candidates) const {
    // Maximise progress toward the destination, hence the negation.
    return best(candidates, [](const Candidate& c) { return -c.progressKm; });
}

const Candidate* GeneralisedCostPolicy::choose(const std::vector<Candidate>& candidates) const {
    const Dollars valueOfTime = valueOfTime_;
    return best(candidates, [valueOfTime](const Candidate& c) {
        return c.generalisedCost(valueOfTime);
    });
}

std::unique_ptr<Policy> makePolicy(const std::string& name, Dollars valueOfTimePerHour) {
    if (name == "cheapest") return std::make_unique<CheapestEnergyPolicy>();
    if (name == "min-wait") return std::make_unique<MinWaitPolicy>();
    if (name == "farthest") return std::make_unique<FarthestReachablePolicy>();
    if (name == "generalised" || name == "generalized") {
        return std::make_unique<GeneralisedCostPolicy>(valueOfTimePerHour);
    }
    throw std::invalid_argument("unknown policy '" + name + "'");
}

std::vector<std::string> policyNames() {
    return {"farthest", "cheapest", "min-wait", "generalised"};
}

}  // namespace evnet
