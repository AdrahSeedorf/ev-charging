#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>

#include "evnet/router.hpp"
#include "fixtures.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

TEST_CASE("Dijkstra reproduces hand-computed distances", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);

    // Direct A-B at 4 must beat the A-C-D-B detour at 6.
    CHECK_THAT(router.distance(0, 1), WithinAbs(4.0, 1e-9));
    CHECK_THAT(router.distance(0, 2), WithinAbs(2.0, 1e-9));
    CHECK_THAT(router.distance(0, 3), WithinAbs(3.0, 1e-9));
    CHECK_THAT(router.distance(0, 4), WithinAbs(13.0, 1e-9));
    CHECK_THAT(router.distance(0, 0), WithinAbs(0.0, 1e-9));
}

TEST_CASE("distances are symmetric on an undirected network", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);
    for (NodeId a = 0; a < 5; ++a) {
        for (NodeId b = 0; b < 5; ++b) {
            CHECK_THAT(router.distance(a, b), WithinAbs(router.distance(b, a), 1e-9));
        }
    }
}

TEST_CASE("unreachable nodes report infinity rather than a sentinel", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);

    // Node F has no edges. The legacy implementation used DBL_MAX here, which
    // meant any arithmetic on an unreachable distance silently overflowed.
    CHECK(router.distance(0, 5) == Router::kUnreachable);
    CHECK(router.path(0, 5).empty());

    // Infinity stays infinity under addition; DBL_MAX would have wrapped.
    CHECK(router.distance(0, 5) + 100.0 == Router::kUnreachable);
}

TEST_CASE("paths are reconstructed, not just measured", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);

    // The legacy Sydney project computed distances and discarded the route, so it
    // could name the cheapest station but never say how to reach it.
    const auto path = router.path(0, 4);
    REQUIRE(path.size() == 4);
    CHECK(network.node(path[0]).name == "A");
    CHECK(network.node(path[1]).name == "C");
    CHECK(network.node(path[2]).name == "D");
    CHECK(network.node(path[3]).name == "E");

    const auto trivial = router.path(2, 2);
    REQUIRE(trivial.size() == 1);
    CHECK(trivial[0] == 2);
}

TEST_CASE("reachableWithin replaces the corridor prefix sums", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);

    // From A: C at 2, D at 3, B at 4 -- and never A itself.
    auto within3 = router.reachableWithin(0, 3.0);
    std::sort(within3.begin(), within3.end());
    CHECK(within3 == std::vector<NodeId>{2, 3});

    auto within4 = router.reachableWithin(0, 4.0);
    std::sort(within4.begin(), within4.end());
    CHECK(within4 == std::vector<NodeId>{1, 2, 3});

    CHECK(router.reachableWithin(0, 1.0).empty());

    // Range covering everything still excludes the unreachable node.
    auto huge = router.reachableWithin(0, 1e9);
    CHECK(huge.size() == 4);
}

TEST_CASE("shortest-path trees are cached per source", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);

    CHECK(router.computations() == 0);
    router.distance(0, 1);
    CHECK(router.computations() == 1);

    // Repeated queries from the same origin must not recompute. This is the
    // property that removes the accidentally cubic behaviour from the legacy
    // siting search, which re-ran Dijkstra in its innermost loop.
    for (int i = 0; i < 50; ++i) {
        router.distance(0, i % 5);
        router.reachableWithin(0, 10.0);
        router.path(0, 4);
    }
    CHECK(router.computations() == 1);

    router.distance(3, 0);
    CHECK(router.computations() == 2);
}

TEST_CASE("router rejects unknown nodes", "[router]") {
    const Network network = testing::diamond();
    const Router router(network);
    CHECK_THROWS_AS(router.distance(0, 99), std::out_of_range);
    CHECK_THROWS_AS(router.distance(-1, 0), std::out_of_range);
}
