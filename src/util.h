#ifndef PI5LA_UTIL_H
#define PI5LA_UTIL_H

#include <stdbool.h>
#include <stdint.h>

/* RP1 PIO state machine clock (piolib pio_rp1.c: rp1_clock_get_hz). */
#define PI5LA_PIO_CLOCK_HZ 200000000.0

/* Clock divider in piolib's int + 1/256 frac form. div_int == 0 is not used. */
typedef struct {
    uint16_t div_int;
    uint8_t div_frac;
} clkdiv_t;

/*
 * Parse a number with an optional k / M suffix ("10M", "115.2k", "4000").
 * Returns false on malformed input or negative values.
 */
bool parse_si(const char *s, double *out);

/* As parse_si, but the result must be a non-negative integer. */
bool parse_si_u64(const char *s, uint64_t *out);

/* Divider for a PIO SM running at freq_hz SM cycles per second. */
bool clkdiv_for_hz(double freq_hz, clkdiv_t *out);

/* SM cycles per second for a divider. */
double clkdiv_hz(clkdiv_t div);

/* Divider value as a real number. */
double clkdiv_value(clkdiv_t div);

/* True when div_frac == 0 (no fractional-divider jitter). */
bool clkdiv_is_integer(clkdiv_t div);

double monotonic_s(void);

#endif
