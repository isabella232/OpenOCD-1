/***************************************************************************
 *   Copyright (C) 2013-2014 Synopsys, Inc.                                *
 *   Frank Dols <frank.dols@synopsys.com>                                  *
 *   Mischa Jonker <mischa.jonker@synopsys.com>                            *
 *   Anton Kolesov <anton.kolesov@synopsys.com>                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arc.h"

/* ----- Supporting functions ---------------------------------------------- */
static int arc_dbg_configure_actionpoint(struct target *target, uint32_t ap_num,
	uint32_t match_value, uint32_t control_tt, uint32_t control_at)
{
	struct arc32_common *arc32 = target_to_arc32(target);
	uint32_t ap_reg_id = ap_num * AP_STRUCT_LEN;

	if (control_tt != AP_AC_TT_DISABLE) {

		if (arc32->num_action_points_avail < 1) {
			LOG_ERROR("No ActionPoint free, maximim amount is %" PRIu32,
					arc32->num_action_points);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		arc_jtag_write_aux_reg_one(&arc32->jtag_info, AP_AMV_BASE + ap_reg_id,
				match_value);
		arc_jtag_write_aux_reg_one(&arc32->jtag_info, AP_AMM_BASE + ap_reg_id, 0x0);
		arc_jtag_write_aux_reg_one(&arc32->jtag_info, AP_AC_BASE  + ap_reg_id,
				control_tt | control_at);
		arc32->num_action_points_avail--;
	} else {
		arc_jtag_write_aux_reg_one(&arc32->jtag_info, AP_AC_BASE  + ap_reg_id, AP_AC_TT_DISABLE);
		arc32->num_action_points_avail++;
	}

	return ERROR_OK;
}

static int arc_dbg_set_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	int retval = ERROR_OK;

	if (breakpoint->set) {
		LOG_WARNING("breakpoint already set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		struct arc32_common *arc32 = target_to_arc32(target);
		struct arc32_comparator *comparator_list = arc32->action_point_list;
		int bp_num = 0;

		while (comparator_list[bp_num].used)
			bp_num++;

		if (bp_num >= arc32->num_action_points) {
			LOG_ERROR("No ActionPoint free, maximim amount is %" PRIu32,
					arc32->num_action_points);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		retval = arc_dbg_configure_actionpoint(target, bp_num,
				breakpoint->address, AP_AC_TT_READWRITE, AP_AC_AT_INST_ADDR);

		if (retval == ERROR_OK) {
			breakpoint->set = bp_num + 1;
			comparator_list[bp_num].used = 1;
			comparator_list[bp_num].bp_value = breakpoint->address;

			LOG_DEBUG("bpid: %" PRIu32 ", bp_num %i bp_value 0x%" PRIx32,
					breakpoint->unique_id, bp_num, comparator_list[bp_num].bp_value);
		}

	} else if (breakpoint->type == BKPT_SOFT) {
		LOG_DEBUG("bpid: %" PRIu32, breakpoint->unique_id);

		if (breakpoint->length == 4) { /* WAS: == 4) { but we have only 32 bits access !!*/
			uint32_t verify = 0xffffffff;

			retval = target_read_buffer(target, breakpoint->address, breakpoint->length,
					breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;

			retval = arc32_write_instruction_u32(target, breakpoint->address,
					ARC32_SDBBP);
			if (retval != ERROR_OK)
				return retval;

			retval = arc32_read_instruction_u32(target, breakpoint->address, &verify);

			if (retval != ERROR_OK)
				return retval;
				if (verify != ARC32_SDBBP) {
				LOG_ERROR("Unable to set 32bit breakpoint at address %08" PRIx32
						" - check that memory is read/writable", breakpoint->address);
				return ERROR_OK;
			}
		} else if (breakpoint->length == 2) {
			uint16_t verify = 0xffff;

			retval = target_read_buffer(target, breakpoint->address, breakpoint->length,
					breakpoint->orig_instr);
			if (retval != ERROR_OK)
				return retval;
			retval = target_write_u16(target, breakpoint->address, ARC16_SDBBP);
			if (retval != ERROR_OK)
				return retval;

			retval = target_read_u16(target, breakpoint->address, &verify);
			if (retval != ERROR_OK)
				return retval;
			if (verify != ARC16_SDBBP) {
				LOG_ERROR("Unable to set 16bit breakpoint at address %08" PRIx32
						" - check that memory is read/writable", breakpoint->address);
				return ERROR_OK;
			}
		} else {
			LOG_ERROR("Invalid breakpoint length: target supports only 2 or 4");
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}

		/* core instruction cache is now invalid */
		arc32_cache_invalidate(target);

		breakpoint->set = 64; /* Any nice value but 0 */
	}

	return retval;
}

static int arc_dbg_unset_breakpoint(struct target *target,
		struct breakpoint *breakpoint)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->action_point_list;
	int retval;

	if (!breakpoint->set) {
		LOG_WARNING("breakpoint not set");
		return ERROR_OK;
	}

	if (breakpoint->type == BKPT_HARD) {
		int bp_num = breakpoint->set - 1;
		if ((bp_num < 0) || (bp_num >= arc32->num_action_points)) {
			LOG_DEBUG("Invalid ActionPoint ID: %" PRIu32 " in breakpoint: %" PRIu32,
					  bp_num, breakpoint->unique_id);
			return ERROR_OK;
		}

		retval =  arc_dbg_configure_actionpoint(target, bp_num,
						breakpoint->address, AP_AC_TT_DISABLE, AP_AC_AT_INST_ADDR);

		if (retval == ERROR_OK) {
			breakpoint->set = 0;
			comparator_list[bp_num].used = 0;
			comparator_list[bp_num].bp_value = 0;

			LOG_DEBUG("bpid: %" PRIu32 " - released ActionPoint ID: %i",
					breakpoint->unique_id, bp_num);
		}

	} else {
		/* restore original instruction (kept in target endianness) */
		LOG_DEBUG("bpid: %" PRIu32, breakpoint->unique_id);
		if (breakpoint->length == 4) {
			uint32_t current_instr;

			/* check that user program has not modified breakpoint instruction */
			retval = arc32_read_instruction_u32(target, breakpoint->address, &current_instr);
			if (retval != ERROR_OK)
				return retval;

			if (current_instr == ARC32_SDBBP) {
				target->running_alg = 1;
				retval = target_write_buffer(target, breakpoint->address,
					breakpoint->length, breakpoint->orig_instr);
				target->running_alg = 0;
				if (retval != ERROR_OK)
					return retval;
			}
		} else if (breakpoint->length == 2) {
			uint16_t current_instr;

			/* check that user program has not modified breakpoint instruction */
			retval = target_read_memory(target, breakpoint->address, 2, 1,
					(uint8_t *)&current_instr);
			if (retval != ERROR_OK)
				return retval;
			current_instr = target_buffer_get_u16(target, (uint8_t *)&current_instr);
			if (current_instr == ARC16_SDBBP) {
				target->running_alg = 1;
				retval = target_write_buffer(target, breakpoint->address,
					breakpoint->length, breakpoint->orig_instr);
				target->running_alg = 0;
				if (retval != ERROR_OK)
					return retval;
			}
		} else {
			LOG_ERROR("Invalid breakpoint length: target supports only 2 or 4");
			return ERROR_COMMAND_ARGUMENT_INVALID;
		}
		breakpoint->set = 0;
	}

	/* core instruction cache is now invalid */
	arc32_cache_invalidate(target);

	return retval;
}

static void arc_dbg_enable_breakpoints(struct target *target)
{
	struct breakpoint *breakpoint = target->breakpoints;

	/* set any pending breakpoints */
	while (breakpoint) {
		if (breakpoint->set == 0)
			arc_dbg_set_breakpoint(target, breakpoint);
		breakpoint = breakpoint->next;
	}
}

static int arc_dbg_set_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->action_point_list;

	if (watchpoint->set) {
		LOG_WARNING("watchpoint already set");
		return ERROR_OK;
	}

	int wp_num = 0;
	while (comparator_list[wp_num].used)
		wp_num++;

	if (wp_num >= arc32->num_action_points) {
		LOG_ERROR("No ActionPoint free, maximim amount is %" PRIu32,
				arc32->num_action_points);
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	/*
	 * TODO: Verify documentation, just tried and worked fine!!
	if (watchpoint->length != 4) {
		LOG_ERROR("Only watchpoints of length 4 are supported");
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	if (watchpoint->address % 4) {
		LOG_ERROR("Watchpoints address should be word aligned");
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}
	*/

	int enable = AP_AC_TT_DISABLE;
	switch (watchpoint->rw) {
		case WPT_READ:
			enable = AP_AC_TT_READ;
			break;
		case WPT_WRITE:
			enable = AP_AC_TT_WRITE;
			break;
		case WPT_ACCESS:
			enable = AP_AC_TT_READWRITE;
			break;
		default:
			LOG_ERROR("BUG: watchpoint->rw neither read, write nor access");
			return ERROR_FAIL;
	}

	int retval =  arc_dbg_configure_actionpoint(target, wp_num,
					watchpoint->address, enable, AP_AC_AT_MEMORY_ADDR);

	if (retval == ERROR_OK) {
		watchpoint->set = wp_num + 1;
		comparator_list[wp_num].used = 1;
		comparator_list[wp_num].bp_value = watchpoint->address;

		LOG_DEBUG("wpid: %" PRIu32 ", bp_num %i bp_value 0x%" PRIx32,
				watchpoint->unique_id, wp_num, comparator_list[wp_num].bp_value);
	}

	return retval;
}

static int arc_dbg_unset_watchpoint(struct target *target,
		struct watchpoint *watchpoint)
{
	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->action_point_list;

	if (!watchpoint->set) {
		LOG_WARNING("watchpoint not set");
		return ERROR_OK;
	}

	int wp_num = watchpoint->set - 1;
	if ((wp_num < 0) || (wp_num >= arc32->num_action_points)) {
		LOG_DEBUG("Invalid ActionPoint ID: %" PRIu32 " in watchpoint: %" PRIu32,
				wp_num, watchpoint->unique_id);
		return ERROR_OK;
	}

	int retval =  arc_dbg_configure_actionpoint(target, wp_num,
				watchpoint->address, AP_AC_TT_DISABLE, AP_AC_AT_MEMORY_ADDR);

	if (retval == ERROR_OK) {
		watchpoint->set = 0;
		comparator_list[wp_num].used = 0;
		comparator_list[wp_num].bp_value = 0;

		LOG_DEBUG("wpid: %" PRIu32 " - releasing ActionPoint ID: %i",
				watchpoint->unique_id, wp_num);
	}

	return retval;
}

static void arc_dbg_enable_watchpoints(struct target *target)
{
	struct watchpoint *watchpoint = target->watchpoints;

	/* set any pending watchpoints */
	while (watchpoint) {
		if (watchpoint->set == 0)
			arc_dbg_set_watchpoint(target, watchpoint);
		watchpoint = watchpoint->next;
	}
}

static int arc_dbg_single_step_core(struct target *target)
{
	arc_dbg_debug_entry(target);

	/* disable interrupts while stepping */
	arc32_enable_interrupts(target, 0);

	/* configure single step mode */
	arc32_config_step(target, 1);

	/* exit debug mode */
	arc_dbg_enter_debug(target);

	return ERROR_OK;
}

/* ----- Exported supporting functions ------s------------------------------- */

int arc_dbg_enter_debug(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t value;

	struct arc32_common *arc32 = target_to_arc32(target);

	target->state = TARGET_HALTED;

	//retval = arc_jtag_read_aux_reg(&arc32->jtag_info, AUX_DEBUG_REG, &value);
	//value |= SET_CORE_FORCE_HALT; /* set the HALT bit */
	value = SET_CORE_FORCE_HALT; /* set the HALT bit */
	retval = arc_jtag_write_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, value);
	alive_sleep(1);

#ifdef DEBUG
	LOG_DEBUG("core stopped (halted) DEGUB-REG: 0x%08" PRIx32, value);
	retval = arc_jtag_read_aux_reg(&arc32->jtag_info, AUX_STATUS32_REG, &value);
	LOG_DEBUG("core STATUS32: 0x%08" PRIx32, value);
#endif

	return retval;
}

int arc_dbg_examine_debug_reason(struct target *target)
{
	/* Only check for reason if don't know it already. */
	/* BTW After singlestep at this point core is not marked as halted, so
	 * reading from memory to get current instruction won't work anyways. */
	if (DBG_REASON_DBGRQ == target->debug_reason ||
	    DBG_REASON_SINGLESTEP == target->debug_reason) {
		return ERROR_OK;
	}

	int retval = ERROR_OK;

	/* Ensure that DEBUG register value is in cache */
	struct reg *debug_reg = &(target->reg_cache->reg_list[ARC_REG_DEBUG]);
	if (!debug_reg->valid) {
		retval = debug_reg->type->get(debug_reg);
		if (ERROR_OK != retval) {
			LOG_ERROR("Can not read DEBUG AUX register");
			return retval;
		}
	}

	/* DEBUG.BH is set if core halted due to BRK instruction. */
	uint32_t debug_reg_value = buf_get_u32(debug_reg->value, 0, debug_reg->size);
	if (debug_reg_value & SET_CORE_BREAKPOINT_HALT) {
		target->debug_reason = DBG_REASON_BREAKPOINT;
	}

	return retval;
}

int arc_dbg_debug_entry(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t dpc;

	struct arc32_common *arc32 = target_to_arc32(target);

	/* save current PC */
	retval = arc_jtag_read_aux_reg_one(&arc32->jtag_info, AUX_PC_REG, &dpc);
	if (retval != ERROR_OK)
		return retval;

	arc32->jtag_info.dpc = dpc;

	arc32_save_context(target);

	/* We must reset internal indicators of caches states, otherwise D$/I$
	 * will not be flushed/invalidated when required. */
	retval = arc32_reset_caches_states(target);
	if (ERROR_OK != retval)
	    return retval;

	retval = arc_dbg_examine_debug_reason(target);

	return retval;
}

int arc_dbg_exit_debug(struct target *target)
{
	int retval = ERROR_OK;
	uint32_t value;

	struct arc32_common *arc32 = target_to_arc32(target);

	target->state = TARGET_RUNNING;

	/* raise the Reset Applied bit flag */
	retval = arc_jtag_read_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, &value);
	value |= SET_CORE_RESET_APPLIED; /* set the RA bit */
	retval = arc_jtag_write_aux_reg_one(&arc32->jtag_info, AUX_DEBUG_REG, value);

#ifdef DEBUG
	arc32_print_core_state(target);
#endif
	return retval;
}

