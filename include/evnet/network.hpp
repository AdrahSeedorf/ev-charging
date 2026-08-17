#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "evnet/units.hpp"

namespace evnet {

using NodeId = int;
inline constexpr NodeId kNoNode = -1;

/// A charging facility at a node. Absent (std::nullopt on Node) means the node
/// is a plain waypoint -- and therefore a candidate site for a new station.
struct Station {
    Dollars pricePerKwh{0.0};
    int chargers{0};
    Kw powerKw{50.0};
};

struct Node {
    NodeId id{kNoNode};
    std::string name;
    std::optional<Station> station;

    bool hasStation() const { return station.has_value(); }
};

struct Edge {
    NodeId to{kNoNode};
    Km distanceKm{0.0};
};

/// A road network: nodes that may carry charging stations, joined by weighted
/// edges.
///
/// Stored as an ADJACENCY LIST, not the dense adjacency matrix both legacy
/// projects used. The Sydney matrix was 24x24 with only 41 real edges (~93%
/// zeros), and a dense matrix does not survive stage 3, where real OpenStreetMap
/// extracts have thousands of nodes.
///
/// The corridor topology of the legacy Hume project is not a special case here:
/// a chain is simply a graph where each node has at most two neighbours, so one
/// representation serves both datasets and the corridor's bespoke prefix-sum
/// distance code disappears entirely.
class Network {
public:
    /// Load from the unified CSV schema. Throws std::runtime_error on malformed
    /// input -- deliberately loud, because the legacy data was silently corrupt.
    static Network load(const std::string& nodesCsvPath, const std::string& edgesCsvPath);

    void addNode(Node node);
    void addEdge(NodeId a, NodeId b, Km distanceKm, bool bidirectional = true);

    std::size_t size() const { return nodes_.size(); }
    bool contains(NodeId id) const { return id >= 0 && static_cast<std::size_t>(id) < nodes_.size(); }

    const Node& node(NodeId id) const;
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& neighbours(NodeId id) const;

    /// Case-sensitive exact name lookup; kNoNode when absent.
    NodeId findByName(const std::string& name) const;
    /// Resolves either a name or a bare integer id, for CLI convenience.
    NodeId resolve(const std::string& nameOrId) const;

    std::vector<NodeId> stationNodes() const;
    /// Nodes without a station -- the candidate set for siting.
    std::vector<NodeId> candidateSites() const;

    /// Adds a station to an existing node. Used by the siting search, which
    /// varies the station set while leaving the topology untouched.
    void setStation(NodeId id, Station station);

    /// Structural warnings a human should see: self-loops, duplicate edges,
    /// unreachable components, stations with zero chargers. Returns an empty
    /// vector for a clean network.
    std::vector<std::string> validate() const;

private:
    std::vector<Node> nodes_;
    std::vector<std::vector<Edge>> adjacency_;
    std::unordered_map<std::string, NodeId> byName_;
};

}  // namespace evnet
