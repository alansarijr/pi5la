#define _POSIX_C_SOURCE 200809L
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>

bool parse_si(const char *s, double *out)
{
    char *end;
    double v;

    if (!s || !*s)
        return false;
    v = strtod(s, &end);
    if (end == s)
        return false;
    switch (*end) {
    case 'k':
        v *= 1e3;
        end++;
        break;
    case 'M':
        v *= 1e6;
        end++;
        break;
    default:
        break;
    }
    if (*end != '\0' || !isfinite(v) || v < 0)
        return false;
    *out = v;
    return true;
}

bool parse_si_u64(const char *s, uint64_t *out)
{
    double v;

    if (!parse_si(s, &v))
        return false;
    if (v != floor(v) || v > 18446744073709549568.0)
        return false;
    *out = (uint64_t)v;
    return true;
}

bool clkdiv_for_hz(double freq_hz, clkdiv_t *out)
{
    double div;
    long units;

    if (!(freq_hz > 0))
        return false;
    div = PI5LA_PIO_CLOCK_HZ / freq_hz;
    units = lround(div * 256.0);
    if (units < 256 || units > 65535L * 256 + 255)
        return false;
    out->div_int = (uint16_t)(units >> 8);
    out->div_frac = (uint8_t)(units & 0xff);
    return true;
}

double clkdiv_value(clkdiv_t div)
{
    return div.div_int + div.div_frac / 256.0;
}

double clkdiv_hz(clkdiv_t div)
{
    return PI5LA_PIO_CLOCK_HZ / clkdiv_value(div);
}

bool clkdiv_is_integer(clkdiv_t div)
{
    return div.div_frac == 0;
}

double monotonic_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
