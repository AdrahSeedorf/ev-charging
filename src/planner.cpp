#include "evnet/planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace evnet {
namespace {

/// How a state was reached, so the first move of a plan can be recovered.
/// At namespace scope rather than inside solve(): a function-local scoped enum with
/// a fixed underlying type trips older GCC parsers.
enum class Move : unsigned char { None, Drive, Charge };

constexpr Kwh kEnergyEpsilon = 1e-6;
constexpr Dollars kInfiniteCost = std::numeric_limits<Dollars>::infinity();

/// Can the vehicle finish from where it stands, keeping its reserve?
bool canFinish(const Router& router, const VehicleState& vehicle, double reserveFraction) {
    const Km remaining = router.distance(vehicle.at, vehicle.destination);
    if (remaining == Router::kUnreachable) return false;
    if (remaining == 0.0) return true;
    const Kwh needed = energyForDistance(remaining, vehicle.efficiency) +
                       vehicle.batteryKwh * reserveFraction;
    return vehicle.socKwh + kEnergyEpsilon >= needed;
}

}  // namespace

// ---------------------------------------------------------------------------
// GreedyPlanner
// ---------------------------------------------------------------------------

GreedyPlanner::GreedyPlanner(const Network& network,
                             const Router& router,
                             std::unique_ptr<Policy> policy,
                             FeasibilityConfig config)
    : network_(&network), router_(&router), policy_(std::move(policy)), config_(config) {
    if (policy_ == nullptr) throw std::invalid_argument("GreedyPlanner requires a policy");
}

Action GreedyPlanner::decide(const VehicleState& vehicle, const WaitOracle& oracle) const {
    if (router_->distance(vehicle.at, vehicle.destination) == Router::kUnreachable) {
        return Action::infeasible("destination unreachable from " + network_->node(vehicle.at).name);
    }
    if (canFinish(*router_, vehicle, config_.reserveFraction)) {
        return Action::driveToDestination();
    }

    const auto candidates = buildCandidates(*network_, *router_, oracle, vehicle, config_);
    if (candidates.empty()) {
        return Action::infeasible("stranded at " + network_->node(vehicle.at).name +
                                  ": no feasible charging stop within range");
    }

    const Candidate* chosen = policy_->choose(candidates);
    if (chosen == nullptr) return Action::infeasible("policy returned no choice");

    if (chosen->node == vehicle.at) return Action::chargeHere(chosen->energyKwh);
    return Action::driveTo(chosen->node);
}

// ---------------------------------------------------------------------------
// OptimalPlanner
// ---------------------------------------------------------------------------

OptimalPlanner::OptimalPlanner(const Network& network,
                               const Router& router,
                               Dollars valueOfTimePerHour,
                               FeasibilityConfig config,
                               int chargeLevels)
    : network_(&network),
      router_(&router),
      valueOfTime_(valueOfTimePerHour),
      config_(config),
      levels_(chargeLevels),
      scoring_(valueOfTimePerHour) {
    if (levels_ < 2) throw std::invalid_argument("OptimalPlanner needs at least 2 charge levels");
}

