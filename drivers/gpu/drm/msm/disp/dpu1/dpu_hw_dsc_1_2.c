// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved
 */

#include <drm/drm_managed.h>

#include <drm/display/drm_dsc_helper.h>

#include "dpu_kms.h"
#include "dpu_hw_catalog.h"
#include "dpu_hwio.h"
#include "dpu_hw_mdss.h"
#include "dpu_hw_dsc.h"

#define DSC_CMN_MAIN_CNF           0x00

/* DPU_DSC_ENC register offsets */
#define ENC_DF_CTRL                0x00
#define ENC_GENERAL_STATUS         0x04
#define ENC_HSLICE_STATUS          0x08
#define ENC_OUT_STATUS             0x0C
#define ENC_INT_STAT               0x10
#define ENC_INT_CLR                0x14
#define ENC_INT_MASK               0x18
#define DSC_MAIN_CONF              0x30
#define DSC_PICTURE_SIZE           0x34
#define DSC_SLICE_SIZE             0x38
#define DSC_MISC_SIZE              0x3C
#define DSC_HRD_DELAYS             0x40
#define DSC_RC_SCALE               0x44
#define DSC_RC_SCALE_INC_DEC       0x48
#define DSC_RC_OFFSETS_1           0x4C
#define DSC_RC_OFFSETS_2           0x50
#define DSC_RC_OFFSETS_3           0x54
#define DSC_RC_OFFSETS_4           0x58
#define DSC_FLATNESS_QP            0x5C
#define DSC_RC_MODEL_SIZE          0x60
#define DSC_RC_CONFIG              0x64
#define DSC_RC_BUF_THRESH_0        0x68
#define DSC_RC_BUF_THRESH_1        0x6C
#define DSC_RC_BUF_THRESH_2        0x70
#define DSC_RC_BUF_THRESH_3        0x74
#define DSC_RC_MIN_QP_0            0x78
#define DSC_RC_MIN_QP_1            0x7C
#define DSC_RC_MIN_QP_2            0x80
#define DSC_RC_MAX_QP_0            0x84
#define DSC_RC_MAX_QP_1            0x88
#define DSC_RC_MAX_QP_2            0x8C
#define DSC_RC_RANGE_BPG_OFFSETS_0 0x90
#define DSC_RC_RANGE_BPG_OFFSETS_1 0x94
#define DSC_RC_RANGE_BPG_OFFSETS_2 0x98

/* DPU_DSC_CTL register offsets */
#define DSC_CTL                    0x00
#define DSC_CFG                    0x04
#define DSC_DATA_IN_SWAP           0x08
#define DSC_CLK_CTRL               0x0C

/*
 * Two things in ENC_DF_CTRL that the vendor driver programs and this one does not.
 * Both only bite on detailed content -- flat colour never uses the rate budget and
 * never exercises the indexed colour history -- which is exactly how zorn's panel
 * fails: solid fills are pixel-perfect, dense text corrupts once per slice row.
 *
 * dsc_ob_max_addr: the DSC output buffer's maximum address. Upstream halves it
 *	whenever the block merely *has* the native 4:2:x capability:
 *
 *		max_addr = 2400 / num_softslice;
 *		if (features & BIT(DPU_DSC_NATIVE_42x_EN)) max_addr /= 2;
 *
 *	and the SM8650 catalog sets that bit on dce_0_0/dce_0_1, so we program 1199.
 *	The vendor uses the 4:2:2 bit only to choose a table row and halves on a
 *	*separate* capability (SDE_DSC_REDUCED_OB_MAX):
 *
 *		if (!(features & BIT(SDE_DSC_NATIVE_422_EN)))
 *			if (num_ss == 1) return reduced_ob_max ? 1199 : 2399;
 *
 *	We are RGB, not 4:2:2, so the vendor value is 2399 unless this SoC sets
 *	has_reduced_ob_max. A half-size output FIFO overflows precisely when the
 *	compressed data is large. 0 = keep upstream's formula.
 *
 * dsc_full_ich_prec: BIT(12), full indexed-colour-history precision. The vendor
 *	sets it when the DPU is >= A00 (ours reports revision 0xa0000000) and
 *	bits_per_component > 8 (ours is 10):
 *
 *		if (test_bit(SDE_DSC_FULL_ICH_PREC, &caps->features) &&
 *		    dsc->config.bits_per_component > 8)
 *			data |= BIT(12);
 *
 *	Upstream has no notion of it and never sets it. ICH caches recently used
 *	pixel values within a slice, so it is dormant on a flat fill and heavily
 *	used on text; running it at reduced precision while the PPS declares 10 bpc
 *	makes the reconstruction drift per slice.
 */
