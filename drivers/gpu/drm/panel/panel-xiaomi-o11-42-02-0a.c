// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi "o11 42 02 0a" 1440x3200 DSC command mode AMOLED panel,
 * as fitted to the Redmi K80 (zorn, SM8650).
 *
 * The initialisation sequences, DSC parameters, timings and reset sequence in
 * this file are not hand-written: they were converted, by
 * linux-mdss-dsi-panel-driver-generator, out of Xiaomi's own panel description
 *   dsi-panel-o11-42-02-0a-dsc-cmd{,-common}.dtsi
 * from MiCode/vendor_opensource_display-drivers, branch bsp-zorn-v-oss. That
 * downstream tree has 155 C files and not one of them mentions this panel --
 * dsi_panel.c parses the .dtsi at runtime -- so the .dtsi *is* the vendor's
 * panel driver, and this file is that same data expressed as a drm_panel.
 *
 * The panel is command mode: it does not scan out on its own, the DPU pushes a
 * frame per TE. That is deliberate and is what makes it cheap; do not add
 * MIPI_DSI_MODE_VIDEO* here.
 *
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026 Xiaomi (panel data, via the vendor device tree)
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct o11_42_02_0a_dsc {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

/*
 * dsi_panel_pwr_supply_o11 in the vendor device tree, in its own order:
 *   vddio      1.800 V, 2 ms settle
 *   vddd-gpio  1.070 V, 12 ms settle   (display_panel_vddd: a regulator-fixed
 *                                       switched by tlmm 126, active high)
 *   vci        3.000 V, 2 ms settle
 */
static const struct regulator_bulk_data o11_42_02_0a_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vddd" },
	{ .supply = "vci" },
};

/* qcom,mdss-brightness-init-level, in a 15..16383 range (14 bit) */
#define O11_BRIGHTNESS_INIT	670
#define O11_BRIGHTNESS_MAX	16383

/*
 * Two behaviours that turned out to matter and that cannot be settled by reading
 * the vendor .dtsi, so they are tunable: sweeping them costs a reboot and an edit
 * to /etc/modprobe.d instead of a rebuild.
 *
 * init_brightness: DBV to write as the last command of the on-sequence. The
 *	vendor's sequence sets 0x51 to 0x0000 early and never raises it, leaving
 *	the panel dark until the backlight framework pushes a level -- and on an
 *	AMOLED a dark panel is indistinguishable from a dead one. But the sequence
 *	also ends with CMD2 page 4 selected (0xF0 55 AA 52 08 04), and 0x51 is
 *	page-sensitive on this panel: the vendor writes 0x51 0x0000 at offset 0 and
 *	0x51 0xFF0F at offset 4. Writing a DBV there may land on a different
 *	register entirely. 0 = do not write it (what the panel was known to work
 *	with); 670 = the vendor's qcom,mdss-brightness-init-level.
 *
 * display_on: send DCS 0x29 from enable(). The vendor .dtsi has it commented out
 *	because their stack issues it after the first frame. Without it the panel
 *	takes frames and shows nothing, so 1 is almost certainly right -- but it is
 *	worth being able to take it back out while bisecting.
 */
static uint init_brightness;
module_param(init_brightness, uint, 0600);
MODULE_PARM_DESC(init_brightness,
		 "DBV written at the end of the on-sequence (0 = none)");

static bool display_on = true;
module_param(display_on, bool, 0600);
MODULE_PARM_DESC(display_on, "Send DCS 0x29 (Display On) from enable()");

/*
 * The striping is content-dependent: solid colour renders perfectly, dense text
 * stripes, and it does so at both 960 Mbps and 1.2 Gbps per lane. That rules
 * bandwidth out and points at the DSC configuration -- flat content never uses
 * its rate budget, detailed content does. Two DSC values are worth sweeping, so
 * they are parameters rather than constants:
 *
 * dsc_bpc: 8, and NOT the 10 the vendor .dtsi asks for
 *	(qcom,mdss-dsc-bit-per-component). The vendor says 10 because Android's
 *	pipeline really does feed 10-bit content; the DPU here fetches XR24, which
 *	is 8 bits per component. Declaring 10 tells the DSC encoder one depth while
 *	the pipe delivers another, and this one field drives two separate things:
 *	dpu_hw_dsc_1_2.c's explicit 8-bit-input bit
 *	(if (dsc->bits_per_component == 8) data |= BIT(11)) and which row
 *	drm_dsc_setup_rc_params() picks.
 *
 *	The failure is content-dependent, which is what made it hard to see: flat
 *	colour hides it because the low bits are identical anyway, while dense
 *	content uses the full rate budget and the error accumulates into one
 *	corruption band per DSC slice row. Set this back to 10 only along with a
 *	10-bit-per-component framebuffer format.
 *
 * dsc_minor: qcom,mdss-dsc-version = <0x12> is DSC 1.2, which is what goes in
 *	the PPS -- but msm hardcodes drm_dsc_setup_rc_params(dsc,
 *	DRM_DSC_1_1_PRE_SCR) with the comment "DPU supports only pre-SCR panels".
 *	Announcing 1.2 in the PPS while using the 1.1 table is worth testing
 *	against announcing 1.1.
 */
