#include "evnet/network.hpp"

#include <algorithm>
#include <deque>
#include <iomanip>
#include <sstream>
#include <set>
#include <stdexcept>

#include "evnet/csv.hpp"

namespace evnet {
namespace {

/// Above this, a road is indirect enough to be worth a second look. City streets run
/// about 1.3-1.6 and highways about 1.1-1.4, so this sits well clear of normal.
constexpr double kImplausibleDetourRatio = 4.0;

}  // namespace

Network Network::load(const std::string& nodesCsvPath, const std::string& edgesCsvPath) {
    Network network;

    const csv::Reader nodesCsv(nodesCsvPath);
    nodesCsv.requireColumns({"id", "name", "has_station", "price_per_kwh", "chargers", "power_kw"});

    // Nodes are keyed by a contiguous id, so sort defensively rather than trusting
    // file order -- the vector index must match the declared id.
    std::vector<Node> parsed;
    parsed.reserve(nodesCsv.rows().size());
    for (const auto& row : nodesCsv.rows()) {
        Node node;
        node.id = row.integer("id");
        node.name = row.str("name");
        if (node.name.empty()) {
            throw std::runtime_error("network: node " + std::to_string(node.id) + " has an empty name");
        }
        if (row.boolean("has_station")) {
            Station station;
            station.pricePerKwh = row.number("price_per_kwh");
            station.chargers = row.integer("chargers");
            station.powerKw = row.number("power_kw");
            if (station.pricePerKwh < 0.0) {
                throw std::runtime_error("network: negative price at '" + node.name + "'");
            }
            node.station = station;
        }
        // Coordinates are optional so that networks built in code, and the schema as
        // it stood before stage 3, both still load.
        if (row.has("latitude") && row.has("longitude")) {
            Coordinate location;
            location.latitude = row.number("latitude");
            location.longitude = row.number("longitude");
            if (location.latitude < -90.0 || location.latitude > 90.0 ||
                location.longitude < -180.0 || location.longitude > 180.0) {
                throw std::runtime_error("network: '" + node.name +
                                         "' has coordinates outside the valid range");
            }
            node.location = location;
        }
        parsed.push_back(std::move(node));
    }

    std::sort(parsed.begin(), parsed.end(), [](const Node& a, const Node& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        if (parsed[i].id != static_cast<NodeId>(i)) {
            throw std::runtime_error("network: node ids must be contiguous from 0; found " +
                                     std::to_string(parsed[i].id) + " at position " + std::to_string(i));
        }
        network.addNode(parsed[i]);
    }

    const csv::Reader edgesCsv(edgesCsvPath);
    edgesCsv.requireColumns({"from_id", "to_id", "distance_km"});
    for (const auto& row : edgesCsv.rows()) {
        const NodeId from = row.integer("from_id");
        const NodeId to = row.integer("to_id");
        const Km distance = row.number("distance_km");
        if (!network.contains(from) || !network.contains(to)) {
            throw std::runtime_error("network: edge on line " + std::to_string(row.lineNumber()) +
                                     " references an unknown node id");
        }
        if (distance <= 0.0) {
            throw std::runtime_error("network: edge on line " + std::to_string(row.lineNumber()) +
                                     " has a non-positive distance");
        }
        network.addEdge(from, to, distance);
    }

    return network;
}

void Network::addNode(Node node) {
    node.id = static_cast<NodeId>(nodes_.size());
    byName_[node.name] = node.id;
    nodes_.push_back(std::move(node));
    adjacency_.emplace_back();
}

void Network::addEdge(NodeId a, NodeId b, Km distanceKm, bool bidirectional) {
    if (!contains(a) || !contains(b)) throw std::out_of_range("network: addEdge on unknown node");
    adjacency_[static_cast<std::size_t>(a)].push_back(Edge{b, distanceKm});
    if (bidirectional) {
        adjacency_[static_cast<std::size_t>(b)].push_back(Edge{a, distanceKm});
    }
}

const Node& Network::node(NodeId id) const {
    if (!contains(id)) throw std::out_of_range("network: unknown node id " + std::to_string(id));
    return nodes_[static_cast<std::size_t>(id)];
}

const std::vector<Edge>& Network::neighbours(NodeId id) const {
    if (!contains(id)) throw std::out_of_range("network: unknown node id " + std::to_string(id));
    return adjacency_[static_cast<std::size_t>(id)];
}

NodeId Network::findByName(const std::string& name) const {
    const auto it = byName_.find(name);
    return it == byName_.end() ? kNoNode : it->second;
}

NodeId Network::resolve(const std::string& nameOrId) const {
    const NodeId byName = findByName(nameOrId);
    if (byName != kNoNode) return byName;

    // Fall back to a bare integer id, but only if it parses cleanly.
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(nameOrId, &consumed);
        if (consumed == nameOrId.size() && contains(value)) return value;
    } catch (const std::exception&) {
        // not an integer; fall through
    }
    return kNoNode;
}

void Network::setStation(NodeId id, Station station) {
    if (!contains(id)) throw std::out_of_range("network: setStation on unknown node");
    nodes_[static_cast<std::size_t>(id)].station = station;
}

std::vector<NodeId> Network::stationNodes() const {
    std::vector<NodeId> out;
    for (const auto& node : nodes_) {
        if (node.hasStation()) out.push_back(node.id);
    }
    return out;
}

std::vector<NodeId> Network::candidateSites() const {
    std::vector<NodeId> out;
    for (const auto& node : nodes_) {
        if (!node.hasStation()) out.push_back(node.id);
    }
    return out;
}

std::optional<Km> Network::straightLineKm(NodeId a, NodeId b) const {
    const Node& from = node(a);
    const Node& to = node(b);
    if (!from.hasLocation() || !to.hasLocation()) return std::nullopt;
    return greatCircleKm(*from.location, *to.location);
}

std::vector<std::string> Network::validate() const {
    std::vector<std::string> warnings;
    if (nodes_.empty()) {
        warnings.push_back("network has no nodes");
        return warnings;
    }

    std::set<std::pair<NodeId, NodeId>> seen;
    for (const auto& node : nodes_) {
        for (const auto& edge : adjacency_[static_cast<std::size_t>(node.id)]) {
            if (edge.to == node.id) {
                warnings.push_back("self-loop at '" + node.name + "'");
            }
            const auto key = std::minmax(node.id, edge.to);
            if (!seen.insert({key.first, key.second}).second) continue;
        }
        if (adjacency_[static_cast<std::size_t>(node.id)].empty()) {
            warnings.push_back("'" + node.name + "' has no edges (isolated)");
        }
        if (node.hasStation() && node.station->chargers <= 0) {
            warnings.push_back("'" + node.name + "' has a station with no chargers");
        }
        if (node.hasStation() && node.station->powerKw <= 0.0) {
            warnings.push_back("'" + node.name + "' has a station with no charging power");
        }
    }

    // Geometry, where coordinates allow it. A road cannot be shorter than the
    // straight line it spans, so an edge below its great-circle distance is
    // impossible rather than merely odd -- a check with no threshold to tune.
    //
    // Note the deliberate asymmetry in how the two findings are worded. Falling below
    // the lower bound is a statement about physics. A high detour ratio is only a
    // statement about plausibility, and both it and the bound depend on the
    // coordinates being accurate, so it is reported as worth a look rather than as a
    // defect.
    std::set<std::pair<NodeId, NodeId>> checked;
    for (const auto& from : nodes_) {
        for (const auto& edge : adjacency_[static_cast<std::size_t>(from.id)]) {
            const auto key = std::minmax(from.id, edge.to);
            if (!checked.insert({key.first, key.second}).second) continue;

            const auto straight = straightLineKm(from.id, edge.to);
            if (!straight.has_value() || *straight <= 0.0) continue;

            const double ratio = detourRatio(edge.distanceKm, *straight);
            std::ostringstream detail;
            detail << std::fixed << std::setprecision(1) << "'" << from.name << "' <-> '"
                   << nodes_[static_cast<std::size_t>(edge.to)].name << "' is " << edge.distanceKm
                   << " km by road but " << *straight << " km in a straight line";
            if (ratio < 1.0) {
                warnings.push_back(detail.str() + " -- shorter than the straight line, so impossible");
            } else if (ratio > kImplausibleDetourRatio) {
                warnings.push_back(detail.str() + " (detour ratio " +
                                   std::to_string(static_cast<int>(ratio * 100) / 100.0).substr(0, 4) +
                                   ") -- unusually indirect, worth checking");
            }
        }
    }

    // Connectivity, treating edges as traversable in the direction stored.
    std::vector<bool> visited(nodes_.size(), false);
    std::deque<NodeId> frontier{nodes_.front().id};
    visited[0] = true;
    std::size_t reached = 1;
    while (!frontier.empty()) {
        const NodeId current = frontier.front();
        frontier.pop_front();
        for (const auto& edge : adjacency_[static_cast<std::size_t>(current)]) {
            const auto index = static_cast<std::size_t>(edge.to);
            if (!visited[index]) {
                // Note: std::vector<bool> yields a proxy, so this cannot be bound
                // to an lvalue reference.
                visited[index] = true;
                ++reached;
                frontier.push_back(edge.to);
            }
        }
    }
    if (reached != nodes_.size()) {
        std::string unreachable;
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            if (!visited[i]) {
                if (!unreachable.empty()) unreachable += ", ";
                unreachable += nodes_[i].name;
            }
        }
        warnings.push_back("network is not fully connected from '" + nodes_.front().name +
                           "'; unreachable: " + unreachable);
    }

    return warnings;
}

}  // namespace evnet
