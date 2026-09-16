/*
 * I2C slave models: 16-bit addressed EEPROM and Sensirion SHT31-DIS.
 *
 * simavr's TWI master sends one message per bus event:
 *   START (with 8-bit address incl. R/W)  -> slave answers ACK if addressed
 *   WRITE (data byte)                     -> slave answers ACK
 *   READ                                  -> slave answers READ with data
 *   STOP
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "board.h"
#include "avr_twi.h"

static const char *_irq_names[2] = {
	[TWI_IRQ_INPUT] = "8>i2c.out",
	[TWI_IRQ_OUTPUT] = "32<i2c.in",
};

static void i2c_ack(avr_irq_t *irq, uint8_t addr)
{
	avr_raise_irq(irq + TWI_IRQ_INPUT, avr_twi_irq_msg(TWI_COND_ACK, addr, 1));
}

/* ================================================================== */
/* EEPROM                                                              */
/* ================================================================== */

static void eeprom_hook(avr_irq_t *irq, uint32_t value, void *param)
{
	i2c_eeprom16_t *p = param;
	avr_twi_msg_irq_t v = { .u.v = value };

	if (v.u.twi.msg & TWI_COND_STOP) {
		p->selected = 0;
		p->index = 0;
	}
	if (v.u.twi.msg & TWI_COND_START) {
		p->selected = 0;
		p->index = 0;
		if ((v.u.twi.addr >> 1) == p->addr7) {
			p->selected = v.u.twi.addr;
			i2c_ack(p->irq, p->selected);
		}
	}
	if (!p->selected)
		return;

	if (v.u.twi.msg & TWI_COND_WRITE) {
		i2c_ack(p->irq, p->selected);
		if (p->index == 0)
			p->reg = (uint16_t)v.u.twi.data << 8;
		else if (p->index == 1) {
			p->reg |= v.u.twi.data;
			if (p->verbose)
				fprintf(stderr, "[%s] address pointer 0x%04x\n", p->name, p->reg);
		} else {
			if (p->verbose)
				fprintf(stderr, "[%s] write 0x%04x = %02x\n", p->name, p->reg, v.u.twi.data);
			p->mem[p->reg++] = v.u.twi.data;
		}
		p->index++;
	}
	if (v.u.twi.msg & TWI_COND_READ) {
		uint8_t d = p->mem[p->reg];
		if (p->verbose)
			fprintf(stderr, "[%s] read 0x%04x = %02x\n", p->name, p->reg, d);
		p->reg++;
		avr_raise_irq(p->irq + TWI_IRQ_INPUT,
				avr_twi_irq_msg(TWI_COND_READ, p->selected, d));
	}
}

void i2c_eeprom16_init(avr_t *avr, i2c_eeprom16_t *p, const char *name, uint8_t addr7)
{
	memset(p, 0, sizeof(*p));
	memset(p->mem, 0xff, sizeof(p->mem));
	p->name = name;
	p->addr7 = addr7;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, 2, _irq_names);
	avr_irq_register_notify(p->irq + TWI_IRQ_OUTPUT, eeprom_hook, p);
}

void i2c_eeprom16_attach(avr_t *avr, i2c_eeprom16_t *p)
{
	avr_connect_irq(p->irq + TWI_IRQ_INPUT,
			avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_INPUT));
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_OUTPUT),
			p->irq + TWI_IRQ_OUTPUT);
}

/* ================================================================== */
/* SHT31-DIS                                                           */
/* ================================================================== */

/* CRC-8, polynomial 0x31, init 0xFF (Sensirion datasheet) */
static uint8_t sht_crc(const uint8_t *d, int n)
{
	uint8_t crc = 0xff;
	for (int i = 0; i < n; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
	}
	return crc;
}

static void sht_put16(sht31_t *p, uint16_t w)
{
	p->out[p->out_len] = w >> 8;
	p->out[p->out_len + 1] = w & 0xff;
	p->out[p->out_len + 2] = sht_crc(p->out + p->out_len, 2);
	p->out_len += 3;
}

