#ifndef __MONO_MINI_MAGIC_DIVISION_H__
#define __MONO_MINI_MAGIC_DIVISION_H__

#include <config.h>
#include "mini.h"
#include "ir-emit.h"

#if SIZEOF_REGISTER == 8
struct magic_unsigned {
	guint32 magic_number;
	gboolean addition;
	int shift;
};

struct magic_signed {
	gint32 magic_number;
	int shift;
};

/* http://www.hackersdelight.org/hdcodetxt/magicu.c.txt */
static struct magic_unsigned
compute_magic_unsigned (guint32 divisor) {
	guint32 nc, delta, q1, r1, q2, r2;
	struct magic_unsigned magu;
	gboolean gt = FALSE;
	int p;

	magu.addition = 0;
	nc = -1 - (-divisor) % divisor;
	p = 31;
	q1 = 0x80000000 / nc;
	r1 = 0x80000000 - q1 * nc;
	q2 = 0x7FFFFFFF / divisor;
	r2 = 0x7FFFFFFF - q2 * divisor;
	do {
		p = p + 1;
		if (q1 >= 0x80000000)
			gt = TRUE;
		if (r1 >= nc - r1) {
			q1 = 2 * q1 + 1;
			r1 = 2 * r1 - nc;
		} else {
			q1 = 2 * q1;
			r1 = 2 * r1;
		}
		if (r2 + 1 >= divisor - r2) {
			if (q2 >= 0x7FFFFFFF)
				magu.addition = 1;
			q2 = 2 * q2 + 1;
			r2 = 2 * r2 + 1 - divisor;
		} else {
			if (q2 >= 0x80000000)
				magu.addition = 1;
			q2 = 2 * q2;
			r2 = 2 * r2 + 1;
		}
		delta = divisor - 1 - r2;
	} while (!gt && (q1 < delta || (q1 == delta && r1 == 0)));

	magu.magic_number = q2 + 1;
	magu.shift = p - 32;
	return magu;
}

/* http://www.hackersdelight.org/hdcodetxt/magic.c.txt */
static struct magic_signed
compute_magic_signed (gint32 divisor) {
	int p;
	guint32 ad, anc, delta, q1, r1, q2, r2, t;
	const guint32 two31 = 0x80000000;
	struct magic_signed mag;

	ad = abs (divisor);
	t = two31 + ((unsigned)divisor >> 31);
	anc = t - 1 - t % ad;
	p = 31;
	q1 = two31 / anc;
	r1 = two31 - q1 * anc;
	q2 = two31 / ad;
	r2 = two31 - q2 * ad;
	do {
		p++;
		q1 *= 2;
		r1 *= 2;
		if (r1 >= anc) {
			q1++;
			r1 -= anc;
		}

		q2 *= 2;
		r2 *= 2;

		if (r2 >= ad) {
			q2++;
			r2 -= ad;
		}

		delta = ad - r2;
	} while (q1 < delta || (q1 == delta && r1 == 0));

	mag.magic_number = q2 + 1;
	if (divisor < 0)
		mag.magic_number = -mag.magic_number;
	mag.shift = p - 32;
	return mag;
}
#endif

