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
#include "idle.h"
#include "record.h"
#include "piolib.h"
#include "srzip.h"
#include "unpack.h"
#include "util.h"

#define EXIT_DROPPED 2

#define DEFAULT_GAP "100ms"
#define DEFAULT_PRE "10ms"
#define DEFAULT_POST "50ms"
#define DEFAULT_MAX_SIZE "4G"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: pi5la --rate RATE --out FILE.sr [--silence-timeout T]\n"
        "       pi5la --rate RATE --monitor DIR [--gap T] [--pre T] [--post T]\n"
        "       pi5la --rate RATE --samples N --raw FILE [--mode naive]\n"
        "       pi5la --generator FREQ --generator-only\n"
        "       pi5la --rate RATE --stream [--channels N] [--trigger T:N] | sigrok-cli ...\n"
        "\n"
        "Capture runs until Ctrl-C unless a limit is given. Samples are written as they\n"
        "arrive, so length is bounded by disk space, not RAM.\n"
        "\n"
        "  --rate RATE        sample rate, suffix k/M (200 MHz / integer+1/256 divider)\n"
        "  --out FILE.sr      write one sigrok session file\n"
        "  --monitor DIR      record only while the bus is active: one .sr per burst in\n"
        "                     DIR, plus index.json. Idle is never written\n"
        "  --silence-timeout T  wait for the first change, then stop once the line has\n"
        "                     been quiet for T (500ms, 10s, 2m)\n"
        "  --gap T            silence that ends a burst, --monitor only (default %s)\n"
        "  --pre T            lead-in kept before the first change (default %s)\n"
        "  --post T           tail kept after the last change (default %s)\n"
        "  --duration T       stop after T of samples (optional cap)\n"
        "  --samples N        stop after N samples, suffix k/M (optional cap)\n"
        "  --max-size BYTES   stop once this much sample data is written (default %s)\n"
        "  --raw FILE         write the packed 32-bit FIFO words; needs --samples/--duration\n"
        "  --channels N       1, 2, 4 or 8 (default 8)\n"
        "  --base-gpio G      first capture GPIO (default 2)\n"
        "  --trigger T:N      wait for condition on channel N before sampling\n"
        "  --mode M           dma (default) or naive (M2 baseline, with --raw)\n"
        "  --generator FREQ   square wave on GPIO %d while capturing (loopback self-test)\n"
        "  --generator-only   run the generator until Ctrl-C, no capture (M1)\n"
        "  --stream           capture until Ctrl-C, writing samples to stdout as they\n"
        "                     arrive (1 byte per sample, channel N in bit N); for\n"
        "                     sigrok-cli -I binary:numchannels=N:samplerate=RATE -i -\n"
        "  --dma-buf BYTES    cyclic DMA period size, ring of %d periods (default %u)\n"
        "\n"
        "Silence, gap and size are checked once per DMA period, so a stop can lag by one\n"
        "period (1 MiB = 1 M samples at 8 channels).\n"
        "\n"
        "Exit status: 0 ok, 1 error, %d dropped samples detected.\n",
        DEFAULT_GAP, DEFAULT_PRE, DEFAULT_POST, DEFAULT_MAX_SIZE, GENERATOR_DEFAULT_GPIO,
        CAPTURE_DMA_BUF_COUNT, CAPTURE_DMA_BUF_DEFAULT, EXIT_DROPPED);
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

