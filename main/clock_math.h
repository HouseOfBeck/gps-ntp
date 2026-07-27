#pragma once

#include <stdint.h>

/*
 * Convert elapsed local monotonic microseconds to disciplined UTC
 * microseconds. measured_elapsed_us is the local timer advance measured
 * across measured_seconds true PPS intervals.
 */
int64_t clock_math_correct_elapsed_us(int64_t elapsed_monotonic_us,
                                      int64_t measured_elapsed_us,
                                      uint32_t measured_seconds);

/* Convert microseconds within a second to an unsigned NTP Q0.32 fraction. */
uint32_t clock_math_ntp_fraction_from_us(uint32_t microseconds);

