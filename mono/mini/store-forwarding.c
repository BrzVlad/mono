/*
 * store-forwarding.c: Implement simple store forwarding analysis for local variables.
 *
 * Author:
 *   Vlad Brezae (brezaevlad@gmail.com)
 *
 */

#include <config.h>
#include <stdio.h>

#include "mini.h"
#include "ir-emit.h"
#include "glib.h"

#ifndef DISABLE_JIT
	
struct hash_tables {
	/* Maps a vreg containing an address to the vreg that it addresses */
	GHashTable *addr_loads;
	/* Maps a vreg containing an address to the offset from the original
	 address introduced by OP_LDADDR */
	GHashTable *addr_offsets;
	/* Maps a vreg that has addresses to a hash containing mappings from
	 offsets to the corresponding last recorded store instruction */
	GHashTable *stores;
	/* Maps a vreg that has addresses to 1 or 0, indicating whether is
	 tracked or not */
	GHashTable *vreg_is_tracked;
};

static guint16
store_to_regmove (guint16 opcode)
{
	switch (opcode) {
		case OP_STORE_MEMBASE_REG:
		case OP_STOREI1_MEMBASE_REG:
		case OP_STOREI2_MEMBASE_REG:
		case OP_STOREI4_MEMBASE_REG:
			return OP_MOVE;
		case OP_STOREI8_MEMBASE_REG:
#if SIZEOF_REGISTER == 8
			return OP_MOVE;
#else
			return OP_LMOVE;
#endif
		case OP_STORER4_MEMBASE_REG:
		case OP_STORER8_MEMBASE_REG:
			return OP_FMOVE;
		case OP_STOREV_MEMBASE:
			return OP_VMOVE;
	}

	g_assert_not_reached ();
	return 0;
}

static guint16
store_to_iconst (guint16 opcode)
{
	switch (opcode) {
#if SIZEOF_VOID_P == 4
		case OP_STORE_MEMBASE_IMM:
#endif
		case OP_STOREI1_MEMBASE_IMM:
		case OP_STOREI2_MEMBASE_IMM:
		case OP_STOREI4_MEMBASE_IMM:
			return OP_ICONST;
		break;

#if SIZEOF_VOID_P == 8
		case OP_STORE_MEMBASE_IMM:
#endif
		case OP_STOREI8_MEMBASE_IMM:
			return OP_I8CONST;
		break;
	}

	g_assert_not_reached ();
	return 0;
}

static gboolean
is_store_immediate (guint16 opcode)
{
	return opcode >= OP_STORE_MEMBASE_IMM &&
		opcode <= OP_STOREI8_MEMBASE_IMM;
}

static gboolean
equivalent_store_load (guint16 load_op, guint16 store_op)
{
	return TRUE;
}

static gboolean
lower_load (MonoCompile *cfg, MonoInst *load, MonoInst *store)
{
	if (!equivalent_store_load (load->opcode, store->opcode)) {
		printf ("TODO");
		return FALSE;
	}

	if (is_store_immediate (store->opcode)) {
		load->opcode = store_to_iconst (store->opcode);
		load->sreg1 = -1;
		if (load->opcode == OP_ICONST) {
			load->type = STACK_I4;
			load->inst_c0 = store->inst_imm;
		} else {
			load->type = STACK_I8;
			load->inst_l = store->inst_imm;
		}
	} else {
		if (store_to_regmove (store->opcode) == OP_FMOVE)
			return FALSE;
        	load->opcode = store_to_regmove (store->opcode);
        	load->sreg1 = store->sreg1;
	}

	if (cfg->verbose_level > 2) {
		printf ("Lower load: ");
		mono_print_ins (load);
	}
        
	return TRUE;
}

//static void
//value_destroy_func (gpointer value)
//{
//
//}

