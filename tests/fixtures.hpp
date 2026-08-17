#pragma once

#include "evnet/network.hpp"
#include "evnet/simulation.hpp"

namespace evnet::testing {

/// A small graph with hand-computable shortest paths.
///
///        A --4-- B
///        |       |
///        2       3
///        |       |
///        C --1-- D --10-- E        F (isolated)
///
/// Expected distances from A: B=4 (direct beats A-C-D-B at 6), C=2, D=3, E=13,
/// F=unreachable.
inline Network diamond() {
    Network network;
    for (const char* name : {"A", "B", "C", "D", "E", "F"}) {
        Node node;
        node.name = name;
        network.addNode(node);
    }
    network.addEdge(0, 1, 4.0);   // A-B
    network.addEdge(0, 2, 2.0);   // A-C
    network.addEdge(2, 3, 1.0);   // C-D
    network.addEdge(1, 3, 3.0);   // B-D
    network.addEdge(3, 4, 10.0);  // D-E
    return network;
}

/// A four-town corridor with stations at the two middle towns.
///
///   Start --100-- Mid1 --100-- Mid2 --100-- Target
///
/// Mid1 is cheap but has a single charger; Mid2 is dearer with four. This is the
/// minimal setup in which the cheapest and min-wait policies disagree.
inline Network corridor() {
    Network network;

    Node start;
    start.name = "Start";
    network.addNode(start);

    Node mid1;
    mid1.name = "Mid1";
    mid1.station = Station{0.30, 1, 100.0};
    network.addNode(mid1);

    Node mid2;
    mid2.name = "Mid2";
    mid2.station = Station{0.60, 4, 100.0};
    network.addNode(mid2);

    Node target;
    target.name = "Target";
    network.addNode(target);

    network.addEdge(0, 1, 100.0);
    network.addEdge(1, 2, 100.0);
    network.addEdge(2, 3, 100.0);
    return network;
}

/// Journey demand across the corridor above. 30 kWh battery at 18 kWh/100km is
/// a 166 km full range, so a 300 km trip requires two stops.
inline Demand corridorJourney(Kwh soc = 20.0) {
    Demand demand;
    demand.id = 1;
    demand.origin = 0;
    demand.destination = 3;
    demand.batteryKwh = 30.0;
    demand.socKwh = soc;
    demand.efficiency = 18.0;
    demand.requiredKwh = 0.0;
    return demand;
}

}  // namespace evnet::testing