/*
 * Rails-down time in the forced power cycle at the top of prepare(). 20 ms was
 * not enough -- the panel still refused a fresh init; the DPMS off/on cycle that
 * does work stayed down for about two seconds. AMOLED panels hold their internal
 * rails for a while, so this is the value to raise first if boot still comes up
 * dark while a manual blank/unblank still fixes it.
 */
static uint power_off_ms = 200;
module_param(power_off_ms, uint, 0600);
MODULE_PARM_DESC(power_off_ms, "ms to hold the panel rails down in prepare()");

/*
 * All five are writable at runtime and re-applied by prepare(). msm calls
 * dsi_populate_dsc_params() from dsi_timing_setup() on every modeset, so every
 * derived field -- rc parameters, slice_chunk_size, the PPS -- is recomputed from
 * whatever is in drm_dsc_config at that moment.
 *
 * A DPMS cycle is NOT enough, whatever this comment used to say:
 * dpu_encoder_prep_dsc() only runs on a full modeset, so sweeping these with
 * fb0/blank gives results that look like the value had no effect. Switch VTs,
 * which is a full modeset:
 *
 *	echo 10 > /sys/module/panel_xiaomi_o11_42_02_0a/parameters/dsc_bpc
 *	chvt 2; chvt 8
 */
static uint dsc_bpc = 8;
module_param(dsc_bpc, uint, 0600);
MODULE_PARM_DESC(dsc_bpc,
		 "DSC bits per component; 8 to match an XR24 framebuffer, 10 is what the vendor declares");

static uint dsc_minor = 2;
module_param(dsc_minor, uint, 0600);
MODULE_PARM_DESC(dsc_minor, "DSC version minor (vendor says 2; try 1)");

static uint dsc_slice_height = 20;
module_param(dsc_slice_height, uint, 0600);
MODULE_PARM_DESC(dsc_slice_height, "DSC slice height (vendor says 20)");

static bool dsc_block_pred = true;
module_param(dsc_block_pred, bool, 0600);
MODULE_PARM_DESC(dsc_block_pred, "DSC block prediction (vendor enables it)");

/*
 * The vendor drives this panel as DSC native 4:2:2, not 4:4:4 RGB. Two
 * independent sources say so:
 *
 *   Android's DSC_MAIN_CONF (mdss + 0x81130) reads 0x205402ca -- bit 22 is
 *   native_422 and bit 4 (convert_rgb) is clear, alongside 10 bpc and
 *   line_buf_depth 11.
 *
 *   The UEFI panel description Panel_O11_42_02_0a_amoled_dsc_cmd.xml carries
 *   <DSIDSCChromaFormat>1</DSIDSCChromaFormat>, Qualcomm's enum for YUV 4:2:2.
 *
 * mainline instead drives 4:4:4 and lets DSC do the RGB -> YCoCg transform, so
 * the decoder reads the chroma channels under the wrong format. That is exactly
 * the observed fault: every colour with Co = R - B nonzero is wrong (red, blue,
 * cyan, yellow) and every colour with Co = 0 is fine (green, magenta, white,
 * grey, black) -- see work/tmp/drm-colortest.c in the Mu-Silicium tree.
 *
 * Turning this on is not expected to be sufficient by itself: a native 4:2:2
 * source has to arrive already YCbCr with the chroma subsampled, and nothing in
 * drm/msm does that conversion. What it does establish is whether the fault
 * moves at all, which eleven experiments inside the 4:4:4 assumption could not.
 */
static bool dsc_native_422;
module_param(dsc_native_422, bool, 0600);
MODULE_PARM_DESC(dsc_native_422,
		 "DSC native 4:2:2, as the vendor and Android both use; mainline's default 4:4:4 is what breaks saturated red and blue");

