/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "dccg.h"
#include "clk_mgr_internal.h"


#include "dcn20/dcn20_clk_mgr.h"
#include "dce100/dce_clk_mgr.h"
#include "reg_helper.h"
#include "core_types.h"
#include "dm_helpers.h"

#include "navi10_ip_offset.h"
#include "dcn/dcn_2_0_0_offset.h"
#include "dcn/dcn_2_0_0_sh_mask.h"
#include "clk/clk_11_0_0_offset.h"
#include "clk/clk_11_0_0_sh_mask.h"

#undef FN
#define FN(reg_name, field_name) \
	clk_mgr->clk_mgr_shift->field_name, clk_mgr->clk_mgr_mask->field_name

#define REG(reg) \
	(clk_mgr->regs->reg)

#define BASE_INNER(seg) DCN_BASE__INST0_SEG ## seg

#define BASE(seg) BASE_INNER(seg)

#define SR(reg_name)\
		.reg_name = BASE(mm ## reg_name ## _BASE_IDX) +  \
					mm ## reg_name

#define CLK_BASE_INNER(seg) \
	CLK_BASE__INST0_SEG ## seg


static const struct clk_mgr_registers clk_mgr_regs = {
	CLK_REG_LIST_NV10()
};

static const struct clk_mgr_shift clk_mgr_shift = {
	CLK_MASK_SH_LIST_NV10(__SHIFT)
};

static const struct clk_mgr_mask clk_mgr_mask = {
	CLK_MASK_SH_LIST_NV10(_MASK)
};

static uint32_t dentist_get_did_from_divider(int divider)
{
	uint32_t divider_id;

	/* we want to floor here to get higher clock than required rather than lower */
	if (divider < DENTIST_DIVIDER_RANGE_2_START) {
		if (divider < DENTIST_DIVIDER_RANGE_1_START)
			divider_id = DENTIST_BASE_DID_1;
		else
			divider_id = DENTIST_BASE_DID_1
				+ (divider - DENTIST_DIVIDER_RANGE_1_START)
					/ DENTIST_DIVIDER_RANGE_1_STEP;
	} else if (divider < DENTIST_DIVIDER_RANGE_3_START) {
		divider_id = DENTIST_BASE_DID_2
				+ (divider - DENTIST_DIVIDER_RANGE_2_START)
					/ DENTIST_DIVIDER_RANGE_2_STEP;
	} else if (divider < DENTIST_DIVIDER_RANGE_4_START) {
		divider_id = DENTIST_BASE_DID_3
				+ (divider - DENTIST_DIVIDER_RANGE_3_START)
					/ DENTIST_DIVIDER_RANGE_3_STEP;
	} else {
		divider_id = DENTIST_BASE_DID_4
				+ (divider - DENTIST_DIVIDER_RANGE_4_START)
					/ DENTIST_DIVIDER_RANGE_4_STEP;
		if (divider_id > DENTIST_MAX_DID)
			divider_id = DENTIST_MAX_DID;
	}

	return divider_id;
}

void dcn20_update_clocks_update_dpp_dto(struct clk_mgr_internal *clk_mgr,
		struct dc_state *context)
{
	int i;

	clk_mgr->dccg->ref_dppclk = clk_mgr->base.clks.dppclk_khz;
	for (i = 0; i < clk_mgr->base.ctx->dc->res_pool->pipe_count; i++) {
		int dpp_inst, dppclk_khz;

		if (!context->res_ctx.pipe_ctx[i].plane_state)
			continue;

		dpp_inst = context->res_ctx.pipe_ctx[i].plane_res.dpp->inst;
		dppclk_khz = context->res_ctx.pipe_ctx[i].plane_res.bw.dppclk_khz;
		clk_mgr->dccg->funcs->update_dpp_dto(
				clk_mgr->dccg, dpp_inst, dppclk_khz);
	}
}

void dcn20_update_clocks_update_dentist(struct clk_mgr_internal *clk_mgr)
{
	int dpp_divider = DENTIST_DIVIDER_RANGE_SCALE_FACTOR
			* clk_mgr->dentist_vco_freq_khz / clk_mgr->base.clks.dppclk_khz;
	int disp_divider = DENTIST_DIVIDER_RANGE_SCALE_FACTOR
			* clk_mgr->dentist_vco_freq_khz / clk_mgr->base.clks.dispclk_khz;

	uint32_t dppclk_wdivider = dentist_get_did_from_divider(dpp_divider);
	uint32_t dispclk_wdivider = dentist_get_did_from_divider(disp_divider);

	REG_UPDATE(DENTIST_DISPCLK_CNTL,
			DENTIST_DISPCLK_WDIVIDER, dispclk_wdivider);
//	REG_WAIT(DENTIST_DISPCLK_CNTL, DENTIST_DISPCLK_CHG_DONE, 1, 5, 100);
	REG_UPDATE(DENTIST_DISPCLK_CNTL,
			DENTIST_DPPCLK_WDIVIDER, dppclk_wdivider);
	REG_WAIT(DENTIST_DISPCLK_CNTL, DENTIST_DPPCLK_CHG_DONE, 1, 5, 100);
}


void dcn2_update_clocks(struct clk_mgr *clk_mgr_base,
			struct dc_state *context,
			bool safe_to_lower)
{
	struct clk_mgr_internal *clk_mgr = TO_CLK_MGR_INTERNAL(clk_mgr_base);
	struct dc_clocks *new_clocks = &context->bw_ctx.bw.dcn.clk;
	struct dc *dc = clk_mgr_base->ctx->dc;
	struct pp_smu_funcs_nv *pp_smu = NULL;
	int display_count;
	bool update_dppclk = false;
	bool update_dispclk = false;
	bool enter_display_off = false;
	bool dpp_clock_lowered = false;
	struct dmcu *dmcu = clk_mgr_base->ctx->dc->res_pool->dmcu;

	display_count = clk_mgr_helper_get_active_display_cnt(dc, context);
	if (dc->res_pool->pp_smu)
		pp_smu = &dc->res_pool->pp_smu->nv_funcs;

	if (display_count == 0)
		enter_display_off = true;

	if (enter_display_off == safe_to_lower) {
		if (pp_smu && pp_smu->set_display_count)
			pp_smu->set_display_count(&pp_smu->pp_smu, display_count);
	}

	if (should_set_clock(safe_to_lower, new_clocks->phyclk_khz, clk_mgr_base->clks.phyclk_khz)) {
		clk_mgr_base->clks.phyclk_khz = new_clocks->phyclk_khz;
		if (pp_smu && pp_smu->set_voltage_by_freq)
			pp_smu->set_voltage_by_freq(&pp_smu->pp_smu, PP_SMU_NV_PHYCLK, clk_mgr_base->clks.phyclk_khz / 1000);
	}

	if (should_set_clock(safe_to_lower, new_clocks->dcfclk_khz, clk_mgr_base->clks.dcfclk_khz)) {
		clk_mgr_base->clks.dcfclk_khz = new_clocks->dcfclk_khz;
		if (pp_smu && pp_smu->set_hard_min_dcfclk_by_freq)
			pp_smu->set_hard_min_dcfclk_by_freq(&pp_smu->pp_smu, clk_mgr_base->clks.dcfclk_khz / 1000);
	}

	if (should_set_clock(safe_to_lower,
			new_clocks->dcfclk_deep_sleep_khz, clk_mgr_base->clks.dcfclk_deep_sleep_khz)) {
		clk_mgr_base->clks.dcfclk_deep_sleep_khz = new_clocks->dcfclk_deep_sleep_khz;
		if (pp_smu && pp_smu->set_min_deep_sleep_dcfclk)
			pp_smu->set_min_deep_sleep_dcfclk(&pp_smu->pp_smu, clk_mgr_base->clks.dcfclk_deep_sleep_khz / 1000);
	}

	if (should_set_clock(safe_to_lower, new_clocks->socclk_khz, clk_mgr_base->clks.socclk_khz)) {
		clk_mgr_base->clks.socclk_khz = new_clocks->socclk_khz;
		if (pp_smu && pp_smu->set_hard_min_socclk_by_freq)
			pp_smu->set_hard_min_socclk_by_freq(&pp_smu->pp_smu, clk_mgr_base->clks.socclk_khz / 1000);
	}

	if (!safe_to_lower && pp_smu && pp_smu->set_pstate_handshake_support) {
		clk_mgr_base->clks.p_state_change_support = false;
		pp_smu->set_pstate_handshake_support(&pp_smu->pp_smu, false);
	} else if (safe_to_lower && pp_smu && pp_smu->set_pstate_handshake_support) {
		clk_mgr_base->clks.p_state_change_support = new_clocks->p_state_change_support;
		pp_smu->set_pstate_handshake_support(&pp_smu->pp_smu, clk_mgr_base->clks.p_state_change_support);
	}

	if (should_set_clock(safe_to_lower, new_clocks->dramclk_khz, clk_mgr_base->clks.dramclk_khz)) {
		clk_mgr_base->clks.dramclk_khz = new_clocks->dramclk_khz;
		if (pp_smu && pp_smu->set_hard_min_uclk_by_freq)
			pp_smu->set_hard_min_uclk_by_freq(&pp_smu->pp_smu, clk_mgr_base->clks.dramclk_khz / 1000);
	}

	if (should_set_clock(safe_to_lower, new_clocks->dppclk_khz, clk_mgr->base.clks.dppclk_khz)) {
		if (clk_mgr->base.clks.dppclk_khz > new_clocks->dppclk_khz)
			dpp_clock_lowered = true;
		clk_mgr->base.clks.dppclk_khz = new_clocks->dppclk_khz;

		if (pp_smu && pp_smu->set_voltage_by_freq)
			pp_smu->set_voltage_by_freq(&pp_smu->pp_smu, PP_SMU_NV_PIXELCLK, clk_mgr_base->clks.dppclk_khz / 1000);

		update_dppclk = true;
	}

	if (should_set_clock(safe_to_lower, new_clocks->dispclk_khz, clk_mgr_base->clks.dispclk_khz)) {
		clk_mgr_base->clks.dispclk_khz = new_clocks->dispclk_khz;
		if (pp_smu && pp_smu->set_voltage_by_freq)
			pp_smu->set_voltage_by_freq(&pp_smu->pp_smu, PP_SMU_NV_DISPCLK, clk_mgr_base->clks.dispclk_khz / 1000);

		update_dispclk = true;
	}

	if (dpp_clock_lowered) {
		// if clock is being lowered, increase DTO before lowering refclk
		dcn20_update_clocks_update_dpp_dto(clk_mgr, context);
		dcn20_update_clocks_update_dentist(clk_mgr);
	} else {
		// if clock is being raised, increase refclk before lowering DTO
		if (update_dppclk || update_dispclk)
			dcn20_update_clocks_update_dentist(clk_mgr);
		if (update_dppclk)
			dcn20_update_clocks_update_dpp_dto(clk_mgr, context);
	}

	if (update_dispclk &&
			dmcu && dmcu->funcs->is_dmcu_initialized(dmcu)) {
		/*update dmcu for wait_loop count*/
		dmcu->funcs->set_psr_wait_loop(dmcu,
			clk_mgr_base->clks.dispclk_khz / 1000 / 7);
	}
}

void dcn2_update_clocks_fpga(struct clk_mgr *clk_mgr,
		struct dc_state *context,
		bool safe_to_lower)
{
	struct dc_clocks *new_clocks = &context->bw_ctx.bw.dcn.clk;

	if (should_set_clock(safe_to_lower, new_clocks->phyclk_khz, clk_mgr->clks.phyclk_khz)) {
		clk_mgr->clks.phyclk_khz = new_clocks->phyclk_khz;
	}

	if (should_set_clock(safe_to_lower, new_clocks->dcfclk_khz, clk_mgr->clks.dcfclk_khz)) {
		clk_mgr->clks.dcfclk_khz = new_clocks->dcfclk_khz;
	}

	if (should_set_clock(safe_to_lower,
			new_clocks->dcfclk_deep_sleep_khz, clk_mgr->clks.dcfclk_deep_sleep_khz)) {
		clk_mgr->clks.dcfclk_deep_sleep_khz = new_clocks->dcfclk_deep_sleep_khz;
	}

	if (should_set_clock(safe_to_lower, new_clocks->socclk_khz, clk_mgr->clks.socclk_khz)) {
		clk_mgr->clks.socclk_khz = new_clocks->socclk_khz;
	}

	if (should_set_clock(safe_to_lower, new_clocks->dramclk_khz, clk_mgr->clks.dramclk_khz)) {
		clk_mgr->clks.dramclk_khz = new_clocks->dramclk_khz;
	}

	if (should_set_clock(safe_to_lower, new_clocks->dppclk_khz, clk_mgr->clks.dppclk_khz)) {
		clk_mgr->clks.dppclk_khz = new_clocks->dppclk_khz;
	}

	/* Add 250MHz as safety margin */
	if (should_set_clock(safe_to_lower, new_clocks->fclk_khz + 250000, clk_mgr->clks.fclk_khz)) {
		clk_mgr->clks.fclk_khz = new_clocks->fclk_khz + 250000;
	}

	if (should_set_clock(safe_to_lower, new_clocks->dispclk_khz, clk_mgr->clks.dispclk_khz)) {
		clk_mgr->clks.dispclk_khz = new_clocks->dispclk_khz;
	}

	/* Both fclk and dppclk ref are run on the same scemi clock so we
	 * need to keep the same value for both
	 */
	if (clk_mgr->clks.fclk_khz > clk_mgr->clks.dppclk_khz)
		clk_mgr->clks.dppclk_khz = clk_mgr->clks.fclk_khz;

	dm_set_dcn_clocks(clk_mgr->ctx, &clk_mgr->clks);
}

void dcn2_init_clocks(struct clk_mgr *clk_mgr)
{
	memset(&(clk_mgr->clks), 0, sizeof(struct dc_clocks));
}

static struct clk_mgr_funcs dcn2_funcs = {
	.get_dp_ref_clk_frequency = dce12_get_dp_ref_freq_khz,
	.update_clocks = dcn2_update_clocks,
	.init_clocks = dcn2_init_clocks
};


void dcn20_clk_mgr_construct(
		struct dc_context *ctx,
		struct clk_mgr_internal *clk_mgr,
		struct pp_smu_funcs *pp_smu,
		struct dccg *dccg)
{
	clk_mgr->base.ctx = ctx;
	clk_mgr->base.funcs = &dcn2_funcs;
	clk_mgr->regs = &clk_mgr_regs;
	clk_mgr->clk_mgr_shift = &clk_mgr_shift;
	clk_mgr->clk_mgr_mask = &clk_mgr_mask;

	clk_mgr->dccg = dccg;
	clk_mgr->dfs_bypass_disp_clk = 0;

	clk_mgr->dprefclk_ss_percentage = 0;
	clk_mgr->dprefclk_ss_divider = 1000;
	clk_mgr->ss_on_dprefclk = false;

	clk_mgr->base.dprefclk_khz = 700000; // 700 MHz planned if VCO is 3.85 GHz, will be retrieved

	if (IS_FPGA_MAXIMUS_DC(ctx->dce_environment)) {
		dcn2_funcs.update_clocks = dcn2_update_clocks_fpga;
		clk_mgr->dentist_vco_freq_khz = 3850000;

	} else {
		/* DFS Slice 2 should be used for DPREFCLK */
		int dprefclk_did = REG_READ(CLK3_CLK2_DFS_CNTL);
		/* Convert DPREFCLK DFS Slice DID to actual divider*/
		int target_div = dentist_get_divider_from_did(dprefclk_did);

		/* get FbMult value */
		uint32_t pll_req_reg = REG_READ(CLK3_CLK_PLL_REQ);
		struct fixed31_32 pll_req;

		/* set up a fixed-point number
		 * this works because the int part is on the right edge of the register
		 * and the frac part is on the left edge
		 */

		pll_req = dc_fixpt_from_int(pll_req_reg & clk_mgr->clk_mgr_mask->FbMult_int);
		pll_req.value |= pll_req_reg & clk_mgr->clk_mgr_mask->FbMult_frac;

		/* multiply by REFCLK period */
		pll_req = dc_fixpt_mul_int(pll_req, 100000);

		/* integer part is now VCO frequency in kHz */
		clk_mgr->dentist_vco_freq_khz = dc_fixpt_floor(pll_req);

		/* in case we don't get a value from the register, use default */
		if (clk_mgr->dentist_vco_freq_khz == 0)
			clk_mgr->dentist_vco_freq_khz = 3850000;

		/* Calculate the DPREFCLK in kHz.*/
		clk_mgr->base.dprefclk_khz = (DENTIST_DIVIDER_RANGE_SCALE_FACTOR
			* clk_mgr->dentist_vco_freq_khz) / target_div;
	}
	//Integrated_info table does not exist on dGPU projects so should not be referenced
	//anywhere in code for dGPUs.
	//Also there is no plan for now that DFS BYPASS will be used on NV10/12/14.
	clk_mgr->dfs_bypass_enabled = false;

	dce_clock_read_ss_info(clk_mgr);
}