static gboolean
lower_memory_access (MonoCompile *cfg)
{
	MonoBasicBlock *bb;
	MonoInst *ins;
	gint tmp_vreg, tmp_offset;
	gboolean needs_dce = FALSE;
	
	struct hash_tables hash_tables;
	hash_tables.addr_loads = g_hash_table_new (NULL, NULL);
	hash_tables.addr_offsets = g_hash_table_new (NULL, NULL);
	hash_tables.stores = g_hash_table_new (NULL, NULL);
	hash_tables.vreg_is_tracked = g_hash_table_new (NULL, NULL);

	g_hash_table_remove_all (hash_tables.vreg_is_tracked);
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		g_hash_table_remove_all (hash_tables.addr_loads);
		g_hash_table_remove_all (hash_tables.addr_offsets);
		g_hash_table_remove_all (hash_tables.stores);

		for (ins = bb->code; ins; ins = ins->next) {
			switch (ins->opcode) {
			case OP_LDADDR: {
				gpointer value;
				MonoInst *var = (MonoInst*)ins->inst_p0;
				gint32 vreg = var->dreg;

				if (var->type != STACK_VTYPE && var->type != STACK_I8) {
					if (cfg->verbose_level > 2) { printf ("Only handling loads of vtypes and longs\n"); mono_print_ins (ins); }
					break;
				}

				if (!g_hash_table_lookup_extended (hash_tables.vreg_is_tracked, GINT_TO_POINTER (vreg), NULL, &value)
					|| GPOINTER_TO_INT (value) == 1) {
					g_hash_table_insert (hash_tables.addr_loads, GINT_TO_POINTER (ins->dreg), GINT_TO_POINTER (vreg));
					g_hash_table_insert (hash_tables.addr_offsets, GINT_TO_POINTER (ins->dreg), GINT_TO_POINTER (0));
					g_hash_table_insert (hash_tables.vreg_is_tracked, GINT_TO_POINTER (vreg), GINT_TO_POINTER (1));
					if (cfg->verbose_level > 2) { printf ("New address for R%d: ", vreg); mono_print_ins (ins); }
				}
				break;
			}
			case OP_MOVE:
			case OP_PADD_IMM:
                                tmp_vreg = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_loads, GINT_TO_POINTER (ins->sreg1)));
				tmp_offset = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_offsets, GINT_TO_POINTER (ins->sreg1)));

				if (ins->opcode == OP_PADD_IMM)
					tmp_offset += ins->inst_imm;

				if (tmp_vreg && g_hash_table_lookup (hash_tables.vreg_is_tracked, GINT_TO_POINTER (tmp_vreg))) {
					/* Forward propagate known addresses */ 
					g_hash_table_insert (hash_tables.addr_loads, GINT_TO_POINTER (ins->dreg), GINT_TO_POINTER (tmp_vreg));
					g_hash_table_insert (hash_tables.addr_offsets, GINT_TO_POINTER (ins->dreg), GINT_TO_POINTER (tmp_offset));
				} else {
					/* Unkown alias. Kill the variable */
					g_hash_table_remove (hash_tables.addr_loads, GINT_TO_POINTER (ins->dreg));
					g_hash_table_remove (hash_tables.addr_offsets, GINT_TO_POINTER (ins->dreg));
				}
				break;
			case OP_STOREV_MEMBASE:
			case OP_STORE_MEMBASE_REG:
			case OP_STOREI1_MEMBASE_REG:
			case OP_STOREI2_MEMBASE_REG:
			case OP_STOREI4_MEMBASE_REG:
			case OP_STOREI8_MEMBASE_REG:
			case OP_STORER4_MEMBASE_REG:
			case OP_STORER8_MEMBASE_REG:
			case OP_STORE_MEMBASE_IMM:
			case OP_STOREI1_MEMBASE_IMM:
			case OP_STOREI2_MEMBASE_IMM:
			case OP_STOREI4_MEMBASE_IMM:
			case OP_STOREI8_MEMBASE_IMM: {
				GHashTable *addr_stores;
				MonoInst *latest_store;

				tmp_vreg = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_loads, GINT_TO_POINTER (ins->inst_destbasereg)));
				if (!tmp_vreg || !g_hash_table_lookup (hash_tables.vreg_is_tracked, GINT_TO_POINTER (tmp_vreg))) {
					/* Unknown base address */
					if (!is_store_immediate (ins->opcode)) {
						/* If sreg is known address, discard the register to which it points,
						 since it will be stored in untracked memory */
						tmp_vreg = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_loads, GINT_TO_POINTER (ins->sreg1)));
						if (tmp_vreg)
							g_hash_table_replace (hash_tables.vreg_is_tracked, GINT_TO_POINTER (tmp_vreg), GINT_TO_POINTER (0));
					}
					break;
				} 

				tmp_offset = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_offsets, GINT_TO_POINTER (ins->inst_destbasereg)));
				tmp_offset += ins->inst_offset;
			
				/* Obtain hash with stores for the base address */	
				addr_stores = (GHashTable*)g_hash_table_lookup (hash_tables.stores, GINT_TO_POINTER (tmp_vreg));
				if (!addr_stores) {
					addr_stores = g_hash_table_new (NULL, NULL);
					g_hash_table_insert (hash_tables.stores, GINT_TO_POINTER (tmp_vreg), addr_stores); 
				}

				latest_store = (MonoInst*) g_hash_table_lookup (addr_stores, GINT_TO_POINTER (tmp_offset));