static void sht_command(sht31_t *p)
{
	avr_t *avr = p->avr;
	uint16_t c = p->cmd;

	switch (c) {
	/* single shot, no clock stretching: high / medium / low repeatability */
	case 0x2400: case 0x240B: case 0x2416:
	/* single shot, clock stretching (modelled without stretching) */
	case 0x2C06: case 0x2C0D: case 0x2C10: {
		scenario_point_t e;
		double t = sim_time(avr);
		scenario_eval(p->scen, t, &e);
		double tc = e.temp_c, rh = e.rh_pct;
		if (tc < -45) tc = -45;
		if (tc > 130) tc = 130;
		if (rh < 0) rh = 0;
		if (rh > 100) rh = 100;
		uint16_t t_raw = (uint16_t)lround((tc + 45.0) / 175.0 * 65535.0);
		uint16_t rh_raw = (uint16_t)lround(rh / 100.0 * 65535.0);
		p->out_len = p->out_pos = 0;
		sht_put16(p, t_raw);
		sht_put16(p, rh_raw);
		p->measuring = 1;
		p->ready_at = avr->cycle + sec_to_cycles(avr, p->meas_ms / 1000.0);
		p->measurements++;
		truth(avr, "sht31", "%.3f,%.3f,%u,%u", tc, rh, t_raw, rh_raw);
		if (p->verbose)
			fprintf(stderr, "[sht31] t=%.3f s measure %.2f C %.2f %%RH\n", t, tc, rh);
		break;
	}
	case 0x30A2:            /* soft reset */
	case 0x3041:            /* clear status */
		p->out_len = p->out_pos = 0;
		p->measuring = 0;
		break;
	case 0xF32D:            /* read status register */
		p->out_len = p->out_pos = 0;
		sht_put16(p, 0x0000);
		p->measuring = 0;
		break;
	default:
		p->unknown_cmds++;
		if (p->verbose)
			fprintf(stderr, "[sht31] unknown command 0x%04x\n", c);
	}
}

static void sht_hook(avr_irq_t *irq, uint32_t value, void *param)
{
	sht31_t *p = param;
	avr_t *avr = p->avr;
	avr_twi_msg_irq_t v = { .u.v = value };

	if (v.u.twi.msg & TWI_COND_STOP) {
		if (p->selected && !(p->selected & 1) && p->index >= 2)
			sht_command(p);
		p->selected = 0;
		p->index = 0;
	}
	if (v.u.twi.msg & TWI_COND_START) {
		/* a repeated start also terminates a pending write */
		if (p->selected && !(p->selected & 1) && p->index >= 2)
			sht_command(p);
		p->selected = 0;
		p->index = 0;
		if (p->present && (v.u.twi.addr >> 1) == p->addr7) {
			if ((v.u.twi.addr & 1) && p->measuring && avr->cycle < p->ready_at) {
				/* measurement still in progress: device NACKs its address */
				p->busy_nacks++;
				if (p->verbose)
					fprintf(stderr, "[sht31] read while busy -> NACK\n");
				return;
			}
			p->selected = v.u.twi.addr;
			i2c_ack(p->irq, p->selected);
		}
	}
	if (!p->selected)
		return;

	if (v.u.twi.msg & TWI_COND_WRITE) {
		i2c_ack(p->irq, p->selected);
		if (p->index == 0)
			p->cmd = (uint16_t)v.u.twi.data << 8;
		else if (p->index == 1)
			p->cmd |= v.u.twi.data;
		p->index++;
	}
	if (v.u.twi.msg & TWI_COND_READ) {
		uint8_t d = 0xff;
		if (p->out_pos < p->out_len)
			d = p->out[p->out_pos++];
		if (p->out_pos >= p->out_len)
			p->measuring = 0;
		avr_raise_irq(p->irq + TWI_IRQ_INPUT,
				avr_twi_irq_msg(TWI_COND_READ, p->selected, d));
	}
}

void sht31_init(avr_t *avr, sht31_t *p, uint8_t addr7, const scenario_t *scen, double meas_ms)
{
	memset(p, 0, sizeof(*p));
	p->avr = avr;
	p->addr7 = addr7;
	p->scen = scen;
	p->meas_ms = meas_ms;
	p->present = 1;
	p->irq = avr_alloc_irq(&avr->irq_pool, 0, 2, _irq_names);
	avr_irq_register_notify(p->irq + TWI_IRQ_OUTPUT, sht_hook, p);
}

void sht31_attach(avr_t *avr, sht31_t *p)
{
	avr_connect_irq(p->irq + TWI_IRQ_INPUT,
			avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_INPUT));
	avr_connect_irq(avr_io_getirq(avr, AVR_IOCTL_TWI_GETIRQ(0), TWI_IRQ_OUTPUT),
			p->irq + TWI_IRQ_OUTPUT);
}
