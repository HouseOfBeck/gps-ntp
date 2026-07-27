#include "clock_math.h"

#include <limits.h>

int64_t clock_math_correct_elapsed_us(int64_t elapsed_monotonic_us,
                                      int64_t measured_elapsed_us,
                                      uint32_t measured_seconds)
{
    if (measured_elapsed_us <= 0 || measured_seconds == 0) {
        return 0;
    }

    /*
     * Conceptually:
     *
     *   elapsed * measured_seconds * 1,000,000 / measured_elapsed
     *
     * Split the quotient first. With the bounded 32-second PPS window the
     * remainder term is also safely inside int64_t.
     */
    int64_t whole_windows = elapsed_monotonic_us / measured_elapsed_us;
    int64_t remainder_us = elapsed_monotonic_us % measured_elapsed_us;

    if (whole_windows > INT64_MAX / (int64_t)measured_seconds / 1000000LL ||
        whole_windows < INT64_MIN / (int64_t)measured_seconds / 1000000LL) {
        return whole_windows < 0 ? INT64_MIN : INT64_MAX;
    }

    return whole_windows * (int64_t)measured_seconds * 1000000LL +
           (remainder_us * (int64_t)measured_seconds * 1000000LL) /
               measured_elapsed_us;
}

uint32_t clock_math_ntp_fraction_from_us(uint32_t microseconds)
{
    return (uint32_t)(((uint64_t)microseconds << 32) / 1000000ULL);
}

