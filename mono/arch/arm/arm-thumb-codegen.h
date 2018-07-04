#ifndef __MONO_ARM_THUMB_CODEGEN_H__
#define __MONO_ARM_THUMB_CODEGEN_H__

typedef unsigned short arminstrtb_t;

#if defined(_MSC_VER) || defined(__CC_NORCROFT)
	void __inline _arm_emit_tb(arminstrtb_t** p, arminstrtb_t i) {**p = i; (*p)++;}
#	define ARM_EMIT_TB(p, i) _arm_emit_tb((arminstrtb_t**)&p, (arminstrtb_t)(i))
#else
#	define ARM_EMIT_TB(p, i) do { arminstrtb_t *__ainstrp = (void*)(p); *__ainstrp = (arminstrtb_t)(i); (p) = (void*)(__ainstrp+1);} while (0)
#endif

#define ARMTB_PUSH(p, regs) do { \
		if ((regs) & ~0x40FF) { \
			g_assert_not_reached (); \
		} else { \
			ARM_EMIT_TB(p, ((regs) & 0xff) | (((regs) >> ARMREG_LR) << 8) | (0x5a << 9)); \
		} \
	} while (0)

#define ARMTB_POP(p, regs) do { \
		if ((regs) & ~0x80FF) { \
			g_assert_not_reached (); \
		} else { \
			ARM_EMIT_TB(p, ((regs) & 0xff) | (((regs) >> ARMREG_PC) << 8) | (0x5e << 9)); \
		} \
	} while (0)

#define ARMTB_MOV_REG_REG(p, rd, rm) ARM_EMIT_TB(p, ((rd) & 0x7) | ((rm) << 3) | (((rd) >> 3) << 7) | (0x46 << 8))

#define ARMTB_SUB_REG_REG(p, rd, rn, rm) do { \
		g_assert ((rd) < 8 && (rn) < 8 && (rm) < 8); \
		ARM_EMIT_TB(p, (rd) | ((rn) << 3) | ((rm) << 6) | (0xd << 9)); \
	} while (0)

#define ARMTB_ADD_REG_REG(p, rd, rn, rm) do { \
		g_assert ((rd) < 8 && (rn) < 8 && (rm) < 8); \
		ARM_EMIT_TB(p, (rd) | ((rn) << 3) | ((rm) << 6) | (0xc << 9)); \
	} while (0)

#define ARMTB_SUB_SP_REG(p, rd, rm) do { \
		ARM_EMIT_TB(p, 0xebad); \
		ARM_EMIT_TB(p, (rm) | ((rd) << 8)); \
	} while (0)

#define ARMTB_SUB_REG_IMM(p, rd, rn, imm) do { \
		if ((imm) < 8 && (rd) < 8 && (rn) < 8) { \
			ARM_EMIT_TB(p, (rd) | ((rn) << 3) | ((imm) << 6) | (0xf << 9)); \
		} else if ((imm) < 256 && (rd) < 8 && (rd) == (rn)) { \
			ARM_EMIT_TB(p, (imm) | ((rd) << 8) | (0x7 << 11)); \
		} else { \
			g_assert ((imm) < 4096); \
			ARM_EMIT_TB(p, (rn) | (0x2a << 4) | (((imm) >> 11) << 10) | (0x1e << 11)); \
			ARM_EMIT_TB(p, ((imm) & 0xff) | ((rd) << 8) | (((imm) & 0x700) << 4)); \
		} \
	} while (0)

#define ARMTB_ADD_REG_IMM(p, rd, rn, imm) do { \
		if ((imm) < 8 && (rd) < 8 && (rn) < 8) { \
			ARM_EMIT_TB(p, (rd) | ((rn) << 3) | ((imm) << 6) | (0xe << 9)); \
		} else if ((imm) < 256 && (rd) < 8 && (rd) == (rn)) { \
			ARM_EMIT_TB(p, (imm) | ((rd) << 8) | (0x6 << 11)); \
		} else { \
			g_assert ((imm) < 4096); \
			ARM_EMIT_TB(p, (rn) | (0x1 << 9) |  (((imm) >> 11) << 10) | (0x1e << 11)); \
			ARM_EMIT_TB(p, ((imm) & 0xff) | ((rd) << 8) | (((imm) & 0x700) << 4)); \
		} \
	} while (0)

