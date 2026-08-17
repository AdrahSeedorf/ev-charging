#pragma once

// Unit vocabulary for the whole toolkit.
//
// The two projects this toolkit was merged from used incompatible unit systems:
// the Sydney metro study worked in kWh and dollars and never modelled time; the
// Hume corridor study worked in km of battery range and hours of waiting and
// never modelled money. Neither could express the trade-off the other cared
// about.
//
// Reconciling them needs exactly one physical constant -- energy consumption
// per distance -- which the two conversions below encode. Everything downstream
// (generalised cost, siting, policy comparison) becomes possible once range and
// energy are interchangeable.

namespace evnet {

// Semantic aliases. These are documentation, not type safety -- a strongly
// typed units library would catch mixing them up, but it would also make the
// arithmetic in the router considerably noisier for little gain at this scale.
using Km = double;
using Kwh = double;
using Kw = double;
using Hours = double;
using Dollars = double;
using KwhPer100Km = double;

/// Distance a vehicle can travel on `energy`, given its consumption rate.
inline Km rangeFromEnergy(Kwh energy, KwhPer100Km efficiency) {
    if (efficiency <= 0.0) return 0.0;
    return (energy / efficiency) * 100.0;
}

/// Energy required to travel `distance`, given a consumption rate.
inline Kwh energyForDistance(Km distance, KwhPer100Km efficiency) {
    return (distance / 100.0) * efficiency;
}

/// Wall-clock time to deliver `energy` at a given charger power.
inline Hours chargeDuration(Kwh energy, Kw powerKw) {
    if (powerKw <= 0.0) return 0.0;
    return energy / powerKw;
}

/// Time to cover `distance` at an average speed. Stage 1 had no notion of driving
/// taking time at all, which is why nothing spread out across a day.
inline Hours drivingTime(Km distance, double speedKmh) {
    if (speedKmh <= 0.0) return 0.0;
    return distance / speedKmh;
}

}  // namespace evnet
