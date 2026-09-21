#pragma once

// Domain-neutral quantity vocabulary.
//
// This file is the top half of what used to be `units.hpp`. The split is the
// point: everything named here is meaningful for *any* agent traversing a
// network and competing for capacity at nodes, while `units.hpp` holds the
// electric-vehicle reading of these same quantities.
//
// The model this vocabulary encodes, stated once so it can be argued with:
//
//     An agent carries a RESOURCE which depletes as it traverses edges and is
//     replenished at nodes, where a limited number of parallel servers means
//     arrivals may have to queue.
//
// For an EV the resource is charge in kWh, depleting with distance and
// replenished by a charger at some kW. For a truck driver under hours-of-service
// rules it is legal driving time, depleting with distance at 1/speed and
// "replenished" by a rest period. The engine above the seam should be able to
// say which is cheaper, faster or better sited without knowing which it is
// looking at.
//
// These are semantic aliases, not distinct types -- every one is a `double`, so
// the compiler will not stop you mixing them up. That is a deliberate trade:
// strong unit types would catch such mistakes but would make the arithmetic in
// the router considerably noisier, and at this scale the noise costs more than
// the mistakes do. The names are here to be read, not enforced.

namespace evnet {

/// Distance along an edge.
using Km = double;

/// Elapsed or scheduled time.
using Hours = double;

/// Money. The common currency that money-versus-time trade-offs resolve into.
using Dollars = double;

/// The quantity an agent carries, spends by moving, and tops up at a station.
/// kWh of charge for an EV; hours of legal driving time for a truck driver.
using Resource = double;

/// How fast a station restores `Resource`, in resource units per hour. kW for a
/// charger. Note that not every domain has a meaningful one: a legally mandated
/// rest period takes a fixed time regardless of how depleted the driver is, so
/// there the service duration is a constant rather than amount / rate.
using Rate = double;

/// Resource consumed per unit distance. Consumption in kWh/100km for an EV; the
/// reciprocal of speed for a driver burning hours to cover ground.
using PerDistance = double;

/// Time to cover `distance` at an average speed. Lives here rather than with the
/// EV physics because it is true of anything that moves: distance over speed
/// needs no domain. Stage 1 had no notion of travel taking time at all, which is
/// why nothing spread out across a day.
inline Hours drivingTime(Km distance, double speedKmh) {
    if (speedKmh <= 0.0) return 0.0;
    return distance / speedKmh;
}

}  // namespace evnet
