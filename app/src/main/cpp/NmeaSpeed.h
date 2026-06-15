#pragma once

// ADR 007 — pure $GPRMC/$GNRMC field-7 ground-speed parser (host-testable, no Android deps).
//
// RMC comma-separated fields: 0 talker ($G?RMC), 1 UTC, 2 status (A=valid/V=void),
// 3 lat, 4 N/S, 5 lon, 6 E/W, 7 speed (KNOTS), 8 course, 9 date, ... [*checksum].
// Field 7 ground speed is Doppler-derived on most receivers. Returns m/s (knots
// ×0.514444) only when status == 'A' and field 7 parses; invalid otherwise.
//
// NOTE: the *real* speedup (multi-Hz delivery) is a device property proven by the
// ADR 007 M0 NMEA-rate probe; this parser is just the pure decode step.

#include <cmath>
#include <cstdlib>
#include <cstring>

struct NmeaSpeedResult {
    bool  valid;
    float speedMps;
};

inline NmeaSpeedResult parseRmcSpeedMps(const char* sentence) {
    constexpr float KNOTS_TO_MPS = 0.514444f;
    if (!sentence || sentence[0] != '$') return {false, 0.0f};
    // Any talker: $GPRMC, $GNRMC, $GLRMC … — "RMC" sits at offset 3.
    if (std::strlen(sentence) < 6 || std::strncmp(sentence + 3, "RMC", 3) != 0) {
        return {false, 0.0f};
    }

    int         field    = 0;     // field 0 = talker token
    const char* start    = sentence;
    char        status   = 0;
    bool        haveSpeed = false;
    float       knots    = 0.0f;

    for (const char* c = sentence; ; ++c) {
        if (*c == ',' || *c == '\0') {
            const int len = static_cast<int>(c - start);
            if (field == 2 && len > 0) {
                status = start[0];
            } else if (field == 7 && len > 0) {
                char buf[32];
                const int n = len < 31 ? len : 31;
                std::memcpy(buf, start, n);
                buf[n] = '\0';
                knots = static_cast<float>(std::atof(buf));
                haveSpeed = true;
            }
            if (*c == '\0') break;
            ++field;
            start = c + 1;
        }
    }

    if (status != 'A' || !haveSpeed || !std::isfinite(knots) || knots < 0.0f) {
        return {false, 0.0f};
    }
    return {true, knots * KNOTS_TO_MPS};
}