OptimalPlanner::Plan OptimalPlanner::solve(const VehicleState& vehicle,
                                           const WaitOracle& oracle) const {
    Plan plan;
    plan.first = Action::infeasible("no feasible plan");

    const std::size_t nodeCount = network_->size();
    const auto levelCount = static_cast<std::size_t>(levels_) + 1;
    const Kwh step = vehicle.batteryKwh / static_cast<double>(levels_);
    if (step <= 0.0) return plan;

    // Round the starting charge DOWN onto the grid: the planner must never assume
    // more energy than the vehicle actually has, or it will produce plans the
    // simulator cannot execute.
    const auto startLevel = static_cast<std::size_t>(std::floor(vehicle.socKwh / step));
    const std::size_t reserveLevel = static_cast<std::size_t>(
        std::ceil(vehicle.batteryKwh * config_.reserveFraction / step));

    if (startLevel >= levelCount) return plan;

    const auto index = [levelCount](std::size_t node, std::size_t level) {
        return node * levelCount + level;
    };
    const std::size_t stateCount = nodeCount * levelCount;

    std::vector<Dollars> best(stateCount, kInfiniteCost);
    std::vector<std::size_t> parent(stateCount, stateCount);  // stateCount == "none"
    std::vector<Move> via(stateCount, Move::None);

    // Wait estimates are taken once, at planning time, and treated as fixed. The
    // arrival time at each node is approximated from its shortest-path distance;
    // see the honest-limit note in the header.
    std::vector<Hours> waitAt(nodeCount, 0.0);
    for (const auto& node : network_->nodes()) {
        if (!node.hasStation()) continue;
        const Km reach = router_->distance(vehicle.at, node.id);
        const Hours arrival =
            reach == Router::kUnreachable ? vehicle.now
                                          : vehicle.now + drivingTime(reach, config_.speedKmh);
        waitAt[static_cast<std::size_t>(node.id)] = oracle.expectedWait(node.id, arrival);
    }

    using Entry = std::pair<Dollars, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;

    const std::size_t startState = index(static_cast<std::size_t>(vehicle.at), startLevel);
    best[startState] = 0.0;
    frontier.emplace(0.0, startState);

    std::size_t goalState = stateCount;
    while (!frontier.empty()) {
        const auto [cost, state] = frontier.top();
        frontier.pop();
        if (cost > best[state]) continue;  // stale heap entry

        const std::size_t node = state / levelCount;
        const std::size_t level = state % levelCount;

        if (static_cast<NodeId>(node) == vehicle.destination && level >= reserveLevel) {
            goalState = state;
            plan.cost = cost;
            plan.feasible = true;
            break;
        }

        const auto relax = [&](std::size_t next, Dollars added, Move move) {
            const Dollars candidate = cost + added;
            if (candidate < best[next]) {
                best[next] = candidate;
                parent[next] = state;
                via[next] = move;
                frontier.emplace(candidate, next);
            }
        };

        // Transition 1: drive along one edge. Per-edge rather than per-shortest-path
        // so that energy is accounted along the route actually taken.
        for (const auto& edge : network_->neighbours(static_cast<NodeId>(node))) {
            const Kwh burn = energyForDistance(edge.distanceKm, vehicle.efficiency);
            const auto burnLevels = static_cast<std::size_t>(std::ceil(burn / step));
            if (burnLevels > level) continue;
            const std::size_t remaining = level - burnLevels;

            // The reserve exists so a vehicle is never stranded between chargers, so
            // it is required on arrival at a plain waypoint but not at a station --
            // rolling into a charger nearly empty is the entire point of the charger.
            //
            // Applying it everywhere made this planner reject journeys the greedy
            // planners happily completed, which meant the two were being compared on
            // different feasibility problems rather than on their decisions.
            const std::size_t floorLevel =
                network_->node(edge.to).hasStation() ? 0u : reserveLevel;
            if (remaining < floorLevel) continue;

            const Dollars added = edge.distanceKm * config_.travelCostPerKm +
                                  drivingTime(edge.distanceKm, config_.speedKmh) * valueOfTime_;
            relax(index(static_cast<std::size_t>(edge.to), remaining), added, Move::Drive);
        }

        // Transition 2: charge here, to any higher level.
        const Node& here = network_->node(static_cast<NodeId>(node));
        if (here.hasStation() && here.station->chargers > 0) {
            for (std::size_t target = level + 1; target < levelCount; ++target) {
                const Kwh energy = static_cast<double>(target - level) * step;
                const Hours duration = chargeDuration(energy, here.station->powerKw);
                const Dollars added = energy * here.station->pricePerKwh +
                                      (waitAt[node] + duration) * valueOfTime_;
                relax(index(node, target), added, Move::Charge);
            }
        }
    }

    if (!plan.feasible) return plan;

    // Walk back to the first move, then translate it into an Action the simulator
    // can carry out. A leading run of drive moves collapses into "head for the next
    // station"; a leading charge becomes "charge this much here".
    std::vector<std::size_t> path;
    for (std::size_t s = goalState; s != stateCount; s = parent[s]) {
        path.push_back(s);
        if (s == startState) break;
    }
    std::reverse(path.begin(), path.end());

    if (path.size() < 2) {
        plan.first = Action::driveToDestination();
        return plan;
    }

    if (via[path[1]] == Move::Charge) {
        const std::size_t fromLevel = path[0] % levelCount;
        const std::size_t toLevel = path[1] % levelCount;
        plan.first = Action::chargeHere(static_cast<double>(toLevel - fromLevel) * step);
        return plan;
    }

    // Follow the drive prefix to whichever node the plan next charges at.
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (via[path[i]] == Move::Charge) {
            plan.first = Action::driveTo(static_cast<NodeId>(path[i] / levelCount));
            return plan;
        }
    }
    plan.first = Action::driveToDestination();
    return plan;
}

Action OptimalPlanner::decide(const VehicleState& vehicle, const WaitOracle& oracle) const {
    if (router_->distance(vehicle.at, vehicle.destination) == Router::kUnreachable) {
        return Action::infeasible("destination unreachable from " + network_->node(vehicle.at).name);
    }
    if (canFinish(*router_, vehicle, config_.reserveFraction)) {
        return Action::driveToDestination();
    }

    const Plan plan = solve(vehicle, oracle);
    if (!plan.feasible) {
        return Action::infeasible("stranded at " + network_->node(vehicle.at).name +
                                  ": no feasible charging plan reaches the destination");
    }
    return plan.first;
}

Dollars OptimalPlanner::planCost(const VehicleState& vehicle, const WaitOracle& oracle) const {
    const Plan plan = solve(vehicle, oracle);
    return plan.feasible ? plan.cost : kInfiniteCost;
}

// ---------------------------------------------------------------------------

std::unique_ptr<Planner> makePlanner(const std::string& name,
                                     const Network& network,
                                     const Router& router,
                                     Dollars valueOfTimePerHour,
                                     FeasibilityConfig config) {
    if (name == "optimal") {
        return std::make_unique<OptimalPlanner>(network, router, valueOfTimePerHour, config);
    }
    return std::make_unique<GreedyPlanner>(network, router,
                                           makePolicy(name, valueOfTimePerHour), config);
}

std::vector<std::string> plannerNames() {
    return {"farthest", "cheapest", "min-wait", "generalised", "optimal"};
}

}  // namespace evnet
