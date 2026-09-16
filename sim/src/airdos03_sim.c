/*
 * airdos03_sim - virtual AIRDOS03B (TFUNIPAYLOAD01 + USTSIPIN03) for simavr
 *
 * Runs the unmodified AIRDOS03 firmware ELF on a simulated ATmega1284P
 * and attaches models of the digital peripherals:
 *
 *   I2C  0x5B  EEPROM (analog board serial number at 0x0800)
 *   I2C  0x53  EEPROM (ADC configuration at 0x0000)
 *   I2C  0x45  SHT31-DIS temperature / humidity
 *   SPI + PB0 (CONV) + PC2 (DRESET)   USTSIPIN03 peak detector / ADC
 *   PD4 (1PPS) + UART1 (NMEA)         GNSS receiver
 *   UART0                             data output (stdout / file / pty)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim_avr.h"
#include "sim_elf.h"
#include "sim_core.h"
#include "sim_gdb.h"
#include "sim_vcd_file.h"
#include "avr_ioport.h"
#include "avr_uart.h"
#include "avr_spi.h"
#include "avr_twi.h"
#include "uart_pty.h"

#include "board.h"

/* ================================================================== */
/* helpers: RNG, truth log, scenario                                   */
/* ================================================================== */

static uint64_t rng_state = 1;

