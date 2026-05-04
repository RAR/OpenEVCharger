#pragma once
#include <cstdint>

namespace esphome {
namespace openbhzd_tlv {

// 150-entry NTC raw → temperature LUT, extracted verbatim from the
// stock Rippleon firmware V1.0.066 at flash 0x08024f28..0x08025054.
//
// Hardware: 10 kΩ NTC pulldown to GND with 10 kΩ pullup to 3.3 V (NOT
// 4.7 kΩ as our pin-map pre-bench guess assumed). 12-bit ADC.
//
// Mapping: lut[idx] = raw_count_at(idx − 30 °C). Index 0 = −30 °C,
// index 149 = +119 °C, in 1 °C steps. Monotonic decreasing — as the
// thermistor heats, resistance drops, divider voltage drops, ADC
// count drops.
//
// Out-of-range semantics matching the stock firmware: raw > lut[0]
// (= 3889) means thermistor is colder than −30 °C (likely
// disconnected / open circuit); raw < lut[149] (= 158) means hotter
// than 119 °C (likely shorted / above range). Stock fw clamps both
// to 120 °C (= max user setpoint) which is treated as a hard fault.
//
// Cross-check anchors (from the agent's static analysis):
//   index 55 (= 25 °C) → raw 2048   ← perfect 50/50 divider on 10k+10k
//   index 115 (= 85 °C) → raw 396
//   index 125 (= 95 °C) → raw 300   ← stock factory trip threshold
constexpr uint16_t NTC_LUT[150] = {
    3889, 3876, 3862, 3848, 3833, 3817, 3801, 3784,  //  -30..-23 °C
    3766, 3747, 3727, 3707, 3686, 3663, 3640, 3616,  //  -22..-15 °C
    3592, 3566, 3539, 3511, 3483, 3453, 3423, 3392,  //  -14.. -7 °C
    3359, 3326, 3292, 3257, 3221, 3185, 3147, 3109,  //   -6.. +1 °C
    3070, 3030, 2990, 2948, 2906, 2864, 2821, 2777,  //   +2.. +9 °C
    2733, 2689, 2644, 2599, 2553, 2508, 2462, 2416,  //  +10..+17 °C
    2369, 2324, 2277, 2231, 2185, 2139, 2093, 2048,  //  +18..+25 °C
    2003, 1958, 1914, 1870, 1825, 1783, 1739, 1698,  //  +26..+33 °C
    1656, 1615, 1575, 1534, 1495, 1457, 1419, 1382,  //  +34..+41 °C
    1345, 1310, 1275, 1242, 1207, 1174, 1143, 1111,  //  +42..+49 °C
    1080, 1051, 1021,  993,  965,  938,  911,  886,  //  +50..+57 °C
     861,  835,  811,  790,  766,  744,  725,  702,  //  +58..+65 °C
     683,  663,  645,  625,  607,  589,  574,  556,  //  +66..+73 °C
     540,  525,  512,  497,  481,  468,  455,  442,  //  +74..+81 °C
     429,  419,  406,  396,  382,  372,  362,  352,  //  +82..+89 °C
     342,  335,  324,  314,  307,  300,  289,  282,  //  +90..+97 °C
     275,  268,  261,  254,  246,  239,  232,  228,  //  +98..+105 °C
     221,  214,  210,  202,  199,  195,  188,  184,  // +106..+113 °C
     180,  173,  169,  165,  161,  158,              // +114..+119 °C
};

// Convert a 12-bit NTC ADC raw count to a temperature in °C. Returns
// 120.0f for both out-of-range cases (open / shorted) — the stock
// firmware's safety-conservative interpretation. Linear interpolates
// between LUT entries for sub-degree resolution.
inline float ntc_raw_to_celsius(uint16_t raw) {
    // OOR: raw > LUT[0] is colder than −30 °C → likely open NTC;
    // raw < LUT[149] is hotter than +119 °C → likely shorted.
    // Both map to 120 °C per stock fw.
    if (raw > NTC_LUT[0] || raw < NTC_LUT[149]) return 120.0f;

    // LUT is monotonic decreasing; binary search for the highest
    // index whose raw is >= the input. Linear scan is fine here too
    // (150 entries, ~75 cmps amortised) but binary keeps it tidy.
    int lo = 0, hi = 149;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (NTC_LUT[mid] >= raw) lo = mid + 1;
        else                     hi = mid;
    }
    // lo = first index with raw < input → the temperature is one
    // cooler. Interpolate against the previous entry.
    if (lo == 0) return -30.0f;
    int idx_hi = lo;       // raw < NTC_LUT[idx_hi-1], raw >= NTC_LUT[idx_hi]
    int idx_lo = lo - 1;
    uint16_t r_hi_temp = NTC_LUT[idx_lo];   // higher raw → cooler temp
    uint16_t r_lo_temp = NTC_LUT[idx_hi];   // lower raw → hotter temp
    float frac = (float)(r_hi_temp - raw) / (float)(r_hi_temp - r_lo_temp);
    return (float)(idx_lo - 30) + frac;
}

}  // namespace openbhzd_tlv
}  // namespace esphome
