#pragma once

#include <limits>

#include "evnet/network.hpp"
#include "evnet/quantities.hpp"

namespace evnet {

/// The expected wait an oracle reports for a station that would refuse the agent
/// outright -- full, in a domain whose stations turn arrivals away. Infinite
/// rather than a flag so that every consumer that prices a wait already does the
/// right thing with it: a cost-minimising policy never picks it, and the optimal
/// planner's search never relaxes through it. The candidate builder drops such
/// stations explicitly, so no planner is ever offered one.
inline constexpr Hours kNoAdmission = std::numeric_limits<Hours>::infinity();

/// How long will an agent wait, and how long will it charge?
///
/// This is the seam between the two simulation engines. The feasibility rules for
/// a charging stop -- the progress guard, the onward-feasibility guard, the energy
/// arithmetic -- are subtle and must not be duplicated, but the two engines answer
/// the congestion question completely differently:
///
///   * StationState (stage 1) is timeless. It counts arrivals and divides by
///     servers, so `arrivalTime` is ignored entirely. Kept because it reproduces
///     the legacy corridor project's semantics, which is what makes comparing
///     against that project meaningful.
///
///   * StationRuntime (stage 2) knows when every charger next frees up, so it can
///     answer the question properly: an agent arriving at 09:30 waits until a
///     charger is actually available.
///
/// Abstracting this lets one candidate builder serve both.
class WaitOracle {
public:
    virtual ~WaitOracle() = default;

    /// Expected queueing delay for an agent reaching `node` at `arrivalTime`, or
    /// kNoAdmission if the station would turn it away. Implementations that do
    /// not model time ignore the second argument.
    virtual Hours expectedWait(NodeId node, Hours arrivalTime) const = 0;

    /// Time to transfer `energy` once plugged in.
    virtual Hours serviceTime(NodeId node, Resource amount) const = 0;
};

}  // namespace evnet
