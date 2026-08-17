#include "evnet/router.hpp"

#include <algorithm>
#include <queue>
#include <stdexcept>
#include <utility>

namespace evnet {

Router::Router(const Network& network) : network_(&network), cache_(network.size()) {}

const Router::Tree& Router::from(NodeId source) const {
    if (!network_->contains(source)) {
        throw std::out_of_range("router: unknown source node " + std::to_string(source));
    }
    auto& slot = cache_[static_cast<std::size_t>(source)];
    if (slot.has_value()) return *slot;

    const std::size_t n = network_->size();
    Tree tree;
    tree.distance.assign(n, kUnreachable);
    tree.parent.assign(n, kNoNode);

    // Binary-heap Dijkstra: O((V+E) log V). The legacy implementation used a
    // linear scan for the minimum, giving O(V^2), and relied on DBL_MAX as its
    // unreachable sentinel -- which meant relaxing an edge out of an unreachable
    // node would have overflowed had the min-selection not happened to exclude
    // it. Using infinity here makes that arithmetic well defined regardless.
    using Entry = std::pair<Km, NodeId>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;

    tree.distance[static_cast<std::size_t>(source)] = 0.0;
    frontier.emplace(0.0, source);

    while (!frontier.empty()) {
        const auto [distance, current] = frontier.top();
        frontier.pop();

        // Stale heap entry: we have already settled this node via a shorter path.
        if (distance > tree.distance[static_cast<std::size_t>(current)]) continue;

        for (const auto& edge : network_->neighbours(current)) {
            const Km relaxed = distance + edge.distanceKm;
            auto& best = tree.distance[static_cast<std::size_t>(edge.to)];
            if (relaxed < best) {
                best = relaxed;
                tree.parent[static_cast<std::size_t>(edge.to)] = current;
                frontier.emplace(relaxed, edge.to);
            }
        }
    }

    ++computations_;
    slot = std::move(tree);
    return *slot;
}

Km Router::distance(NodeId from, NodeId to) const {
    if (!network_->contains(to)) {
        throw std::out_of_range("router: unknown target node " + std::to_string(to));
    }
    return this->from(from).distance[static_cast<std::size_t>(to)];
}

std::vector<NodeId> Router::path(NodeId from, NodeId to) const {
    const Tree& tree = this->from(from);
    if (tree.distance[static_cast<std::size_t>(to)] == kUnreachable) return {};

    std::vector<NodeId> reversed;
    for (NodeId at = to; at != kNoNode; at = tree.parent[static_cast<std::size_t>(at)]) {
        reversed.push_back(at);
        if (at == from) break;
    }
    if (reversed.empty() || reversed.back() != from) return {};
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::vector<NodeId> Router::reachableWithin(NodeId origin, Km rangeKm) const {
    const Tree& tree = this->from(origin);
    std::vector<NodeId> out;
    for (std::size_t i = 0; i < tree.distance.size(); ++i) {
        if (static_cast<NodeId>(i) == origin) continue;
        if (tree.distance[i] <= rangeKm) out.push_back(static_cast<NodeId>(i));
    }
    return out;
}

}  // namespace evnet
