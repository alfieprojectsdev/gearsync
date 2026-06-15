// ADR 007 throwaway spike — proves the $GxRMC field-7 speed parse off-device.
// The actual win (multi-Hz NMEA delivery) is a device property, validated by the
// M0 NMEA-rate probe; this only validates the pure decode.
//
// Build: g++ -std=c++17 -I.. spike_nmea_parser_host.cpp -o spike_nmea_parser
// Run:   ./spike_nmea_parser

#include "../NmeaSpeed.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    // 022.4 knots × 0.514444 = 11.5235 m/s
    auto a = parseRmcSpeedMps(
        "$GPRMC,110415.00,A,1438.9213,N,12103.4412,E,022.4,180.0,150626,,,A*6F");
    assert(a.valid && std::fabs(a.speedMps - 11.5235f) < 0.01f);
    std::printf("PASS valid GPRMC: %.4f m/s\n", a.speedMps);

    // $GN talker also accepted
    auto b = parseRmcSpeedMps(
        "$GNRMC,110416.00,A,1438.9213,N,12103.4412,E,000.0,180.0,150626,,,A*7A");
    assert(b.valid && std::fabs(b.speedMps - 0.0f) < 0.001f);
    std::printf("PASS valid GNRMC zero speed: %.4f m/s\n", b.speedMps);

    // status 'V' (void / no fix) → invalid
    auto v = parseRmcSpeedMps(
        "$GPRMC,110415.00,V,1438.9213,N,12103.4412,E,022.4,180.0,150626,,,N*0F");
    assert(!v.valid);
    std::printf("PASS void status rejected\n");

    // empty speed field → invalid
    auto e = parseRmcSpeedMps(
        "$GPRMC,110415.00,A,1438.9213,N,12103.4412,E,,180.0,150626,,,A*00");
    assert(!e.valid);
    std::printf("PASS empty speed rejected\n");

    // non-RMC sentence → invalid
    auto g = parseRmcSpeedMps(
        "$GPGGA,110415.00,1438.9213,N,12103.4412,E,1,08,0.9,545.4,M,46.9,M,,*47");
    assert(!g.valid);
    std::printf("PASS non-RMC rejected\n");

    std::printf("All NMEA parser spike checks passed.\n");
    return 0;
}
