#include "evnet/station_state.hpp"

#include <algorithm>
#include <stdexcept>

namespace evnet {

StationState::StationState(const Network& network)
    : network_(&network), queue_(network.size(), 0) {}

Hours StationState::expectedWait(NodeId id) const {
    const Node& node = network_->node(id);
    if (!node.hasStation() || node.station->servers <= 0) return 0.0;
    const int queued = queue_[static_cast<std::size_t>(id)];
    // This engine never releases anyone, so "full" means `servers` agents have
    // ever been assigned. Under turn-away that closes the station for the rest of
    // the run -- stage 1's never-departing queue taken to its conclusion, and one
    // more reason the event-driven engine is the one to believe.
    if (queued >= node.station->servers && network_->domain().admission() == Admission::TurnAway) {
        return kNoAdmission;
    }
    return kHoursPerQueuedVehicle * static_cast<double>(queued) /
           static_cast<double>(node.station->servers);
}

Hours StationState::expectedWait(NodeId id, Hours /*arrivalTime*/) const {
    return expectedWait(id);
}

Hours StationState::serviceTime(NodeId id, Resource amount) const {
    const Node& node = network_->node(id);
    if (!node.hasStation()) return 0.0;
    return network_->domain().serviceDuration(amount, node.station->ratePerHour);
}

int StationState::queueLength(NodeId id) const {
    if (!network_->contains(id)) throw std::out_of_range("station state: unknown node");
    return queue_[static_cast<std::size_t>(id)];
}

void StationState::enqueue(NodeId id) {
    if (!network_->contains(id)) throw std::out_of_range("station state: unknown node");
    ++queue_[static_cast<std::size_t>(id)];
}

void StationState::reset() { std::fill(queue_.begin(), queue_.end(), 0); }

std::pair<NodeId, int> StationState::peak() const {
    NodeId worst = kNoNode;
    int highest = 0;
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        if (queue_[i] > highest) {
            highest = queue_[i];
            worst = static_cast<NodeId>(i);
        }
    }
    return {worst, highest};
}

}  // namespace evnet