/*
 * Two command sequences the vendor .dtsi carries and Xiaomi's HAL sends, which
 * a plain drm_panel never would. Neither is part of qcom,mdss-dsi-on-command, so
 * a mainline bring-up leaves the panel in a state Android never displays in.
 *
 * mi,mdss-dsi-set-csc-command -- colour space conversion:
 *      39 ... F0 55 AA 52 08 09     select register page 9
 *      15 ... B2 03
 *      39 ... 81 02 1C
 * Without it the panel runs its uncorrected native gamut. The .dtsi's own
 * qcom,mdss-dsi-panel-hdr-color-primaries describe the *corrected* target --
 * R (0.640,0.340) G (0.310,0.600) B (0.160,0.060), i.e. essentially sRGB -- so
 * whatever the panel does untouched is not what the vendor calibrates it to.
 * The two payload bytes of DCS 0x81 are exactly what the HAL rewrites per colour
 * mode (the .dtsi marks them -command-update = <0x81 2 2>), hence csc_value.
 *
 * mi,mdss-dsi-flat-mode-on-command -- DCS 0x5F. The .dtsi sets
 * mi,flatmode-default-on-enabled, so Android turns it on at every panel-on.
 * Mind the vendor's inverted naming: their "on" writes 0x00, "off" writes 0x01.
 */
static bool send_csc = true;
module_param(send_csc, bool, 0600);
MODULE_PARM_DESC(send_csc, "send the vendor colour-space-conversion sequence");

static ushort csc_value = 0x021c;
module_param(csc_value, ushort, 0600);
MODULE_PARM_DESC(csc_value,
		 "payload of DCS 0x81 in the CSC sequence (vendor default 0x021c)");

static int flat_mode;
module_param(flat_mode, int, 0600);
MODULE_PARM_DESC(flat_mode,
		 "-1 leave alone, 0 flat mode on (DCS 0x5F 00), 1 off (0x5F 01)");

/*
 * The one command in the vendor on-command list that mainline cannot honour.
 *
 * This panel is pentile, not RGB stripe: the .dtsi says
 *      qcom,spr-pack-type = "pentile";
 * and downstream's SDE reads that to program the DPU's SPR (sub-pixel
 * rendering) block, which repacks each line into the panel's real subpixel
 * layout. DCS 0x91 -- the .dtsi comments it "AP SPR" -- is how the panel is told
 * that the incoming stream is already packed that way.
 *
 * mainline drm/msm has no SPR block support at all, so the DPU sends ordinary
 * RGB while this command promises pentile-packed data. Every subpixel then lands
 * one position out, which shows up not as noise but as a hue shift on fine
 * detail -- flat colour is unaffected because neighbouring subpixels carry the
 * same value. KDE's blue accent reading as violet is exactly that.
 *
 * So the default here is off: do not claim a packing the DPU is not doing, and
 * let the panel fall back to its own internal handling.
 */
static bool send_spr;
module_param(send_spr, bool, 0600);
MODULE_PARM_DESC(send_spr,
		 "send the vendor 'AP does SPR' command (DCS 0x91); off by default because mainline's DPU has no SPR block");


static inline
struct o11_42_02_0a_dsc *to_o11_42_02_0a_dsc(struct drm_panel *panel)
{
	return container_of_const(panel, struct o11_42_02_0a_dsc, panel);
}

/*
 * qcom,mdss-dsi-reset-sequence = <1 11>, <0 1>, <1 11>, driven raw by
 * dsi_panel_reset(). Reset is active low on the panel, so with
 * reset-gpios = <&tlmm 133 GPIO_ACTIVE_LOW> the logical values below give the
 * physical high / low / high the vendor sequence asks for, and leave the panel
 * out of reset.
 */
static void o11_42_02_0a_dsc_reset(struct o11_42_02_0a_dsc *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
}

