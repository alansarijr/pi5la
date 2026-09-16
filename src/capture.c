#include "capture.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "la8.pio.h"
#include "la8_trig.pio.h"
#include "dmacyclic.h"
#include "fdebug.h"
#include "unpack.h"


static bool same_program(const la_prog_t *built, const pio_program_t *ref, uint wrap_target,
                         uint wrap)
{
    return built->length == ref->length && built->wrap_target == wrap_target &&
           built->wrap == wrap &&
           !memcmp(built->instr, ref->instructions, ref->length * sizeof(uint16_t));
}

bool capture_selftest_programs(void)
{
    la_prog_t p;

    if (!la_build_program(8, TRIG_NONE, 0, &p) ||
        !same_program(&p, &la8_program, la8_wrap_target, la8_wrap)) {
        fprintf(stderr, "capture: runtime program differs from pioasm la8\n");
        return false;
    }
    if (!la_build_program(8, TRIG_HIGH, 0, &p) ||
        !same_program(&p, &la8_trig_program, la8_trig_wrap_target, la8_trig_wrap)) {
        fprintf(stderr, "capture: runtime program differs from pioasm la8_trig\n");
        return false;
    }
    return true;
}

/* 1 if RXSTALL was set for sm (it is then cleared), 0 if not, -1 on error. */
static int take_rxstall(int fd, uint sm)
{
    uint32_t v;

    if (fdebug_read(fd, &v)) {
        fprintf(stderr, "capture: reading FDEBUG failed: %s\n", strerror(errno));
        return -1;
    }
    if (!(v & FDEBUG_RXSTALL(sm)))
        return 0;
    if (fdebug_clear(fd, FDEBUG_RXSTALL(sm))) {
        fprintf(stderr, "capture: clearing FDEBUG failed: %s\n", strerror(errno));
        return -1;
    }
    return 1;
}

static bool stopped(const capture_config_t *cfg)
{
    return cfg->stop && *cfg->stop;
}

static void finish_estimate(const capture_config_t *cfg, capture_result_t *res)
{
    unsigned spw = unpack_samples_per_word(cfg->channels);
    double got, expected;

    if (res->steady_bytes == 0 || res->steady_s <= 0)
        return;
    got = (double)(res->steady_bytes / 4) * spw;
    expected = res->rate_hz * res->steady_s;
    res->est_lost_samples = llround(expected - got);
    res->est_valid = true;
}

/*
 * Cyclic DMA (see dmacyclic.h): the kernel ring is configured and running
 * before the SM is enabled; each read returns the next completed period.
 * Because DMA never stops draining the FIFO, the SM does not stall when the
 * capture ends, so FDEBUG can be checked after the last period without a
 * false positive.
 */
static int run_dma(PIO pio, uint sm, int fd, const capture_config_t *cfg,
                   capture_result_t *res, size_t capacity_bytes)
{
    size_t total = res->n_words * 4;
    size_t period = cfg->dma_buf_bytes;
    size_t got = 0;
    double t_enable, t_first = 0, t_last = 0;
    int stall;

    pio_sm_set_enabled(pio, sm, true);
    t_enable = monotonic_s();

    while (got < total) {
        if (got + period > capacity_bytes) {
            fprintf(stderr, "capture: internal error: buffer too small<NL>");
            return -1;
        }
        if (dmacyclic_read(fd, sm, (uint32_t)period, (uint8_t *)res->words + got)) {
            if (errno == EOVERFLOW) {
                res->overflow = true;
                break;
            }
            if (stopped(cfg)) {
                res->interrupted = true;
                break;
            }
            fprintf(stderr, "capture: DMA receive failed: %s<NL>", strerror(errno));
            return -1;
        }
        t_last = monotonic_s();
        if (got == 0)
            t_first = t_last;
        got += period;

        stall = take_rxstall(fd, sm);
        if (stall < 0)
            return -1;
        res->stall_events += (uint64_t)stall;

        if (stopped(cfg)) {
            res->interrupted = true;
            break;
        }
    }

    /* The final period may run past the requested samples. */
    res->bytes = got < total ? got : total;
    res->total_s = t_last > 0 ? t_last - t_enable : 0;
    if (got > period) {
        res->steady_bytes = (got < total ? got : total) - period;
        res->steady_s = t_last - t_first;
    }
    return 0;
}

/*
 * Milestone 2 baseline: one PCIe round trip per FIFO word. Deliberately slow.
 * RXSTALL is read once, straight after the last word.
 */
static int run_naive(PIO pio, uint sm, int fd, const capture_config_t *cfg,
                     capture_result_t *res)
{
    double t_enable, t_first = 0, t_last = 0;
    size_t i;
    int stall;

    pio_sm_set_enabled(pio, sm, true);
    t_enable = monotonic_s();

    for (i = 0; i < res->n_words; i++) {
        if (stopped(cfg)) {
            res->interrupted = true;
            break;
        }
        res->words[i] = pio_sm_get_blocking(pio, sm);
        if (i == 0)
            t_first = monotonic_s();
    }
    t_last = monotonic_s();

    stall = take_rxstall(fd, sm);
    if (stall < 0)
        return -1;
    res->stall_events = (uint64_t)stall;

    res->bytes = (uint64_t)i * 4;
    res->total_s = t_last - t_enable;
    if (i > 1) {
        res->steady_bytes = (uint64_t)(i - 1) * 4;
        res->steady_s = t_last - t_first;
    }
    return 0;
}

