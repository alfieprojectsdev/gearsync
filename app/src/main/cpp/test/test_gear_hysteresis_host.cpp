// Host tests for ADR 008 gear-display hysteresis.
// Build: g++ -std=c++17 -I.. test_gear_hysteresis_host.cpp -o test_gear_hysteresis
// Run:   ./test_gear_hysteresis

#include "../GearHysteresis.h"

#include <cassert>
#include <cstdio>

int main() {
    const int N = 3;

    // Commits only after N consecutive consistent classifications.
    {
        GearHysteresis h;
        assert(h.update(2, false, N) == -1);   // count 1
        assert(h.update(2, false, N) == -1);   // count 2
        assert(h.update(2, false, N) == 2);    // count 3 → commit gear 2
        std::printf("PASS commit_after_n_consecutive\n");
    }

    // A single-frame neighbour flip never reaches the display.
    {
        GearHysteresis h;
        for (int i = 0; i < N; ++i) h.update(2, false, N);   // committed = 2
        assert(h.update(1, false, N) == 2);   // one stray frame → still 2
        assert(h.update(2, false, N) == 2);   // back to 2 → still 2
        std::printf("PASS single_flip_rejected\n");
    }

    // A genuine, sustained change does commit after N frames.
    {
        GearHysteresis h;
        for (int i = 0; i < N; ++i) h.update(2, false, N);   // committed = 2
        assert(h.update(3, false, N) == 2);
        assert(h.update(3, false, N) == 2);
        assert(h.update(3, false, N) == 3);   // sustained → commit 3
        std::printf("PASS sustained_change_commits\n");
    }

    // Transient freezes the committed gear and resets the candidate count.
    {
        GearHysteresis h;
        for (int i = 0; i < N; ++i) h.update(2, false, N);   // committed = 2
        assert(h.update(4, true, N) == 2);    // transient: hold 2, ignore the 4
        assert(h.update(4, false, N) == 2);   // count restarts at 1
        assert(h.update(4, false, N) == 2);   // 2
        assert(h.update(4, false, N) == 4);   // 3 → now commits
        std::printf("PASS transient_holds_and_resets\n");
    }

    // stableFrames == 1 is a passthrough (no hysteresis).
    {
        GearHysteresis h;
        assert(h.update(2, false, 1) == 2);
        assert(h.update(3, false, 1) == 3);
        std::printf("PASS passthrough_when_n_is_1\n");
    }

    // A single stray "unknown" (-1) frame (mid-shift/declutch) does not drop the
    // committed gear; a *sustained* run of -1 commits to unknown after N frames.
    {
        GearHysteresis h;
        for (int i = 0; i < N; ++i) h.update(2, false, N);   // committed = 2
        assert(h.update(-1, false, N) == 2);   // one stray unknown → still 2
        assert(h.update(2, false, N) == 2);    // back to 2
        assert(h.update(-1, false, N) == 2);   // sustained unknown begins, count 1
        assert(h.update(-1, false, N) == 2);   // count 2
        assert(h.update(-1, false, N) == -1);  // count 3 → commit unknown
        std::printf("PASS unknown_flip_rejected_then_sustained_commits\n");
    }

    // From committed unknown, a sustained real gear commits normally.
    {
        GearHysteresis h;   // committed starts at -1
        assert(h.update(3, false, N) == -1);
        assert(h.update(3, false, N) == -1);
        assert(h.update(3, false, N) == 3);    // sustained → commit 3
        std::printf("PASS recovers_from_unknown_to_gear\n");
    }

    std::printf("All gear hysteresis tests passed.\n");
    return 0;
}
