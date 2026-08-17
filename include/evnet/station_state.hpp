#pragma once

#include <vector>

#include "evnet/network.hpp"
#include "evnet/units.hpp"
#include "evnet/wait_oracle.hpp"

namespace evnet {

/// Mutable congestion state layered over an immutable Network.
///
/// This is the component that makes the merged model qualitatively different
/// from the Sydney study it inherits its graph from. There, the answer for one
/// vehicle was independent of every other vehicle -- a static optimisation with
/// a fixed correct answer. Here, assigning a vehicle to a station makes that
/// station worse for every vehicle behind it, so the fleet's outcome is emergent
/// rather than merely computed.
///
/// KNOWN LIMITATION (stage 1): arrivals accumulate and never depart, so
/// `queueLength` is really "vehicles assigned over the horizon" rather than an
/// instantaneous queue, and the wait model below is a static proxy. This
/// faithfully reproduces the legacy corridor project's semantics, which is what
/// makes the policy comparison against it meaningful -- but it does mean
/// absolute wait figures should be read as relative, not predictive. Stage 2
/// replaces this class with a discrete-event simulator carrying a clock, where
/// vehicles occupy a charger for a duration and then release it.
class StationState : public WaitOracle {
public:
    /// Half an hour of queueing per vehicle already waiting per charger. Carried
    /// over verbatim from the legacy corridor project so the two are comparable.
    static constexpr Hours kHoursPerQueuedVehicle = 0.5;

    explicit StationState(const Network& network);

    /// Expected wait before a charger frees up. Zero at nodes with no station.
    Hours expectedWait(NodeId id) const;

    /// WaitOracle interface. This engine is timeless, so `arrivalTime` is ignored
    /// -- which is precisely the limitation the event-driven engine removes.
    Hours expectedWait(NodeId id, Hours arrivalTime) const override;

    /// Time to actually deliver `energy` once plugged in.
    Hours chargeTime(NodeId id, Kwh energy) const override;

    int queueLength(NodeId id) const;
    void enqueue(NodeId id);
    void reset();

    const std::vector<int>& queues() const { return queue_; }

    /// Highest queue length across the network, with the node it occurred at.
    /// Peak utilisation is the metric that separates the policies most sharply.
    std::pair<NodeId, int> peak() const;

private:
    const Network* network_;
    std::vector<int> queue_;
};

}  // namespace evnet