static int o11_42_02_0a_dsc_on(struct o11_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x059f);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x0c7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03, 0x43);
	if (send_spr)
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
					     0xab, 0xf0, 0x00, 0x14, 0xd1, 0x00, 0x01,
					     0xde, 0x00, 0xa3, 0x00, 0x3c, 0x05, 0x7a,
					     0x15, 0x9a, 0x11, 0x50);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xde,
				     0x10, 0x34, 0x25, 0x30, 0x14, 0x25);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0,
				     0x84, 0x44, 0x01, 0x00, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0xcc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2, 0x00);
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0x0000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x04);
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0xff0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x20);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8b, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0xa2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0xff, 0xff, 0xff, 0xff, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc7, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x35);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x22, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xca, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8,
				     0x11, 0x90, 0x00, 0x00, 0x00, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8,
				     0x00, 0x70, 0x74, 0x74, 0x78, 0x78, 0x7c,
				     0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c,
				     0x78, 0x78, 0x74, 0x74, 0x70, 0x70, 0x64,
				     0x64, 0x64);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8,
				     0x00, 0x64, 0x6c, 0x6c, 0x80, 0x80, 0x88,
				     0x88, 0x88, 0x88, 0x8c, 0x8c, 0x8c, 0x8c,
				     0x88, 0x88, 0x84, 0x84, 0x84, 0x84, 0x70,
				     0x70, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x34);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8,
				     0x00, 0xa4, 0xa8, 0xa8, 0xa8, 0xa8, 0xa8,
				     0xa8, 0xac, 0xac, 0xac, 0xac, 0xac, 0xac,
				     0xa8, 0xa8, 0xa4, 0xa4, 0xa4, 0xa4, 0x8c,
				     0x8c, 0x8c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe0, 0x01, 0x01, 0x01, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe0,
				     0x80, 0x00, 0x21, 0x32, 0x43, 0x54, 0x65,
				     0x76, 0x87, 0x98, 0x99, 0x99, 0x99);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x03, 0x03, 0x03, 0x06, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x01, 0x02, 0x04, 0x05, 0x0a, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x03, 0x04, 0x06, 0x07, 0x0b, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x38);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
				     0x01, 0x04, 0x07, 0x09, 0x0a, 0x0c, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
				     0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x54);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
				     0x03, 0x04, 0x06, 0x07, 0x09, 0x0a, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x62);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
				     0x01, 0x03, 0x05, 0x08, 0x0c, 0x0d, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02,
				     0x03, 0x05, 0x06, 0x09, 0x0b, 0x0e, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x01, 0x01, 0x01, 0x02, 0x19, 0x1a, 0x10,
				     0x0f, 0x0f, 0x17, 0x08, 0x12, 0x15, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x04, 0x08, 0x0b, 0x0f, 0x11, 0x11, 0x13,
				     0x13, 0x0c, 0x0c, 0x0a, 0x0b, 0x12, 0x13);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x02, 0x04, 0x05, 0x07, 0x0a, 0x0d, 0x0f,
				     0x0d, 0x06, 0x09, 0x0a, 0x0e, 0x0f, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x02, 0x04, 0x0a, 0x0b, 0x02, 0x03, 0x06,
				     0x07, 0x07, 0x09, 0x0b, 0x0c, 0x0f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x38);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x03, 0x05, 0x05, 0x08, 0x02, 0x02, 0x04,
				     0x05, 0x05, 0x08, 0x0b, 0x0d, 0x10, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x02, 0x02, 0x03, 0x05, 0x02, 0x01, 0x04,
				     0x05, 0x07, 0x09, 0x0a, 0x0c, 0x0f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x54);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x02, 0x02, 0x03, 0x05, 0x02, 0x01, 0x04,
				     0x05, 0x07, 0x09, 0x0a, 0x0c, 0x0f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x62);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x01, 0x01, 0x02, 0x02, 0x01, 0x01, 0x03,
				     0x03, 0x07, 0x08, 0x09, 0x0d, 0x0d, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x03,
				     0x04, 0x06, 0x08, 0x09, 0x0b, 0x0f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x02, 0x02, 0x07, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
				     0x01, 0x01, 0x02, 0x04, 0x04, 0x0e, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
				     0x01, 0x01, 0x04, 0x03, 0x05, 0x08, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
				     0x02, 0x02, 0x06, 0x08, 0x0c, 0x0d, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x38);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
				     0x02, 0x05, 0x06, 0x0a, 0x0a, 0x0c, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x46);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
				     0x02, 0x04, 0x07, 0x06, 0x0a, 0x0b, 0x0d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x54);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
				     0x02, 0x04, 0x07, 0x06, 0x0a, 0x0b, 0x0d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x62);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x00, 0x01, 0x01, 0x01, 0x02, 0x03,
				     0x04, 0x05, 0x07, 0x0a, 0x0c, 0x0f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe3,
				     0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
				     0x03, 0x06, 0x07, 0x0a, 0x0d, 0x0f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe5,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe6,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe0, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x47, 0x45);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc8, 0x01, 0x02, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x22);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x82);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf3, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x0a, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x78);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0,
				     0x11, 0x00, 0x2a, 0x00, 0xc2, 0x01, 0xda,
				     0x03, 0x7c, 0x05, 0xb1, 0x08, 0x80, 0x0b,
				     0xee, 0x0f, 0xff, 0x05, 0x55, 0x05, 0x56,
				     0x05, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x6f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0,
				     0x02, 0xd0, 0x05, 0xa0, 0x02, 0xd0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1,
				     0x8c, 0xff, 0xcc, 0xff, 0xff, 0xcc, 0xff,
				     0xff, 0xcc, 0xff, 0xff, 0xec, 0xff, 0x7f,
				     0xee, 0x7f, 0x7f, 0xff, 0x7f, 0xff, 0x0e,
				     0x7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1, 0x4f, 0x00, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc1,
				     0x0c, 0xff, 0xcc, 0xff, 0xff, 0xcc, 0xff,
				     0xff, 0xcc, 0xff, 0xff, 0xec, 0xff, 0x7f,
				     0xee, 0x7f, 0x7f, 0xcf, 0x7f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x87);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x99);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x20, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x70, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x26, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x20, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x3a, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x62, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x14);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x62, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x62, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x62, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x26);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x3f, 0x62, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x29);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x11, 0x11, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0xaa, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0xaa, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0xe3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0xe4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x29);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x29);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x02, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea, 0x01, 0x39);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea, 0x01, 0xac);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea, 0x01, 0x39);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea, 0x01, 0xac);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0xea);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0xea);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x77);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4, 0x90);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4, 0x90);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb,
				     0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0,
				     0x55, 0xaa, 0x52, 0x08, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xeb, 0x01);

	/*
	 * See the init_brightness module parameter. Default off: the panel is known
	 * to render with the vendor sequence left exactly as it is, and known not to
	 * render with a DBV appended here.
	 *
	 * Written as raw bytes rather than through a helper because the byte order
	 * matters and the two helpers disagree: mipi_dsi_dcs_set_display_brightness()
	 * sends the low byte first, _large() sends the high byte first. Downstream
	 * swaps the value itself (qcom,mdss-dsi-bl-inverted-dbv) and then uses the
	 * low-byte-first helper, which puts the high byte on the wire first -- so
	 * high byte first is what this panel wants, and it is what _large() in
	 * bl_update_status() below also does.
	 */
	/* write_seq_multi() needs compile-time bytes; this value is a parameter. */
	if (init_brightness) {
		u8 bl[] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
			    init_brightness >> 8, init_brightness & 0xff };

		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, bl, sizeof(bl));
	}

	return dsi_ctx.accum_err;
}