void rng_seed(uint64_t seed)
{
	rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

double rng_uniform(void)
{
	/* splitmix64 */
	uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z ^= z >> 31;
	return ((z >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

double rng_normal(void)
{
	return sqrt(-2.0 * log(rng_uniform())) * cos(2 * M_PI * rng_uniform());
}

FILE *truth_log;

void truth(avr_t *avr, const char *kind, const char *fmt, ...)
{
	if (!truth_log)
		return;
	fprintf(truth_log, "%.6f,%s,", sim_time(avr), kind);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(truth_log, fmt, ap);
	va_end(ap);
	fputc('\n', truth_log);
}

int scenario_load(scenario_t *s, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[256];
	int cap = 0;
	while (fgets(line, sizeof(line), f)) {
		char *c = line;
		while (*c == ' ' || *c == '\t')
			c++;
		if (!(*c == '-' || *c == '.' || (*c >= '0' && *c <= '9')))
			continue;       /* comment or header */
		scenario_point_t p = { 0, s->temp_c, s->rh_pct, s->rate_cps };
		if (sscanf(c, "%lf,%lf,%lf,%lf", &p.t, &p.temp_c, &p.rh_pct, &p.rate_cps) < 1)
			continue;
		if (s->n == cap) {
			cap = cap ? cap * 2 : 32;
			s->pts = realloc(s->pts, cap * sizeof(*s->pts));
		}
		s->pts[s->n++] = p;
	}
	fclose(f);
	return s->n > 0 ? 0 : -1;
}

void scenario_eval(const scenario_t *s, double t, scenario_point_t *out)
{
	if (s->n == 0) {
		*out = (scenario_point_t){ t, s->temp_c, s->rh_pct, s->rate_cps };
		return;
	}
	if (t <= s->pts[0].t) {
		*out = s->pts[0];
	} else if (t >= s->pts[s->n - 1].t) {
		*out = s->pts[s->n - 1];
	} else {
		int i = 1;
		while (s->pts[i].t < t)
			i++;
		const scenario_point_t *a = &s->pts[i - 1], *b = &s->pts[i];
		double k = (t - a->t) / (b->t - a->t);
		out->temp_c = a->temp_c + k * (b->temp_c - a->temp_c);
		out->rh_pct = a->rh_pct + k * (b->rh_pct - a->rh_pct);
		out->rate_cps = a->rate_cps + k * (b->rate_cps - a->rate_cps);
	}
	out->t = t;
}

/* ================================================================== */
/* UART0 capture                                                       */
/* ================================================================== */

typedef struct uart_capture_t {
	avr_t *avr;
	FILE *raw;              /* all bytes, unmodified */
	FILE *timed;            /* "<t_first_byte>\t<t_last_byte>\t<line>" per text line */
	int to_stdout;
	char line[8192];
	int len;
	double line_t;
	unsigned long bytes;
} uart_capture_t;

static void uart0_out_hook(avr_irq_t *irq, uint32_t value, void *param)
{
	uart_capture_t *c = param;
	uint8_t b = value;

	c->bytes++;
	if (c->raw)
		fputc(b, c->raw);
	if (c->to_stdout)
		fputc(b, stdout);
	if (!c->timed)
		return;
	if (b == '\r')
		return;
	if (b == '\n') {
		if (c->len) {
			c->line[c->len] = 0;
			fprintf(c->timed, "%.6f\t%.6f\t%s\n", c->line_t, sim_time(c->avr), c->line);
		}
		c->len = 0;
		return;
	}
	if (c->len == 0)
		c->line_t = sim_time(c->avr);
	if (c->len < (int)sizeof(c->line) - 1)
		c->line[c->len++] = b;
}

/* ================================================================== */
/* real-time pacing                                                    */
/* ================================================================== */

static struct timespec rt_start;
static double rt_factor;

static avr_cycle_count_t realtime_timer(avr_t *avr, avr_cycle_count_t when, void *param)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double wall = (now.tv_sec - rt_start.tv_sec) + (now.tv_nsec - rt_start.tv_nsec) * 1e-9;
	double want = sim_time(avr) / rt_factor;
	if (want > wall)
		usleep((useconds_t)((want - wall) * 1e6));
	return when + sec_to_cycles(avr, 0.01);
}

/* ================================================================== */

/* simavr messages go to stderr so that stdout carries only UART0 data */
static int log_level = LOG_ERROR;

static void stderr_logger(avr_t *avr, const int level, const char *format, va_list ap)
{
	if (level <= (avr ? avr->log : log_level))
		vfprintf(stderr, format, ap);
}

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	stop_requested = 1;
}

static int parse_hex(const char *s, uint8_t *out, int n)
{
	if ((int)strlen(s) != 2 * n)
		return -1;
	for (int i = 0; i < n; i++) {
		unsigned v;
		if (sscanf(s + 2 * i, "%2x", &v) != 1)
			return -1;
		out[i] = v;
	}
	return 0;
}

static int64_t parse_utc(const char *s)
{
	struct tm tm = { 0 };
	if (!strptime(s, "%Y-%m-%dT%H:%M:%S", &tm) && !strptime(s, "%Y-%m-%d %H:%M:%S", &tm))
		return -1;
	return (int64_t)timegm(&tm);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
"Usage: %s [options] firmware.elf\n"
"\n"
"Simulation\n"
"  -t, --duration SEC        simulated time to run (default 60, 0 = forever)\n"
"      --realtime[=FACTOR]   pace simulation to wall clock (FACTOR x faster)\n"
"      --seed N              random seed (default 1)\n"
"      --gdb PORT            start halted and wait for avr-gdb on PORT\n"
"\n"
"Outputs\n"
"  -o, --out FILE            raw UART0 byte stream (ASCII or MAVLink)\n"
"      --timed FILE          UART0 text lines with simulated start/end time\n"
"      --truth FILE          CSV log of what the virtual hardware did\n"
"      --vcd FILE            digital waveform trace for GTKWave\n"
"      --pty                 connect UART0 to a pseudo terminal (/tmp/simavr-uart0)\n"
"  -q, --quiet               do not echo UART0 to stdout\n"
"  -v, --verbose             log peripheral activity to stderr\n"
"\n"
"Environment (constant, or --scenario CSV: t_s,temp_c,rh_pct,rate_cps)\n"
"      --temp C              temperature (default 22.5)\n"
"      --rh PCT              relative humidity (default 45)\n"
"      --rate CPS            particle hits per second (default 5)\n"
"      --scenario FILE       time profile, linearly interpolated\n"
"\n"
"Detector (synthetic spectrum)\n"
"      --noise-frac F        fraction of hits below channel 64 (default 0.7)\n"
"      --alpha A             power-law index of hits >= 64 (default 1.3)\n"
"\n"
"Board\n"
"      --sn HEX32            analog board serial number (16 bytes)\n"
"      --adc-conf HEX4       ADC configuration EEPROM bytes\n"
"      --no-sht              SHT31 not populated\n"
"      --sht-meas-ms MS      SHT31 measurement duration (default 12.5)\n"
"\n"
"GNSS\n"
"      --no-gnss             no GNSS receiver (no PPS, no NMEA)\n"
"      --fix-at SEC          first second with a valid fix (default 5)\n"
"      --fix-loss A:B        no fix between A and B seconds\n"
"      --utc-start ISO       UTC at t=0 (default 2026-09-16T12:00:00)\n",
	argv0);
}

int main(int argc, char *argv[])
{
	double duration = 60;
	double realtime = 0;
	uint64_t seed = 1;
	int gdb_port = 0;
	const char *out_path = NULL, *timed_path = NULL, *truth_path = NULL;
	const char *vcd_path = NULL, *scen_path = NULL;
	int use_pty = 0, quiet = 0, verbose = 0;
	int no_sht = 0, no_gnss = 0;
	double sht_meas_ms = 12.5;
	double noise_frac = 0.7, alpha = 1.3;
	uint8_t sn[16] = { 0x53, 0x49, 0x4D, 0x41, 0x49, 0x52, 0x44, 0x4F,
			   0x53, 0x30, 0x33, 0x00, 0x00, 0x00, 0x00, 0x01 };
	uint8_t adc_conf[2] = { 0x03, 0x2C };
	scenario_t scen = { .temp_c = 22.5, .rh_pct = 45, .rate_cps = 5 };
	double fix_at = 5, loss_from = -1, loss_to = -1;
	int64_t utc_start = 1789560000;

	enum { O_REALTIME = 1000, O_SEED, O_GDB, O_TIMED, O_TRUTH, O_VCD, O_PTY,
	       O_TEMP, O_RH, O_RATE, O_SCEN, O_NOISE, O_ALPHA, O_SN, O_CONF,
	       O_NOSHT, O_SHTMS, O_NOGNSS, O_FIXAT, O_FIXLOSS, O_UTC };
	static const struct option opts[] = {
		{ "duration", required_argument, 0, 't' },
		{ "realtime", optional_argument, 0, O_REALTIME },
		{ "seed", required_argument, 0, O_SEED },
		{ "gdb", required_argument, 0, O_GDB },
		{ "out", required_argument, 0, 'o' },
		{ "timed", required_argument, 0, O_TIMED },
		{ "truth", required_argument, 0, O_TRUTH },
		{ "vcd", required_argument, 0, O_VCD },
		{ "pty", no_argument, 0, O_PTY },
		{ "quiet", no_argument, 0, 'q' },
		{ "verbose", no_argument, 0, 'v' },
		{ "temp", required_argument, 0, O_TEMP },
		{ "rh", required_argument, 0, O_RH },
		{ "rate", required_argument, 0, O_RATE },
		{ "scenario", required_argument, 0, O_SCEN },
		{ "noise-frac", required_argument, 0, O_NOISE },
		{ "alpha", required_argument, 0, O_ALPHA },
		{ "sn", required_argument, 0, O_SN },
		{ "adc-conf", required_argument, 0, O_CONF },
		{ "no-sht", no_argument, 0, O_NOSHT },
		{ "sht-meas-ms", required_argument, 0, O_SHTMS },
		{ "no-gnss", no_argument, 0, O_NOGNSS },
		{ "fix-at", required_argument, 0, O_FIXAT },
		{ "fix-loss", required_argument, 0, O_FIXLOSS },
		{ "utc-start", required_argument, 0, O_UTC },
		{ "help", no_argument, 0, 'h' },
		{ 0 }
	};

	int c;
	while ((c = getopt_long(argc, argv, "t:o:qvh", opts, NULL)) != -1) {
		switch (c) {
		case 't': duration = atof(optarg); break;
		case O_REALTIME: realtime = optarg ? atof(optarg) : 1.0; break;
		case O_SEED: seed = strtoull(optarg, NULL, 0); break;
		case O_GDB: gdb_port = atoi(optarg); break;
		case 'o': out_path = optarg; break;
		case O_TIMED: timed_path = optarg; break;
		case O_TRUTH: truth_path = optarg; break;
		case O_VCD: vcd_path = optarg; break;
		case O_PTY: use_pty = 1; break;
		case 'q': quiet = 1; break;
		case 'v': verbose = 1; break;
		case O_TEMP: scen.temp_c = atof(optarg); break;
		case O_RH: scen.rh_pct = atof(optarg); break;
		case O_RATE: scen.rate_cps = atof(optarg); break;
		case O_SCEN: scen_path = optarg; break;
		case O_NOISE: noise_frac = atof(optarg); break;
		case O_ALPHA: alpha = atof(optarg); break;
		case O_SN:
			if (parse_hex(optarg, sn, 16)) {
				fprintf(stderr, "--sn expects 32 hex digits\n");
				return 2;
			}
			break;
		case O_CONF:
			if (parse_hex(optarg, adc_conf, 2)) {
				fprintf(stderr, "--adc-conf expects 4 hex digits\n");
				return 2;
			}
			break;
		case O_NOSHT: no_sht = 1; break;
		case O_SHTMS: sht_meas_ms = atof(optarg); break;
		case O_NOGNSS: no_gnss = 1; break;
		case O_FIXAT: fix_at = atof(optarg); break;
		case O_FIXLOSS:
			if (sscanf(optarg, "%lf:%lf", &loss_from, &loss_to) != 2) {
				fprintf(stderr, "--fix-loss expects A:B\n");
				return 2;
			}
			break;
		case O_UTC:
			utc_start = parse_utc(optarg);
			if (utc_start < 0) {
				fprintf(stderr, "--utc-start expects YYYY-MM-DDTHH:MM:SS\n");
				return 2;
			}
			break;
		case 'h':
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 2;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}
	const char *fw_path = argv[optind];

	if (scen_path && scenario_load(&scen, scen_path)) {
		fprintf(stderr, "cannot load scenario '%s'\n", scen_path);
		return 1;
	}
	rng_seed(seed);
	log_level = verbose ? LOG_WARNING : LOG_ERROR;
	avr_global_logger_set(stderr_logger);

	/* ---------------------------------------------------------------- */
	/* MCU                                                               */
	/* ---------------------------------------------------------------- */
	elf_firmware_t fw;
	memset(&fw, 0, sizeof(fw));
	if (elf_read_firmware(fw_path, &fw)) {
		fprintf(stderr, "cannot load firmware '%s'\n", fw_path);
		return 1;
	}
	/* PlatformIO ELFs carry no .mmcu section - TFUNIPAYLOAD01 defaults */
	if (!fw.mmcu[0])
		strcpy(fw.mmcu, "atmega1284p");
	if (!fw.frequency)
		fw.frequency = 8000000;

	avr_t *avr = avr_make_mcu_by_name(fw.mmcu);
	if (!avr) {
		fprintf(stderr, "unknown MCU '%s'\n", fw.mmcu);
		return 1;
	}
	avr_init(avr);
	avr->log = log_level;
	avr_load_firmware(avr, &fw);

	/* UARTs: no simavr console echo, no sleeping on RX polling */
	for (char u = '0'; u <= '1'; u++) {
		uint32_t flags = 0;
		avr_ioctl(avr, AVR_IOCTL_UART_SET_FLAGS(u), &flags);
	}

	if (truth_path) {
		truth_log = fopen(truth_path, "w");
		if (!truth_log) {
			perror(truth_path);
			return 1;
		}
		fprintf(truth_log, "t_s,kind,data...\n");
		char snhex[33];
		for (int i = 0; i < 16; i++)
			sprintf(snhex + 2 * i, "%02X", sn[i]);
		truth(avr, "config", "sn=%s,adc_conf=%02X%02X,utc_start=%lld,fix_at=%g,"
			"fix_loss=%g:%g,gnss=%d,sht=%d,seed=%llu",
			snhex, adc_conf[0], adc_conf[1], (long long)utc_start, fix_at,
			loss_from, loss_to, !no_gnss, !no_sht, (unsigned long long)seed);
	}

	/* ---------------------------------------------------------------- */
	/* Peripherals                                                       */
	/* ---------------------------------------------------------------- */
	static i2c_eeprom16_t ee_sn, ee_conf;
	i2c_eeprom16_init(avr, &ee_sn, "eeprom@5B", 0x5B);
	i2c_eeprom16_init(avr, &ee_conf, "eeprom@53", 0x53);
	memcpy(ee_sn.mem + 0x0800, sn, 16);
	memcpy(ee_conf.mem + 0x0000, adc_conf, 2);
	ee_sn.verbose = ee_conf.verbose = verbose;
	i2c_eeprom16_attach(avr, &ee_sn);
	i2c_eeprom16_attach(avr, &ee_conf);

	static sht31_t sht;
	sht31_init(avr, &sht, 0x45, &scen, sht_meas_ms);
	sht.present = !no_sht;
	sht.verbose = verbose;
	sht31_attach(avr, &sht);

	static ustsipin03_t det;
	ustsipin03_init(avr, &det, &scen);
	det.noise_frac = noise_frac;
	det.alpha = alpha;
	det.verbose = verbose;
	ustsipin03_attach(avr, &det);

	static gnss_t gnss;
	gnss_init(avr, &gnss);
	gnss.utc_start = utc_start;
	gnss.fix_at = no_gnss ? -1 : fix_at;
	gnss.loss_from = loss_from;
	gnss.loss_to = loss_to;
	gnss.verbose = verbose;
	if (!no_gnss)
		gnss_attach(avr, &gnss);

	static uart_capture_t cap;
	cap.avr = avr;
	cap.to_stdout = !quiet;
	if (out_path && !(cap.raw = fopen(out_path, "wb"))) {
		perror(out_path);
		return 1;
	}
	if (timed_path && !(cap.timed = fopen(timed_path, "w"))) {
		perror(timed_path);
		return 1;
	}
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT),
			uart0_out_hook, &cap);

	static uart_pty_t pty;
	if (use_pty) {
		uart_pty_init(avr, &pty);
		uart_pty_connect(&pty, '0');
	}

	static avr_vcd_t vcd;
	if (vcd_path) {
		avr_vcd_init(avr, vcd_path, &vcd, 100000);
		const struct { char port; int pin; const char *name; } sig[] = {
			{ 'B', 0, "CONV_PB0" }, { 'C', 2, "DRESET_PC2" }, { 'D', 7, "DSET_PD7" },
			{ 'D', 4, "PPS_PD4" }, { 'C', 5, "LED_RED_PC5" }, { 'C', 6, "LED_BLUE_PC6" },
			{ 'C', 7, "LED_GREEN_PC7" },
		};
		for (size_t i = 0; i < sizeof(sig) / sizeof(sig[0]); i++)
			avr_vcd_add_signal(&vcd, avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ(sig[i].port),
					sig[i].pin), 1, sig[i].name);
		avr_vcd_add_signal(&vcd, avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT),
				8, "UART0_TX");
		avr_vcd_add_signal(&vcd, avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('1'), UART_IRQ_INPUT),
				8, "UART1_RX");
		avr_vcd_add_signal(&vcd, avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_INPUT),
				8, "SPI_MISO");
		avr_vcd_add_signal(&vcd, avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_STATUS),
				8, "TWI_STATUS");
		avr_vcd_start(&vcd);
	}

	if (gdb_port) {
		avr->gdb_port = gdb_port;
		avr->state = cpu_Stopped;
		avr_gdb_init(avr);
		fprintf(stderr, "waiting for gdb on port %d (target remote :%d)\n", gdb_port, gdb_port);
	}

	if (realtime > 0) {
		rt_factor = realtime;
		clock_gettime(CLOCK_MONOTONIC, &rt_start);
		avr_cycle_timer_register(avr, sec_to_cycles(avr, 0.01), realtime_timer, NULL);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* ---------------------------------------------------------------- */
	/* Run                                                               */
	/* ---------------------------------------------------------------- */
	avr_cycle_count_t end = duration > 0 ? sec_to_cycles(avr, duration) : 0;
	struct timespec w0, w1;
	clock_gettime(CLOCK_MONOTONIC, &w0);

	int state = cpu_Running;
	while (!stop_requested && state != cpu_Done && state != cpu_Crashed) {
		state = avr_run(avr);
		if (end && avr->cycle >= end)
			break;
	}
	clock_gettime(CLOCK_MONOTONIC, &w1);
	fflush(stdout);

	if (vcd_path)
		avr_vcd_close(&vcd);
	/* no uart_pty_stop(): it relies on SIGINT killing the pty thread, but we
	 * handle SIGINT ourselves; the thread ends with the process */
	if (cap.raw)
		fclose(cap.raw);
	if (cap.timed)
		fclose(cap.timed);

	double wall = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) * 1e-9;
	double st = sim_time(avr);
	fprintf(stderr,
		"\n--- airdos03_sim summary ---\n"
		"simulated time      %.3f s (wall %.2f s, %.1fx real time)%s\n"
		"UART0 bytes out     %lu\n"
		"particle hits       %lu generated, %lu read by firmware\n"
		"  lost pile-up      %lu (CONV still high)\n"
		"  lost in reset     %lu (DRESET low)\n"
		"  empty ADC reads   %lu (DRESET pulse without CONV)\n"
		"SHT31               %u measurements, %u busy NACKs\n"
		"GNSS                %lu PPS pulses, %lu NMEA sentences, %lu bytes\n",
		st, wall, wall > 0 ? st / wall : 0,
		state == cpu_Crashed ? "  ** CPU CRASHED **" : "",
		cap.bytes, det.generated, det.read_valid, det.pileup, det.lost_in_reset,
		det.read_empty, sht.measurements, sht.busy_nacks,
		gnss.pps_pulses, gnss.sentences, gnss.bytes_sent);

	if (truth_log) {
		truth(avr, "end", "%lu,%lu,%lu,%lu", det.generated, det.read_valid,
			det.pileup, det.lost_in_reset);
		fclose(truth_log);
	}
	return state == cpu_Crashed ? 1 : 0;
}
