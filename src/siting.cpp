#include "evnet/siting.hpp"

#include <algorithm>

namespace evnet {

Siting::Siting(const Network& network, SimulationConfig config)
    : network_(&network), router_(network), config_(config) {}

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

    // Best first: more completed trips beats lower cost, since a stranded
    // vehicle is a worse outcome than an expensive one.
    std::sort(scores.begin(), scores.end(), [](const SiteScore& a, const SiteScore& b) {
        if (a.completed != b.completed) return a.completed > b.completed;
        if (a.meanGeneralisedCost != b.meanGeneralisedCost) {
            return a.meanGeneralisedCost < b.meanGeneralisedCost;
        }
        return a.node < b.node;
    });

    if (topN > 0 && scores.size() > topN) scores.resize(topN);
    return scores;
}

}  // namespace evnet
