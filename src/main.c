#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "capture.h"
#include "generator.h"
#include "piolib.h"
#include "srzip.h"
#include "unpack.h"
#include "util.h"

#define EXIT_DROPPED 2

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: pi5la --rate RATE --samples N [--out FILE.sr] [--raw FILE]\n"
        "             [--channels 1|2|4|8] [--base-gpio G]\n"
        "             [--trigger rising|falling|high|low:N] [--mode dma|naive]\n"
        "             [--generator FREQ] [--dma-buf BYTES]\n"
        "       pi5la --generator FREQ --generator-only\n"
        "\n"
        "  --rate RATE        sample rate, suffix k/M (200 MHz / integer+1/256 divider)\n"
        "  --samples N        samples to capture (suffix k/M allowed)\n"
        "  --out FILE.sr      write a sigrok session file\n"
        "  --raw FILE         write the packed 32-bit FIFO words (native byte order)\n"
        "  --channels N       1, 2, 4 or 8 (default 8)\n"
        "  --base-gpio G      first capture GPIO (default 2)\n"
        "  --trigger T:N      wait for condition on channel N before sampling\n"
        "  --mode M           dma (default) or naive (M2 baseline)\n"
        "  --generator FREQ   square wave on GPIO %d while capturing (loopback self-test)\n"
        "  --generator-only   run the generator until Ctrl-C, no capture (M1)\n"
        "  --dma-buf BYTES    cyclic DMA period size, ring of %d periods (default %u)\n"
        "\n"
        "Exit status: 0 ok, 1 error, %d dropped samples detected.\n",
        GENERATOR_DEFAULT_GPIO, CAPTURE_DMA_BUF_COUNT, CAPTURE_DMA_BUF_DEFAULT, EXIT_DROPPED);
}

