#pragma once

#include <string>
#include <vector>

#include "evnet/network.hpp"
#include "evnet/policy.hpp"
#include "evnet/router.hpp"
#include "evnet/simulation.hpp"

namespace evnet {

struct SiteScore {
    NodeId node{kNoNode};
    std::string name;
    Dollars meanGeneralisedCost{0.0};
    Dollars meanMoneyCost{0.0};
    Hours meanWaitHours{0.0};
    int completed{0};
    int peakQueue{0};
    double adoptionShare{0.0};  ///< fraction of vehicles that actually use the new site
};

/// Where should the next charging station go?
///
/// The legacy Sydney implementation answered this by averaging distance-and-price
/// cost over a demand set while ignoring congestion entirely -- so it would
/// happily nominate a site that immediately became a bottleneck. Here each
/// candidate is scored by re-running the whole fleet with a station actually
/// placed there, so the recommendation accounts for the queue it creates. That
/// feedback loop (a new station attracts demand, which creates congestion, which
/// changes routing, which changes the station's value) is the substance of the
/// merged model.
///
/// Cost note: this is inherently O(candidates x fleet). The legacy version was
/// accidentally a factor of V worse, because it re-ran Dijkstra inside the
/// innermost loop. Here the Router is built once against the base topology and
/// shared across every candidate -- valid because placing a charger changes the
/// station set, never the road network. See the invariant on Router's
/// constructor.
class Siting {
public:
    Siting(const Network& network, SimulationConfig config = {});

    /// Ranked best-first. `prototype` is the station to hypothetically install
    /// (price, charger count, power). `topN == 0` returns every candidate.
    std::vector<SiteScore> rank(const std::vector<Demand>& demands,
                                const Policy& policy,
                                const Station& prototype,
                                std::size_t topN = 5) const;

    /// Fleet outcome with no new station, for comparison against the ranking.
    Summary baseline(const std::vector<Demand>& demands, const Policy& policy) const;

private:
    const Network* network_;
    Router router_;  ///< built once over the base topology, reused for all candidates
    SimulationConfig config_;
};

}  // namespace evnet
