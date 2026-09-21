#include "evnet/candidates.hpp"

#include <algorithm>

namespace evnet {
namespace {

/// Below this, an amount of energy is not worth stopping for.
constexpr Resource kNegligibleAmount = 0.01;

}  // namespace

std::vector<Candidate> buildCandidates(const Network& network,
                                       const Router& router,
                                       const WaitOracle& oracle,
                                       const AgentState& agent,
                                       const FeasibilityConfig& config) {
    std::vector<Candidate> candidates;
    const Domain& domain = network.domain();

    const Km distanceToDestination = router.distance(agent.at, agent.destination);
    if (distanceToDestination == Router::kUnreachable) return candidates;

    const Km rangeAfterFullCharge =
        domain.distanceOnResource(agent.capacity * (1.0 - config.reserveFraction), agent.consumption);

    // The agent's own node is a legitimate option when it has a charger: a car
    // sitting at a charging station can obviously use it. `reachableWithin`
    // excludes the origin, so it must be added back explicitly.
    const Km range = domain.distanceOnResource(agent.level, agent.consumption);
    std::vector<NodeId> options = router.reachableWithin(agent.at, range);
    if (network.node(agent.at).hasStation()) options.push_back(agent.at);

    for (const NodeId candidateNode : options) {
        const Node& node = network.node(candidateNode);
        if (!node.hasStation()) continue;

        const bool servicingInPlace = candidateNode == agent.at;
        const Km detour = servicingInPlace ? 0.0 : router.distance(agent.at, candidateNode);
        const Km remainingAfter = router.distance(candidateNode, agent.destination);
        if (remainingAfter == Router::kUnreachable) continue;

        // Guard 1: progress.
        if (!servicingInPlace && remainingAfter >= distanceToDestination) continue;

        // Guard 2: onward feasibility.
        bool onwardOk = remainingAfter <= rangeAfterFullCharge;
        if (!onwardOk) {
            for (const NodeId onward : router.reachableWithin(candidateNode, rangeAfterFullCharge)) {
                if (!network.node(onward).hasStation()) continue;
                if (router.distance(onward, agent.destination) < remainingAfter) {
                    onwardOk = true;
                    break;
                }
            }
        }
        if (!onwardOk) continue;

        const Resource levelOnArrival = agent.level - domain.resourceForDistance(detour, agent.consumption);
        if (levelOnArrival < 0.0) continue;  // defensive; reachableWithin should prevent this

        const Resource neededToFinish = domain.resourceForDistance(remainingAfter, agent.consumption) +
                                   agent.capacity * config.reserveFraction;
        const Resource wanted = std::min(agent.capacity, neededToFinish);
        // Not worth stopping for is judged on what the agent wants, not on what the
        // station would give it: a driver with hours in hand does not stop for a
        // full reset merely because a reset is on offer.
        if (wanted - levelOnArrival <= kNegligibleAmount) continue;
        const Resource target = domain.levelAfterService(levelOnArrival, wanted, agent.capacity);
        const Resource amount = target - levelOnArrival;

        Candidate candidate;
        candidate.node = candidateNode;
        candidate.detourKm = detour;
        candidate.progressKm = distanceToDestination - remainingAfter;
        candidate.amount = amount;
        candidate.levelAfter = target;
        candidate.travelCost = detour * config.travelCostPerKm;
        candidate.energyCost = amount * node.station->pricePerUnit;
        // The wait is estimated for when the agent would actually ARRIVE, not
        // for the moment the decision is taken. With a timeless oracle this makes
        // no difference; with a clock it is the difference between a useful
        // estimate and a stale one.
        const Hours arrivalTime = agent.now + drivingTime(detour, config.speedKmh);
        candidate.waitHours = oracle.expectedWait(candidateNode, arrivalTime);
        if (candidate.waitHours == kNoAdmission) continue;  // full, and nowhere to wait
        candidate.serviceHours = oracle.serviceTime(candidateNode, amount);
        candidates.push_back(candidate);
    }

    return candidates;
}

std::vector<Candidate> buildTopUpCandidates(const Network& network,
                                            const Router& router,
                                            const WaitOracle& oracle,
                                            const AgentState& agent,
                                            Resource requiredAmount,
                                            const FeasibilityConfig& config) {
    std::vector<Candidate> candidates;
    const Domain& domain = network.domain();

    const Km range = domain.distanceOnResource(agent.level, agent.consumption);
    std::vector<NodeId> options = router.reachableWithin(agent.at, range);
    if (network.node(agent.at).hasStation()) options.push_back(agent.at);

    for (const NodeId candidateNode : options) {
        const Node& node = network.node(candidateNode);
        if (!node.hasStation()) continue;

        const Km distance =
            candidateNode == agent.at ? 0.0 : router.distance(agent.at, candidateNode);
        const Resource outbound = domain.resourceForDistance(distance, agent.consumption);
        const Resource levelOnArrival = agent.level - outbound;
        if (levelOnArrival < 0.0) continue;

        // The agent charges on arrival, so the return leg is funded by the
        // top-up. It still has to have enough left to get home.
        const Resource afterCharging = domain.levelAfterService(
            levelOnArrival, std::min(agent.capacity, levelOnArrival + requiredAmount), agent.capacity);
        if (afterCharging < outbound) continue;

        const Resource delivered = afterCharging - levelOnArrival;
        if (delivered <= kNegligibleAmount) continue;

        Candidate candidate;
        candidate.node = candidateNode;
        candidate.detourKm = distance;
        // A top-up has no destination to progress toward, so "farthest" is
        // meaningless. Negating the detour makes that policy degenerate to
        // "nearest station", which is exactly the naive baseline the legacy Sydney
        // project used for this question.
        candidate.progressKm = -distance;
        candidate.amount = delivered;
        candidate.levelAfter = afterCharging;
        candidate.travelCost = 2.0 * distance * config.travelCostPerKm;  // round trip
        candidate.energyCost = delivered * node.station->pricePerUnit;
        const Hours arrivalTime = agent.now + drivingTime(distance, config.speedKmh);
        candidate.waitHours = oracle.expectedWait(candidateNode, arrivalTime);
        if (candidate.waitHours == kNoAdmission) continue;  // full, and nowhere to wait
        candidate.serviceHours = oracle.serviceTime(candidateNode, delivered);
        candidates.push_back(candidate);
    }

    return candidates;
}

}  // namespace evnet
