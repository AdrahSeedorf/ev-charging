#include "evnet/station_runtime.hpp"

#include <algorithm>
#include <stdexcept>

namespace evnet {

StationRuntime::StationRuntime(const Network& network, Hours stopOverheadHours)
    : network_(&network), stopOverhead_(stopOverheadHours) {
    chargerFreeAt_.resize(network.size());
    records_.resize(network.size());
    for (const auto& node : network.nodes()) {
        if (node.hasStation() && node.station->chargers > 0) {
            chargerFreeAt_[static_cast<std::size_t>(node.id)]
                .assign(static_cast<std::size_t>(node.station->chargers), 0.0);
        }
    }
}

Hours StationRuntime::expectedWait(NodeId node, Hours arrivalTime) const {
    const auto& chargers = chargerFreeAt_[static_cast<std::size_t>(network_->node(node).id)];
    if (chargers.empty()) return 0.0;
    const Hours earliestFree = *std::min_element(chargers.begin(), chargers.end());
    return std::max(0.0, earliestFree - arrivalTime);
}

Hours StationRuntime::chargeTime(NodeId node, Kwh energy) const {
    const Node& n = network_->node(node);
    if (!n.hasStation()) return 0.0;
    return stopOverhead_ + chargeDuration(energy, n.station->powerKw);
}

ServiceRecord StationRuntime::admit(NodeId node, int vehicleId, Hours arrivalTime, Kwh energy) {
    auto& chargers = chargerFreeAt_[static_cast<std::size_t>(network_->node(node).id)];
    if (chargers.empty()) {
        throw std::runtime_error("station runtime: '" + network_->node(node).name +
                                 "' has no chargers to admit to");
    }

    // Earliest-free charger. See the FIFO-equivalence note in the header: because
    // the simulator feeds arrivals in time order, this min-scan yields exactly the
    // waits a single station-wide queue would produce.
    const auto slot = std::min_element(chargers.begin(), chargers.end());

    ServiceRecord record;
    record.vehicleId = vehicleId;
    record.node = node;
    record.arrival = arrivalTime;
    record.start = std::max(arrivalTime, *slot);
    record.finish = record.start + chargeTime(node, energy);
    record.energyKwh = energy;

    *slot = record.finish;
    records_[static_cast<std::size_t>(node)].push_back(record);
    return record;
}

int StationRuntime::waitingAt(NodeId node, Hours t) const {
    int count = 0;
    for (const auto& record : records_[static_cast<std::size_t>(network_->node(node).id)]) {
        if (record.arrival <= t && t < record.start) ++count;
    }
    return count;
}

int StationRuntime::chargingAt(NodeId node, Hours t) const {
    int count = 0;
    for (const auto& record : records_[static_cast<std::size_t>(network_->node(node).id)]) {
        if (record.start <= t && t < record.finish) ++count;
    }
    return count;
}

double StationRuntime::utilisation(NodeId node, Hours horizon) const {
    const Node& n = network_->node(node);
    if (horizon <= 0.0 || !n.hasStation() || n.station->chargers <= 0) return 0.0;

    Hours busy = 0.0;
    for (const auto& record : records_[static_cast<std::size_t>(node)]) {
        // Clip to the reporting window so a session running past the horizon does
        // not push utilisation above 1.
        const Hours from = std::min(record.start, horizon);
        const Hours to = std::min(record.finish, horizon);
        busy += std::max(0.0, to - from);
    }
    return busy / (horizon * static_cast<double>(n.station->chargers));
}

std::pair<int, Hours> StationRuntime::peakWaiting(NodeId node) const {
    const auto& records = records_[static_cast<std::size_t>(network_->node(node).id)];
    int peak = 0;
    Hours when = 0.0;
    // The waiting count only changes at an arrival or a start, so checking those
    // instants is sufficient to find the maximum.
    for (const auto& probe : records) {
        for (const Hours t : {probe.arrival, probe.start}) {
            const int count = waitingAt(node, t);
            if (count > peak) {
                peak = count;
                when = t;
            }
        }
    }
    return {peak, when};
}

const std::vector<ServiceRecord>& StationRuntime::records(NodeId node) const {
    return records_[static_cast<std::size_t>(network_->node(node).id)];
}

std::vector<ServiceRecord> StationRuntime::allRecords() const {
    std::vector<ServiceRecord> all;
    for (const auto& perNode : records_) {
        all.insert(all.end(), perNode.begin(), perNode.end());
    }
    std::sort(all.begin(), all.end(), [](const ServiceRecord& a, const ServiceRecord& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.node != b.node) return a.node < b.node;
        return a.vehicleId < b.vehicleId;
    });
    return all;
}

Hours StationRuntime::lastFinish() const {
    Hours latest = 0.0;
    for (const auto& perNode : records_) {
        for (const auto& record : perNode) latest = std::max(latest, record.finish);
    }
    return latest;
}

void StationRuntime::reset() {
    for (auto& chargers : chargerFreeAt_) std::fill(chargers.begin(), chargers.end(), 0.0);
    for (auto& perNode : records_) perNode.clear();
}

}  // namespace evnet
