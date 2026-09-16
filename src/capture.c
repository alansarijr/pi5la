#include "capture.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "la8.pio.h"
#include "la8_trig.pio.h"
#include "unpack.h"

#define QCAP (CAPTURE_DMA_BUF_COUNT + 1)

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
 * Rotating DMA read-ahead (rp1-pio on kernel 6.18+):
 *   pio_sm_xfer_data(..., len, NULL) queues a receive of len bytes and returns;
 *   pio_sm_xfer_data(..., len, buf) waits for the oldest queued buffer and copies it.
 * Up to CAPTURE_DMA_BUF_COUNT receives are kept queued so the SM always has a
 * DMA destination while user space copies a completed buffer out.
 *
 * One extra "guard" buffer is queued after the data. It keeps the FIFO drained
 * while the RXSTALL flag is checked after the final data buffer, so the stall
 * that inevitably follows the end of the capture is not counted.
 */
static int run_dma(PIO pio, uint sm, const capture_config_t *cfg, capture_result_t *res)
{
    size_t total = res->n_words * 4;
    size_t buf = cfg->dma_buf_bytes;
    size_t submitted = 0, got = 0, first_len = 0;
    size_t queue[QCAP];
    unsigned q_head = 0, q_len = 0;
    bool guard_queued = false;
    double t_enable, t_first = 0, t_last = 0;

    while (q_len < CAPTURE_DMA_BUF_COUNT && !guard_queued) {
        size_t len = submitted < total ? (total - submitted < buf ? total - submitted : buf) : buf;

        if (pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, (uint)len, NULL) < 0) {
            fprintf(stderr, "capture: queueing DMA receive failed: %s\n", strerror(errno));
            return -1;
        }
        queue[(q_head + q_len++) % QCAP] = len;
        if (submitted < total)
            submitted += len;
        else
            guard_queued = true;
    }

    pio_sm_set_enabled(pio, sm, true);
    t_enable = monotonic_s();

    while (got < total) {
        size_t len = queue[q_head];

        if (pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, (uint)len,
                             (uint8_t *)res->words + got) < 0) {
            if (stopped(cfg)) {
                res->interrupted = true;
                break;
            }
            fprintf(stderr, "capture: DMA receive failed: %s\n", strerror(errno));
            return -1;
        }
        t_last = monotonic_s();
        q_head = (q_head + 1) % QCAP;
        q_len--;

        if (got == 0) {
            t_first = t_last;
            first_len = len;
        }
        got += len;

        if (pio_sm_get_flags(pio, sm, PIO_SM_FLAG_RXSTALL, true) & PIO_SM_FLAG_RXSTALL)
            res->stall_events++;

        if (!guard_queued) {
            size_t next = submitted < total
                              ? (total - submitted < buf ? total - submitted : buf)
                              : buf;

            if (pio_sm_xfer_data(pio, sm, PIO_DIR_FROM_SM, (uint)next, NULL) < 0) {
                fprintf(stderr, "capture: queueing DMA receive failed: %s\n", strerror(errno));
                return -1;
            }
            queue[(q_head + q_len++) % QCAP] = next;
            if (submitted < total)
                submitted += next;
            else
                guard_queued = true;
        }

        if (stopped(cfg)) {
            res->interrupted = true;
            break;
        }
    }

    res->bytes = got;
    res->total_s = t_last > 0 ? t_last - t_enable : 0;
    if (got > first_len) {
        res->steady_bytes = got - first_len;
        res->steady_s = t_last - t_first;
    }
    return 0;
}

/*
 * Milestone 2 baseline: one PCIe round trip per FIFO word. Deliberately slow.
 * RXSTALL is read once, straight after the last word.
 */
static int run_naive(PIO pio, uint sm, const capture_config_t *cfg, capture_result_t *res)
{
    double t_enable, t_first = 0, t_last = 0;
    size_t i;

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

    if (pio_sm_get_flags(pio, sm, PIO_SM_FLAG_RXSTALL, true) & PIO_SM_FLAG_RXSTALL)
        res->stall_events = 1;

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
    uint offset;
    int sm, ret;

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
    res->words = malloc(res->n_words * sizeof(uint32_t));
    if (!res->words) {
        fprintf(stderr, "capture: cannot allocate %zu words\n", res->n_words);
        return -1;
    }
    res->rate_hz = clkdiv_hz(cfg->div);

    program.instructions = lp.instr;
    program.length = lp.length;
    program.origin = -1;
    program.pio_version = 0;

    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        fprintf(stderr, "capture: no free PIO state machine\n");
        return -1;
    }
    offset = pio_add_program(pio, &program);
    if (offset == PIO_ORIGIN_INVALID) {
        fprintf(stderr, "capture: no PIO program space\n");
        pio_sm_unclaim(pio, sm);
        return -1;
    }

    if (cfg->mode == CAPTURE_DMA &&
        pio_sm_config_xfer(pio, sm, PIO_DIR_FROM_SM, cfg->dma_buf_bytes,
                           CAPTURE_DMA_BUF_COUNT) < 0) {
        fprintf(stderr, "capture: pio_sm_config_xfer(%u x %u bytes) failed: %s\n",
                CAPTURE_DMA_BUF_COUNT, cfg->dma_buf_bytes, strerror(errno));
        pio_remove_program(pio, &program, offset);
        pio_sm_unclaim(pio, sm);
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
    pio_sm_clear_flags(pio, sm, PIO_SM_FLAG_ALL);

    if (cfg->mode == CAPTURE_DMA)
        ret = run_dma(pio, (uint)sm, cfg, res);
    else
        ret = run_naive(pio, (uint)sm, cfg, res);

    pio_sm_set_enabled(pio, sm, false);
    pio_remove_program(pio, &program, offset);
    pio_sm_unclaim(pio, sm);

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