static int o11_42_02_0a_dsc_off(struct o11_42_02_0a_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x28, 0x00);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x10, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 105);

	return dsi_ctx.accum_err;
}

static int o11_42_02_0a_dsc_off(struct o11_42_02_0a_dsc *ctx);

static int o11_42_02_0a_dsc_prepare(struct drm_panel *panel)
{
	struct o11_42_02_0a_dsc *ctx = to_o11_42_02_0a_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	/*
	 * Re-apply the DSC knobs on every prepare, so they can be swept with a DPMS
	 * cycle rather than a reboot. msm recomputes everything derived from these in
	 * dsi_timing_setup() during the modeset that follows.
	 */
	ctx->dsc.bits_per_component = dsc_bpc;
	ctx->dsc.dsc_version_minor = dsc_minor;
	ctx->dsc.slice_height = dsc_slice_height;
	ctx->dsc.block_pred_enable = dsc_block_pred;
	ctx->dsc.native_422 = dsc_native_422;
	dev_info(dev,
		 "dsc 1.%u, %ux%u slices x%u, %u bpc, block_pred %d, native_422 %d\n",
		 dsc_minor, ctx->dsc.slice_width, dsc_slice_height,
		 ctx->dsc.slice_count, dsc_bpc, dsc_block_pred, dsc_native_422);

	/*
	 * ABL brings this panel up itself and hands it over initialised and
	 * scanning out -- UEFI here is SimpleFbDxe, which only paints into the
	 * framebuffer ABL left behind, so nothing between ABL and Linux ever turns
	 * the panel off. From that state a reset pulse is not enough: the panel
	 * ignores a fresh init sequence and stays dark. It was only ever seen to
	 * light up after a DPMS off/on cycle, i.e. after unprepare() had actually
	 * dropped its rails.
	 *
	 * The rails are already up at boot, so regulator_bulk_enable() below would
	 * merely take a reference and no power cycle would happen. Force one: take
	 * the reference, hold reset asserted, drop the reference again so the rails
	 * really go down (the panel is their only consumer), settle, and then run the
	 * normal path. Costs ~50 ms on every enable, which is worth not needing a
	 * manual blank/unblank to get a picture.
	 */
	ret = regulator_bulk_enable(ARRAY_SIZE(o11_42_02_0a_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	/*
	 * Tell the panel to stop displaying before yanking its rails, exactly as
	 * unprepare() would: DCS 0x28, 0x10, 105 ms. On a fresh boot the panel may
	 * not answer at all and that is fine -- off() keeps its own error context, so
	 * a failure here cannot poison the init that follows. A 20 ms power-off was
	 * not enough on its own; the cycle that demonstrably works had the off
	 * commands in front of it and stayed down far longer, hence power_off_ms.
	 */
	o11_42_02_0a_dsc_off(ctx);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	regulator_bulk_disable(ARRAY_SIZE(o11_42_02_0a_supplies), ctx->supplies);
	msleep(power_off_ms);

	ret = regulator_bulk_enable(ARRAY_SIZE(o11_42_02_0a_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	/* Longest of the vendor's per-supply settle times (vddd-gpio, 12 ms) */
	usleep_range(12000, 13000);

	o11_42_02_0a_dsc_reset(ctx);

	ret = o11_42_02_0a_dsc_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(o11_42_02_0a_supplies),
				       ctx->supplies);
		return ret;
	}

	/*
	 * Fill in everything the PPS needs before packing it.
	 *
	 * The bridge chain calls pre_enable from the connector end backwards, so
	 * this runs *before* msm_dsi_host_power_on() -> dsi_timing_setup() ->
	 * dsi_populate_dsc_params(). Left alone, the PPS would go out with every
	 * derived field still zero -- slice_chunk_size, final_offset, the rc
	 * parameters -- because probe() only sets the raw ones. A 4:4:4 decoder can
	 * recompute chunk size from slice_width * bpp and gets away with it; a
	 * native 4:2:2 chunk size involves halving the width and doubling bpp, so
	 * the panel has to be told.
	 *
	 * msm recomputes all of this from the same inputs a moment later, so the
	 * DPU registers are unaffected; only the PPS contents change. The two
	 * panel-specific overrides match what the vendor stack hardcodes for this
	 * panel id (sde_dsc_helper.c) and what dsc_xmit_delay / dsc_dec_delay are
	 * set to on the msm side.
	 */
	drm_dsc_set_const_params(&ctx->dsc);
	drm_dsc_set_rc_buf_thresh(&ctx->dsc);

	ret = drm_dsc_setup_rc_params(&ctx->dsc, ctx->dsc.native_422 ?
					DRM_DSC_1_2_422 : DRM_DSC_1_1_PRE_SCR);
	if (ret) {
		dev_err(dev, "no DSC rate-control parameters: %d\n", ret);
		return ret;
	}

	ctx->dsc.line_buf_depth = ctx->dsc.bits_per_component + 1;
	ctx->dsc.initial_scale_value = drm_dsc_initial_scale_value(&ctx->dsc);

	if (ctx->dsc.native_422) {
		ctx->dsc.initial_xmit_delay = 256;
		ctx->dsc.first_line_bpg_offset = 13;
	}

	ret = drm_dsc_compute_rc_parameters(&ctx->dsc);
	if (ret) {
		dev_err(dev, "failed to compute DSC rc parameters: %d\n", ret);
		return ret;
	}

	if (ctx->dsc.native_422)
		ctx->dsc.initial_dec_delay = 478;

	dev_info(dev,
		 "pps: chunk %u final_off %u xmit %u dec %u scale %u inc %u dec_int %u nfl %u slice_bpg %u\n",
		 ctx->dsc.slice_chunk_size, ctx->dsc.final_offset,
		 ctx->dsc.initial_xmit_delay, ctx->dsc.initial_dec_delay,
		 ctx->dsc.initial_scale_value,
		 ctx->dsc.scale_increment_interval,
		 ctx->dsc.scale_decrement_interval,
		 ctx->dsc.nfl_bpg_offset, ctx->dsc.slice_bpg_offset);

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}

	msleep(28); /* TODO: Is this panel-dependent? */

	return 0;
}

static int o11_42_02_0a_dsc_unprepare(struct drm_panel *panel)
{
	struct o11_42_02_0a_dsc *ctx = to_o11_42_02_0a_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = o11_42_02_0a_dsc_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(o11_42_02_0a_supplies),
			       ctx->supplies);

	return 0;
}