static uint dsc_ob_max_addr;
module_param(dsc_ob_max_addr, uint, 0600);
MODULE_PARM_DESC(dsc_ob_max_addr,
		 "Force the DSC output buffer max address (0 = upstream formula)");

static bool dsc_full_ich_prec;
module_param(dsc_full_ich_prec, bool, 0600);
MODULE_PARM_DESC(dsc_full_ich_prec,
		 "Set ENC_DF_CTRL BIT(12), full ICH precision, as the vendor does for >8 bpc");

/*
 * Dump the DSC encoder block after configuring it, for a field-by-field diff
 * against the same registers read off a running vendor stack. Off by default --
 * this runs on every kickoff, and left on it fills dmesg fast enough to keep
 * journald writing continuously.
 */
static bool dsc_dump_regs;
module_param(dsc_dump_regs, bool, 0600);
MODULE_PARM_DESC(dsc_dump_regs,
		 "dump the DSC encoder registers after each config; noisy");

/*
 * DSC_CFG BIT(17). Upstream sets it for every command-mode panel. The vendor
 * touches that bit only inside its 4HS-merge path, where it means the opposite
 * (1 = *video* mode) and belongs to a group -- BIT(16) 4HS-merge-enable,
 * bits[21:18] padding, bits[25:22] alignment -- that is only written when
 * dsc_4hsmerge_en is set. zorn has two DSC blocks, not four, so 4HS merge is off
 * and the vendor's value for this bit is 0.
 *
 * Default off, i.e. the vendor's value. The register is rewritten every frame
 * from dpu_hw_dsc_config_1_2(), so this parameter takes effect on the next frame
 * with no modeset needed.
 */
static bool dsc_cfg_bit17;
module_param(dsc_cfg_bit17, bool, 0600);
MODULE_PARM_DESC(dsc_cfg_bit17,
		 "set DSC_CFG BIT(17) in command mode (upstream behaviour; vendor leaves it clear)");

/* -1 leaves DSC_DATA_IN_SWAP at its reset value; see where it is written. */
static int dsc_data_in_swap = -1;
module_param(dsc_data_in_swap, int, 0600);
MODULE_PARM_DESC(dsc_data_in_swap,
		 "value to program into DSC_DATA_IN_SWAP (-1 = leave at reset)");

static int _dsc_calc_output_buf_max_addr(struct dpu_hw_dsc *hw_dsc, int num_softslice)
{
	int max_addr = 2400 / num_softslice;

	if (dsc_ob_max_addr)
		return dsc_ob_max_addr;

	if (hw_dsc->caps->features & BIT(DPU_DSC_NATIVE_42x_EN))
		max_addr /= 2;

	return max_addr - 1;
};

static void dpu_hw_dsc_disable_1_2(struct dpu_hw_dsc *hw_dsc)
{
	struct dpu_hw_blk_reg_map *hw;
	const struct dpu_dsc_sub_blks *sblk;

	if (!hw_dsc)
		return;

	hw = &hw_dsc->hw;
	sblk = hw_dsc->caps->sblk;
	DPU_REG_WRITE(hw, sblk->ctl.base + DSC_CFG, 0);

	DPU_REG_WRITE(hw, sblk->enc.base + ENC_DF_CTRL, 0);
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_MAIN_CONF, 0);
}

