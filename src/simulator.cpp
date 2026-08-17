#include "evnet/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <utility>

namespace evnet {
namespace {

/// One scheduled moment: vehicle `index` becomes ready to act at `time`, having
/// reached whatever node it was travelling to (or finished charging where it was).
struct Event {
    Hours time{0.0};
    std::size_t index{0};

    /// Ties broken by index so the run is reproducible. Determinism matters here
    /// more than usual: the whole output is a comparison between planners, and a
    /// comparison against a coin flip measures nothing.
    bool operator>(const Event& other) const {
        if (time != other.time) return time > other.time;
        return index > other.index;
    }
};

/// A vehicle in flight.
struct Runner {
    NodeId at{kNoNode};
    Kwh soc{0.0};
    int steps{0};
    bool done{false};
    TimedTrip trip;
};

}  // namespace

Simulator::Simulator(const Network& network, const Router& router, SimulatorConfig config)
    : network_(&network), router_(&router), config_(config) {}

std::vector<TimedTrip> Simulator::run(const std::vector<Demand>& demands,
                                      const Planner& planner,
                                      StationRuntime& runtime) const {
    const FeasibilityConfig feasibility = config_.feasibility();

    std::vector<Runner> runners(demands.size());
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> schedule;

    for (std::size_t i = 0; i < demands.size(); ++i) {
        const Demand& demand = demands[i];
        Runner& runner = runners[i];
        runner.at = demand.origin;
        runner.soc = demand.socKwh;
        runner.trip.demandId = demand.id;
        runner.trip.releaseTime = demand.releaseHour;
        runner.trip.finishTime = demand.releaseHour;

        if (!network_->contains(demand.origin) || !network_->contains(demand.destination)) {
            runner.done = true;
            runner.trip.failure = "origin or destination is not a node in this network";
            continue;
        }
        schedule.push(Event{demand.releaseHour, i});
    }

    while (!schedule.empty()) {
        const Event event = schedule.top();
        schedule.pop();

        Runner& runner = runners[event.index];
        if (runner.done) continue;
        const Demand& demand = demands[event.index];

        if (++runner.steps > config_.maxStepsPerVehicle) {
            runner.done = true;
            runner.trip.failure = "exceeded " + std::to_string(config_.maxStepsPerVehicle) +
                                  " steps without reaching the destination";
            continue;
        }

        VehicleState vehicle;
        vehicle.id = demand.id;
        vehicle.at = runner.at;
        vehicle.destination = demand.destination;
        vehicle.socKwh = runner.soc;
        vehicle.batteryKwh = demand.batteryKwh;
        vehicle.efficiency = demand.efficiency;
        vehicle.now = event.time;

        // A top-up mission is a single round trip with one decision, so it is
        // resolved in one step rather than driven through the arrival loop.
        if (demand.isTopUp()) {
            const auto candidates = buildTopUpCandidates(*network_, *router_, runtime, vehicle,
                                                         demand.requiredKwh, feasibility);
            const Candidate* chosen =
                candidates.empty() ? nullptr : planner.scoringPolicy().choose(candidates);
            if (chosen == nullptr) {
                runner.done = true;
                runner.trip.failure = "no reachable charging station can serve a " +
                                      std::to_string(static_cast<int>(demand.requiredKwh)) +
                                      " kWh top-up from " + network_->node(runner.at).name;
                continue;
            }

            const Hours outboundTime = drivingTime(chosen->detourKm, config_.speedKmh);
            const ServiceRecord record = runtime.admit(chosen->node, demand.id,
                                                       event.time + outboundTime, chosen->energyKwh);

            runner.trip.stops.push_back(Stop{chosen->node, chosen->energyKwh, chosen->energyCost,
                                             record.wait(), record.service()});
            runner.trip.distanceKm = 2.0 * chosen->detourKm;
            runner.trip.travelCost = chosen->travelCost;
            runner.trip.energyCost = chosen->energyCost;
            runner.trip.waitHours = record.wait();
            runner.trip.chargeHours = record.service();
            runner.trip.drivingHours = 2.0 * outboundTime;
            runner.trip.finishTime = record.finish + outboundTime;  // and home again
            runner.trip.completed = true;
            runner.done = true;
            continue;
        }

        const Action action = planner.decide(vehicle, runtime);

        switch (action.kind) {
            case Action::Kind::DriveToDestination: {
                const Km leg = router_->distance(runner.at, demand.destination);
                const Hours travel = drivingTime(leg, config_.speedKmh);
                runner.soc -= energyForDistance(leg, demand.efficiency);
                runner.trip.distanceKm += leg;
                runner.trip.drivingHours += travel;
                runner.trip.travelCost = runner.trip.distanceKm * config_.travelCostPerKm;
                runner.trip.finishTime = event.time + travel;
                runner.trip.completed = true;
                runner.done = true;
                break;
            }

            case Action::Kind::DriveToStation: {
                const Km leg = router_->distance(runner.at, action.target);
                if (leg == Router::kUnreachable) {
                    runner.done = true;
                    runner.trip.failure = "planner chose an unreachable station";
                    break;
                }
                const Hours travel = drivingTime(leg, config_.speedKmh);
                runner.soc -= energyForDistance(leg, demand.efficiency);
                runner.trip.distanceKm += leg;
                runner.trip.drivingHours += travel;
                runner.at = action.target;
                // Decide again on arrival: congestion may have moved on since.
                schedule.push(Event{event.time + travel, event.index});
                break;
            }

            case Action::Kind::ChargeHere: {
                const Node& node = network_->node(runner.at);
                if (!node.hasStation()) {
                    runner.done = true;
                    runner.trip.failure = "planner chose to charge where there is no station";
                    break;
                }
                const ServiceRecord record =
                    runtime.admit(runner.at, demand.id, event.time, action.energyKwh);

                runner.soc = std::min(demand.batteryKwh, runner.soc + action.energyKwh);
                const Dollars cost = action.energyKwh * node.station->pricePerKwh;
                runner.trip.stops.push_back(Stop{runner.at, action.energyKwh, cost, record.wait(),
                                                 record.service()});
                runner.trip.energyCost += cost;
                runner.trip.waitHours += record.wait();
                runner.trip.chargeHours += record.service();
                // The vehicle is occupied until it unplugs.
                schedule.push(Event{record.finish, event.index});
                break;
            }

            case Action::Kind::Infeasible:
            default: {
                runner.done = true;
                runner.trip.failure = action.reason;
                runner.trip.travelCost = runner.trip.distanceKm * config_.travelCostPerKm;
                break;
            }
        }
    }

    std::vector<TimedTrip> trips;
    trips.reserve(runners.size());
    for (auto& runner : runners) trips.push_back(std::move(runner.trip));
    return trips;
}

TimedSummary Simulator::summarise(const std::vector<TimedTrip>& trips,
                                  const StationRuntime& runtime,
                                  const std::string& plannerName) const {
    TimedSummary summary;
    summary.planner = plannerName;
    summary.demands = static_cast<int>(trips.size());

    std::vector<Hours> waits;
    double totalStops = 0.0;
    for (const auto& trip : trips) {
        if (!trip.completed) {
            ++summary.stranded;
            continue;
        }
        ++summary.completed;
        summary.meanMoneyCost += trip.moneyCost();
        summary.meanGeneralisedCost += trip.generalisedCost(config_.valueOfTimePerHour);
        summary.meanWaitHours += trip.waitHours;
        summary.meanElapsedHours += trip.elapsed();
        totalStops += static_cast<double>(trip.stops.size());
        waits.push_back(trip.waitHours);
        summary.makespan = std::max(summary.makespan, trip.finishTime);
    }

    if (summary.completed > 0) {
        const auto n = static_cast<double>(summary.completed);
        summary.meanMoneyCost /= n;
        summary.meanGeneralisedCost /= n;
        summary.meanWaitHours /= n;
        summary.meanElapsedHours /= n;
        summary.meanStops = totalStops / n;

        std::sort(waits.begin(), waits.end());
        const auto rank = static_cast<std::size_t>(std::ceil(0.95 * n));
        summary.p95WaitHours = waits[std::min(waits.size() - 1, rank == 0 ? 0 : rank - 1)];
        summary.maxWaitHours = waits.back();
    }

    summary.peakStation = "-";
    summary.busiestStation = "-";
    for (const NodeId id : network_->stationNodes()) {
        const auto [peak, when] = runtime.peakWaiting(id);
        (void)when;
        if (peak > summary.peakWaiting) {
            summary.peakWaiting = peak;
            summary.peakStation = network_->node(id).name;
        }
        const double used = runtime.utilisation(id, summary.makespan);
        if (used > summary.peakUtilisation) {
            summary.peakUtilisation = used;
            summary.busiestStation = network_->node(id).name;
        }
    }
    return summary;
}

std::vector<TimeSeriesSample> sampleTimeSeries(const Network& network,
                                               const StationRuntime& runtime,
                                               Hours horizon,
                                               Hours interval) {
    std::vector<TimeSeriesSample> samples;
    if (interval <= 0.0 || horizon < 0.0) return samples;

    const auto stations = network.stationNodes();
    for (Hours t = 0.0; t <= horizon + 1e-9; t += interval) {
        for (const NodeId id : stations) {
            TimeSeriesSample sample;
            sample.time = t;
            sample.node = id;
            sample.station = network.node(id).name;
            sample.waiting = runtime.waitingAt(id, t);
            sample.charging = runtime.chargingAt(id, t);
            sample.chargers = network.node(id).station->chargers;
            samples.push_back(sample);
        }
    }
    return samples;
}

}  // namespace evnet
