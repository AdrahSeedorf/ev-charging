// Command-line front end for the EV network toolkit.
//
//   evnet inspect  --network data/sydney
//   evnet route    --network data/sydney --from Penrith --to Manly --battery 60 --soc 12
//   evnet simulate --network data/hume --demands data/hume/demands.csv --policy min-wait
//   evnet compare  --network data/hume --demands data/hume/demands.csv
//   evnet site     --network data/sydney --demands data/sydney/demands.csv --top 5

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "evnet/csv.hpp"
#include "evnet/network.hpp"
#include "evnet/policy.hpp"
#include "evnet/router.hpp"
#include "evnet/planner.hpp"
#include "evnet/simulation.hpp"
#include "evnet/simulator.hpp"
#include "evnet/station_runtime.hpp"
#include "evnet/siting.hpp"
#include "evnet/station_state.hpp"

#include <fstream>

namespace {

using namespace evnet;

struct Options {
    std::string command;
    std::string networkDir = "data/sydney";
    std::string demandsPath;
    std::string policy = "generalised";
    std::string from;
    std::string to;
    Kwh battery = 60.0;
    Kwh soc = -1.0;  // negative means "default to a fraction of the battery"
    Dollars valueOfTime = 20.0;
    Dollars travelCostPerKm = 0.28;
    Dollars newStationPrice = 0.45;
    int newStationChargers = 4;
    Kw newStationPower = 150.0;
    std::size_t top = 5;
    bool verbose = false;
    std::string engine = "events";     // events | static
    double speedKmh = 80.0;
    Hours stopOverhead = 0.1;
    Hours horizon = 0.0;               // 0 => derive from the run's makespan
    Hours sampleInterval = 0.25;
    std::string timeSeriesPath;
    std::string tripsPath;
    std::string format = "table";
};

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "error: " << message << "\n";
    std::exit(2);
}

void printUsage() {
    std::cout << R"(evnet -- capacitated network routing, congestion and siting

Usage: evnet <command> [options]

Commands:
  inspect    Summarise a network and report structural warnings
  route      Shortest path and charging plan for a single vehicle
  simulate   Run a fleet under one policy
  compare    Run a fleet under every policy and tabulate the outcomes
  site       Rank candidate locations for a new charging station

Options:
  --network <dir>       Directory holding nodes.csv and edges.csv  [data/sydney]
  --demands <file>      Demand CSV (simulate, compare, site)
  --policy <name>       farthest | cheapest | min-wait | generalised | optimal
                        (also accepted as --planner)  [generalised]
  --engine <name>       events | static  [events]
                          events -- discrete-event clock; waits are measured
                          static -- stage 1's timeless tally, kept for comparison
  --speed <km/h>        Average road speed (events engine)  [80]
  --stop-overhead <h>   Fixed time cost per charging session  [0.1]
  --horizon <hours>     Reporting window; 0 derives it from the run  [0]
  --interval <hours>    Time-series sampling step  [0.25]
  --timeseries <file>   Write per-station occupancy over time as CSV
  --trips <file>        Write per-trip records as CSV
  --format <kind>       table | csv  [table]  (csv makes output machine-readable
                        for the Python analysis layer)
  --from <node>         Origin, by name or id (route)
  --to <node>           Destination, by name or id (route)
  --battery <kwh>       Battery capacity (route)  [60]
  --soc <kwh>           Starting charge (route)  [20% of battery]
  --vot <dollars>       Value of time per hour  [20.00]
  --travel-cost <rate>  Dollars per km  [0.28]
  --site-price <rate>   Price per kWh at the hypothetical station (site)  [0.45]
  --site-chargers <n>   Chargers at the hypothetical station (site)  [4]
  --site-power <kw>     Power of the hypothetical station (site)  [150]
  --top <n>             Sites to list; 0 for all (site)  [5]
  --verbose             Extra detail
  -h, --help            This message
)";
}