/* The streaming path: unbounded capture, silence timeout and monitor mode. */
static void report_stream(const capture_config_t *cfg, const record_opts_t *rec,
                          const record_stats_t *st, const capture_stream_result_t *sres)
{
    double rate = clkdiv_hz(cfg->div);

    fprintf(stderr, "\ncaptured %" PRIu64 " samples (%.3f s) in %.3f s, wrote %" PRIu64
                    " sample bytes\n",
            st->samples, rate > 0 ? st->samples / rate : 0.0, sres->total_s, st->written);
    fprintf(stderr, "stall events: %" PRIu64 " (DMA periods with RXSTALL set)\n",
            sres->stall_events);

    if (rec->monitor_dir) {
        double kept = st->samples ? 100.0 * st->written / st->samples : 0.0;

        fprintf(stderr, "monitor: %u burst(s) in %s, %.2f%% of the time recorded\n",
                st->bursts, rec->monitor_dir, kept);
        if (!st->activity_seen)
            fprintf(stderr, "monitor: no change seen on any channel: check the wiring\n");
    } else if (rec->silence_s > 0) {
        if (!st->activity_seen)
            fprintf(stderr, "silence timeout: no change seen on any channel\n");
        else if (st->silence_stopped)
            fprintf(stderr, "silence timeout: stopped after %.3f s without a change\n",
                    rec->silence_s);
        else
            fprintf(stderr, "silence timeout: stopped before the line went quiet\n");
    }
    if (st->samples_cap_hit)
        fprintf(stderr, "stopped at the sample limit\n");
    if (st->size_cap_hit)
        fprintf(stderr, "stopped at --max-size (%" PRIu64 " bytes)\n", rec->max_bytes);
    if (sres->overflow)
        fprintf(stderr, "DMA ring overflow: writing could not keep up. Lower --rate, use "
                        "fewer --channels, or use --monitor so idle is not written.\n");
    if (st->write_failed)
        fprintf(stderr, "writing failed: the capture is incomplete\n");
    if (sres->interrupted)
        fprintf(stderr, "stopped by signal\n");
}

typedef struct {
    unsigned channels;
    uint8_t *buf;
    size_t cap;
    uint64_t samples;
    bool write_failed;
} stream_ctx_t;