/*
 * The vendor's on-sequence ends without a Display On -- their .dtsi has it
 * written out and then commented:
 *
 *	[Display On]
 *	[05 00 00 00 00 00 02 29 00]
 *
 * because the downstream stack issues it itself, once the first frame has landed
 * in the panel's RAM. Same intent as qcom,bl-update-flag =
 * "delay_until_first_frame": do not show the panel until there is something in
 * it, or the first thing anyone sees is a flash of whatever was left over.
 *
 * Skipping it altogether is not an option though. Without DCS 0x29 the panel
 * initialises, accepts every one of the 418 command transfers, takes frames, and
 * stays black -- which is exactly what it did. Sending it from enable() instead
 * of from prepare() is the closest mainline equivalent of the vendor's ordering:
 * the DSI bridge calls drm_panel_prepare() from atomic_pre_enable and
 * drm_panel_enable() from atomic_enable, so by the time this runs the encoder
 * and CRTC are already up.
 */
static int o11_42_02_0a_dsc_enable(struct drm_panel *panel)
{
	struct o11_42_02_0a_dsc *ctx = to_o11_42_02_0a_dsc(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	if (!display_on) {
		dev_info(&ctx->dsi->dev, "display on suppressed\n");
		return 0;
	}

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	/*
	 * After display-on, which is where Xiaomi's HAL applies both of these.
	 * Failures are logged but not fatal: a panel with the wrong gamut still
	 * shows a usable picture, and refusing to enable would leave a black
	 * screen instead.
	 */
	if (send_csc) {
		u8 page9[] = { 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x09 };
		u8 b2[] = { 0xb2, 0x03 };
		u8 csc[] = { 0x81, csc_value >> 8, csc_value & 0xff };

		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, page9, sizeof(page9));
		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, b2, sizeof(b2));
		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, csc, sizeof(csc));
	}

	if (flat_mode >= 0) {
		u8 flat[] = { 0x5f, flat_mode ? 0x01 : 0x00 };

		mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, flat, sizeof(flat));
	}

	dev_info(&ctx->dsi->dev,
		 "display on, err %d, init_brightness %u, csc %d/%#06x, flat_mode %d, spr %d\n",
		 dsi_ctx.accum_err, init_brightness, send_csc, csc_value,
		 flat_mode, send_spr);

	return dsi_ctx.accum_err;
}