/* ----- Exported functions ------------------------------------------------ */

int arc_dbg_halt(struct target *target)
{
	int retval = ERROR_OK;

	LOG_DEBUG("target->state: %s", target_state_name(target));

	if (target->state == TARGET_HALTED) {
		LOG_DEBUG("target was already halted");
		return ERROR_OK;
	}

	if (target->state == TARGET_UNKNOWN)
		LOG_WARNING("target was in unknown state when halt was requested");

	if (target->state == TARGET_RESET) {
		if ((jtag_get_reset_config() & RESET_SRST_PULLS_TRST) && jtag_get_srst()) {
			LOG_ERROR("can't request a halt while in reset if nSRST pulls nTRST");
			return ERROR_TARGET_FAILURE;
		} else {
			/*
			 * we came here in a reset_halt or reset_init sequence
			 * debug entry was already prepared in arc700_assert_reset()
			 */
			target->debug_reason = DBG_REASON_DBGRQ;

			return ERROR_OK;
		}
	}

	/* break (stop) processor */
	retval = arc_dbg_enter_debug(target);
	if (retval != ERROR_OK)
		return retval;

	/* update state and notify gdb*/
	target->state = TARGET_HALTED;
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	return retval;
}

int arc_dbg_resume(struct target *target, int current, uint32_t address,
	int handle_breakpoints, int debug_execution)
{
	int retval = ERROR_OK;
	struct arc32_common *arc32 = target_to_arc32(target);
	struct breakpoint *breakpoint = NULL;
	uint32_t resume_pc = 0;

	LOG_DEBUG("current:%i, address:0x%08" PRIx32 ", handle_breakpoints:%i, debug_execution:%i",
		current, address, handle_breakpoints, debug_execution);

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (!debug_execution) {
		/* (gdb) continue = execute until we hit break/watch-point */
		LOG_DEBUG("we are in debug execution mode");
		target_free_all_working_areas(target);
		arc_dbg_enable_breakpoints(target);
		arc_dbg_enable_watchpoints(target);
	}

	/* current = 1: continue on current PC, otherwise continue at <address> */
	if (!current) {
		buf_set_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32, address);
		arc32->core_cache->reg_list[ARC_REG_PC].dirty = 1;
		arc32->core_cache->reg_list[ARC_REG_PC].valid = 1;
		LOG_DEBUG("Changing the value of current PC to 0x%08" PRIx32, address);
	}

	if (!current)
		resume_pc = address;
	else
		resume_pc = buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value,
			0, 32);

	arc32_restore_context(target);

	LOG_DEBUG("Target resumes from PC=0x%" PRIx32 ", pc.dirty=%i, pc.valid=%i",
		resume_pc,
		arc32->core_cache->reg_list[ARC_REG_PC].dirty,
		arc32->core_cache->reg_list[ARC_REG_PC].valid);

	/* check if GDB tells to set our PC where to continue from */
	if ((arc32->core_cache->reg_list[ARC_REG_PC].valid == 1) &&
		(resume_pc == buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value,
			0, 32))) {

		uint32_t value;
		value = buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32);
		LOG_DEBUG("resume Core (when start-core) with PC @:0x%08" PRIx32, value);
		arc_jtag_write_aux_reg_one(&arc32->jtag_info, AUX_PC_REG, value);
	}

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		/* Single step past breakpoint at current address */
		breakpoint = breakpoint_find(target, resume_pc);
		if (breakpoint) {
			LOG_DEBUG("unset breakpoint at 0x%08" PRIx32,
				breakpoint->address);
			arc_dbg_unset_breakpoint(target, breakpoint);
			arc_dbg_single_step_core(target);
			arc_dbg_set_breakpoint(target, breakpoint);
		}
	}

	/* enable interrupts if we are running */
	arc32_enable_interrupts(target, !debug_execution);

	/* exit debug mode */
	arc_dbg_enter_debug(target);
	target->debug_reason = DBG_REASON_NOTHALTED;

	/* ready to get us going again */
	arc32_start_core(target);

	/* registers are now invalid */
	register_cache_invalidate(arc32->core_cache);

	if (!debug_execution) {
		target->state = TARGET_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_RESUMED);
		LOG_DEBUG("target resumed at 0x%08" PRIx32, resume_pc);
	} else {
		target->state = TARGET_DEBUG_RUNNING;
		target_call_event_callbacks(target, TARGET_EVENT_DEBUG_RESUMED);
		LOG_DEBUG("target debug resumed at 0x%08" PRIx32, resume_pc);
	}

	return retval;
}

