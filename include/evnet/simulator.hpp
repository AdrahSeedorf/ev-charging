#pragma once

#include <string>
#include <vector>

#include "evnet/network.hpp"
#include "evnet/planner.hpp"
#include "evnet/router.hpp"
#include "evnet/simulation.hpp"
#include "evnet/station_runtime.hpp"

namespace evnet {

/// One vehicle's journey with times that were measured, not assumed.
struct TimedTrip {
    int demandId{0};
    bool completed{false};
    std::string failure;
    Hours releaseTime{0.0};
    Hours finishTime{0.0};
    std::vector<Stop> stops;
    Km distanceKm{0.0};
    Dollars travelCost{0.0};
    Dollars energyCost{0.0};
    Hours waitHours{0.0};    ///< measured queueing, summed over stops
    Hours chargeHours{0.0};  ///< measured time plugged in
    Hours drivingHours{0.0};

    Dollars moneyCost() const { return travelCost + energyCost; }
    /// Time spent at stations.
    Hours timeHours() const { return waitHours + chargeHours; }
    /// All time the journey consumed, driving included.
    Hours totalTimeHours() const { return waitHours + chargeHours + drivingHours; }
    /// Wall-clock door to door.
    Hours elapsed() const { return finishTime - releaseTime; }
    /// Money plus every hour the trip cost, valued at a rate.
    ///
    /// Driving time is included here where the stage 1 metric omitted it, because
    /// stage 1 had no clock and driving was instantaneous. The optimal planner
    /// accounts for road time; the greedy policies do not, which is a real
    /// difference between them rather than a mismatch in how they are scored.
    Dollars generalisedCost(Dollars valueOfTime) const {
        return moneyCost() + totalTimeHours() * valueOfTime;
    }
};

/// Fleet outcome under the event-driven engine.
struct TimedSummary {
    std::string planner;
    int demands{0};
    int completed{0};
    int stranded{0};
    Dollars meanMoneyCost{0.0};
    Hours meanWaitHours{0.0};
    Hours p95WaitHours{0.0};
    Hours maxWaitHours{0.0};
    Dollars meanGeneralisedCost{0.0};
    double meanStops{0.0};
    Hours meanElapsedHours{0.0};
    int peakWaiting{0};
    std::string peakStation;
    Hours makespan{0.0};        ///< last finish across the fleet
    double peakUtilisation{0.0};///< busiest station's charger utilisation
    std::string busiestStation;
};

/// Tunable parameters for the event-driven engine.
struct SimulatorConfig {
    Dollars travelCostPerKm{0.28};
    Dollars valueOfTimePerHour{20.00};
    double reserveFraction{0.10};
    /// Average road speed. Stage 1 had no clock, so driving was instantaneous and
    /// nothing ever spread across a day.
    double speedKmh{80.0};
    Hours stopOverheadHours{0.1};
    int maxStepsPerVehicle{40};

    FeasibilityConfig feasibility() const {
        FeasibilityConfig out;
        out.travelCostPerKm = travelCostPerKm;
        out.reserveFraction = reserveFraction;
        out.speedKmh = speedKmh;
        out.stopOverheadHours = stopOverheadHours;
        return out;
    }
};

/// Discrete-event simulation of a fleet over a charging network.
///
/// This is what stage 2 adds. Stage 1's Allocator walked each vehicle to
/// completion in isolation and incremented a counter at every station it used, so
/// congestion was a tally rather than a process: nothing ever departed, waits were
/// an index instead of a duration, and driving took no time at all.
///
/// Here a clock drives everything. Vehicles are released at their own times, spend
/// real hours on the road, occupy a charger for a computed duration and then free
/// it. Waits are what the simulation measured, not what a formula guessed.
///
/// There is exactly one event type -- a vehicle reaching a node -- because charger
/// admission is resolved synchronously against the earliest-free charger. See the
/// FIFO-equivalence argument on StationRuntime for why that is exact rather than an
/// approximation.
///
/// Vehicles replan on every arrival, so a vehicle heading for a station that turns
/// out to be busier than estimated can change its mind en route.
class Simulator {
public:
    using Config = SimulatorConfig;

    Simulator(const Network& network, const Router& router, SimulatorConfig config = {});

    /// Mutates `runtime`, which afterwards holds every measured service record.
    std::vector<TimedTrip> run(const std::vector<Demand>& demands,
                               const Planner& planner,
                               StationRuntime& runtime) const;

    TimedSummary summarise(const std::vector<TimedTrip>& trips,
                           const StationRuntime& runtime,
                           const std::string& plannerName) const;

    const SimulatorConfig& config() const { return config_; }

private:
    const Network* network_;
    const Router* router_;
    SimulatorConfig config_;
};

/// A per-station sample of the simulation state at one instant.
struct TimeSeriesSample {
    Hours time{0.0};
    NodeId node{kNoNode};
    std::string station;
    int waiting{0};
    int charging{0};
    int chargers{0};
};

/// Samples every station at a fixed interval across [0, horizon]. This is the
/// output stage 1 could not produce at all, and it is what the stage 3 charts will
/// be drawn from.
std::vector<TimeSeriesSample> sampleTimeSeries(const Network& network,
                                               const StationRuntime& runtime,
                                               Hours horizon,
                                               Hours interval);

}  // namespace evnet
