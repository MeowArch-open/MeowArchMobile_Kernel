// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/moduleparam.h>

#include <drm/drm_managed.h>

#include "dpu_hwio.h"
#include "dpu_hw_catalog.h"
#include "dpu_hw_lm.h"
#include "dpu_hw_dspp.h"
#include "dpu_kms.h"


/* DSPP_PCC */
#define PCC_EN BIT(0)
#define PCC_DIS 0
#define PCC_RED_R_OFF 0x10
#define PCC_RED_G_OFF 0x1C
#define PCC_RED_B_OFF 0x28
#define PCC_GREEN_R_OFF 0x14
#define PCC_GREEN_G_OFF 0x20
#define PCC_GREEN_B_OFF 0x2C
#define PCC_BLUE_R_OFF 0x18
#define PCC_BLUE_G_OFF 0x24
#define PCC_BLUE_B_OFF 0x30

/* DSPP_GC */
#define GC_EN BIT(0)
#define GC_DIS 0
#define GC_8B_ROUND_EN BIT(1)

/*
 * DSPP_SPR -- sub-pixel rendering.
 *
 * This block repacks each scanline for panels whose subpixels are not an RGB
 * stripe. Xiaomi's o11 panel on zorn is pentile: its green subpixels are one per
 * pixel but red and blue are only one per two, so red and blue have to be phase
 * shifted and merged across neighbouring pixels. Without that, green and white
 * come out correct while saturated red washes out to grey and blue shifts to
 * violet -- which is exactly what the panel does with SPR left off.
 *
 * The configuration below is not derived, it is transcribed: the whole 0x100
 * window was read out of a running Android through the downstream SDE debugfs
 * (dri/0/debug/sde_off + sde_reg) with the panel displaying correctly. That
 * sidesteps having to reverse the ten undocumented scalar fields packed into
 * SPR_OPMODE, for which the vendor sources carry neither comments nor bindings.
 *
 * Two of the values can be recognised in Binaries/zorn/RawFiles/
 * sprconfig_o11_42_02_0a_dsc_cmd.xml, which is a useful cross-check that the
 * block and the offsets are the ones being talked about:
 *
 *   ColorPhaseOffsetsRed  "2, -2"  -> 0x000003c2 at +0x24
 *					(2 and 30, i.e. -2, in 5-bit fields)
 *   ColorPhaseOffsetsBlue "-2, 2"  -> 0x0000005e at +0x2c
 *   FilterCoeffs "0,256,256,0,..." -> 0x01000000 / 0x00000100 from +0x34
 *
 * The phase offsets sit at +0x24 and +0x2c, not +0x28 and +0x30 -- they are the
 * first and third word of one array the vendor writes as a block from +0x24
 * (reg_dmav1_setup_spr_init_common: reg_off = base_off + 0x24, six 5-bit fields
 * per word), and both the bootloader and a running Android read back 0x3c2 at
 * +0x24 and 0x5e at +0x2c with +0x28 and +0x30 zero. Writing them one word high
 * lands them on array entries 6..7 and 18..19 and leaves the real ones zero.
 *
 * Being a transcription it is specific to this panel at 1440x3200 with two
 * layer mixers. Anything else needs its own dump; there is no DT binding for
 * these yet, and inventing one before a second panel exists would be guesswork.
 */
/*
 * Sub-pixel rendering. Needed on pentile panels: with it off, red and blue are
 * visibly wrong while green and white are fine. Setting this to N restores the
 * previous behaviour, which is the comparison to reach for if a panel's
 * transcribed configuration turns out not to suit it.
 */
static bool dpu_enable_spr = true;
module_param_named(enable_spr, dpu_enable_spr, bool, 0600);
MODULE_PARM_DESC(enable_spr,
		 "Program the DPU sub-pixel rendering block (pentile panels)");

#define SPR_OPMODE			0x04
#define SPR_OPMODE_EN			BIT(0)