int arc_dbg_step(struct target *target, int current, uint32_t address,
	int handle_breakpoints)
{
	int retval = ERROR_OK;

	/* get pointers to arch-specific information */
	struct arc32_common *arc32 = target_to_arc32(target);
	struct breakpoint *breakpoint = NULL;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* current = 1: continue on current pc, otherwise continue at <address> */
	if (!current) {
		buf_set_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32, address);
		arc32->core_cache->reg_list[ARC_REG_PC].dirty = 1;
		arc32->core_cache->reg_list[ARC_REG_PC].valid = 1;
	}

	LOG_DEBUG("Target steps one instruction from PC=0x%" PRIx32,
		buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32));

	/* the front-end may request us not to handle breakpoints */
	if (handle_breakpoints) {
		breakpoint = breakpoint_find(target,
			buf_get_u32(arc32->core_cache->reg_list[ARC_REG_PC].value, 0, 32));
		if (breakpoint)
			arc_dbg_unset_breakpoint(target, breakpoint);
	}

	/* restore context */
	arc32_restore_context(target);

	target->debug_reason = DBG_REASON_SINGLESTEP;

	target_call_event_callbacks(target, TARGET_EVENT_RESUMED);

	/* disable interrupts while stepping */
	arc32_enable_interrupts(target, 0);

	/* exit debug mode */
	arc_dbg_enter_debug(target);

	/* do a single step */
	arc32_config_step(target, 1);

	/* make sure we done our step */
	alive_sleep(1);

	/* registers are now invalid */
	register_cache_invalidate(arc32->core_cache);

	if (breakpoint)
		arc_dbg_set_breakpoint(target, breakpoint);

	LOG_DEBUG("target stepped ");

	/* target_call_event_callbacks() will send a response to GDB that
	 * execution has stopped (packet T05). If target state is not set to
	 * HALTED beforehand, then this creates a race condition: target state
	 * will not be changed to HALTED until next invocation of
	 * arc_ocd_poll(), however GDB can issue next command _before_
	 * arc_ocd_poll() will be invoked. If GDB request requires target to be
	 * halted this request execution will fail. Also it seems that
	 * gdb_server cannot handle this failure properly, causing some
	 * unexpected results instead of error message. Strangely no other
	 * target does this except for ARM11, which sets target state to HALTED
	 * in debug_entry. Thus either every other target is suspect to the
	 * error, or they do something else differently, but I couldn't
	 * understand this. */
	target->state = TARGET_HALTED; 
	arc_dbg_debug_entry(target);
	target_call_event_callbacks(target, TARGET_EVENT_HALTED);

	return retval;
}