//				if (latest_store) {
//					/* Since the value at the address will be rewritten the previous store can be removed */
//					if (cfg->verbose_level > 2) { printf ("Removed store: "); mono_print_ins (latest_store); mono_print_ins (ins); }
//					MONO_DELETE_INS (bb, latest_store);
//					needs_dce = TRUE;
//				}

				/* Record the value of the store */
				g_hash_table_replace (addr_stores, GINT_TO_POINTER (tmp_offset), ins);

				break;
			}
			case OP_LOADV_MEMBASE:
			case OP_LOAD_MEMBASE:
			case OP_LOADU1_MEMBASE:
			case OP_LOADI2_MEMBASE:
			case OP_LOADU2_MEMBASE:
			case OP_LOADI4_MEMBASE:
			case OP_LOADU4_MEMBASE:
			case OP_LOADI1_MEMBASE:
			case OP_LOADI8_MEMBASE:
			case OP_LOADR4_MEMBASE:
			case OP_LOADR8_MEMBASE: {
				GHashTable *addr_stores;
				MonoInst *latest_store;

				tmp_vreg = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_loads, GINT_TO_POINTER (ins->inst_basereg)));
				if (!tmp_vreg || !g_hash_table_lookup (hash_tables.vreg_is_tracked, GINT_TO_POINTER (tmp_vreg))) {
					/* Unknown base address. Kill the variable */
                                        g_hash_table_remove (hash_tables.addr_loads, GINT_TO_POINTER (ins->dreg));
					g_hash_table_remove (hash_tables.addr_offsets, GINT_TO_POINTER (ins->dreg));
					break;
				}

				tmp_offset = GPOINTER_TO_INT (g_hash_table_lookup (hash_tables.addr_offsets, GINT_TO_POINTER (ins->inst_basereg)));
				tmp_offset += ins->inst_offset;

				/* Obtain latest store to address */
				addr_stores = (GHashTable*)g_hash_table_lookup (hash_tables.stores, GINT_TO_POINTER (tmp_vreg));
				if (!addr_stores) {
					/* No available store to forward */
					break;
				}

				latest_store = (MonoInst*)g_hash_table_lookup (addr_stores, GINT_TO_POINTER (tmp_offset));
				if (!latest_store) {
					/* No available store to forward */
					break;
				}

				lower_load (cfg, ins, latest_store);

				break;
			}
			default: {
				gpointer value;
				const char *spec = INS_INFO (ins->opcode);
				int num_vregs, vregs [1 + MONO_MAX_SRC_REGS];
				int i;

				if (spec [MONO_INST_DEST] == ' ') {
					num_vregs = mono_inst_get_src_registers (ins, vregs);
				} else {
					vregs [0] = ins->dreg;
					num_vregs = 1 + mono_inst_get_src_registers (ins, vregs + 1);
				}

				/* If any tracked registers are used in other instructions,
				 we no longer try to optimize those addresses */
				for (i = 0; i < num_vregs; i++) {
					if (g_hash_table_lookup_extended (hash_tables.vreg_is_tracked, GINT_TO_POINTER (vregs [i]), NULL, &value)
						&& GPOINTER_TO_INT (value) == 1) {
						GHashTable *addr_stores;
						/* Value vreg */
						if (vregs [i] == ins->dreg) {
							addr_stores = (GHashTable*)g_hash_table_lookup (hash_tables.stores, GINT_TO_POINTER (vregs [i]));
							if (addr_stores) {
								if (cfg->verbose_level > 2) { printf ("Flush stores for vreg R%d: ", vregs [i]); mono_print_ins (ins); }
								g_hash_table_remove_all (addr_stores);
							}
						}
					} else if (g_hash_table_lookup_extended (hash_tables.addr_loads, GINT_TO_POINTER (vregs [i]), NULL, &value)) {
						/* Address vreg */
						g_hash_table_replace (hash_tables.vreg_is_tracked, value, GINT_TO_POINTER (0));
						if (cfg->verbose_level > 2) { printf ("Untracked vreg R%d: ", GPOINTER_TO_INT (value)); mono_print_ins (ins); }
					}
				}
				break;
			}
			}
		}
	}
	g_hash_table_destroy (hash_tables.addr_loads);
	g_hash_table_destroy (hash_tables.addr_offsets);
	g_hash_table_destroy (hash_tables.stores);
	g_hash_table_destroy (hash_tables.vreg_is_tracked);
	return needs_dce;
}

