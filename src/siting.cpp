#include "evnet/siting.hpp"

#include <algorithm>

namespace evnet {

namespace {

/// The shared knobs, so switching engine changes the model and nothing else. Anything
/// only one engine has (a clock speed, a stop overhead, a stop cap) keeps its default.
SimulatorConfig timedFrom(const SimulationConfig& config) {
    SimulatorConfig out;
    out.travelCostPerKm = config.travelCostPerKm;
    out.valueOfTimePerHour = config.valueOfTimePerHour;
    out.reserveFraction = config.reserveFraction;
    return out;
}

SimulationConfig staticFrom(const SimulatorConfig& config) {
    SimulationConfig out;
    out.travelCostPerKm = config.travelCostPerKm;
    out.valueOfTimePerHour = config.valueOfTimePerHour;
    out.reserveFraction = config.reserveFraction;
    return out;
}

/// Best first: more completed trips beats lower cost, since a stranded vehicle is a
/// worse outcome than an expensive one -- and a mean cost is taken over the trips that
/// finished, so two runs with different completion counts are averaging over different
/// populations and their means are not directly comparable anyway.
bool betterSite(const SiteScore& a, const SiteScore& b) {
    if (a.completed != b.completed) return a.completed > b.completed;
    if (a.meanGeneralisedCost != b.meanGeneralisedCost) {
        return a.meanGeneralisedCost < b.meanGeneralisedCost;
    }
    return a.node < b.node;
}

}  // namespace

Siting::Siting(const Network& network, SimulationConfig config)
    : network_(&network), router_(network), config_(config), timedConfig_(timedFrom(config)) {}

Siting::Siting(const Network& network, SimulatorConfig config)
    : network_(&network), router_(network), config_(staticFrom(config)), timedConfig_(config) {}

Summary Siting::baseline(const std::vector<Demand>& demands, const Policy& policy) const {
    const Allocator allocator(*network_, router_, config_);
    StationState state(*network_);
    const auto results = allocator.run(demands, policy, state);
    return allocator.summarise(results, state, policy.name() + " (no new station)");
}

std::vector<SiteScore> Siting::rank(const std::vector<Demand>& demands,
                                    const Policy& policy,
                                    const Station& prototype,
                                    std::size_t topN) const {
    std::vector<SiteScore> scores;

    for (const NodeId candidate : network_->candidateSites()) {
        // Copy the network and install the hypothetical station. The COPY is of
        // the station set; the topology is identical, which is why `router_` --
        // built once in the constructor over the base network -- stays valid for
        // every candidate. This is the whole reason the search is O(candidates x
        // fleet) rather than a factor of V worse: the legacy implementation
        // re-ran Dijkstra in its innermost loop, allocating and freeing a fresh
        // distance array every iteration.
        Network hypothetical = *network_;
        hypothetical.setStation(candidate, prototype);

        const Allocator allocator(hypothetical, router_, config_);
        StationState state(hypothetical);
        const auto results = allocator.run(demands, policy, state);
        const Summary summary = allocator.summarise(results, state, policy.name());

        int usedNewSite = 0;
        for (const auto& result : results) {
            const bool used = std::any_of(result.stops.begin(), result.stops.end(),
                                          [candidate](const Stop& s) { return s.node == candidate; });
            if (used) ++usedNewSite;
        }

        SiteScore score;
        score.node = candidate;
        score.name = network_->node(candidate).name;
        score.meanGeneralisedCost = summary.meanGeneralisedCost;
        score.meanMoneyCost = summary.meanMoneyCost;
        score.meanWaitHours = summary.meanWaitHours;
        score.completed = summary.completed;
        score.peakQueue = summary.peakQueue;
        score.adoptionShare = demands.empty()
                                  ? 0.0
                                  : static_cast<double>(usedNewSite) / static_cast<double>(demands.size());
        scores.push_back(score);
    }

    std::sort(scores.begin(), scores.end(), betterSite);
    if (topN > 0 && scores.size() > topN) scores.resize(topN);
    return scores;
}

TimedSummary Siting::baselineTimed(const std::vector<Demand>& demands,
                                   const std::string& plannerName) const {
    const auto planner = makePlanner(plannerName, *network_, router_,
                                     timedConfig_.valueOfTimePerHour, timedConfig_.feasibility());
    const Simulator simulator(*network_, router_, timedConfig_);
    StationRuntime runtime(*network_);
    const auto trips = simulator.run(demands, *planner, runtime);
    return simulator.summarise(trips, runtime, planner->name() + " (no new station)");
}

std::vector<SiteScore> Siting::rankTimed(const std::vector<Demand>& demands,
                                         const std::string& plannerName,
                                         const Station& prototype,
                                         std::size_t topN) const {
    std::vector<SiteScore> scores;

    for (const NodeId candidate : network_->candidateSites()) {
        // Same trick as `rank`: the topology is untouched, so the Router built once over
        // the base network stays valid. Only the station set differs, which is why this
        // is O(candidates x fleet) and not a factor of V worse.
        Network hypothetical = *network_;
        hypothetical.setStation(candidate, prototype);

        // Built per candidate on purpose -- see the note on the declaration.
        const auto planner = makePlanner(plannerName, hypothetical, router_,
                                         timedConfig_.valueOfTimePerHour,
                                         timedConfig_.feasibility());
        const Simulator simulator(hypothetical, router_, timedConfig_);
        StationRuntime runtime(hypothetical);
        const auto trips = simulator.run(demands, *planner, runtime);
        const TimedSummary summary = simulator.summarise(trips, runtime, planner->name());

        int usedNewSite = 0;
        for (const auto& trip : trips) {
            const bool used = std::any_of(trip.stops.begin(), trip.stops.end(),
                                          [candidate](const Stop& s) { return s.node == candidate; });
            if (used) ++usedNewSite;
        }

        SiteScore score;
        score.node = candidate;
        score.name = network_->node(candidate).name;
        score.meanGeneralisedCost = summary.meanGeneralisedCost;
        score.meanMoneyCost = summary.meanMoneyCost;
        score.meanWaitHours = summary.meanWaitHours;
        score.completed = summary.completed;
        score.peakQueue = summary.peakWaiting;
        score.adoptionShare = demands.empty()
                                  ? 0.0
                                  : static_cast<double>(usedNewSite) / static_cast<double>(demands.size());
        scores.push_back(score);
    }

    std::sort(scores.begin(), scores.end(), betterSite);
    if (topN > 0 && scores.size() > topN) scores.resize(topN);
    return scores;
}

}  // namespace evnet