Options parse(int argc, char** argv) {
    Options options;
    if (argc < 2) {
        printUsage();
        std::exit(1);
    }
    options.command = argv[1];
    if (options.command == "-h" || options.command == "--help" || options.command == "help") {
        printUsage();
        std::exit(0);
    }

    const auto next = [&](int& i, const char* flag) -> std::string {
        if (i + 1 >= argc) fail(std::string(flag) + " requires a value");
        return argv[++i];
    };

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        try {
            if (arg == "--network") options.networkDir = next(i, "--network");
            else if (arg == "--demands") options.demandsPath = next(i, "--demands");
            else if (arg == "--policy" || arg == "--planner") options.policy = next(i, "--policy");
            else if (arg == "--engine") options.engine = next(i, "--engine");
            else if (arg == "--speed") options.speedKmh = std::stod(next(i, "--speed"));
            else if (arg == "--stop-overhead") options.stopOverhead = std::stod(next(i, "--stop-overhead"));
            else if (arg == "--horizon") options.horizon = std::stod(next(i, "--horizon"));
            else if (arg == "--interval") options.sampleInterval = std::stod(next(i, "--interval"));
            else if (arg == "--timeseries") options.timeSeriesPath = next(i, "--timeseries");
            else if (arg == "--trips") options.tripsPath = next(i, "--trips");
            else if (arg == "--format") options.format = next(i, "--format");
            else if (arg == "--from") options.from = next(i, "--from");
            else if (arg == "--to") options.to = next(i, "--to");
            else if (arg == "--battery") options.battery = std::stod(next(i, "--battery"));
            else if (arg == "--soc") options.soc = std::stod(next(i, "--soc"));
            else if (arg == "--vot") options.valueOfTime = std::stod(next(i, "--vot"));
            else if (arg == "--travel-cost") options.travelCostPerKm = std::stod(next(i, "--travel-cost"));
            else if (arg == "--site-price") options.newStationPrice = std::stod(next(i, "--site-price"));
            else if (arg == "--site-chargers") options.newStationChargers = std::stoi(next(i, "--site-chargers"));
            else if (arg == "--site-power") options.newStationPower = std::stod(next(i, "--site-power"));
            else if (arg == "--top") options.top = static_cast<std::size_t>(std::stoul(next(i, "--top")));
            else if (arg == "--verbose") options.verbose = true;
            else if (arg == "-h" || arg == "--help") { printUsage(); std::exit(0); }
            else fail("unknown option '" + arg + "'");
        } catch (const std::invalid_argument&) {
            fail("'" + arg + "' was given a value that is not a number");
        } catch (const std::out_of_range&) {
            fail("'" + arg + "' was given a value out of range");
        }
    }
    return options;
}

Network loadNetwork(const Options& options) {
    return Network::load(options.networkDir + "/nodes.csv", options.networkDir + "/edges.csv");
}

SimulationConfig makeConfig(const Options& options) {
    SimulationConfig config;
    config.travelCostPerKm = options.travelCostPerKm;
    config.valueOfTimePerHour = options.valueOfTime;
    return config;
}

SimulatorConfig makeSimulatorConfig(const Options& options) {
    SimulatorConfig config;
    config.travelCostPerKm = options.travelCostPerKm;
    config.valueOfTimePerHour = options.valueOfTime;
    config.speedKmh = options.speedKmh;
    config.stopOverheadHours = options.stopOverhead;
    return config;
}

NodeId resolveOrFail(const Network& network, const std::string& token, const char* what) {
    if (token.empty()) fail(std::string("--") + what + " is required");
    const NodeId id = network.resolve(token);
    if (id == kNoNode) fail("no node called '" + token + "' in this network");
    return id;
}