/* Unpacks one DMA period and writes it to stdout. Non-zero stops the stream. */
static int stream_sink(const uint32_t *words, size_t n_words, void *arg)
{
    stream_ctx_t *ctx = arg;
    size_t n = n_words * unpack_samples_per_word(ctx->channels);
    size_t done = 0;

    if (n > ctx->cap) {
        uint8_t *nb = realloc(ctx->buf, n);

        if (!nb) {
            fprintf(stderr, "stream: cannot allocate %zu bytes\n", n);
            return -1;
        }
        ctx->buf = nb;
        ctx->cap = n;
    }
    unpack_words(words, n_words, ctx->channels, ctx->buf, n);

    while (done < n) {
        ssize_t w = write(STDOUT_FILENO, ctx->buf + done, n - done);

        if (w < 0) {
            if (errno == EINTR && !g_stop)
                continue;
            if (errno != EINTR) {
                if (errno != EPIPE)
                    fprintf(stderr, "stream: write to stdout failed: %s\n", strerror(errno));
                ctx->write_failed = true;
            }
            return -1;
        }
        done += (size_t)w;
    }
    ctx->samples += n;
    return 0;
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
    enum { OPT_GEN_ONLY = 256, OPT_DMA_BUF, OPT_RAW, OPT_STREAM, OPT_DURATION, OPT_IDLE_STOP,
           OPT_SILENCE, OPT_MONITOR, OPT_GAP, OPT_PRE, OPT_POST, OPT_MAX_SIZE };
    static const struct option opts[] = {
        { "rate", required_argument, NULL, 'r' },
        { "samples", required_argument, NULL, 'n' },
        { "duration", required_argument, NULL, OPT_DURATION },
        { "silence-timeout", required_argument, NULL, OPT_SILENCE },
        { "idle-stop", required_argument, NULL, OPT_IDLE_STOP },   /* old name */
        { "monitor", required_argument, NULL, OPT_MONITOR },
        { "gap", required_argument, NULL, OPT_GAP },
        { "pre", required_argument, NULL, OPT_PRE },
        { "post", required_argument, NULL, OPT_POST },
        { "max-size", required_argument, NULL, OPT_MAX_SIZE },
        { "out", required_argument, NULL, 'o' },
        { "raw", required_argument, NULL, OPT_RAW },
        { "channels", required_argument, NULL, 'c' },
        { "base-gpio", required_argument, NULL, 'b' },
        { "trigger", required_argument, NULL, 't' },
        { "mode", required_argument, NULL, 'm' },
        { "generator", required_argument, NULL, 'g' },
        { "generator-only", no_argument, NULL, OPT_GEN_ONLY },
        { "stream", no_argument, NULL, OPT_STREAM },
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
    record_opts_t rec = { 0 };
    const char *out_path = NULL, *raw_path = NULL;
    double rate = 0, gen_hz = 0, duration_s = 0;
    bool have_rate = false, have_samples = false, gen_only = false, stream = false;
    bool buffered = false;
    stream_ctx_t sctx = { 0 };
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
        case OPT_DURATION:
            if (!parse_duration(optarg, &duration_s)) {
                fprintf(stderr, "invalid --duration '%s' (e.g. 500ms, 10s, 2m)\n", optarg);
                return 1;
            }
            break;
        case OPT_IDLE_STOP:
            fprintf(stderr, "note: --idle-stop is now --silence-timeout\n");
            /* fall through */
        case OPT_SILENCE:
            if (!parse_duration(optarg, &rec.silence_s)) {
                fprintf(stderr, "invalid --silence-timeout '%s' (e.g. 500ms, 2s)\n", optarg);
                return 1;
            }
            break;
        case OPT_MONITOR:
            rec.monitor_dir = optarg;
            break;
        case OPT_GAP:
            if (!parse_duration(optarg, &rec.gap_s)) {
                fprintf(stderr, "invalid --gap '%s' (e.g. 100ms, 1s)\n", optarg);
                return 1;
            }
            break;
        case OPT_PRE:
            if (!parse_duration(optarg, &rec.pre_s)) {
                fprintf(stderr, "invalid --pre '%s' (e.g. 10ms)\n", optarg);
                return 1;
            }
            break;
        case OPT_POST:
            if (!parse_duration(optarg, &rec.post_s)) {
                fprintf(stderr, "invalid --post '%s' (e.g. 50ms)\n", optarg);
                return 1;
            }
            break;
        case OPT_MAX_SIZE:
            if (!parse_si_u64(optarg, &rec.max_bytes) || rec.max_bytes == 0) {
                fprintf(stderr, "invalid --max-size '%s' (bytes, suffix k/M/G)\n", optarg);
                return 1;
            }
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
        case OPT_STREAM:
            stream = true;
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
        if (stream) {
            if (!have_rate) {
                fprintf(stderr, "--stream needs --rate\n");
                return 1;
            }
            if (have_samples || duration_s > 0 || rec.silence_s > 0 || rec.monitor_dir ||
                out_path || raw_path || cfg.mode != CAPTURE_DMA) {
                fprintf(stderr, "--stream runs until Ctrl-C and writes to stdout: "
                                "don't combine it with --samples, --duration, "
                                "--silence-timeout, --monitor, --out, --raw or --mode naive\n");
                return 1;
            }
            if (isatty(STDOUT_FILENO)) {
                fprintf(stderr, "--stream writes binary samples: pipe it into a decoder, e.g.\n"
                                "  pi5la --rate 2M --stream | sigrok-cli -I "
                                "binary:numchannels=8:samplerate=2000000 -i - "
                                "-P uart:rx=0:baudrate=115200 -B uart=rx\n");
                return 1;
            }
        } else {
            if (have_samples && duration_s > 0) {
                fprintf(stderr, "give either --samples or --duration, not both\n");
                return 1;
            }
            if (!have_rate || (!out_path && !raw_path && !rec.monitor_dir)) {
                usage(stderr);
                return 1;
            }
            if (out_path && rec.monitor_dir) {
                fprintf(stderr, "give either --out or --monitor, not both\n");
                return 1;
            }
            if (duration_s > 0) {
                double n = ceil(duration_s * clkdiv_hz(cfg.div));

                if (n > 1e15) {
                    fprintf(stderr, "--duration too long\n");
                    return 1;
                }
                cfg.samples = (uint64_t)n;
                have_samples = true;
            }
            /* The old in-RAM path still serves --raw and the naive baseline. */
            buffered = raw_path != NULL || cfg.mode != CAPTURE_DMA;
            if (buffered) {
                if (!have_samples) {
                    fprintf(stderr, "%s needs --samples or --duration\n",
                            raw_path ? "--raw" : "--mode naive");
                    return 1;
                }
                if (rec.silence_s > 0 || rec.monitor_dir) {
                    fprintf(stderr, "--silence-timeout and --monitor need DMA mode "
                                    "without --raw\n");
                    return 1;
                }
            } else {
                if (!rec.gap_s && !parse_duration(DEFAULT_GAP, &rec.gap_s))
                    return 1;
                if (!rec.pre_s && !parse_duration(DEFAULT_PRE, &rec.pre_s))
                    return 1;
                if (!rec.post_s && !parse_duration(DEFAULT_POST, &rec.post_s))
                    return 1;
                if (!rec.max_bytes && !parse_si_u64(DEFAULT_MAX_SIZE, &rec.max_bytes))
                    return 1;
                rec.out_path = out_path;
                rec.max_samples = have_samples ? cfg.samples : 0;
                if (rec.monitor_dir && rec.silence_s > 0 && rec.silence_s < rec.gap_s)
                    fprintf(stderr, "note: --silence-timeout is shorter than --gap, so the "
                                    "capture stops before a burst can close\n");
            }
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
    /* A closed reader (e.g. sigrok-cli exiting) surfaces as EPIPE from write(). */
    signal(SIGPIPE, SIG_IGN);

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

    if (stream) {
        capture_stream_result_t sres;

        sctx.channels = cfg.channels;
        fprintf(stderr, "streaming %u ch at %.3f Hz on GPIO %u..%u until Ctrl-C "
                        "(sigrok samplerate=%.0f)\n",
                cfg.channels, clkdiv_hz(cfg.div), cfg.base_gpio,
                cfg.base_gpio + cfg.channels - 1, clkdiv_hz(cfg.div));
        if (capture_stream(pio, &cfg, stream_sink, &sctx, &sres))
            goto out;
        fprintf(stderr, "\nstreamed %" PRIu64 " samples in %.3f s, stall events: %" PRIu64 "\n",
                sctx.samples, sres.total_s, sres.stall_events);
        if (sres.overflow)
            fprintf(stderr, "DMA ring overflow: the reader could not keep up. Lower --rate "
                            "(or --channels) so the decoder keeps pace.\n");
        if (sres.stall_events > 0 || sres.overflow)
            exit_code = EXIT_DROPPED;
        else
            exit_code = sctx.write_failed && !sres.interrupted ? 1 : 0;
        goto out;
    }

    if (!buffered) {
        capture_stream_result_t sres;
        record_stats_t rstats;
        int rret;

        fprintf(stderr, "capturing %u ch at %.3f Hz on GPIO %u..%u%s\n", cfg.channels,
                clkdiv_hz(cfg.div), cfg.base_gpio, cfg.base_gpio + cfg.channels - 1,
                rec.max_samples ? "" : " until Ctrl-C");
        if (rec.monitor_dir)
            fprintf(stderr, "monitor: writing bursts to %s (gap %.3f s, pre %.3f s, "
                            "post %.3f s); idle is not recorded\n",
                    rec.monitor_dir, rec.gap_s, rec.pre_s, rec.post_s);
        else if (rec.silence_s > 0)
            fprintf(stderr, "waiting for the first change, then stopping after %.3f s "
                            "of silence\n", rec.silence_s);

        rret = record_run(pio, &cfg, &rec, &rstats, &sres);
        if (gen.sm >= 0)
            generator_stop(&gen);
        report_stream(&cfg, &rec, &rstats, &sres);
        if (rret)
            goto out;
        if (sres.stall_events > 0 || sres.overflow)
            exit_code = EXIT_DROPPED;
        else
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
    free(sctx.buf);
    capture_result_free(&res);
    pio_close(pio);
    return exit_code;
}