static int write_raw(const char *path, const uint32_t *words, size_t n_words)
{
    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (n_words && fwrite(words, sizeof(uint32_t), n_words, f) != n_words) {
        fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }
    if (fclose(f)) {
        fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void report(const capture_config_t *cfg, const capture_result_t *res, uint64_t samples_got)
{
    fprintf(stderr, "mode=%s rate=%.3f Hz (div %.4f) channels=%u base=GPIO%u trigger=%s",
            cfg->mode == CAPTURE_DMA ? "dma" : "naive", res->rate_hz, clkdiv_value(cfg->div),
            cfg->channels, cfg->base_gpio, trig_name(cfg->trig));
    if (cfg->trig != TRIG_NONE)
        fprintf(stderr, ":%u", cfg->trig_pin);
    if (cfg->mode == CAPTURE_DMA)
        fprintf(stderr, " dma-buf=%ux%u", CAPTURE_DMA_BUF_COUNT, cfg->dma_buf_bytes);
    fprintf(stderr, "\n");

    fprintf(stderr, "captured %" PRIu64 " samples, %" PRIu64 " packed bytes in %.3f s\n",
            samples_got, res->bytes, res->total_s);
    if (res->steady_s > 0) {
        double bps = res->steady_bytes / res->steady_s;
        double sps = bps / 4 * unpack_samples_per_word(cfg->channels);

        fprintf(stderr, "throughput %.1f kB/s (%.3f MB/s), %.0f samples/s\n", bps / 1e3,
                bps / 1e6, sps);
    }
    fprintf(stderr, "stall events: %" PRIu64 " (%s)\n", res->stall_events,
            cfg->mode == CAPTURE_DMA ? "DMA periods with RXSTALL set"
                                     : "RXSTALL checked once at end");
    if (res->est_valid)
        fprintf(stderr, "estimated lost samples: %" PRId64 " (wall clock, +/- scheduling jitter)\n",
                res->est_lost_samples);
    else
        fprintf(stderr, "estimated lost samples: n/a (capture too short)\n");
    if (res->overflow)
        fprintf(stderr, "DMA ring overflow: host fell behind, capture stopped early\n");
    if (res->interrupted)
        fprintf(stderr, "capture interrupted\n");
}

static int check_dev_pio(void)
{
    if (access("/dev/pio0", F_OK) != 0) {
        fprintf(stderr, "/dev/pio0 not found: is the rp1-pio driver loaded? (see README)\n");
        return -1;
    }
    if (access("/dev/pio0", R_OK | W_OK) != 0) {
        fprintf(stderr, "no read/write access to /dev/pio0 (see README, Permissions)\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    enum { OPT_GEN_ONLY = 256, OPT_DMA_BUF, OPT_RAW };
    static const struct option opts[] = {
        { "rate", required_argument, NULL, 'r' },
        { "samples", required_argument, NULL, 'n' },
        { "out", required_argument, NULL, 'o' },
        { "raw", required_argument, NULL, OPT_RAW },
        { "channels", required_argument, NULL, 'c' },
        { "base-gpio", required_argument, NULL, 'b' },
        { "trigger", required_argument, NULL, 't' },
        { "mode", required_argument, NULL, 'm' },
        { "generator", required_argument, NULL, 'g' },
        { "generator-only", no_argument, NULL, OPT_GEN_ONLY },
        { "dma-buf", required_argument, NULL, OPT_DMA_BUF },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    capture_config_t cfg = {
        .base_gpio = 2,
        .channels = 8,
        .mode = CAPTURE_DMA,
        .trig = TRIG_NONE,
        .dma_buf_bytes = CAPTURE_DMA_BUF_DEFAULT,
        .stop = &g_stop,
    };
    const char *out_path = NULL, *raw_path = NULL;
    double rate = 0, gen_hz = 0;
    bool have_rate = false, have_samples = false, gen_only = false;
    capture_result_t res = { 0 };
    generator_t gen = { .sm = -1 };
    struct sigaction sa;
    uint64_t u, samples_got;
    uint8_t *samples = NULL;
    int opt, exit_code = 1;
    PIO pio;

    while ((opt = getopt_long(argc, argv, "r:n:o:c:b:t:m:g:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'r':
            if (!parse_si(optarg, &rate) || rate <= 0 || !clkdiv_for_hz(rate, &cfg.div)) {
                fprintf(stderr, "invalid --rate '%s' (range ~3.06 kHz .. 200 M)\n", optarg);
                return 1;
            }
            have_rate = true;
            break;
        case 'n':
            if (!parse_si_u64(optarg, &cfg.samples) || cfg.samples == 0) {
                fprintf(stderr, "invalid --samples '%s'\n", optarg);
                return 1;
            }
            have_samples = true;
            break;
        case 'o':
            out_path = optarg;
            break;
        case OPT_RAW:
            raw_path = optarg;
            break;
        case 'c':
            if (!parse_si_u64(optarg, &u) || !unpack_channels_valid((unsigned)u) || u > 8) {
                fprintf(stderr, "invalid --channels '%s' (1, 2, 4 or 8)\n", optarg);
                return 1;
            }
            cfg.channels = (unsigned)u;
            break;
        case 'b':
            if (!parse_si_u64(optarg, &u) || u >= CAPTURE_GPIO_COUNT) {
                fprintf(stderr, "invalid --base-gpio '%s'\n", optarg);
                return 1;
            }
            cfg.base_gpio = (unsigned)u;
            break;
        case 't':
            if (!trig_parse(optarg, &cfg.trig, &cfg.trig_pin)) {
                fprintf(stderr, "invalid --trigger '%s' (rising|falling|high|low:N)\n", optarg);
                return 1;
            }
            break;
        case 'm':
            if (!strcmp(optarg, "dma"))
                cfg.mode = CAPTURE_DMA;
            else if (!strcmp(optarg, "naive"))
                cfg.mode = CAPTURE_NAIVE;
            else {
                fprintf(stderr, "invalid --mode '%s' (dma|naive)\n", optarg);
                return 1;
            }
            break;
        case 'g':
            if (!parse_si(optarg, &gen_hz) || gen_hz <= 0) {
                fprintf(stderr, "invalid --generator '%s'\n", optarg);
                return 1;
            }
            break;
        case OPT_GEN_ONLY:
            gen_only = true;
            break;
        case OPT_DMA_BUF:
            if (!parse_si_u64(optarg, &u) || u < 4 || u % 4 || u > UINT32_MAX) {
                fprintf(stderr, "invalid --dma-buf '%s' (non-zero multiple of 4)\n", optarg);
                return 1;
            }
            cfg.dma_buf_bytes = (uint32_t)u;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return 1;
    }

    if (gen_only) {
        if (gen_hz <= 0) {
            fprintf(stderr, "--generator-only needs --generator FREQ\n");
            return 1;
        }
    } else {
        if (!have_rate || !have_samples || (!out_path && !raw_path)) {
            usage(stderr);
            return 1;
        }
        if (cfg.base_gpio + cfg.channels > CAPTURE_GPIO_COUNT) {
            fprintf(stderr, "capture GPIOs %u..%u out of range\n", cfg.base_gpio,
                    cfg.base_gpio + cfg.channels - 1);
            return 1;
        }
        if (cfg.trig != TRIG_NONE && cfg.trig_pin >= cfg.channels) {
            fprintf(stderr, "--trigger channel %u is not below --channels %u\n", cfg.trig_pin,
                    cfg.channels);
            return 1;
        }
        if (gen_hz > 0 && GENERATOR_DEFAULT_GPIO >= cfg.base_gpio &&
            GENERATOR_DEFAULT_GPIO < cfg.base_gpio + cfg.channels) {
            fprintf(stderr, "generator GPIO %d overlaps the capture pins\n",
                    GENERATOR_DEFAULT_GPIO);
            return 1;
        }
        if (!clkdiv_is_integer(cfg.div))
            fprintf(stderr, "note: rate %.3f Hz needs a fractional divider (%.4f); "
                            "sample spacing will jitter\n",
                    clkdiv_hz(cfg.div), clkdiv_value(cfg.div));
    }

    if (check_dev_pio())
        return 1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pio = pio_open(0);
    if (PIO_IS_ERR(pio)) {
        fprintf(stderr, "pio_open(0) failed: %s\n", strerror(-PIO_ERR_VAL(pio)));
        return 1;
    }

    if (gen_hz > 0) {
        if (generator_start(pio, GENERATOR_DEFAULT_GPIO, gen_hz, &gen))
            goto out;
        fprintf(stderr, "generator: GPIO %u, requested %.3f Hz, actual %.6f Hz (%s)\n", gen.gpio,
                gen_hz, gen.actual_hz, gen.slow ? "sqwave_slow" : "sqwave");
    }

    if (gen_only) {
        fprintf(stderr, "running until Ctrl-C\n");
        while (!g_stop)
            pause();
        exit_code = 0;
        goto out;
    }

    if (capture_run(pio, &cfg, &res))
        goto out;

    samples_got = (res.bytes / 4) * unpack_samples_per_word(cfg.channels);
    if (samples_got > cfg.samples)
        samples_got = cfg.samples;

    if (gen.sm >= 0)
        generator_stop(&gen);

    report(&cfg, &res, samples_got);

    if (raw_path && write_raw(raw_path, res.words, (size_t)(res.bytes / 4)))
        goto out;

    if (out_path) {
        char names[8][16];
        const char *name_ptrs[8];
        unsigned k;

        samples = malloc(samples_got ? samples_got : 1);
        if (!samples) {
            fprintf(stderr, "cannot allocate %" PRIu64 " sample bytes\n", samples_got);
            goto out;
        }
        unpack_words(res.words, (size_t)(res.bytes / 4), cfg.channels, samples, samples_got);
        for (k = 0; k < cfg.channels; k++) {
            snprintf(names[k], sizeof(names[k]), "GPIO%u", cfg.base_gpio + k);
            name_ptrs[k] = names[k];
        }
        if (srzip_write(out_path, samples, samples_got, (uint64_t)llround(res.rate_hz),
                        cfg.channels, name_ptrs))
            goto out;
    }

    if (res.stall_events > 0 || res.overflow) {
        fprintf(stderr, "DROPPED SAMPLES: %" PRIu64 " stall event(s)%s\n", res.stall_events,
                res.overflow ? ", DMA ring overflow" : "");
        exit_code = EXIT_DROPPED;
    } else if (res.interrupted) {
        exit_code = 1;
    } else {
        exit_code = 0;
    }

out:
    if (gen.sm >= 0)
        generator_stop(&gen);
    free(samples);
    capture_result_free(&res);
    pio_close(pio);
    return exit_code;
}