int capture_run(PIO pio, const capture_config_t *cfg, capture_result_t *res)
{
    pio_program_t program;
    pio_sm_config c;
    la_prog_t lp;
    unsigned spw, k;
    size_t capacity_bytes;
    uint offset;
    int sm, ret, fd;

    memset(res, 0, sizeof(*res));

    if (!unpack_channels_valid(cfg->channels)) {
        fprintf(stderr, "capture: channels must be 1, 2, 4 or 8\n");
        return -1;
    }
    if (cfg->base_gpio + cfg->channels > CAPTURE_GPIO_COUNT) {
        fprintf(stderr, "capture: GPIO %u..%u is outside GPIO 0..%u\n", cfg->base_gpio,
                cfg->base_gpio + cfg->channels - 1, CAPTURE_GPIO_COUNT - 1);
        return -1;
    }
    if (cfg->samples == 0) {
        fprintf(stderr, "capture: sample count must be > 0\n");
        return -1;
    }
    if (cfg->mode == CAPTURE_DMA && (cfg->dma_buf_bytes < 4 || cfg->dma_buf_bytes % 4)) {
        fprintf(stderr, "capture: DMA buffer size must be a non-zero multiple of 4\n");
        return -1;
    }
    if (!capture_selftest_programs())
        return -1;
    if (!la_build_program(cfg->channels, cfg->trig, cfg->trig_pin, &lp)) {
        fprintf(stderr, "capture: invalid trigger (pin must be < channels)\n");
        return -1;
    }

    spw = unpack_samples_per_word(cfg->channels);
    res->n_words = (size_t)((cfg->samples + spw - 1) / spw);
    capacity_bytes = res->n_words * 4;
    /* DMA reads whole periods, so round the buffer up to a period multiple. */
    if (cfg->mode == CAPTURE_DMA)
        capacity_bytes = (capacity_bytes + cfg->dma_buf_bytes - 1) / cfg->dma_buf_bytes *
                         cfg->dma_buf_bytes;
    res->words = malloc(capacity_bytes);
    if (!res->words) {
        fprintf(stderr, "capture: cannot allocate %zu words\n", res->n_words);
        return -1;
    }
    res->rate_hz = clkdiv_hz(cfg->div);

    program.instructions = lp.instr;
    program.length = lp.length;
    program.origin = -1;
    program.pio_version = 0;

    fd = fdebug_open();
    if (fd < 0) {
        fprintf(stderr, "capture: opening /dev/pio0 for FDEBUG: %s\n", strerror(errno));
        return -1;
    }

    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        fprintf(stderr, "capture: no free PIO state machine\n");
        fdebug_close(fd);
        return -1;
    }
    offset = pio_add_program(pio, &program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "capture: no PIO program space\n");
        pio_sm_unclaim(pio, sm);
        fdebug_close(fd);
        return -1;
    }

    if (cfg->mode == CAPTURE_DMA &&
        dmacyclic_config(fd, (unsigned)sm, cfg->dma_buf_bytes, CAPTURE_DMA_BUF_COUNT) < 0) {
        fprintf(stderr, "capture: cyclic DMA config (%u x %u bytes) failed: %s\n",
                CAPTURE_DMA_BUF_COUNT, cfg->dma_buf_bytes, strerror(errno));
        pio_remove_program(pio, &program, offset);
        pio_sm_unclaim(pio, sm);
        fdebug_close(fd);
        return -1;
    }

    for (k = 0; k < cfg->channels; k++)
        pio_gpio_init(pio, cfg->base_gpio + k);
    pio_sm_set_consecutive_pindirs(pio, sm, cfg->base_gpio, cfg->channels, false);

    c = pio_get_default_sm_config_for_pio(pio);
    sm_config_set_wrap(&c, offset + lp.wrap_target, offset + lp.wrap);
    sm_config_set_in_pins(&c, cfg->base_gpio);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac(&c, cfg->div.div_int, cfg->div.div_frac);
    pio_sm_init(pio, sm, offset, &c);
    if (take_rxstall(fd, (uint)sm) < 0)
        ret = -1;
    else if (cfg->mode == CAPTURE_DMA)
        ret = run_dma(pio, (uint)sm, fd, cfg, res, capacity_bytes);
    else
        ret = run_naive(pio, (uint)sm, fd, cfg, res);

    pio_sm_set_enabled(pio, sm, false);
    pio_remove_program(pio, &program, offset);
    pio_sm_unclaim(pio, sm);
    fdebug_close(fd);

    if (ret == 0)
        finish_estimate(cfg, res);
    return ret;
}

void capture_result_free(capture_result_t *res)
{
    free(res->words);
    res->words = NULL;
    res->n_words = 0;
}