static const struct {
	u32 off;
	u32 val;
} dpu_spr_o11_cfg[] = {
	{ 0x08, 0x00000001 }, /* two layer mixers */
	{ 0x10, 0x02d002d0 }, /* 720 + 720 */
	{ 0x24, 0x000003c2 }, /* red phase offsets   +2, -2 */
	{ 0x2c, 0x0000005e }, /* blue phase offsets  -2, +2 */
	{ 0x34, 0x01000000 }, /* filter coefficients, 0/256 alternating */
	{ 0x38, 0x00000100 },
	{ 0x3c, 0x01000000 },
	{ 0x40, 0x00000100 },
	{ 0x44, 0x01000000 },
	{ 0x48, 0x00000100 },
	{ 0x4c, 0x01000000 },
	{ 0x50, 0x00000100 },
	/*
	 * From +0x54 down to +0xf4 the values below already match what the block
	 * holds at reset -- they were read back identically from an Android that
	 * had not yet programmed SPR. They are written anyway so the block is in
	 * a known state rather than an assumed one. The genuinely
	 * HAL-programmed part is everything above, plus the ramp at +0xa0, which
	 * reads 0x00800000/0x01800100... unconfigured and the values below once
	 * Android has set it up.
	 */
	{ 0x54, 0x01062080 },
	{ 0x60, 0x0000000f },
	{ 0x70, 0x15555555 },
	{ 0x7c, 0x04441212 },
	{ 0x90, 0x00000008 },
	{ 0x98, 0x0000e401 },
	{ 0xa0, 0x00840000 }, /* the 16-entry ramp from +0xa0 */
	{ 0xa4, 0x018c0108 },
	{ 0xa8, 0x02950211 },
	{ 0xac, 0x039d0319 },
	{ 0xb0, 0x04a50421 },
	{ 0xb4, 0x05ad0529 },
	{ 0xb8, 0x06b60632 },
	{ 0xbc, 0x07be073a },
	{ 0xc0, 0x08c60842 },
	{ 0xc4, 0x09ce094a },
	{ 0xc8, 0x0ad70a53 },
	{ 0xcc, 0x0bdf0b5b },
	{ 0xd0, 0x0ce70c63 },
	{ 0xd4, 0x0def0d6b },
	{ 0xd8, 0x0ef80e74 },
	{ 0xdc, 0x0fff0f7c },
	{ 0xe4, 0x000000c0 },
	{ 0xe8, 0x00005d7f },
	{ 0xec, 0x000091b3 },
	{ 0xf0, 0x00006e4c },
	{ 0xf4, 0x0000a280 },
};

/* opmode as Android leaves it; bit 0 is the enable. */
#define SPR_OPMODE_O11			0x05289107

/*
 * ABL's opmode, and the reason there is a choice. ABL renders this panel with
 * correct colour -- blue text in the boot log is blue -- and a snapshot taken
 * from UEFI while its DPU was still live (SiliciumPkg/Drivers/DpuSnapshotDxe)
 * shows SPR *enabled but otherwise at reset*: opmode 0x00289107, the +0xa0 ramp
 * still 0x00800000/0x01800100..., and +0x54/+0x78/+0x7c untouched. Android, also
 * correct, has all of those programmed differently.
 *
 * Two references that both work and disagree on a register mean that register is
 * not what breaks the third one -- so this whole table is off the critical path.
 * The parameter exists to make that conclusion testable rather than argued:
 * spr_abl_minimal=1 writes exactly what ABL had, nothing else.
 */
#define SPR_OPMODE_O11_ABL		0x00289107

/*
 * ABL's SPR configuration: the registers it actually differs from reset on, and
 * nothing more. Deliberately short -- the point is to reproduce the minimal
 * state that is known to render correctly, not to configure the block well.
 */
static const struct {
	u32 off;
	u32 val;
} dpu_spr_o11_abl_cfg[] = {
	{ 0x08, 0x00000001 }, /* two layer mixers */
	{ 0x10, 0x02d002d0 }, /* 720 + 720 */
	{ 0x24, 0x000003c2 }, /* red phase offsets   +2, -2 */
	{ 0x2c, 0x0000005e }, /* blue phase offsets  -2, +2 */
	{ 0x34, 0x01000000 }, /* filter coefficients, 0/256 alternating */
	{ 0x38, 0x00000100 },
	{ 0x3c, 0x01000000 },
	{ 0x40, 0x00000100 },
	{ 0x44, 0x01000000 },
	{ 0x48, 0x00000100 },
	{ 0x4c, 0x01000000 },
	{ 0x50, 0x00000100 },
	{ 0x54, 0x01084210 }, /* ABL: 0x01084210, Android: 0x01062080 */
	{ 0x60, 0x0000000f },
	{ 0x70, 0x15555555 },
	{ 0x78, 0x00000000 }, /* ABL leaves both of these at zero */
	{ 0x7c, 0x00000000 },
	{ 0x90, 0x00000008 },
	{ 0x98, 0x0000e401 },
	/* the +0xa0 ramp as ABL leaves it: unconfigured, a plain 0x100 step */
	{ 0xa0, 0x00800000 },
	{ 0xa4, 0x01800100 },
	{ 0xa8, 0x02800200 },
	{ 0xac, 0x03800300 },
	{ 0xb0, 0x04800400 },
	{ 0xb4, 0x05800500 },
	{ 0xb8, 0x06800600 },
	{ 0xbc, 0x07800700 },
	{ 0xc0, 0x08800800 },
	{ 0xc4, 0x09800900 },
	{ 0xc8, 0x0a800a00 },
	{ 0xcc, 0x0b800b00 },
	{ 0xd0, 0x0c800c00 },
	{ 0xd4, 0x0d800d00 },
	{ 0xd8, 0x0e800e00 },
	{ 0xdc, 0x0f800f00 },
	{ 0xe4, 0x000000c0 },
	{ 0xe8, 0x00005d7f },
	{ 0xec, 0x000091b3 },
	{ 0xf0, 0x00006e4c },
	{ 0xf4, 0x0000a280 },
};