/* ......................................................................... */

int arc_dbg_add_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	if (target->state == TARGET_HALTED) {
		return arc_dbg_set_breakpoint(target, breakpoint);

	} else {
		LOG_WARNING(" > core was not halted, please try again.");
		return ERROR_TARGET_NOT_HALTED;
	}
}

int arc_dbg_add_context_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	int retval = ERROR_OK;

	LOG_ERROR("Context breakpoints are NOT SUPPORTED IN THIS RELEASE.");

	return retval;
}

int arc_dbg_add_hybrid_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	int retval = ERROR_OK;

	LOG_ERROR("Hybryd breakpoints are NOT SUPPORTED IN THIS RELEASE.");

	return retval;
}

int arc_dbg_remove_breakpoint(struct target *target,
	struct breakpoint *breakpoint)
{
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (breakpoint->set)
		retval = arc_dbg_unset_breakpoint(target, breakpoint);

	return retval;
}

int arc_dbg_add_watchpoint(struct target *target,
	struct watchpoint *watchpoint)
{
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = arc_dbg_set_watchpoint(target, watchpoint);

	return retval;
}

int arc_dbg_remove_watchpoint(struct target *target,
	struct watchpoint *watchpoint)
{
	int retval = ERROR_OK;

	if (target->state != TARGET_HALTED) {
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (watchpoint->set)
		retval = arc_dbg_unset_watchpoint(target, watchpoint);

	return retval;
}
int arc_dbg_add_auxreg_actionpoint(struct target *target,
	uint32_t auxreg_addr, uint32_t transaction)
{

	if (target->state == TARGET_HALTED) {
		struct arc32_common *arc32 = target_to_arc32(target);
		struct arc32_comparator *comparator_list = arc32->action_point_list;
		int ap_num = 0;

		while (comparator_list[ap_num].used)
			ap_num++;

		if (ap_num >= arc32->num_action_points) {
			LOG_ERROR("No ActionPoint free, maximim amount is %" PRIu32,
					arc32->num_action_points);
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}

		int retval =  arc_dbg_configure_actionpoint(target, ap_num,
				auxreg_addr, transaction, AP_AC_AT_AUXREG_ADDR);

		if (retval == ERROR_OK) {
			comparator_list[ap_num].used = 1;
			comparator_list[ap_num].reg_address = auxreg_addr;
		}

		return retval;

	} else {
		return ERROR_TARGET_NOT_HALTED;
	}
}

int arc_dbg_remove_auxreg_actionpoint(struct target *target, uint32_t auxreg_addr)
{
	int retval = ERROR_OK;

	if (target->state == TARGET_HALTED) {
		struct arc32_common *arc32 = target_to_arc32(target);
		struct arc32_comparator *comparator_list = arc32->action_point_list;
		int ap_found = 0;
		int ap_num = 0;

		while ((comparator_list[ap_num].used) && (ap_num < arc32->num_action_points)) {
			if (comparator_list[ap_num].reg_address == auxreg_addr) {
				ap_found = 1;
				break;
			}
			ap_num++;
		}

		if (ap_found) {
			retval =  arc_dbg_configure_actionpoint(target, ap_num,
					auxreg_addr, AP_AC_TT_DISABLE, AP_AC_AT_AUXREG_ADDR);

			if (retval == ERROR_OK) {
				comparator_list[ap_num].used = 0;
				comparator_list[ap_num].bp_value = 0;
			}
		} else {
			LOG_ERROR("Register ActionPoint not found");
		}

		return retval;

	} else {
		return ERROR_TARGET_NOT_HALTED;
	}
}

void arc_dbg_reset_breakpoints_watchpoints(struct target *target)
{
	struct arc32_common *arc32 = target_to_arc32(target);
	struct arc32_comparator *comparator_list = arc32->action_point_list;
	struct breakpoint *next_b;
	struct watchpoint *next_w;

	while (target->breakpoints) {
		next_b = target->breakpoints->next;
		arc_dbg_remove_breakpoint(target, target->breakpoints);
		free(target->breakpoints->orig_instr);
		free(target->breakpoints);
		target->breakpoints = next_b;
	}
	while (target->watchpoints) {
		next_w = target->watchpoints->next;
		arc_dbg_remove_watchpoint(target, target->watchpoints);
		free(target->watchpoints);
		target->watchpoints = next_w;
	}
	for (int i = 0; i < arc32->num_action_points; i++) {
		if ((comparator_list[i].used) && (comparator_list[i].reg_address)) {
			arc_dbg_remove_auxreg_actionpoint(target, comparator_list[i].reg_address);
		}
	}
}

