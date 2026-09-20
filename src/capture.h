#ifndef PI5LA_CAPTURE_H
#define PI5LA_CAPTURE_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "piolib.h"
#include "progbuild.h"
#include "util.h"

#define CAPTURE_GPIO_COUNT 28        /* RP1 PIO drives GPIO 0..27 */
#define CAPTURE_DMA_BUF_COUNT 4      /* cyclic ring periods (rp1-pio max 4) */
#define CAPTURE_DMA_BUF_DEFAULT (1u << 20)

typedef enum {
    CAPTURE_DMA = 0,
    CAPTURE_NAIVE,
} capture_mode_t;

typedef struct {
    unsigned base_gpio;
    unsigned channels;          /* 1, 2, 4 or 8 */
    clkdiv_t div;               /* sample rate = 200 MHz / div */
    uint64_t samples;
    capture_mode_t mode;
    trig_type_t trig;
    unsigned trig_pin;          /* relative to base_gpio */
    uint32_t dma_buf_bytes;     /* cyclic DMA period, multiple of 4 */
    volatile sig_atomic_t *stop; /* optional: set from a signal handler */
} capture_config_t;

typedef struct {
    uint32_t *words;            /* packed FIFO words, as delivered */
    size_t n_words;
    double rate_hz;

    uint64_t bytes;             /* packed bytes received */
    double total_s;             /* SM enable -> last data */
    uint64_t steady_bytes;      /* bytes received after the first buffer/word */
    double steady_s;            /* first buffer/word -> last data */

    /* Drop detection */
    uint64_t stall_events;      /* FDEBUG RXSTALL seen: DMA per period; naive 0/1 at end */
    bool est_valid;
    int64_t est_lost_samples;   /* rate * steady_s - samples in steady_bytes */

    bool overflow;              /* DMA ring wrapped over unread data; capture cut short */
    bool interrupted;
} capture_result_t;

/* Returns 0 on success, -1 on error (message on stderr). */
int capture_run(PIO pio, const capture_config_t *cfg, capture_result_t *res);

/*
 * Called with each completed DMA period of packed FIFO words, in order.
 * Return 0 to keep streaming, non-zero to stop.
 */
typedef int (*capture_sink_t)(const uint32_t *words, size_t n_words, void *ctx);

typedef struct {
    double rate_hz;
    uint64_t bytes;             /* packed bytes delivered to the sink */
    double total_s;             /* SM enable -> last period */
    uint64_t stall_events;      /* periods with RXSTALL set */
    bool overflow;              /* DMA ring wrapped: sink too slow, stream stopped */
    bool sink_stopped;          /* sink returned non-zero */
    bool interrupted;           /* cfg->stop was set */
} capture_stream_result_t;

/*
 * Capture continuously (cyclic DMA, no gaps) until cfg->stop is set, the sink
 * asks to stop, or the ring overflows. cfg->samples is ignored.
 * Returns 0 on a clean stop, -1 on error (message on stderr).
 */
int capture_stream(PIO pio, const capture_config_t *cfg, capture_sink_t sink, void *ctx,
                   capture_stream_result_t *res);

void capture_result_free(capture_result_t *res);

/* Checks progbuild output against the pioasm-generated la8 / la8_trig programs. */
bool capture_selftest_programs(void);

#endif
