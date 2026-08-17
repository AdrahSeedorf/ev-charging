#include "evnet/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "evnet/candidates.hpp"
#include "evnet/csv.hpp"

namespace evnet {
namespace {

/// Slack for "do I have enough charge" comparisons. Energy figures are derived
/// through several multiplications and divisions, so a vehicle that mathematically
/// has exactly enough can land a fraction of a microjoule short. Without this
/// tolerance such a vehicle is declared stranded, which is a rounding artefact
/// rather than a transport outcome.
constexpr Kwh kEnergyEpsilon = 1e-6;

}  // namespace

std::vector<Demand> Demand::load(const std::string& csvPath) {
    const csv::Reader reader(csvPath);
    reader.requireColumns({"id", "origin_id", "destination_id", "battery_kwh", "soc_kwh",
                           "efficiency_kwh_per_100km", "required_kwh"});

    std::vector<Demand> demands;
    demands.reserve(reader.rows().size());
    for (const auto& row : reader.rows()) {
        Demand demand;
        demand.id = row.integer("id");
        demand.origin = row.integer("origin_id");
        demand.destination = row.integer("destination_id");
        demand.batteryKwh = row.number("battery_kwh");
        demand.socKwh = row.number("soc_kwh");
        demand.efficiency = row.number("efficiency_kwh_per_100km");
        demand.requiredKwh = row.number("required_kwh");
        // Optional: absent in stage 1 datasets, which then all release at t=0.
        if (row.has("release_hour")) demand.releaseHour = row.number("release_hour");

        if (demand.batteryKwh <= 0.0) {
            throw std::runtime_error("demand " + std::to_string(demand.id) + " has no battery capacity");
        }
        if (demand.efficiency <= 0.0) {
            throw std::runtime_error("demand " + std::to_string(demand.id) + " has non-positive efficiency");
        }
        if (demand.socKwh > demand.batteryKwh) {
            throw std::runtime_error("demand " + std::to_string(demand.id) +
                                     " starts with more charge than its battery holds");
        }
        demands.push_back(demand);
    }
    return demands;
}

Allocator::Allocator(const Network& network, const Router& router, SimulationConfig config)
    : network_(&network), router_(&router), config_(config) {}

std::vector<Candidate> Allocator::candidatesFor(NodeId at,
                                                NodeId destination,
                                                Kwh socKwh,
                                                Kwh batteryKwh,
                                                KwhPer100Km efficiency,
                                                const StationState& state) const {
    // Delegates to the shared builder in candidates.cpp. The feasibility rules --
    // both stranding guards and the energy arithmetic -- are subtle enough that
    // having two copies drifting apart would be the likeliest source of a silent
    // bug, so the static and event-driven engines share one implementation and
    // differ only in how they answer the congestion question.
    VehicleState vehicle;
    vehicle.at = at;
    vehicle.destination = destination;
    vehicle.socKwh = socKwh;
    vehicle.batteryKwh = batteryKwh;
    vehicle.efficiency = efficiency;
    vehicle.now = 0.0;  // this engine has no clock

    FeasibilityConfig feasibility;
    feasibility.travelCostPerKm = config_.travelCostPerKm;
    feasibility.reserveFraction = config_.reserveFraction;

    return buildCandidates(*network_, *router_, state, vehicle, feasibility);
}

TripResult Allocator::runJourney(const Demand& demand, const Policy& policy, StationState& state) const {
    TripResult result;
    result.demandId = demand.id;

    if (!network_->contains(demand.origin) || !network_->contains(demand.destination)) {
        result.failure = "origin or destination is not a node in this network";
        return result;
    }

    NodeId at = demand.origin;
    Kwh soc = demand.socKwh;

    for (int stop = 0; stop <= config_.maxStopsPerTrip; ++stop) {
        const Km remaining = router_->distance(at, demand.destination);
        if (remaining == Router::kUnreachable) {
            result.failure = "destination unreachable from " + network_->node(at).name;
            return result;
        }

        // Can we finish from here, keeping the reserve intact?
        const Kwh needed =
            energyForDistance(remaining, demand.efficiency) + demand.batteryKwh * config_.reserveFraction;
        if (soc + kEnergyEpsilon >= needed || remaining == 0.0) {
            result.distanceKm += remaining;
            result.travelCost = result.distanceKm * config_.travelCostPerKm;
            result.completed = true;
            return result;
        }

        const auto candidates = candidatesFor(at, demand.destination, soc, demand.batteryKwh,
                                              demand.efficiency, state);
        if (candidates.empty()) {
            result.failure = "stranded at " + network_->node(at).name +
                             ": no feasible charging stop within range";
            return result;
        }

        const Candidate* chosen = policy.choose(candidates);
        if (chosen == nullptr) {
            result.failure = "policy returned no choice";
            return result;
        }

        // Drive to the chosen station, then charge. The resulting state of charge
        // is taken from the candidate rather than recomputed here, so it is exact.
        result.distanceKm += chosen->detourKm;
        soc = std::min(demand.batteryKwh, chosen->socAfterCharge);

        state.enqueue(chosen->node);
        result.stops.push_back(Stop{chosen->node, chosen->energyKwh, chosen->energyCost,
                                   chosen->waitHours, chosen->chargeHours});
        result.energyCost += chosen->energyCost;
        result.waitHours += chosen->waitHours;
        result.chargeHours += chosen->chargeHours;
        at = chosen->node;
    }

    result.failure = "exceeded the maximum of " + std::to_string(config_.maxStopsPerTrip) +
                     " stops without reaching the destination";
    return result;
}

TripResult Allocator::runTopUp(const Demand& demand, const Policy& policy, StationState& state) const {
    TripResult result;
    result.demandId = demand.id;

    if (!network_->contains(demand.origin)) {
        result.failure = "origin is not a node in this network";
        return result;
    }

    VehicleState vehicle;
    vehicle.id = demand.id;
    vehicle.at = demand.origin;
    vehicle.destination = demand.origin;
    vehicle.socKwh = demand.socKwh;
    vehicle.batteryKwh = demand.batteryKwh;
    vehicle.efficiency = demand.efficiency;
    vehicle.now = 0.0;

    FeasibilityConfig feasibility;
    feasibility.travelCostPerKm = config_.travelCostPerKm;
    feasibility.reserveFraction = config_.reserveFraction;

    const auto candidates =
        buildTopUpCandidates(*network_, *router_, state, vehicle, demand.requiredKwh, feasibility);

    if (candidates.empty()) {
        result.failure = "no reachable charging station can serve a " +
                         std::to_string(static_cast<int>(demand.requiredKwh)) + " kWh top-up from " +
                         network_->node(demand.origin).name;
        return result;
    }

    const Candidate* chosen = policy.choose(candidates);
    if (chosen == nullptr) {
        result.failure = "policy returned no choice";
        return result;
    }

    state.enqueue(chosen->node);
    result.stops.push_back(Stop{chosen->node, chosen->energyKwh, chosen->energyCost,
                               chosen->waitHours, chosen->chargeHours});
    result.distanceKm = 2.0 * chosen->detourKm;
    result.travelCost = chosen->travelCost;
    result.energyCost = chosen->energyCost;
    result.waitHours = chosen->waitHours;
    result.chargeHours = chosen->chargeHours;
    result.completed = true;
    return result;
}

TripResult Allocator::runOne(const Demand& demand, const Policy& policy, StationState& state) const {
    return demand.isTopUp() ? runTopUp(demand, policy, state) : runJourney(demand, policy, state);
}

std::vector<TripResult> Allocator::run(const std::vector<Demand>& demands,
                                       const Policy& policy,
                                       StationState& state) const {
    std::vector<TripResult> results;
    results.reserve(demands.size());
    for (const auto& demand : demands) {
        results.push_back(runOne(demand, policy, state));
    }
    return results;
}

Summary Allocator::summarise(const std::vector<TripResult>& results,
                             const StationState& state,
                             const std::string& policyName) const {
    Summary summary;
    summary.policy = policyName;
    summary.demands = static_cast<int>(results.size());

    std::vector<Hours> waits;
    double totalStops = 0.0;
    for (const auto& result : results) {
        if (!result.completed) {
            ++summary.stranded;
            continue;
        }
        ++summary.completed;
        summary.meanMoneyCost += result.moneyCost();
        summary.meanGeneralisedCost += result.generalisedCost(config_.valueOfTimePerHour);
        summary.meanWaitHours += result.waitHours;
        totalStops += static_cast<double>(result.stops.size());
        waits.push_back(result.waitHours);
    }

    if (summary.completed > 0) {
        const auto n = static_cast<double>(summary.completed);
        summary.meanMoneyCost /= n;
        summary.meanGeneralisedCost /= n;
        summary.meanWaitHours /= n;
        summary.meanStops = totalStops / n;

        // 95th percentile, nearest-rank. Means hide exactly the cases that make
        // a congestion model worth building, so the tail is reported alongside.
        std::sort(waits.begin(), waits.end());
        const auto rank = static_cast<std::size_t>(std::ceil(0.95 * n));
        summary.p95WaitHours = waits[std::min(waits.size() - 1, rank == 0 ? 0 : rank - 1)];
    }

    const auto [peakNode, peakQueue] = state.peak();
    summary.peakQueue = peakQueue;
    summary.peakStation = peakNode == kNoNode ? "-" : network_->node(peakNode).name;
    return summary;
}

}  // namespace evnet