/*
 * The vendor .dtsi carries three timings -- timing@0 60 Hz, timing@1 120 Hz,
 * timing@2 90 Hz -- and they are byte-for-byte identical apart from two DCS
 * writes in the on-sequence:
 *
 *	         0x2F   0xB2
 *	 60 Hz   0x02   0x01
 *	 90 Hz   0x01   0x00
 *	120 Hz   0x00   0x00
 *
 * Same 1440x3200, same porches, same 1.2 GHz bit clock, same 7300 us transfer
 * time: on a command mode panel the refresh rate is how often the DPU is
 * allowed to push, not a blanking figure. Only 120 Hz is exposed because
 * drm_panel_funcs has no mode_set hook, so a panel driver cannot learn which
 * mode the atomic commit picked and therefore cannot re-issue those two bytes.
 * Adding the other two rates means teaching the DSI panel API about mode
 * switching first; until then this is the fast one, which is the one worth
 * having.
 */

static const struct drm_display_mode o11_42_02_0a_dsc_mode = {
	.clock = (1440 + 8 + 12 + 8) * (3200 + 16 + 14 + 6) * 120 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 8,
	.hsync_end = 1440 + 8 + 12,
	.htotal = 1440 + 8 + 12 + 8,
	.vdisplay = 3200,
	.vsync_start = 3200 + 16,
	.vsync_end = 3200 + 16 + 14,
	.vtotal = 3200 + 16 + 14 + 6,
	.width_mm = 695,
	.height_mm = 1545,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int o11_42_02_0a_dsc_get_modes(struct drm_panel *panel,
				      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &o11_42_02_0a_dsc_mode);
}

static const struct drm_panel_funcs o11_42_02_0a_dsc_panel_funcs = {
	.prepare = o11_42_02_0a_dsc_prepare,
	.enable = o11_42_02_0a_dsc_enable,
	.unprepare = o11_42_02_0a_dsc_unprepare,
	.get_modes = o11_42_02_0a_dsc_get_modes,
};

