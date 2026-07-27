#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "clock_math.h"

static void expect_between(const char *name, int64_t actual,
                           int64_t minimum, int64_t maximum)
{
    if (actual < minimum || actual > maximum) {
        fprintf(stderr,
                "%s: got %" PRId64 ", expected [%" PRId64 ", %" PRId64 "]\n",
                name, actual, minimum, maximum);
        assert(0);
    }
}

static void test_requested_rates(void)
{
    assert(clock_math_correct_elapsed_us(100000, 1000000, 1) == 100000);

    /* Integer microseconds truncate the ideal 99,999.500... result. */
    expect_between("100 ms at +5 ppm",
                   clock_math_correct_elapsed_us(100000, 1000005, 1),
                   99999, 100000);

    /* Integer microseconds truncate the ideal 499,997.500... result. */
    expect_between("500 ms at +5 ppm",
                   clock_math_correct_elapsed_us(500000, 1000005, 1),
                   499997, 499998);

    expect_between("999 ms at +5 ppm",
                   clock_math_correct_elapsed_us(999000, 1000005, 1),
                   998995, 998996);
}

static void test_rolling_window_units(void)
{
    /* The production estimator stores a 32-second total, not a one-second rate. */
    assert(clock_math_correct_elapsed_us(100000, 32000160, 32) == 99999);
    assert(clock_math_correct_elapsed_us(500000, 32000160, 32) == 499997);
    assert(clock_math_correct_elapsed_us(999000, 32000160, 32) == 998995);
}

static void test_monotonicity_between_anchors(void)
{
    int64_t previous = -1;
    for (int64_t elapsed = 0; elapsed < 1000005; elapsed += 37) {
        int64_t disciplined =
            clock_math_correct_elapsed_us(elapsed, 32000160, 32);
        assert(disciplined >= previous);
        assert(disciplined <= elapsed);
        previous = disciplined;
    }
}

static void test_ntp_fraction(void)
{
    assert(clock_math_ntp_fraction_from_us(0) == 0x00000000U);
    assert(clock_math_ntp_fraction_from_us(500000) == 0x80000000U);
    assert(clock_math_ntp_fraction_from_us(999999) == 0xffffef39U);
}

int main(void)
{
    test_requested_rates();
    test_rolling_window_units();
    test_monotonicity_between_anchors();
    test_ntp_fraction();
    puts("clock_math_test: PASS");
    return 0;
}

