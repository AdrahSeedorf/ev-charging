#pragma once

#include <vector>

#include "evnet/network.hpp"
#include "evnet/units.hpp"
#include "evnet/wait_oracle.hpp"

namespace evnet {

/// One vehicle's use of one charger, with times that were measured rather than
/// assumed. Every congestion statistic the toolkit reports derives from these.
struct ServiceRecord {
    int vehicleId{0};
    NodeId node{kNoNode};
    Hours arrival{0.0};  ///< reached the station
    Hours start{0.0};    ///< plugged in
    Hours finish{0.0};   ///< unplugged
    Kwh energyKwh{0.0};

    Hours wait() const { return start - arrival; }
    Hours service() const { return finish - start; }
};

/// Charging stations with a clock.
///
/// This replaces StationState's timeless proxy, and it is the substance of stage
/// 2. Stage 1 counted arrivals and never let them depart, so its "queue length"
/// was really cumulative arrivals and its waits were an index rather than a
/// duration -- which is why a 53-hour 95th percentile appeared in the stage 1
/// results. Here a charger is occupied for a computed duration and then released.
///
/// DESIGN NOTE -- why there are no queue events.
/// A vehicle wanting to charge is assigned the charger that frees up soonest, and
/// begins at max(its arrival, that charger's free time). Because the simulator
/// processes arrivals in nondecreasing time order, this is provably equivalent to
/// a single station-wide FIFO queue: an earlier arrival is always assigned first
/// and therefore always takes the earliest slot. So exact waits fall out of a
/// min-scan over `chargers`, and the simulator needs no StartCharge or
/// FinishCharge events at all -- one event type suffices.
class StationRuntime : public WaitOracle {
public:
    explicit StationRuntime(const Network& network);

    /// Estimated queueing delay for a vehicle reaching `node` at `arrivalTime`,
    /// given everything committed so far. This is what planners consult; unlike
    /// stage 1's estimate it actually depends on when the vehicle turns up.
    Hours expectedWait(NodeId node, Hours arrivalTime) const override;
    Hours chargeTime(NodeId node, Kwh energy) const override;

    /// Commit a charging session and return its measured record. Occupies the
    /// earliest-free charger from max(arrivalTime, that charger's free time).
    ServiceRecord admit(NodeId node, int vehicleId, Hours arrivalTime, Kwh energy);

    /// Vehicles that have arrived but not yet plugged in, at instant `t`.
    int waitingAt(NodeId node, Hours t) const;
    /// Chargers in use at instant `t`.
    int chargingAt(NodeId node, Hours t) const;

    /// Fraction of charger-hours used over [0, horizon]; 0 when horizon <= 0.
    double utilisation(NodeId node, Hours horizon) const;

    /// Highest simultaneous waiting count at a node, and when it occurred.
    /// Sampled at record boundaries, which is where any extremum must lie.
    std::pair<int, Hours> peakWaiting(NodeId node) const;

    const std::vector<ServiceRecord>& records(NodeId node) const;
    /// Every record across the network, in commit order.
    std::vector<ServiceRecord> allRecords() const;

    /// Latest finish time committed anywhere; 0 if nothing has been served.
    Hours lastFinish() const;

    void reset();

private:
    const Network* network_;
    /// Per node, per charger: the time that charger next becomes free.
    std::vector<std::vector<Hours>> chargerFreeAt_;
    std::vector<std::vector<ServiceRecord>> records_;
};

}  // namespace evnet