static bool dpu_spr_abl_minimal;
module_param_named(spr_abl_minimal, dpu_spr_abl_minimal, bool, 0600);
MODULE_PARM_DESC(spr_abl_minimal,
		 "Program SPR exactly as ABL leaves it instead of as Android does");
#define GC_LUT_SWAP_OFF 0x1c
#define GC_C0_OFF 0x4
#define GC_C1_OFF 0xc
#define GC_C2_OFF 0x14
#define GC_C0_INDEX_OFF 0x8
#define GC_C1_INDEX_OFF 0x10
#define GC_C2_INDEX_OFF 0x18

static void dpu_setup_dspp_pcc(struct dpu_hw_dspp *ctx,
		struct dpu_hw_pcc_cfg *cfg)
{

	u32 base;

	if (!ctx) {
		DRM_ERROR("invalid ctx %p\n", ctx);
		return;
	}

	base = ctx->cap->sblk->pcc.base;

	if (!base) {
		DRM_ERROR("invalid ctx %p pcc base 0x%x\n", ctx, base);
		return;
	}

	if (!cfg) {
		DRM_DEBUG_DRIVER("disable pcc feature\n");
		DPU_REG_WRITE(&ctx->hw, base, PCC_DIS);
		return;
	}

	DPU_REG_WRITE(&ctx->hw, base + PCC_RED_R_OFF, cfg->r.r);
	DPU_REG_WRITE(&ctx->hw, base + PCC_RED_G_OFF, cfg->r.g);
	DPU_REG_WRITE(&ctx->hw, base + PCC_RED_B_OFF, cfg->r.b);

	DPU_REG_WRITE(&ctx->hw, base + PCC_GREEN_R_OFF, cfg->g.r);
	DPU_REG_WRITE(&ctx->hw, base + PCC_GREEN_G_OFF, cfg->g.g);
	DPU_REG_WRITE(&ctx->hw, base + PCC_GREEN_B_OFF, cfg->g.b);

	DPU_REG_WRITE(&ctx->hw, base + PCC_BLUE_R_OFF, cfg->b.r);
	DPU_REG_WRITE(&ctx->hw, base + PCC_BLUE_G_OFF, cfg->b.g);
	DPU_REG_WRITE(&ctx->hw, base + PCC_BLUE_B_OFF, cfg->b.b);

	DPU_REG_WRITE(&ctx->hw, base, PCC_EN);
}

static void dpu_setup_dspp_gc(struct dpu_hw_dspp *ctx,
		struct dpu_hw_gc_lut *gc_lut)
{
	int i = 0;
	u32 base, reg;

	if (!ctx) {
		DRM_ERROR("invalid ctx\n");
		return;
	}

	base = ctx->cap->sblk->gc.base;

	if (!base) {
		DRM_ERROR("invalid ctx %p gc base\n", ctx);
		return;
	}

	if (!gc_lut) {
		DRM_DEBUG_DRIVER("disable gc feature\n");
		DPU_REG_WRITE(&ctx->hw, base, GC_DIS);
		return;
	}

	DPU_REG_WRITE(&ctx->hw, base + GC_C0_INDEX_OFF, 0);
	DPU_REG_WRITE(&ctx->hw, base + GC_C1_INDEX_OFF, 0);
	DPU_REG_WRITE(&ctx->hw, base + GC_C2_INDEX_OFF, 0);

	for (i = 0; i < PGC_TBL_LEN; i++) {
		DPU_REG_WRITE(&ctx->hw, base + GC_C0_OFF, gc_lut->c0[i]);
		DPU_REG_WRITE(&ctx->hw, base + GC_C1_OFF, gc_lut->c1[i]);
		DPU_REG_WRITE(&ctx->hw, base + GC_C2_OFF, gc_lut->c2[i]);
	}

