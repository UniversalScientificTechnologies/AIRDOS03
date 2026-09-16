/*
 * AIRDOS03B / TFUNIPAYLOAD01 virtual board for simavr.
 *
 * Shared declarations for the peripheral models attached to the
 * ATmega1284P (8 MHz) running the unmodified AIRDOS03 firmware.
 */
#ifndef AIRDOS03_BOARD_H
#define AIRDOS03_BOARD_H

#include <stdint.h>
#include <stdio.h>
#include "sim_avr.h"
#include "sim_irq.h"

/* Simulated time in seconds */
static inline double sim_time(avr_t *avr)
{
	return (double)avr->cycle / (double)avr->frequency;
}

static inline avr_cycle_count_t sec_to_cycles(avr_t *avr, double s)
{
	return (avr_cycle_count_t)(s * (double)avr->frequency + 0.5);
}

/* ------------------------------------------------------------------ */
/* Environment scenario (CSV: t_s, temp_c, rh_pct, rate_cps)           */
/* ------------------------------------------------------------------ */
typedef struct scenario_point_t {
	double t, temp_c, rh_pct, rate_cps;
} scenario_point_t;

typedef struct scenario_t {
	scenario_point_t *pts;
	int n;
	/* used when no file is given */
	double temp_c, rh_pct, rate_cps;
} scenario_t;

int scenario_load(scenario_t *s, const char *path);
void scenario_eval(const scenario_t *s, double t, scenario_point_t *out);

/* Deterministic random numbers (seeded from the command line) */
void rng_seed(uint64_t seed);
double rng_uniform(void);       /* (0, 1) */
double rng_normal(void);        /* N(0, 1) */

/* ------------------------------------------------------------------ */
/* Truth log - what the virtual hardware really did                    */
/* ------------------------------------------------------------------ */
extern FILE *truth_log;
void truth(avr_t *avr, const char *kind, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* I2C EEPROM with 16-bit memory addressing (24xx32 style)            */
/* ------------------------------------------------------------------ */
typedef struct i2c_eeprom16_t {
	avr_irq_t *irq;
	const char *name;
	uint8_t addr7;
	uint8_t selected;
	int index;          /* bytes written since START */
	uint16_t reg;
	uint8_t mem[65536];
	int verbose;
} i2c_eeprom16_t;

void i2c_eeprom16_init(avr_t *avr, i2c_eeprom16_t *p, const char *name, uint8_t addr7);
void i2c_eeprom16_attach(avr_t *avr, i2c_eeprom16_t *p);

/* ------------------------------------------------------------------ */
/* Sensirion SHT31-DIS                                                 */
/* ------------------------------------------------------------------ */
typedef struct sht31_t {
	avr_irq_t *irq;
	avr_t *avr;
	const scenario_t *scen;
	uint8_t addr7;
	uint8_t selected;
	int index;
	uint16_t cmd;
	uint8_t out[6];
	int out_len, out_pos;
	avr_cycle_count_t ready_at;   /* measurement finished at this cycle */
	int measuring;
	double meas_ms;
	int present;
	int verbose;
	/* statistics */
	unsigned measurements, busy_nacks, unknown_cmds;
} sht31_t;

void sht31_init(avr_t *avr, sht31_t *p, uint8_t addr7, const scenario_t *scen, double meas_ms);
void sht31_attach(avr_t *avr, sht31_t *p);

/* ------------------------------------------------------------------ */
/* USTSIPIN03 detector: peak detector + ADC on SPI, CONV on PB0,       */
/* DRESET on PC2 (active low)                                          */
/* ------------------------------------------------------------------ */
typedef struct ustsipin03_t {
	avr_t *avr;
	avr_irq_t *irq;                 /* own IRQs: CONV out, SPI in */
	const scenario_t *scen;

	int conv;                       /* CONV line level */
	int dreset;                     /* DRESET line level */
	uint16_t latched;               /* value held by the peak detector */
	uint16_t spi_word;              /* word being shifted out */
	int spi_idx;

	/* synthetic spectrum */
	double noise_frac;              /* fraction of hits below THRESHOLD */
	double noise_mu, noise_sigma;
	double alpha;                   /* power-law index above threshold */
	uint16_t threshold;

	int verbose;
	/* statistics */
	unsigned long generated, pileup, lost_in_reset, read_valid, read_empty;
} ustsipin03_t;

void ustsipin03_init(avr_t *avr, ustsipin03_t *p, const scenario_t *scen);
void ustsipin03_attach(avr_t *avr, ustsipin03_t *p);

/* ------------------------------------------------------------------ */
/* GNSS receiver: 1PPS on PD4 + NMEA on UART1                          */
/* ------------------------------------------------------------------ */
typedef struct gnss_t {
	avr_t *avr;
	avr_irq_t *irq;
	char uart;
	uint32_t baud;
	int64_t utc_start;              /* unix time of simulated t = 0 */
	double fix_at;                  /* first second with fix, <0 = never */
	double loss_from, loss_to;      /* fix-loss window [from, to) */
	double pps_width;
	double nmea_delay;              /* seconds after PPS edge */
	uint8_t queue[4096];
	int q_head, q_tail;
	unsigned long second;
	int verbose;
	unsigned long pps_pulses, sentences, bytes_sent;
} gnss_t;

void gnss_init(avr_t *avr, gnss_t *g);
void gnss_attach(avr_t *avr, gnss_t *g);
int gnss_has_fix(const gnss_t *g, double t);

#endif