std::string money(Dollars value) {
    std::ostringstream out;
    out << "$" << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string hoursText(Hours value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value << "h";
    return out.str();
}

// -------------------------------------------------------------------------
// Commands
// -------------------------------------------------------------------------

int cmdInspect(const Options& options) {
    const Network network = loadNetwork(options);
    const auto stations = network.stationNodes();
    const auto candidates = network.candidateSites();

    std::size_t edgeCount = 0;
    Km totalKm = 0.0;
    for (const auto& node : network.nodes()) {
        for (const auto& edge : network.neighbours(node.id)) {
            ++edgeCount;
            totalKm += edge.distanceKm;
        }
    }

    std::cout << "Network: " << options.networkDir << "\n"
              << "  nodes            " << network.size() << "\n"
              << "  edges            " << edgeCount / 2 << " undirected\n"
              << "  total road length " << std::fixed << std::setprecision(1) << totalKm / 2.0 << " km\n"
              << "  stations         " << stations.size() << "\n"
              << "  candidate sites  " << candidates.size() << "\n\n";

    std::cout << std::left << std::setw(4) << "id" << std::setw(18) << "name" << std::setw(10) << "station"
              << std::setw(12) << "$/kWh" << std::setw(11) << "chargers" << std::setw(9) << "kW"
              << "degree\n";
    std::cout << std::string(68, '-') << "\n";
    for (const auto& node : network.nodes()) {
        std::cout << std::left << std::setw(4) << node.id << std::setw(18) << node.name << std::setw(10)
                  << (node.hasStation() ? "yes" : "-");
        if (node.hasStation()) {
            std::ostringstream price;
            price << std::fixed << std::setprecision(2) << node.station->pricePerKwh;
            std::cout << std::setw(12) << (node.station->pricePerKwh == 0.0 ? "free" : price.str())
                      << std::setw(11) << node.station->chargers << std::setw(9)
                      << static_cast<int>(node.station->powerKw);
        } else {
            std::cout << std::setw(12) << "-" << std::setw(11) << "-" << std::setw(9) << "-";
        }
        std::cout << network.neighbours(node.id).size() << "\n";
    }

    const auto warnings = network.validate();
    if (warnings.empty()) {
        std::cout << "\nvalidation: clean\n";
    } else {
        std::cout << "\nvalidation: " << warnings.size() << " warning(s)\n";
        for (const auto& warning : warnings) std::cout << "  - " << warning << "\n";
    }
    return 0;
}

int cmdRoute(const Options& options) {
    const Network network = loadNetwork(options);
    const Router router(network);
    const NodeId from = resolveOrFail(network, options.from, "from");
    const NodeId to = resolveOrFail(network, options.to, "to");

    const Km distance = router.distance(from, to);
    if (distance == Router::kUnreachable) {
        std::cout << network.node(from).name << " -> " << network.node(to).name << ": unreachable\n";
        return 1;
    }

    std::cout << network.node(from).name << " -> " << network.node(to).name << "\n"
              << "  shortest distance " << std::fixed << std::setprecision(1) << distance << " km\n  route  ";
    const auto path = router.path(from, to);
    for (std::size_t i = 0; i < path.size(); ++i) {
        std::cout << network.node(path[i]).name;
        if (i + 1 < path.size()) std::cout << " -> ";
    }
    std::cout << "\n\n";

    Demand demand;
    demand.id = 1;
    demand.origin = from;
    demand.destination = to;
    demand.batteryKwh = options.battery;
    demand.socKwh = options.soc >= 0.0 ? options.soc : options.battery * 0.2;
    if (demand.socKwh > demand.batteryKwh) fail("--soc cannot exceed --battery");

    const Allocator allocator(network, router, makeConfig(options));
    const auto policy = makePolicy(options.policy, options.valueOfTime);
    StationState state(network);
    const TripResult result = allocator.runOne(demand, *policy, state);

    std::cout << "Charging plan (policy: " << policy->name() << ", battery " << std::setprecision(0)
              << demand.batteryKwh << " kWh, starting charge " << demand.socKwh << " kWh -> range "
              << std::setprecision(0) << demand.rangeKm() << " km)\n";
    if (!result.completed) {
        std::cout << "  INCOMPLETE: " << result.failure << "\n";
        return 1;
    }
    if (result.stops.empty()) {
        std::cout << "  no charging needed\n";
    } else {
        for (std::size_t i = 0; i < result.stops.size(); ++i) {
            const auto& stop = result.stops[i];
            std::cout << "  " << (i + 1) << ". " << network.node(stop.node).name << " -- take "
                      << std::fixed << std::setprecision(1) << stop.energyKwh << " kWh for "
                      << money(stop.energyCost) << ", wait " << hoursText(stop.waitHours) << ", charge "
                      << hoursText(stop.chargeHours) << "\n";
        }
    }
    std::cout << "  distance " << std::setprecision(1) << result.distanceKm << " km"
              << "  travel " << money(result.travelCost) << "  energy " << money(result.energyCost)
              << "  time " << hoursText(result.timeHours()) << "\n"
              << "  generalised cost " << money(result.generalisedCost(options.valueOfTime))
              << " (time valued at " << money(options.valueOfTime) << "/h)\n";
    return 0;
}

std::vector<Demand> loadDemands(const Options& options) {
    if (options.demandsPath.empty()) fail("--demands is required for this command");
    return Demand::load(options.demandsPath);
}

/// Stage 1 models congestion statically: arrivals accumulate at a station and
/// never depart, so wait figures are a comparable congestion index rather than a
/// predicted duration. Saying so in the output is cheaper than having someone
/// quote a 50-hour queue back at us.
void printWaitCaveat() {
    std::cout << "\nnote: waits are a relative congestion index, not predicted hours -- arrivals\n"
                 "      accumulate without departing until the event-driven clock lands.\n";
}

void printSummaryHeader() {
    std::cout << std::left << std::setw(14) << "policy" << std::right << std::setw(10) << "completed"
              << std::setw(10) << "stranded" << std::setw(12) << "mean $" << std::setw(12) << "mean wait"
              << std::setw(12) << "p95 wait" << std::setw(14) << "mean gen $" << std::setw(8) << "stops"
              << std::setw(9) << "peak Q" << "  peak station\n";
    std::cout << std::string(112, '-') << "\n";
}

void printSummaryRow(const Summary& summary) {
    std::cout << std::left << std::setw(14) << summary.policy << std::right << std::setw(10)
              << summary.completed << std::setw(10) << summary.stranded << std::setw(12)
              << money(summary.meanMoneyCost) << std::setw(12) << hoursText(summary.meanWaitHours)
              << std::setw(12) << hoursText(summary.p95WaitHours) << std::setw(14)
              << money(summary.meanGeneralisedCost) << std::setw(8) << std::fixed
              << std::setprecision(2) << summary.meanStops << std::setw(9) << summary.peakQueue << "  "
              << summary.peakStation << "\n";
}


// -------------------------------------------------------------------------
// Event-driven reporting
// -------------------------------------------------------------------------

void printTimedHeader() {
    std::cout << std::left << std::setw(14) << "planner" << std::right << std::setw(10) << "completed"
              << std::setw(10) << "stranded" << std::setw(12) << "mean $" << std::setw(12) << "mean wait"
              << std::setw(11) << "p95 wait" << std::setw(11) << "max wait" << std::setw(13)
              << "mean gen $" << std::setw(8) << "stops" << std::setw(11) << "elapsed"
              << std::setw(8) << "peak Q" << std::setw(8) << "util" << "  busiest\n";
    std::cout << std::string(128, '-') << "\n";
}

void printTimedRow(const TimedSummary& s) {
    std::ostringstream util;
    util << std::fixed << std::setprecision(0) << s.peakUtilisation * 100.0 << "%";
    std::cout << std::left << std::setw(14) << s.planner << std::right << std::setw(10) << s.completed
              << std::setw(10) << s.stranded << std::setw(12) << money(s.meanMoneyCost)
              << std::setw(12) << hoursText(s.meanWaitHours) << std::setw(11)
              << hoursText(s.p95WaitHours) << std::setw(11) << hoursText(s.maxWaitHours)
              << std::setw(13) << money(s.meanGeneralisedCost) << std::setw(8) << std::fixed
              << std::setprecision(2) << s.meanStops << std::setw(11)
              << hoursText(s.meanElapsedHours) << std::setw(8) << s.peakWaiting << std::setw(8)
              << util.str() << "  " << s.busiestStation << "\n";
}

void writeTimeSeries(const std::string& path,
                     const Network& network,
                     const StationRuntime& runtime,
                     Hours horizon,
                     Hours interval) {
    std::ofstream out(path);
    if (!out) fail("cannot write '" + path + "'");
    out << "# Per-station occupancy sampled over time. Generated by evnet.\n";
    out << "time_hours,node_id,station,waiting,charging,chargers,utilisation\n";
    for (const auto& sample : sampleTimeSeries(network, runtime, horizon, interval)) {
        const double used = sample.chargers > 0
                                ? static_cast<double>(sample.charging) / sample.chargers
                                : 0.0;
        out << std::fixed << std::setprecision(3) << sample.time << ","
            << sample.node << "," << csv::escape(sample.station) << "," << sample.waiting << ","
            << sample.charging << "," << sample.chargers << ","
            << std::setprecision(4) << used << "\n";
    }
    std::cout << "wrote time series to " << path << "\n";
}

void writeTrips(const std::string& path,
                const Network& network,
                const std::vector<TimedTrip>& trips,
                Dollars valueOfTime) {
    std::ofstream out(path);
    if (!out) fail("cannot write '" + path + "'");
    out << "# Per-trip records. Generated by evnet.\n";
    out << "demand_id,completed,release_hour,finish_hour,elapsed_hours,distance_km,stops,"
           "travel_cost,energy_cost,wait_hours,charge_hours,driving_hours,generalised_cost,"
           "stop_names,failure\n";
    for (const auto& trip : trips) {
        std::string names;
        for (const auto& stop : trip.stops) {
            if (!names.empty()) names += "|";
            names += network.node(stop.node).name;
        }
        out << trip.demandId << "," << (trip.completed ? 1 : 0) << "," << std::fixed
            << std::setprecision(3) << trip.releaseTime << "," << trip.finishTime << ","
            << trip.elapsed() << "," << std::setprecision(1) << trip.distanceKm << ","
            << trip.stops.size() << "," << std::setprecision(2) << trip.travelCost << ","
            << trip.energyCost << "," << std::setprecision(3) << trip.waitHours << ","
            << trip.chargeHours << "," << trip.drivingHours << "," << std::setprecision(2)
            << trip.generalisedCost(valueOfTime) << "," << csv::escape(names) << ","
            << csv::escape(trip.failure) << "\n";
    }
    std::cout << "wrote trip records to " << path << "\n";
}

int cmdSimulateEvents(const Options& options) {
    const Network network = loadNetwork(options);
    const Router router(network);
    const auto demands = loadDemands(options);
    const auto planner = makePlanner(options.policy, network, router, options.valueOfTime,
                                     makeSimulatorConfig(options).feasibility());

    const Simulator simulator(network, router, makeSimulatorConfig(options));
    StationRuntime runtime(network, options.stopOverhead);
    const auto trips = simulator.run(demands, *planner, runtime);
    const TimedSummary summary = simulator.summarise(trips, runtime, planner->name());

    std::cout << "Fleet of " << demands.size() << " over " << options.networkDir
              << " (event-driven, " << static_cast<int>(options.speedKmh) << " km/h)\n\n";
    printTimedHeader();
    printTimedRow(summary);

    const Hours horizon = options.horizon > 0.0 ? options.horizon : summary.makespan;
    std::cout << "\nStation load over " << std::fixed << std::setprecision(1) << horizon
              << " simulated hours\n"
              << std::left << std::setw(18) << "station" << std::right << std::setw(10) << "chargers"
              << std::setw(10) << "served" << std::setw(11) << "peak Q" << std::setw(13)
              << "mean wait" << std::setw(13) << "max wait" << std::setw(10) << "util\n";
    std::cout << std::string(85, '-') << "\n";
    for (const NodeId id : network.stationNodes()) {
        const auto& records = runtime.records(id);
        if (records.empty() && !options.verbose) continue;
        Hours totalWait = 0.0;
        Hours worstWait = 0.0;
        for (const auto& record : records) {
            totalWait += record.wait();
            worstWait = std::max(worstWait, record.wait());
        }
        const auto [peak, when] = runtime.peakWaiting(id);
        (void)when;
        std::ostringstream util;
        util << std::fixed << std::setprecision(0) << runtime.utilisation(id, horizon) * 100.0 << "%";
        std::cout << std::left << std::setw(18) << network.node(id).name << std::right
                  << std::setw(10) << network.node(id).station->chargers << std::setw(10)
                  << records.size() << std::setw(11) << peak << std::setw(13)
                  << hoursText(records.empty() ? 0.0 : totalWait / static_cast<double>(records.size()))
                  << std::setw(13) << hoursText(worstWait) << std::setw(10) << util.str() << "\n";
    }

    if (summary.stranded > 0) {
        std::cout << "\nStranded vehicles (" << summary.stranded << ")\n";
        int shown = 0;
        for (const auto& trip : trips) {
            if (trip.completed) continue;
            if (++shown > (options.verbose ? summary.stranded : 5)) {
                std::cout << "  ... and " << (summary.stranded - shown + 1) << " more\n";
                break;
            }
            std::cout << "  demand " << trip.demandId << ": " << trip.failure << "\n";
        }
    }

    if (!options.timeSeriesPath.empty()) {
        writeTimeSeries(options.timeSeriesPath, network, runtime, horizon, options.sampleInterval);
    }
    if (!options.tripsPath.empty()) {
        writeTrips(options.tripsPath, network, trips, options.valueOfTime);
    }
    return 0;
}

int cmdCompareEvents(const Options& options) {
    const Network network = loadNetwork(options);
    const Router router(network);
    const auto demands = loadDemands(options);
    const SimulatorConfig config = makeSimulatorConfig(options);
    const Simulator simulator(network, router, config);

    const bool asCsv = options.format == "csv";
    if (asCsv) {
        std::cout << "planner,demands,completed,stranded,mean_money,mean_wait_h,p95_wait_h,"
                     "max_wait_h,mean_generalised,mean_stops,mean_elapsed_h,peak_waiting,"
                     "peak_utilisation,busiest_station\n";
    } else {
        std::cout << "Fleet of " << demands.size() << " over " << options.networkDir
                  << ", event-driven, time valued at " << money(options.valueOfTime) << "/h\n\n";
        printTimedHeader();
    }

    std::map<std::string, TimedSummary> summaries;
    for (const auto& name : plannerNames()) {
        const auto planner = makePlanner(name, network, router, options.valueOfTime,
                                         config.feasibility());
        StationRuntime runtime(network, options.stopOverhead);
        const auto trips = simulator.run(demands, *planner, runtime);
        const TimedSummary summary = simulator.summarise(trips, runtime, name);
        summaries.emplace(name, summary);
        if (asCsv) {
            std::cout << summary.planner << "," << summary.demands << "," << summary.completed << ","
                      << summary.stranded << "," << std::fixed << std::setprecision(4)
                      << summary.meanMoneyCost << "," << summary.meanWaitHours << ","
                      << summary.p95WaitHours << "," << summary.maxWaitHours << ","
                      << summary.meanGeneralisedCost << "," << summary.meanStops << ","
                      << summary.meanElapsedHours << "," << summary.peakWaiting << ","
                      << summary.peakUtilisation << "," << csv::escape(summary.busiestStation)
                      << "\n";
        } else {
            printTimedRow(summary);
        }
    }
    if (asCsv) return 0;

    const auto bestBy = [&](auto projection) {
        return std::min_element(summaries.begin(), summaries.end(),
                                [&](const auto& a, const auto& b) {
                                    return projection(a.second) < projection(b.second);
                                })
            ->first;
    };
    std::cout << "\nlowest money cost   " << bestBy([](const TimedSummary& s) { return s.meanMoneyCost; })
              << "\nlowest mean wait    " << bestBy([](const TimedSummary& s) { return s.meanWaitHours; })
              << "\nlowest p95 wait     " << bestBy([](const TimedSummary& s) { return s.p95WaitHours; })
              << "\nlowest generalised  "
              << bestBy([](const TimedSummary& s) { return s.meanGeneralisedCost; })
              << "\nfewest stranded     "
              << bestBy([](const TimedSummary& s) { return static_cast<double>(s.stranded); })
              << "\n";
    return 0;
}

int cmdSimulateStatic(const Options& options) {
    const Network network = loadNetwork(options);
    const Router router(network);
    const auto demands = loadDemands(options);
    const auto policy = makePolicy(options.policy, options.valueOfTime);

    const Allocator allocator(network, router, makeConfig(options));
    StationState state(network);
    const auto results = allocator.run(demands, *policy, state);
    const Summary summary = allocator.summarise(results, state, policy->name());

    std::cout << "Fleet of " << demands.size() << " over " << options.networkDir << "\n\n";
    printSummaryHeader();
    printSummaryRow(summary);

    std::cout << "\nStation load\n"
              << std::left << std::setw(18) << "station" << std::right << std::setw(10) << "chargers"
              << std::setw(9) << "queue" << std::setw(14) << "exp. wait\n";
    std::cout << std::string(51, '-') << "\n";
    for (const NodeId id : network.stationNodes()) {
        const int queue = state.queueLength(id);
        if (queue == 0 && !options.verbose) continue;
        std::cout << std::left << std::setw(18) << network.node(id).name << std::right << std::setw(10)
                  << network.node(id).station->chargers << std::setw(9) << queue << std::setw(14)
                  << hoursText(state.expectedWait(id)) << "\n";
    }

    if (summary.stranded > 0) {
        std::cout << "\nStranded vehicles (" << summary.stranded << ")\n";
        int shown = 0;
        for (const auto& result : results) {
            if (result.completed) continue;
            if (++shown > (options.verbose ? summary.stranded : 5)) {
                std::cout << "  ... and " << (summary.stranded - shown + 1) << " more\n";
                break;
            }
            std::cout << "  demand " << result.demandId << ": " << result.failure << "\n";
        }
    }
    printWaitCaveat();
    if (options.verbose) {
        std::cout << "Dijkstra runs: " << router.computations() << " (one per distinct origin, cached)\n";
    }
    return 0;
}

int cmdCompareStatic(const Options& options) {
    const Network network = loadNetwork(options);
    const Router router(network);
    const auto demands = loadDemands(options);
    const Allocator allocator(network, router, makeConfig(options));

    const bool asCsv = options.format == "csv";
    if (asCsv) {
        std::cout << "planner,demands,completed,stranded,mean_money,mean_wait_h,p95_wait_h,"
                     "mean_generalised,mean_stops,peak_queue,peak_station\n";
    } else {
        std::cout << "Fleet of " << demands.size() << " over " << options.networkDir
                  << ", time valued at " << money(options.valueOfTime) << "/h\n\n";
        printSummaryHeader();
    }

    std::map<std::string, Summary> summaries;
    for (const auto& name : policyNames()) {
        const auto policy = makePolicy(name, options.valueOfTime);
        StationState state(network);
        const auto results = allocator.run(demands, *policy, state);
        const Summary summary = allocator.summarise(results, state, name);
        summaries.emplace(name, summary);
        if (asCsv) {
            std::cout << summary.policy << "," << summary.demands << "," << summary.completed << ","
                      << summary.stranded << "," << std::fixed << std::setprecision(4)
                      << summary.meanMoneyCost << "," << summary.meanWaitHours << ","
                      << summary.p95WaitHours << "," << summary.meanGeneralisedCost << ","
                      << summary.meanStops << "," << summary.peakQueue << ","
                      << csv::escape(summary.peakStation) << "\n";
        } else {
            printSummaryRow(summary);
        }
    }
    if (asCsv) return 0;

    // The comparison only tells a story if the reader can see which policy won
    // on which axis, so state it rather than leaving it to be squinted at.
    const auto bestBy = [&](auto projection) {
        return std::min_element(summaries.begin(), summaries.end(),
                                [&](const auto& a, const auto& b) {
                                    return projection(a.second) < projection(b.second);
                                })
            ->first;
    };
    std::cout << "\nlowest money cost   " << bestBy([](const Summary& s) { return s.meanMoneyCost; })
              << "\nlowest mean wait    " << bestBy([](const Summary& s) { return s.meanWaitHours; })
              << "\nlowest p95 wait     " << bestBy([](const Summary& s) { return s.p95WaitHours; })
              << "\nlowest peak queue   " << bestBy([](const Summary& s) {
                     return static_cast<double>(s.peakQueue);
                 })
              << "\nlowest generalised  " << bestBy([](const Summary& s) { return s.meanGeneralisedCost; })
              << "\n";
    printWaitCaveat();
    return 0;
}

int cmdSite(const Options& options) {
    const Network network = loadNetwork(options);
    const auto demands = loadDemands(options);
    const auto policy = makePolicy(options.policy, options.valueOfTime);

    const Siting siting(network, makeConfig(options));
    const Summary base = siting.baseline(demands, *policy);

    Station prototype;
    prototype.pricePerKwh = options.newStationPrice;
    prototype.chargers = options.newStationChargers;
    prototype.powerKw = options.newStationPower;

    const auto scores = siting.rank(demands, *policy, prototype, options.top);

    std::cout << "Siting a new station (" << prototype.chargers << " chargers, "
              << static_cast<int>(prototype.powerKw) << " kW, " << money(prototype.pricePerKwh)
              << "/kWh) for a fleet of " << demands.size() << "\n"
              << "Policy: " << policy->name() << ", time valued at " << money(options.valueOfTime)
              << "/h\n\n";

    printSummaryHeader();
    printSummaryRow(base);
    std::cout << "\nCandidate sites, best first\n"
              << std::left << std::setw(18) << "site" << std::right << std::setw(10) << "completed"
              << std::setw(14) << "mean gen $" << std::setw(12) << "vs base" << std::setw(12)
              << "mean wait" << std::setw(9) << "peak Q" << std::setw(11) << "uptake\n";
    std::cout << std::string(86, '-') << "\n";
    for (const auto& score : scores) {
        const Dollars delta = score.meanGeneralisedCost - base.meanGeneralisedCost;
        std::ostringstream deltaText;
        deltaText << (delta <= 0 ? "" : "+") << std::fixed << std::setprecision(2) << delta;
        std::ostringstream uptake;
        uptake << std::fixed << std::setprecision(0) << score.adoptionShare * 100.0 << "%";
        std::cout << std::left << std::setw(18) << score.name << std::right << std::setw(10)
                  << score.completed << std::setw(14) << money(score.meanGeneralisedCost)
                  << std::setw(12) << deltaText.str() << std::setw(12)
                  << hoursText(score.meanWaitHours) << std::setw(9) << score.peakQueue
                  << std::setw(11) << uptake.str() << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    try {
        if (options.command == "inspect") return cmdInspect(options);
        if (options.command == "route") return cmdRoute(options);
        if (options.command == "simulate") {
            if (options.engine == "events") return cmdSimulateEvents(options);
            if (options.engine == "static") return cmdSimulateStatic(options);
            fail("unknown engine '" + options.engine + "' (expected events or static)");
        }
        if (options.command == "compare") {
            if (options.engine == "events") return cmdCompareEvents(options);
            if (options.engine == "static") return cmdCompareStatic(options);
            fail("unknown engine '" + options.engine + "' (expected events or static)");
        }
        if (options.command == "site") return cmdSite(options);
        std::cerr << "error: unknown command '" << options.command << "'\n\n";
        printUsage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