static gboolean
mono_strength_reduction_division (MonoCompile *cfg, MonoInst *ins)
{
	gboolean allocated_vregs = FALSE;
	/*
	 * We don't use it on 32bit systems because on those
	 * platforms we emulate long multiplication, driving the
	 * performance back down.
	 */
#if SIZEOF_REGISTER == 8
	switch (ins->opcode) {
		case OP_IDIV_UN_IMM: {
			guint32 tmp_regl, dividend_reg;
			struct magic_unsigned mag;
			int power2 = mono_is_power_of_two (ins->inst_imm);

			/* The decomposition doesn't handle exception throwing */
			if (ins->inst_imm == 0)
				break;

			if (power2 >= 0) {
				ins->opcode = OP_ISHR_UN_IMM;
				ins->sreg2 = -1;
				ins->inst_imm = power2;
				break;
			}
			allocated_vregs = TRUE;
			/*
			 * Replacement of unsigned division with multiplication,
			 * shifts and additions Hacker's Delight, chapter 10-10.
			 */
			mag = compute_magic_unsigned (ins->inst_imm);
			tmp_regl = alloc_lreg (cfg);
			dividend_reg = alloc_lreg (cfg);
			MONO_EMIT_NEW_I8CONST (cfg, tmp_regl, mag.magic_number);
			MONO_EMIT_NEW_UNALU (cfg, OP_ZEXT_I4, dividend_reg, ins->sreg1);
			MONO_EMIT_NEW_BIALU (cfg, OP_LMUL, tmp_regl, dividend_reg, tmp_regl);
			if (mag.addition) {
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_UN_IMM, tmp_regl, tmp_regl, 32);
				MONO_EMIT_NEW_BIALU (cfg, OP_LADD, tmp_regl, tmp_regl, dividend_reg);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_UN_IMM, ins->dreg, tmp_regl, mag.shift);
			} else {
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_UN_IMM, ins->dreg, tmp_regl, 32 + mag.shift);
			}
			break;
		}
		case OP_IDIV_IMM: {
			guint32 tmp_regl, dividend_reg;
			struct magic_signed mag;
			int power2 = mono_is_power_of_two (ins->inst_imm);

			/* The decomposition doesn't handle exception throwing */
			if (ins->inst_imm == 0 || ins->inst_imm == -1)
				break;
			allocated_vregs = TRUE;
			if (power2 == 1) {
				guint32 r1 = alloc_ireg (cfg);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_UN_IMM, r1, ins->sreg1, 31);
				MONO_EMIT_NEW_BIALU (cfg, OP_IADD, r1, r1, ins->sreg1);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_IMM, ins->dreg, r1, 1);
				break;
			} else if (power2 > 0 && power2 < 31) {
				guint32 r1 = alloc_ireg (cfg);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_IMM, r1, ins->sreg1, 31);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_UN_IMM, r1, r1, (32 - power2));
				MONO_EMIT_NEW_BIALU (cfg, OP_IADD, r1, r1, ins->sreg1);
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISHR_IMM, ins->dreg, r1, power2);
				break;
			}

			/*
			 * Replacement of signed division with multiplication,
			 * shifts and additions Hacker's Delight, chapter 10-6.
			 */
			mag = compute_magic_signed (ins->inst_imm);
			tmp_regl = alloc_lreg (cfg);
			dividend_reg = alloc_lreg (cfg);
			MONO_EMIT_NEW_I8CONST (cfg, tmp_regl, mag.magic_number);
			MONO_EMIT_NEW_UNALU (cfg, OP_SEXT_I4, dividend_reg, ins->sreg1);
			MONO_EMIT_NEW_BIALU (cfg, OP_LMUL, tmp_regl, dividend_reg, tmp_regl);
			if ((ins->inst_imm > 0 && mag.magic_number < 0) || (ins->inst_imm < 0 && mag.magic_number > 0)) {
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_IMM, tmp_regl, tmp_regl, 32);
				if (ins->inst_imm > 0 && mag.magic_number < 0) {
					MONO_EMIT_NEW_BIALU (cfg, OP_LADD, tmp_regl, tmp_regl, dividend_reg);
				} else if (ins->inst_imm < 0 && mag.magic_number > 0) {
					MONO_EMIT_NEW_BIALU (cfg, OP_LSUB, tmp_regl, tmp_regl, dividend_reg);
				}
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_IMM, tmp_regl, tmp_regl, mag.shift);
			} else {
				MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_IMM, tmp_regl, tmp_regl, 32 + mag.shift);
			}
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_LSHR_UN_IMM, ins->dreg, tmp_regl, SIZEOF_REGISTER * 8 - 1);
			MONO_EMIT_NEW_BIALU (cfg, OP_LADD, ins->dreg, ins->dreg, tmp_regl);
			break;
		}
	}
#endif
	return allocated_vregs;
}

#endif