static int o11_42_02_0a_dsc_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dev_info(&dsi->dev, "brightness %u\n", brightness);

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

// TODO: Check if /sys/class/backlight/.../actual_brightness actually returns
// correct values. If not, remove this function.
static int o11_42_02_0a_dsc_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops o11_42_02_0a_dsc_bl_ops = {
	.update_status = o11_42_02_0a_dsc_bl_update_status,
	.get_brightness = o11_42_02_0a_dsc_bl_get_brightness,
};

/*
 * DCS 0x51 with a two-byte payload, i.e. mipi_dsi_dcs_set_display_brightness_
 * large(): qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs" and the vendor's
 * own command is "39 00 00 00 00 00 03 51 00 00". Range 15..16383 (14 bit),
 * initial level 670, all from the vendor .dtsi. The panel is driven entirely
 * over DSI -- there is no PWM and no backlight GPIO -- so brightness works
 * here without any firmware or vendor driver involvement.
 *
 * Two vendor quirks are deliberately not implemented, because they are policy
 * rather than function: qcom,mdss-dsi-bl-inverted-dbv (Xiaomi byte-swaps the
 * 14-bit value, which mipi_dsi_dcs_set_display_brightness_large already sends
 * big-endian, so nothing to do) and qcom,bl-update-flag =
 * "delay_until_first_frame" (hold the first brightness write back until a frame
 * has landed, to avoid a bright flash on enable).
 */
static struct backlight_device *
o11_42_02_0a_dsc_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = O11_BRIGHTNESS_INIT,
		.max_brightness = O11_BRIGHTNESS_MAX,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &o11_42_02_0a_dsc_bl_ops, &props);
}

static int o11_42_02_0a_dsc_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct o11_42_02_0a_dsc *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct o11_42_02_0a_dsc, panel,
				   &o11_42_02_0a_dsc_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(o11_42_02_0a_supplies),
					    o11_42_02_0a_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	/*
	 * The panel is 30 bpp (qcom,mdss-dsi-bpp = <30>, rgb_swap_rgb), but the
	 * wire carries the DSC stream and msm's dsi_get_bpp() only knows
	 * 24/18/16, so the real depth travels in dsc.bits_per_component below.
	 * Every upstream 10-bit DSC panel does the same.
	 *
	 * No MIPI_DSI_MODE_VIDEO*: qcom,mdss-dsi-panel-type = "dsi_cmd_mode".
	 * The generator emitted VIDEO_BURST from qcom,mdss-dsi-traffic-mode =
	 * "burst_mode", which on a command mode panel only describes how a
	 * command DMA is paced. Leaving it in makes the DPU drive continuous
	 * video timing into a panel that expects one frame per TE.
	 *
	 * MIPI_DSI_MODE_NO_EOT_PACKET is likewise absent on purpose: the vendor
	 * sets qcom,mdss-dsi-tx-eot-append, so EoT packets are wanted.
	 */
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = o11_42_02_0a_dsc_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	/*
	 * Straight out of the vendor .dtsi: qcom,mdss-dsc-version = <0x12>
	 * (DSC 1.2), slice 720x20, 10 bits per component, 8 bpp compressed,
	 * block prediction on. slice_per_pkt = 2 in the vendor tree is the same
	 * thing as slice_count here: 1440 / 720.
	 */
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = dsc_minor;

	ctx->dsc.slice_height = dsc_slice_height;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 1440 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = dsc_bpc;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = dsc_block_pred;
	ctx->dsc.native_422 = dsc_native_422;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void o11_42_02_0a_dsc_remove(struct mipi_dsi_device *dsi)
{
	struct o11_42_02_0a_dsc *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id o11_42_02_0a_dsc_of_match[] = {
	{ .compatible = "xiaomi,o11-42-02-0a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, o11_42_02_0a_dsc_of_match);

static struct mipi_dsi_driver o11_42_02_0a_dsc_driver = {
	.probe = o11_42_02_0a_dsc_probe,
	.remove = o11_42_02_0a_dsc_remove,
	.driver = {
		.name = "panel-xiaomi-o11-42-02-0a",
		.of_match_table = o11_42_02_0a_dsc_of_match,
	},
};
module_mipi_dsi_driver(o11_42_02_0a_dsc_driver);

MODULE_DESCRIPTION("DRM driver for the Xiaomi o11 42 02 0a DSC command mode panel");
MODULE_LICENSE("GPL");