static void dpu_hw_dsc_config_1_2(struct dpu_hw_dsc *hw_dsc,
				  struct drm_dsc_config *dsc,
				  u32 mode,
				  u32 initial_lines)
{
	struct dpu_hw_blk_reg_map *hw;
	const struct dpu_dsc_sub_blks *sblk;
	u32 data = 0;
	u32 det_thresh_flatness;
	u32 num_active_slice_per_enc;
	u32 bpp;

	if (!hw_dsc || !dsc)
		return;

	hw = &hw_dsc->hw;

	sblk = hw_dsc->caps->sblk;

	if (mode & DSC_MODE_SPLIT_PANEL)
		data |= BIT(0);

	if (mode & DSC_MODE_MULTIPLEX)
		data |= BIT(1);

	num_active_slice_per_enc = dsc->slice_count;
	if (mode & DSC_MODE_MULTIPLEX)
		num_active_slice_per_enc = dsc->slice_count / 2;

	data |= (num_active_slice_per_enc & 0x3) << 7;

	DPU_REG_WRITE(hw, DSC_CMN_MAIN_CNF, data);

	data = (initial_lines & 0xff);

	if (mode & DSC_MODE_VIDEO)
		data |= BIT(9);

	data |= (_dsc_calc_output_buf_max_addr(hw_dsc, num_active_slice_per_enc) << 18);

	if (dsc_full_ich_prec && dsc->bits_per_component > 8)
		data |= BIT(12);

	DPU_REG_WRITE(hw, sblk->enc.base + ENC_DF_CTRL, data);

	data = (dsc->dsc_version_minor & 0xf) << 28;
	if (dsc->dsc_version_minor == 0x2) {
		if (dsc->native_422)
			data |= BIT(22);
		if (dsc->native_420)
			data |= BIT(21);
	}

	bpp = dsc->bits_per_pixel;
	/* as per hw requirement bpp should be programmed
	 * twice the actual value in case of 420 or 422 encoding
	 */
	if (dsc->native_422 || dsc->native_420)
		bpp = 2 * bpp;

	data |= bpp << 10;

	if (dsc->block_pred_enable)
		data |= BIT(20);

	if (dsc->convert_rgb)
		data |= BIT(4);

	data |= (dsc->line_buf_depth & 0xf) << 6;
	data |= dsc->bits_per_component & 0xf;

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_MAIN_CONF, data);

	/*
	 * MAIN_CONF is the register that decides how the panel's decoder reads the
	 * chroma channels, so it is the one worth seeing. It is printed rather than
	 * read back because there is no safe way to read it from userspace on this
	 * SoC: /dev/mem works only while the DPU happens to be powered, and a
	 * command-mode panel idle-collapses it between one syscall and the next --
	 * reading it collapsed is an unclocked access that TZ answers with a hard
	 * reset, no log, no panic. pr_info and not a debug print because a wrong
	 * value here costs a reboot to discover.
	 *
	 * Only on change: this runs from prepare_for_kickoff, i.e. every frame on
	 * both DSC blocks, and at 120 Hz that is ~240 lines a second -- enough to
	 * push the interesting part of the boot out of the ring buffer, which it
	 * did.
	 */
	if (data != hw_dsc->last_main_conf) {
		hw_dsc->last_main_conf = data;
		pr_info("dpu_dsc: MAIN_CONF %#010x  bpc %u lbd %u bpp %u/16 native_422 %d native_420 %d convert_rgb %d block_pred %d dsc 1.%u\n",
			data, dsc->bits_per_component, dsc->line_buf_depth,
			dsc->bits_per_pixel, dsc->native_422, dsc->native_420,
			dsc->convert_rgb, dsc->block_pred_enable,
			dsc->dsc_version_minor);
	}
	data = (dsc->pic_width & 0xffff) |
		((dsc->pic_height & 0xffff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_PICTURE_SIZE, data);

	data = (dsc->slice_width & 0xffff) |
		((dsc->slice_height & 0xffff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_SLICE_SIZE, data);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_MISC_SIZE,
		      (dsc->slice_chunk_size) & 0xffff);

	data = (dsc->initial_xmit_delay & 0xffff) |
		((dsc->initial_dec_delay & 0xffff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_HRD_DELAYS, data);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_SCALE,
		      dsc->initial_scale_value & 0x3f);

	data = (dsc->scale_increment_interval & 0xffff) |
		((dsc->scale_decrement_interval & 0x7ff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_SCALE_INC_DEC, data);

	data = (dsc->first_line_bpg_offset & 0x1f) |
		((dsc->second_line_bpg_offset & 0x1f) << 5);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_OFFSETS_1, data);

	data = (dsc->nfl_bpg_offset & 0xffff) |
		((dsc->slice_bpg_offset & 0xffff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_OFFSETS_2, data);

	data = (dsc->initial_offset & 0xffff) |
		((dsc->final_offset & 0xffff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_OFFSETS_3, data);

	data = (dsc->nsl_bpg_offset & 0xffff) |
		((dsc->second_line_offset_adj & 0xffff) << 16);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_OFFSETS_4, data);

	det_thresh_flatness = drm_dsc_flatness_det_thresh(dsc);
	data = (dsc->flatness_min_qp & 0x1f) |
		((dsc->flatness_max_qp & 0x1f) << 5) |
		((det_thresh_flatness & 0xff) << 10);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_FLATNESS_QP, data);

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MODEL_SIZE,
		      (dsc->rc_model_size) & 0xffff);

	data = dsc->rc_edge_factor & 0xf;
	data |= (dsc->rc_quant_incr_limit0 & 0x1f) << 8;
	data |= (dsc->rc_quant_incr_limit1 & 0x1f) << 13;
	data |= (dsc->rc_tgt_offset_high & 0xf) << 20;
	data |= (dsc->rc_tgt_offset_low & 0xf) << 24;

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_CONFIG, data);

	/*
	 * zorn 7.3 hua-ping phase-3 (2026-10-02): host side pclk/byte_clk/fmt already
	 * match Android (160/150, slice_per_pkt=1, native_422) yet still snows ->
	 * isolate whether the DSC *compressed bitstream params* differ from the good
	 * 7.2.7 panel blob (e1484ea2). Dump the complete PPS the DPU ships to the
	 * panel decoder; if this matches Android's values, the divergence is upstream
	 * in encoder timing, not the PPS table.
	 */
	pr_info("zorn-dppsprobe: ver 1.%u bpc=%u bpp=%u lbd=%u slice_chunk=%u slice_count=%u slice_w=%u slice_h=%u pic_w=%u pic_h=%u\n"
		"  nfl_bpg=%u slice_bpg=%u init_xmit_delay=%u init_dec_delay=%u scale_inc=%u scale_dec=%u\n"
		"  init_offset=%u final_offset=%u flbpg=%u sl2bpg=%u rc_model=%u rc_edge=%u fminqp=%u fmaxqp=%u\n"
		"  native_422=%u native_420=%u convert_rgb=%u block_pred=%u\n",
		dsc->dsc_version_minor, dsc->bits_per_component, dsc->bits_per_pixel,
		dsc->line_buf_depth, dsc->slice_chunk_size, dsc->slice_count,
		dsc->slice_width, dsc->slice_height, dsc->pic_width, dsc->pic_height,
		dsc->nfl_bpg_offset, dsc->slice_bpg_offset,
		dsc->initial_xmit_delay, dsc->initial_dec_delay,
		dsc->scale_increment_interval, dsc->scale_decrement_interval,
		dsc->initial_offset, dsc->final_offset,
		dsc->first_line_bpg_offset, dsc->second_line_bpg_offset,
		dsc->rc_model_size, dsc->rc_edge_factor,
		dsc->flatness_min_qp, dsc->flatness_max_qp,
		dsc->native_422, dsc->native_420, dsc->convert_rgb,
		dsc->block_pred_enable);

	/* program the dsc wrapper */
	data = BIT(0); /* encoder enable */
	if (dsc->native_422)
		data |= BIT(8);
	else if (dsc->native_420)
		data |= BIT(9);
	if (!dsc->convert_rgb)
		data |= BIT(10);
	if (dsc->bits_per_component == 8)
		data |= BIT(11);
	if (mode & DSC_MODE_SPLIT_PANEL)
		data |= BIT(12);
	if (mode & DSC_MODE_MULTIPLEX)
		data |= BIT(13);
	if (dsc_cfg_bit17 && !(mode & DSC_MODE_VIDEO))
		data |= BIT(17);

	DPU_REG_WRITE(hw, sblk->ctl.base + DSC_CFG, data);

	/*
	 * DSC_DATA_IN_SWAP is never written by either stack -- upstream and the
	 * vendor both only #define it -- so it sits at its reset value. Its name is
	 * the only documentation there is, and it is the one register left on the
	 * path that could plausibly decide which input channels the wrapper reads.
	 * That matters because on zorn the wrapper emits only two components: a
	 * full-rate Y from channel 1 and one half-rate chroma from the horizontal
	 * average of channel 2, with channel 0 never sampled. Measured over
	 * eighteen swatches (nine plain, nine with red forced into the odd-column
	 * blue slot), output is always (C, G, C) with C = avg(P0.b, P1.b).
	 *
	 * Writable so the value can be swept without a rebuild: the register is
	 * rewritten every frame from here, so a new value lands on the next frame.
	 * -1 leaves the reset value alone.
	 */
	if (dsc_data_in_swap >= 0)
		DPU_REG_WRITE(hw, sblk->ctl.base + DSC_DATA_IN_SWAP,
			      (u32)dsc_data_in_swap);

	/*
	 * Read back rather than log what was written: DATA_IN_SWAP's reset value is
	 * the unknown, and CMN_MAIN_CNF and the wrapper's DSC_CFG have no Android
	 * counterpart to compare against -- sde_dbg on the vendor kernel refuses
	 * offsets outside the two encoder sub-blocks, so these three have never been
	 * seen on a working stack. Only on change, for the reason MAIN_CONF is.
	 */
	{
		u32 cfg_rb = DPU_REG_READ(hw, sblk->ctl.base + DSC_CFG);
		u32 swap_rb = DPU_REG_READ(hw, sblk->ctl.base + DSC_DATA_IN_SWAP);
		u32 cmn_rb = DPU_REG_READ(hw, DSC_CMN_MAIN_CNF);

		if (cfg_rb != hw_dsc->last_ctl_cfg ||
		    swap_rb != hw_dsc->last_data_in_swap) {
			hw_dsc->last_ctl_cfg = cfg_rb;
			hw_dsc->last_data_in_swap = swap_rb;
			pr_info("dpu_dsc: DSC_CFG %#010x DATA_IN_SWAP %#010x CMN_MAIN_CNF %#010x\n",
				cfg_rb, swap_rb, cmn_rb);
		}
	}
}