	DPU_REG_WRITE(&ctx->hw, base + GC_LUT_SWAP_OFF, BIT(0));

	reg = GC_EN | ((gc_lut->flags & PGC_8B_ROUND) ? GC_8B_ROUND_EN : 0);
	DPU_REG_WRITE(&ctx->hw, base, reg);
}

/**
 * dpu_hw_dspp_init() - Initializes the DSPP hw driver object.
 * should be called once before accessing every DSPP.
 * @dev:  Corresponding device for devres management
 * @cfg:  DSPP catalog entry for which driver object is required
 * @addr: Mapped register io address of MDP
 * Return: pointer to structure or ERR_PTR
 */
/*
 * Report what the block took, but only when it changes: this runs from
 * dpu_crtc_atomic_begin() on every commit, so an unconditional print would be
 * one line per frame. The window offset is worth printing at all because it is
 * derived, not fixed -- SPR lives outside the DSPP register window and its
 * offset shrinks as the DSPP base grows, and getting that wrong writes a valid
 * configuration into the wrong SPR block, which looks exactly like "SPR has no
 * effect". Compare the offset against a bootloader snapshot: mdp+0x69400 is
 * SPR_0 at 0x0ae6a400, mdp+0x6a400 is SPR_1 at 0x0ae6b400.
 */
static void dpu_spr_report(struct dpu_hw_dspp *ctx, u32 base, u32 had)
{
	u32 off;

	if (DPU_REG_READ(&ctx->hw, base + SPR_OPMODE) == had)
		return;

	/*
	 * Same layout as the bootloader snapshot so the two can be diffed line
	 * by line -- the bootloader renders this panel correctly and its SPR
	 * window is the only readable copy of a working configuration.
	 */
	for (off = 0; off < 0x100; off += 0x10)
		pr_info("dpu_spr: %s mdp+%05x: %08x %08x %08x %08x\n",
			ctx->cap->name, ctx->cap->base + base + off,
			DPU_REG_READ(&ctx->hw, base + off),
			DPU_REG_READ(&ctx->hw, base + off + 4),
			DPU_REG_READ(&ctx->hw, base + off + 8),
			DPU_REG_READ(&ctx->hw, base + off + 12));
}

static void dpu_setup_dspp_spr(struct dpu_hw_dspp *ctx, bool enable)
{
	u32 base = ctx->cap->sblk->spr.base;
	u32 had = DPU_REG_READ(&ctx->hw, base + SPR_OPMODE);
	int i;

	if (!enable || !dpu_enable_spr) {
		DPU_REG_WRITE(&ctx->hw, base + SPR_OPMODE, 0);
		dpu_spr_report(ctx, base, had);
		return;
	}

	/*
	 * Everything before the opmode, so the block is never enabled while it
	 * is still half configured.
	 */
	if (dpu_spr_abl_minimal) {
		for (i = 0; i < ARRAY_SIZE(dpu_spr_o11_abl_cfg); i++)
			DPU_REG_WRITE(&ctx->hw, base + dpu_spr_o11_abl_cfg[i].off,
				      dpu_spr_o11_abl_cfg[i].val);

		DPU_REG_WRITE(&ctx->hw, base + SPR_OPMODE, SPR_OPMODE_O11_ABL);
		dpu_spr_report(ctx, base, had);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(dpu_spr_o11_cfg); i++)
		DPU_REG_WRITE(&ctx->hw, base + dpu_spr_o11_cfg[i].off,
			      dpu_spr_o11_cfg[i].val);

	DPU_REG_WRITE(&ctx->hw, base + SPR_OPMODE, SPR_OPMODE_O11);
	dpu_spr_report(ctx, base, had);
}

struct dpu_hw_dspp *dpu_hw_dspp_init(struct drm_device *dev,
				     const struct dpu_dspp_cfg *cfg,
				     void __iomem *addr)
{
	struct dpu_hw_dspp *c;

	if (!addr)
		return ERR_PTR(-EINVAL);

	c = drmm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->hw.blk_addr = addr + cfg->base;
	c->hw.log_mask = DPU_DBG_MASK_DSPP;

	/* Assign ops */
	c->idx = cfg->id;
	c->cap = cfg;
	if (c->cap->sblk->pcc.base)
		c->ops.setup_pcc = dpu_setup_dspp_pcc;
	if (c->cap->sblk->gc.base)
		c->ops.setup_gc = dpu_setup_dspp_gc;
	if (c->cap->sblk->spr.base)
		c->ops.setup_spr = dpu_setup_dspp_spr;

	return c;
}
