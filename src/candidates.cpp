#include "evnet/candidates.hpp"

#include <algorithm>

namespace evnet {
namespace {

/// Below this, an amount of energy is not worth stopping for.
constexpr Kwh kNegligibleKwh = 0.01;

}  // namespace

std::vector<Candidate> buildCandidates(const Network& network,
                                       const Router& router,
                                       const WaitOracle& oracle,
                                       const VehicleState& vehicle,
                                       const FeasibilityConfig& config) {
    std::vector<Candidate> candidates;

    const Km distanceToDestination = router.distance(vehicle.at, vehicle.destination);
    if (distanceToDestination == Router::kUnreachable) return candidates;

    const Km rangeAfterFullCharge =
        rangeFromEnergy(vehicle.batteryKwh * (1.0 - config.reserveFraction), vehicle.efficiency);

    // The vehicle's own node is a legitimate option when it has a charger: a car
    // sitting at a charging station can obviously use it. `reachableWithin`
    // excludes the origin, so it must be added back explicitly.
    std::vector<NodeId> options = router.reachableWithin(vehicle.at, vehicle.rangeKm());
    if (network.node(vehicle.at).hasStation()) options.push_back(vehicle.at);

    for (const NodeId candidateNode : options) {
        const Node& node = network.node(candidateNode);
        if (!node.hasStation()) continue;

        const bool chargingInPlace = candidateNode == vehicle.at;
        const Km detour = chargingInPlace ? 0.0 : router.distance(vehicle.at, candidateNode);
        const Km remainingAfter = router.distance(candidateNode, vehicle.destination);
        if (remainingAfter == Router::kUnreachable) continue;

        // Guard 1: progress.
        if (!chargingInPlace && remainingAfter >= distanceToDestination) continue;

        // Guard 2: onward feasibility.
        bool onwardOk = remainingAfter <= rangeAfterFullCharge;
        if (!onwardOk) {
            for (const NodeId onward : router.reachableWithin(candidateNode, rangeAfterFullCharge)) {
                if (!network.node(onward).hasStation()) continue;
                if (router.distance(onward, vehicle.destination) < remainingAfter) {
                    onwardOk = true;
                    break;
                }
            }
        }
        if (!onwardOk) continue;

        const Kwh socOnArrival = vehicle.socKwh - energyForDistance(detour, vehicle.efficiency);
        if (socOnArrival < 0.0) continue;  // defensive; reachableWithin should prevent this

        const Kwh energyToFinish = energyForDistance(remainingAfter, vehicle.efficiency) +
                                   vehicle.batteryKwh * config.reserveFraction;
        const Kwh target = std::min(vehicle.batteryKwh, energyToFinish);
        const Kwh energy = target - socOnArrival;
        if (energy <= kNegligibleKwh) continue;

        Candidate candidate;
        candidate.node = candidateNode;
        candidate.detourKm = detour;
        candidate.progressKm = distanceToDestination - remainingAfter;
        candidate.energyKwh = energy;
        candidate.socAfterCharge = target;
        candidate.travelCost = detour * config.travelCostPerKm;
        candidate.energyCost = energy * node.station->pricePerKwh;
        // The wait is estimated for when the vehicle would actually ARRIVE, not
        // for the moment the decision is taken. With a timeless oracle this makes
        // no difference; with a clock it is the difference between a useful
        // estimate and a stale one.
        const Hours arrivalTime = vehicle.now + drivingTime(detour, config.speedKmh);
        candidate.waitHours = oracle.expectedWait(candidateNode, arrivalTime);
        candidate.chargeHours = oracle.chargeTime(candidateNode, energy);
        candidates.push_back(candidate);
    }

    return candidates;
}

std::vector<Candidate> buildTopUpCandidates(const Network& network,
                                            const Router& router,
                                            const WaitOracle& oracle,
                                            const VehicleState& vehicle,
                                            Kwh requiredKwh,
                                            const FeasibilityConfig& config) {
    std::vector<Candidate> candidates;

    std::vector<NodeId> options = router.reachableWithin(vehicle.at, vehicle.rangeKm());
    if (network.node(vehicle.at).hasStation()) options.push_back(vehicle.at);

    for (const NodeId candidateNode : options) {
        const Node& node = network.node(candidateNode);
        if (!node.hasStation()) continue;

        const Km distance =
            candidateNode == vehicle.at ? 0.0 : router.distance(vehicle.at, candidateNode);
        const Kwh outbound = energyForDistance(distance, vehicle.efficiency);
        const Kwh socOnArrival = vehicle.socKwh - outbound;
        if (socOnArrival < 0.0) continue;

        // The vehicle charges on arrival, so the return leg is funded by the
        // top-up. It still has to have enough left to get home.
        const Kwh afterCharging = std::min(vehicle.batteryKwh, socOnArrival + requiredKwh);
        if (afterCharging < outbound) continue;

        const Kwh delivered = afterCharging - socOnArrival;
        if (delivered <= kNegligibleKwh) continue;

        Candidate candidate;
        candidate.node = candidateNode;
        candidate.detourKm = distance;
        // A top-up has no destination to progress toward, so "farthest" is
        // meaningless. Negating the detour makes that policy degenerate to
        // "nearest station", which is exactly the naive baseline the legacy Sydney
        // project used for this question.
        candidate.progressKm = -distance;
        candidate.energyKwh = delivered;
        candidate.socAfterCharge = afterCharging;
        candidate.travelCost = 2.0 * distance * config.travelCostPerKm;  // round trip
        candidate.energyCost = delivered * node.station->pricePerKwh;
        const Hours arrivalTime = vehicle.now + drivingTime(distance, config.speedKmh);
        candidate.waitHours = oracle.expectedWait(candidateNode, arrivalTime);
        candidate.chargeHours = oracle.chargeTime(candidateNode, delivered);
        candidates.push_back(candidate);
    }

    return candidates;
}

}  // namespace evnet