#define ARMTB_SUB_SP_IMM(p, rd, imm) do { \
		if (!((imm) & 0x3) && (imm) < 512 && (rd) == ARMREG_SP) { \
			ARM_EMIT_TB(p, (imm >> 2) | (0x1 << 7) | (0xb << 12)); \
		} else { \
			g_assert ((imm) < 4096); \
			ARM_EMIT_TB(p, 0xf2ad | (((imm) >> 11) << 10)); \
			ARM_EMIT_TB(p, ((imm) & 0xff) | ((rd) << 8) | (((imm) & 0x700) << 4)); \
		} \
	} while (0) 

#define ARMTB_STR_IMM(p, rt, rn, imm) do { \
		if (!((imm) & 0x3) && (imm) >= 0 && (imm) < 128 && (rt) < 8 && (rn) < 8) { \
			ARM_EMIT_TB(p, (rt) | ((rn) << 3) | ((imm) << 4) | (0xc << 11)); \
		} else if ((rn) == ARMREG_SP && (rt) < 8 && !((imm) & 0x3) && (imm) >= 0 && (imm) < 1024) { \
			ARM_EMIT_TB(p, ((imm) >> 2) | ((rt) << 8) | (0x12 << 11)); \
		} else if ((imm) >= 0) { \
			g_assert((imm) < 4096); \
			ARM_EMIT_TB(p, (rn) | 0xf8c0); \
			ARM_EMIT_TB(p, (imm) | ((rt) << 12)); \
		} else { \
			g_assert ((imm) > -256); \
			ARM_EMIT_TB (p, (rn) | 0xf840); \
			ARM_EMIT_TB (p, (-(imm)) | (0x3 << 10) | ((rt) << 12)); \
		} \
	} while (0)

#define ARMTB_LDR_IMM(p, rt, rn, imm) do { \
		if (!((imm) & 0x3) && (imm) >= 0 && (imm) < 128 && (rt) < 8 && (rn) < 8) { \
			ARM_EMIT_TB(p, (rt) | ((rn) << 3) | ((imm) << 4) | (0xd << 11)); \
		} else if ((rn) == ARMREG_SP && (rt) < 8 && !((imm) & 0x3) && (imm) >= 0 && (imm) < 1024) { \
			ARM_EMIT_TB(p, ((imm) >> 2) | ((rt) << 8) | (0x13 << 11)); \
		} else if ((imm) >= 0) { \
			g_assert((imm) < 4096); \
			ARM_EMIT_TB(p, (rn) | 0xf8d0); \
			ARM_EMIT_TB(p, (imm) | ((rt) << 12)); \
		} else { \
			g_assert ((imm) > -256); \
			ARM_EMIT_TB (p, (rn) | 0xf850); \
			ARM_EMIT_TB (p, (-(imm)) | (0x3 << 10) | ((rt) << 12)); \
		} \
	} while (0)

#define ARMTB_BLX_REG(p, reg) ARM_EMIT_TB(p, 0x4780 | ((reg) << 3))
#define ARMTB_B_COND(p, cond, offs) do { \
		if ((cond) == ARMCOND_AL && (offs) >= -2048 && (offs) <= 2046) \
			ARM_EMIT_TB(p, (((offs) >> 1) & 0x7ff) | (0x1c << 11)); \
		else if ((offs) >= -256 && (offs) <= 254) \
			ARM_EMIT_TB(p, ((offs) >> 1) | ((cond) << 8) | (0xd << 12)); \
		else \
			g_assert_not_reached (); \
	} while (0)
#define ARMTB_B(p, offs) ARMTB_B_COND(p, ARMCOND_AL, (offs))

#define ARMTB_CMP_REG_IMM(p, rn, imm) do { \
		if ((rn) < 8 && (imm) < 256) \
			ARM_EMIT_TB(p, (imm) | ((rn) << 8) | (0x5 << 11)); \
		else \
			g_assert_not_reached (); \
	} while (0)

#define ARMTB_FSTD(p,freg,base,offset) do { \
		ARM_EMIT_TB(p, (base) | (((freg) >> 4) << 6) | (0x1db << 7)); \
		ARM_EMIT_TB(p, ((offset) >> 2) | (0xb << 8) | (((freg) & 0xf) << 12)); \
	} while (0)
#define ARMTB_FLDD(p,freg,base,offset) do { \
		ARM_EMIT_TB(p, (base) | (1 << 4) | (((freg) >> 4) << 6) | (0x1db << 7)); \
		ARM_EMIT_TB(p, ((offset) >> 2) | (0xb << 8) | (((freg) & 0xf) << 12)); \
	} while (0)

#endif /* __MONO_ARM_THUMB_CODEGEN_H__ */