static void dpu_hw_dsc_config_thresh_1_2(struct dpu_hw_dsc *hw_dsc,
					 struct drm_dsc_config *dsc)
{
	struct dpu_hw_blk_reg_map *hw;
	const struct dpu_dsc_sub_blks *sblk;
	struct drm_dsc_rc_range_parameters *rc;
	u32 off;

	if (!hw_dsc || !dsc)
		return;

	hw = &hw_dsc->hw;

	sblk = hw_dsc->caps->sblk;

	rc = dsc->rc_range_params;

	/*
	 * With BUF_THRESH -- 14 in total
	 * each register contains 4 thresh values with the last register
	 * containing only 2 thresh values
	 */
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_BUF_THRESH_0,
		      (dsc->rc_buf_thresh[0] << 0) |
		      (dsc->rc_buf_thresh[1] << 8) |
		      (dsc->rc_buf_thresh[2] << 16) |
		      (dsc->rc_buf_thresh[3] << 24));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_BUF_THRESH_1,
		      (dsc->rc_buf_thresh[4] << 0) |
		      (dsc->rc_buf_thresh[5] << 8) |
		      (dsc->rc_buf_thresh[6] << 16) |
		      (dsc->rc_buf_thresh[7] << 24));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_BUF_THRESH_2,
		      (dsc->rc_buf_thresh[8] << 0) |
		      (dsc->rc_buf_thresh[9] << 8) |
		      (dsc->rc_buf_thresh[10] << 16) |
		      (dsc->rc_buf_thresh[11] << 24));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_BUF_THRESH_3,
		      (dsc->rc_buf_thresh[12] << 0) |
		      (dsc->rc_buf_thresh[13] << 8));

	/*
	 * with min/max_QP -- 5 bits
	 * each register contains 5 min_qp or max_qp for total of 15
	 *
	 * With BPG_OFFSET -- 6 bits
	 * each register contains 5 BPG_offset for total of 15
	 */
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MIN_QP_0,
		      (rc[0].range_min_qp << 0) |
		      (rc[1].range_min_qp << 5) |
		      (rc[2].range_min_qp << 10) |
		      (rc[3].range_min_qp << 15) |
		      (rc[4].range_min_qp << 20));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MAX_QP_0,
		      (rc[0].range_max_qp << 0) |
		      (rc[1].range_max_qp << 5) |
		      (rc[2].range_max_qp << 10) |
		      (rc[3].range_max_qp << 15) |
		      (rc[4].range_max_qp << 20));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_0,
		      (rc[0].range_bpg_offset << 0) |
		      (rc[1].range_bpg_offset << 6) |
		      (rc[2].range_bpg_offset << 12) |
		      (rc[3].range_bpg_offset << 18) |
		      (rc[4].range_bpg_offset << 24));

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MIN_QP_1,
		      (rc[5].range_min_qp << 0) |
		      (rc[6].range_min_qp << 5) |
		      (rc[7].range_min_qp << 10) |
		      (rc[8].range_min_qp << 15) |
		      (rc[9].range_min_qp << 20));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MAX_QP_1,
		      (rc[5].range_max_qp << 0) |
		      (rc[6].range_max_qp << 5) |
		      (rc[7].range_max_qp << 10) |
		      (rc[8].range_max_qp << 15) |
		      (rc[9].range_max_qp << 20));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_1,
		      (rc[5].range_bpg_offset << 0) |
		      (rc[6].range_bpg_offset << 6) |
		      (rc[7].range_bpg_offset << 12) |
		      (rc[8].range_bpg_offset << 18) |
		      (rc[9].range_bpg_offset << 24));

	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MIN_QP_2,
		      (rc[10].range_min_qp << 0) |
		      (rc[11].range_min_qp << 5) |
		      (rc[12].range_min_qp << 10) |
		      (rc[13].range_min_qp << 15) |
		      (rc[14].range_min_qp << 20));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_MAX_QP_2,
		      (rc[10].range_max_qp << 0) |
		      (rc[11].range_max_qp << 5) |
		      (rc[12].range_max_qp << 10) |
		      (rc[13].range_max_qp << 15) |
		      (rc[14].range_max_qp << 20));
	DPU_REG_WRITE(hw, sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_2,
		      (rc[10].range_bpg_offset << 0) |
		      (rc[11].range_bpg_offset << 6) |
		      (rc[12].range_bpg_offset << 12) |
		      (rc[13].range_bpg_offset << 18) |
		      (rc[14].range_bpg_offset << 24));

	/*
	 * The whole encoder block, read back after everything is written, so it
	 * can be diffed field by field against the same registers read off a
	 * running Android. Off by default: this is called on every kickoff, not
	 * once per modeset, and left on it floods dmesg badly enough to matter
	 * (thousands of dumps within a minute, journald writing continuously).
	 *
	 *	echo 1 > /sys/module/msm/parameters/dsc_dump_regs
	 *
	 * Offsets are printed relative to the DSC block, so
	 * "dmesg | grep 'dpu_dsc 0'" lines up with a dpu-snapshot.py capture
	 * rebased on 0x0ae81000. Reading here is safe -- the block is being
	 * configured, so it is necessarily powered.
	 */
	if (dsc_dump_regs)
		for (off = 0; off < 0x80; off += 16)
			pr_info("dpu_dsc %08x: %08x %08x %08x %08x\n",
				sblk->enc.base + off,
				DPU_REG_READ(hw, sblk->enc.base + off),
				DPU_REG_READ(hw, sblk->enc.base + off + 4),
				DPU_REG_READ(hw, sblk->enc.base + off + 8),
				DPU_REG_READ(hw, sblk->enc.base + off + 12));
}

