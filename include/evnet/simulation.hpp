#pragma once

#include <string>
#include <vector>

#include "evnet/network.hpp"
#include "evnet/policy.hpp"
#include "evnet/router.hpp"
#include "evnet/station_state.hpp"
#include "evnet/units.hpp"

namespace evnet {

/// One vehicle's requirement. The unified demand type.
///
/// The two legacy projects asked structurally different questions, and this
/// struct is where they meet. The Sydney study asked "I am at X and need N kWh --
/// which station minimises my cost?", a round trip with no destination. The
/// corridor study asked "I am driving X to Y -- where must I stop?", a journey
/// with no energy target. Rather than bolt two code paths together, both are
/// expressed in the same fields and distinguished by `requiredKwh`:
///
///   requiredKwh > 0   -> TopUp mission  (origin == destination, round trip)
///   requiredKwh == 0  -> Journey mission (drive origin -> destination)
struct Demand {
    int id{0};
    NodeId origin{kNoNode};
    NodeId destination{kNoNode};
    Kwh batteryKwh{0.0};
    Kwh socKwh{0.0};  ///< state of charge at the start
    KwhPer100Km efficiency{18.0};
    Kwh requiredKwh{0.0};
    /// When this vehicle enters the system, in hours from the start of the run.
    /// Optional in the CSV (defaults to zero) so stage 1 datasets still load; the
    /// event-driven engine needs it, because a fleet that all departs at once is
    /// not a traffic pattern, it is a thundering herd.
    Hours releaseHour{0.0};

    bool isTopUp() const { return requiredKwh > 0.0; }
    Km rangeKm() const { return rangeFromEnergy(socKwh, efficiency); }

    static std::vector<Demand> load(const std::string& csvPath);
};

struct Stop {
    NodeId node{kNoNode};
    Kwh energyKwh{0.0};
    Dollars energyCost{0.0};
    Hours waitHours{0.0};
    Hours chargeHours{0.0};
};

struct TripResult {
    int demandId{0};
    bool completed{false};
    std::string failure;  ///< populated only when !completed
    std::vector<Stop> stops;
    Km distanceKm{0.0};
    Dollars travelCost{0.0};
    Dollars energyCost{0.0};
    Hours waitHours{0.0};
    Hours chargeHours{0.0};

    Dollars moneyCost() const { return travelCost + energyCost; }
    Hours timeHours() const { return waitHours + chargeHours; }
    Dollars generalisedCost(Dollars valueOfTime) const {
        return moneyCost() + timeHours() * valueOfTime;
    }
};

/// Fleet-level outcome for one policy. These are the columns of the comparison
/// table that is the toolkit's headline result.
struct Summary {
    std::string policy;
    int demands{0};
    int completed{0};
    int stranded{0};
    Dollars meanMoneyCost{0.0};
    Hours meanWaitHours{0.0};
    Hours p95WaitHours{0.0};  ///< tail behaviour; means hide the interesting cases
    Dollars meanGeneralisedCost{0.0};
    double meanStops{0.0};
    int peakQueue{0};
    std::string peakStation;
};

/// Tunable parameters shared by the allocator and the siting search.
///
/// `valueOfTimePerHour` is the interesting one: it is the exchange rate between
/// money and time, and therefore the single knob that decides whether a cheap
/// station with a queue beats a dear one that is free. Sweeping it is how the
/// toolkit shows that the optimal choice genuinely flips.
struct SimulationConfig {
    Dollars travelCostPerKm{0.28};      ///< carried over from the legacy Sydney study
    Dollars valueOfTimePerHour{20.00};  ///< the knob that trades money against time
    double reserveFraction{0.10};       ///< arrive with 10% in hand, not on empty
    int maxStopsPerTrip{12};            ///< guards against pathological cycling
};

/// Runs a fleet through the network under a given policy.
class Allocator {
public:
    using Config = SimulationConfig;

    Allocator(const Network& network, const Router& router, SimulationConfig config = {});

    /// Mutates `state`: each vehicle's chosen stop increments that station's
    /// queue, so vehicles processed later see the congestion created by those
    /// before them. Demand order therefore matters, which is a property of the
    /// problem rather than a defect.
    std::vector<TripResult> run(const std::vector<Demand>& demands,
                                const Policy& policy,
                                StationState& state) const;

    TripResult runOne(const Demand& demand, const Policy& policy, StationState& state) const;

    Summary summarise(const std::vector<TripResult>& results,
                      const StationState& state,
                      const std::string& policyName) const;

    const SimulationConfig& config() const { return config_; }

private:
    TripResult runJourney(const Demand& demand, const Policy& policy, StationState& state) const;
    TripResult runTopUp(const Demand& demand, const Policy& policy, StationState& state) const;

    /// Feasible stops from `at`, given current charge and where we are headed.
    /// Enforces the stranding guard described in the implementation.
    std::vector<Candidate> candidatesFor(NodeId at,
                                         NodeId destination,
                                         Kwh socKwh,
                                         Kwh batteryKwh,
                                         KwhPer100Km efficiency,
                                         const StationState& state) const;

    const Network* network_;
    const Router* router_;
    SimulationConfig config_;
};

}  // namespace evnet
