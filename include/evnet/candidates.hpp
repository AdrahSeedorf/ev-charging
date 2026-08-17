#pragma once

#include <vector>

#include "evnet/network.hpp"
#include "evnet/policy.hpp"
#include "evnet/router.hpp"
#include "evnet/units.hpp"
#include "evnet/wait_oracle.hpp"

namespace evnet {

/// Parameters governing which charging stops are admissible.
struct FeasibilityConfig {
    Dollars travelCostPerKm{0.28};
    double reserveFraction{0.10};  ///< arrive with this fraction of the battery in hand
    /// Average road speed, used to convert distance into elapsed time. Stage 1 had
    /// no clock so this went unused there; the event-driven engine needs it to know
    /// *when* a vehicle turns up at a candidate station, which is what makes a
    /// time-dependent wait estimate possible.
    double speedKmh{80.0};
    /// Fixed time cost of a charging session, on top of the energy transfer:
    /// leaving the road, parking, plugging in, paying, and getting going again.
    ///
    /// Without this the model is INDIFFERENT to how many stops a journey makes,
    /// because total energy and therefore total transfer time are fixed by the
    /// distance. The optimal planner exploited that exactly as it should have,
    /// fragmenting one charge into six identically-priced ones. Six minutes of
    /// overhead per session is both physically real and enough to make fewer,
    /// larger stops genuinely preferable.
    Hours stopOverheadHours{0.1};
};

/// A vehicle's situation at the moment a charging decision is needed.
struct VehicleState {
    int id{0};
    NodeId at{kNoNode};
    NodeId destination{kNoNode};
    Kwh socKwh{0.0};
    Kwh batteryKwh{0.0};
    KwhPer100Km efficiency{18.0};
    Hours now{0.0};  ///< simulated clock; zero for the timeless engine

    Km rangeKm() const { return rangeFromEnergy(socKwh, efficiency); }
};

/// Enumerates the charging stops a vehicle may legally take next.
///
/// This is the single copy of the feasibility rules, shared by the static
/// allocator and the discrete-event simulator. Both guards live here:
///
///   1. PROGRESS -- a stop must leave the vehicle strictly closer to its
///      destination, else it can oscillate between two cheap stations forever or
///      be dragged backwards by an attractive price. Charging in place is exempt,
///      since it adds energy without moving; it cannot loop because a second
///      attempt at the same node yields no useful energy.
///
///   2. ONWARD FEASIBILITY -- once charged, the vehicle must be able to finish the
///      trip or reach a further station that is itself closer to the destination.
///      The legacy corridor allocator omitted this, so it could send a vehicle to
///      a low-queue town at the edge of its range and leave it stranded.
///
/// `arrivalTimeAt` lets the caller tell the oracle when the vehicle would reach a
/// candidate, which is what allows the event-driven engine to estimate a wait that
/// depends on the clock. The static engine passes a function returning `now`.
std::vector<Candidate> buildCandidates(const Network& network,
                                       const Router& router,
                                       const WaitOracle& oracle,
                                       const VehicleState& vehicle,
                                       const FeasibilityConfig& config);

/// Candidates for a round-trip top-up mission: drive out to a station, take on
/// `requiredKwh`, drive home. The Sydney metro project's question.
std::vector<Candidate> buildTopUpCandidates(const Network& network,
                                            const Router& router,
                                            const WaitOracle& oracle,
                                            const VehicleState& vehicle,
                                            Kwh requiredKwh,
                                            const FeasibilityConfig& config);

}  // namespace evnet