static void dpu_hw_dsc_bind_pingpong_blk_1_2(struct dpu_hw_dsc *hw_dsc,
					     const enum dpu_pingpong pp)
{
	struct dpu_hw_blk_reg_map *hw;
	const struct dpu_dsc_sub_blks *sblk;
	int mux_cfg = 0xf; /* Disabled */

	hw = &hw_dsc->hw;

	sblk = hw_dsc->caps->sblk;

	if (pp)
		mux_cfg = (pp - PINGPONG_0) & 0x7;

	DPU_REG_WRITE(hw, sblk->ctl.base + DSC_CTL, mux_cfg);
}

static void _setup_dcs_ops_1_2(struct dpu_hw_dsc_ops *ops)
{
	ops->dsc_disable = dpu_hw_dsc_disable_1_2;
	ops->dsc_config = dpu_hw_dsc_config_1_2;
	ops->dsc_config_thresh = dpu_hw_dsc_config_thresh_1_2;
	ops->dsc_bind_pingpong_blk = dpu_hw_dsc_bind_pingpong_blk_1_2;
}

/**
 * dpu_hw_dsc_init_1_2() - initializes the v1.2 DSC hw driver object
 * @dev:  Corresponding device for devres management
 * @cfg:  DSC catalog entry for which driver object is required
 * @addr: Mapped register io address of MDP
 * Returns: Error code or allocated dpu_hw_dsc context
 */
struct dpu_hw_dsc *dpu_hw_dsc_init_1_2(struct drm_device *dev,
				       const struct dpu_dsc_cfg *cfg,
				       void __iomem *addr)
{
	struct dpu_hw_dsc *c;

	c = drmm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->hw.blk_addr = addr + cfg->base;
	c->hw.log_mask = DPU_DBG_MASK_DSC;

	c->idx = cfg->id;
	c->caps = cfg;
	_setup_dcs_ops_1_2(&c->ops);

	return c;
}