static gboolean
recompute_aliased_variables (MonoCompile *cfg)
{
	int i;
	MonoBasicBlock *bb;
	MonoInst *ins;
	int kills = 0;
	int adds = 0;

	for (i = 0; i < cfg->num_varinfo; i++) {
		MonoInst *var = cfg->varinfo [i];
		if (var->flags & MONO_INST_INDIRECT) {
			if (cfg->verbose_level > 2) {
				printf ("Killing :"); mono_print_ins (var);
			}
			++kills;
		}
		var->flags &= ~MONO_INST_INDIRECT;
	}

	if (!kills)
		return FALSE;

	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		for (ins = bb->code; ins; ins = ins->next) {
			if (ins->opcode == OP_LDADDR) {
				MonoInst *var;

				if (cfg->verbose_level > 2) { printf ("Found op :"); mono_print_ins (ins); }

				var = (MonoInst*)ins->inst_p0;
				if (!(var->flags & MONO_INST_INDIRECT)) {
					if (cfg->verbose_level > 1) { printf ("Restoring :"); mono_print_ins (var); }
					++adds;
				}
				var->flags |= MONO_INST_INDIRECT;
			}
		}
	}
	
	mono_jit_stats.alias_removed += kills - adds;
	if (kills > adds) {
		if (cfg->verbose_level > 2) {
			printf ("Method: %s\n", mono_method_full_name (cfg->method, 1));
			printf ("Kills %d Adds %d\n", kills, adds);
		}
		return TRUE;
	}
	return FALSE;
}

void
mono_local_store_forwarding (MonoCompile *cfg)
{
	if (!cfg->has_indirection)
		return;

	if (cfg->verbose_level > 2)
		mono_print_code (cfg, "BEFORE STORE_FORWARDING");
					
	/*
	 Remove memory access made with loads from store tracked addresses.
	 */
	if (!lower_memory_access (cfg))
		goto done;
	
	if (cfg->opt & MONO_OPT_DEADCE)
		mono_local_deadce (cfg);

	/*
         Some variables no longer need to be flagged as indirect, find them.
         */
	if (!recompute_aliased_variables (cfg))
		goto done;

	/*
	 By replacing indirect access with direct operations, some LDADDR ops become dead. Kill them.
	 */
	mono_handle_global_vregs (cfg);
	if (cfg->opt & MONO_OPT_DEADCE)
		mono_local_deadce (cfg);

done:
	if (cfg->verbose_level > 2)
		mono_print_code (cfg, "AFTER STORE_FORWARDING");
}

#endif /* !DISABLE_JIT */
