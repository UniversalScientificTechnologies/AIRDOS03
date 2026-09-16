/*
 * USTSIPIN03 detector model - digital interface only.
 *
 * Particle hits arrive as a Poisson process with rate taken from the
 * scenario. A hit is captured by the peak detector, which raises CONV
 * (PB0). The firmware polls CONV, pulls DRESET (PC2) low, clocks two
 * bytes over SPI (MSB first) and releases DRESET again.
 *
 * Hits are lost when
 *   - the peak detector already holds an unread value (pile-up), or
 *   - DRESET is held low at the moment of the hit.
 */
#include <math.h>
#include <string.h>
#include "board.h"
#include "avr_ioport.h"
#include "avr_spi.h"

static avr_irq_t *pin_conv;

static uint16_t draw_value(ustsipin03_t *p)
{
	if (rng_uniform() < p->noise_frac) {
		double v = fabs(p->noise_mu + p->noise_sigma * rng_normal());
		if (v > p->threshold - 1)
			v = p->threshold - 1;
		return (uint16_t)v;
	}
	double v = p->threshold * pow(rng_uniform(), -1.0 / p->alpha);
	if (v > 65535)
		v = 65535;
	return (uint16_t)v;
}

static avr_cycle_count_t next_hit_in(ustsipin03_t *p)
{
	scenario_point_t e;
	scenario_eval(p->scen, sim_time(p->avr), &e);
	if (e.rate_cps <= 0)
		return sec_to_cycles(p->avr, 0.1);      /* re-check later */
	double dt = -log(rng_uniform()) / e.rate_cps;
	avr_cycle_count_t c = sec_to_cycles(p->avr, dt);
	return c ? c : 1;
}

static avr_cycle_count_t hit_timer(avr_t *avr, avr_cycle_count_t when, void *param)
{
	ustsipin03_t *p = param;
	scenario_point_t e;
	scenario_eval(p->scen, sim_time(avr), &e);

	if (e.rate_cps > 0) {
		uint16_t v = draw_value(p);
		p->generated++;
		if (!p->dreset) {
			p->lost_in_reset++;
			truth(avr, "hit", "%u,reset", v);
		} else if (p->conv) {
			p->pileup++;
			truth(avr, "hit", "%u,pileup", v);
		} else {
			p->latched = v;
			p->conv = 1;
			truth(avr, "hit", "%u,latched", v);
			avr_raise_irq(pin_conv, 1);
		}
	}
	return when + next_hit_in(p);
}

static void dreset_hook(avr_irq_t *irq, uint32_t value, void *param)
{
	ustsipin03_t *p = param;
	int level = value & 1;

	if (level == p->dreset)
		return;
	p->dreset = level;
	if (level)
		return;

	/* DRESET falling edge: ADC word becomes available on SPI, peak detector resets */
	p->spi_idx = 0;
	if (p->conv) {
		p->spi_word = p->latched;
		p->read_valid++;
		truth(p->avr, "read", "%u", p->latched);
		if (p->verbose)
			fprintf(stderr, "[adc] t=%.6f read %u\n", sim_time(p->avr), p->latched);
	} else {
		p->spi_word = 0;
		p->read_empty++;
	}
	p->conv = 0;
	avr_raise_irq(pin_conv, 0);
}

static void spi_hook(avr_irq_t *irq, uint32_t value, void *param)
{
	ustsipin03_t *p = param;
	uint8_t out = 0;

	if (p->spi_idx == 0)
		out = p->spi_word >> 8;
	else if (p->spi_idx == 1)
		out = p->spi_word & 0xff;
	p->spi_idx++;
	avr_raise_irq(avr_io_getirq(p->avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_INPUT), out);
}

void ustsipin03_init(avr_t *avr, ustsipin03_t *p, const scenario_t *scen)
{
	memset(p, 0, sizeof(*p));
	p->avr = avr;
	p->scen = scen;
	p->dreset = 1;
	p->threshold = 64;
	p->noise_frac = 0.7;
	p->noise_mu = 20;
	p->noise_sigma = 12;
	p->alpha = 1.3;
}

void ustsipin03_attach(avr_t *avr, ustsipin03_t *p)
{
	pin_conv = avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('B'), IOPORT_IRQ_PIN0);
	avr_raise_irq(pin_conv, 0);

	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_IOPORT_GETIRQ('C'), IOPORT_IRQ_PIN2),
			dreset_hook, p);
	avr_irq_register_notify(avr_io_getirq(avr, AVR_IOCTL_SPI_GETIRQ(0), SPI_IRQ_OUTPUT),
			spi_hook, p);
	avr_cycle_timer_register(avr, next_hit_in(p), hit_timer, p);
}
