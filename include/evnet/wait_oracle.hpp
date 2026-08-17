#pragma once

#include "evnet/network.hpp"
#include "evnet/units.hpp"

namespace evnet {

/// How long will a vehicle wait, and how long will it charge?
///
/// This is the seam between the two simulation engines. The feasibility rules for
/// a charging stop -- the progress guard, the onward-feasibility guard, the energy
/// arithmetic -- are subtle and must not be duplicated, but the two engines answer
/// the congestion question completely differently:
///
///   * StationState (stage 1) is timeless. It counts arrivals and divides by
///     chargers, so `arrivalTime` is ignored entirely. Kept because it reproduces
///     the legacy corridor project's semantics, which is what makes comparing
///     against that project meaningful.
///
///   * StationRuntime (stage 2) knows when every charger next frees up, so it can
///     answer the question properly: a vehicle arriving at 09:30 waits until a
///     charger is actually available.
///
/// Abstracting this lets one candidate builder serve both.
class WaitOracle {
public:
    virtual ~WaitOracle() = default;

    /// Expected queueing delay for a vehicle reaching `node` at `arrivalTime`.
    /// Implementations that do not model time ignore the second argument.
    virtual Hours expectedWait(NodeId node, Hours arrivalTime) const = 0;

    /// Time to transfer `energy` once plugged in.
    virtual Hours chargeTime(NodeId node, Kwh energy) const = 0;
};

}  // namespace evnet
