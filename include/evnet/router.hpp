#pragma once

#include <limits>
#include <optional>
#include <vector>

#include "evnet/network.hpp"
#include "evnet/units.hpp"

namespace evnet {

/// Shortest-path queries over a Network.
///
/// Three deliberate improvements over the legacy implementations:
///
///  1. A binary-heap Dijkstra, O((V+E) log V), replacing the legacy linear
///     min-scan at O(V^2). At 24 nodes this is irrelevant; at stage 3 scale it
///     is the difference between usable and not.
///
///  2. Predecessor tracking, so the actual ROUTE can be reported. The legacy
///     Sydney project computed distances and then threw the path away, so it
///     could tell you how far the cheapest station was but never how to get
///     there.
///
///  3. `reachableWithin` replaces both of the legacy corridor project's
///     `farthestCity` / `calculateFarthestCity` functions -- near-duplicates of
///     each other, and both prefix sums that were only correct on a straight
///     chain. On a general graph, "where can I get with this much charge" is a
///     range-limited Dijkstra, which is what this is.
///
/// Results are cached per source vertex and computed lazily. This is what
/// removes the accidentally cubic behaviour in the legacy siting routine, which
/// re-ran Dijkstra inside a nested loop over candidate sites and demands, and
/// allocated a fresh array on every single iteration.
class Router {
public:
    static constexpr Km kUnreachable = std::numeric_limits<Km>::infinity();

    /// Non-owning; `network` must outlive the Router.
    ///
    /// IMPORTANT INVARIANT: the Router depends only on the network's TOPOLOGY
    /// (nodes and edges), never on which nodes carry stations. The siting search
    /// relies on this -- it builds one Router for the base network and reuses it
    /// across every candidate station placement, because adding a charger does
    /// not move any roads.
    explicit Router(const Network& network);

    struct Tree {
        std::vector<Km> distance;    ///< kUnreachable where no path exists
        std::vector<NodeId> parent;  ///< kNoNode at the source and unreachable nodes
    };

    /// Shortest-path tree from `source`, computed once and cached.
    const Tree& from(NodeId source) const;

    Km distance(NodeId from, NodeId to) const;

    /// Node sequence from `from` to `to` inclusive; empty when unreachable.
    std::vector<NodeId> path(NodeId from, NodeId to) const;

    /// Every node within `rangeKm` of `origin`, excluding `origin` itself.
    std::vector<NodeId> reachableWithin(NodeId origin, Km rangeKm) const;

    /// Number of Dijkstra runs actually performed. Exposed so tests can assert
    /// the siting search is not recomputing, and so the CLI can report it.
    std::size_t computations() const { return computations_; }

private:
    const Network* network_;
    mutable std::vector<std::optional<Tree>> cache_;
    mutable std::size_t computations_{0};
};

}  // namespace evnet
