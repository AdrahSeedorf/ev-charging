#pragma once

#include <cmath>

#include "evnet/units.hpp"

namespace evnet {

/// A point on the Earth, in decimal degrees.
struct Coordinate {
    double latitude{0.0};
    double longitude{0.0};
};

/// Great-circle distance between two points, via the haversine formula.
///
/// This matters for more than drawing maps. Straight-line distance is a HARD LOWER
/// BOUND on road distance -- no road can be shorter than the line it spans -- so an
/// edge whose recorded distance falls below it is not merely suspicious, it is
/// impossible. That gives the validator a check with no tuning parameter and no
/// judgement call, which is rare in data cleaning.
///
/// Mean Earth radius, so accurate to a few tenths of a percent at these scales --
/// far tighter than the errors it is used to detect.
inline Km greatCircleKm(const Coordinate& a, const Coordinate& b) {
    constexpr double kEarthRadiusKm = 6371.0088;
    const double lat1 = a.latitude * M_PI / 180.0;
    const double lat2 = b.latitude * M_PI / 180.0;
    const double dLat = lat2 - lat1;
    const double dLon = (b.longitude - a.longitude) * M_PI / 180.0;

    const double h = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
                     std::cos(lat1) * std::cos(lat2) * std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
    return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(std::min(1.0, h)));
}

/// How much further the road goes than the straight line. Roughly 1.1-1.4 for
/// highways and 1.3-1.6 for city streets; below 1.0 is impossible.
inline double detourRatio(Km roadKm, Km straightLineKm) {
    if (straightLineKm <= 0.0) return 1.0;  // coincident points: nothing to compare
    return roadKm / straightLineKm;
}

}  // namespace evnet
