// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Integrated SIA91xx digital smart speaker amplifier.
 *
 * Written from the register-level behaviour of the vendor's sia91xx_dlkm.ko
 * (SI-IN SIA81xx ASoC driver, GPL, unstripped) plus what the chip itself
 * reports. The source is not published anywhere reachable, but the module's
 * tables are readable and its symbol names survived, so the parts this driver
 * needs could be recovered rather than guessed:
 *
 *   reg_map_info_table[]  21 entries x 13 u32
 *       { chip_type, reg_bits, val_bits, id_reg, id_range[4][2], nr_ranges }
 *
 * Entry 0x13 is "sia9187" (the name table at .rodata+0x40e0 is indexed by chip
 * type; 0x0e/0x0f/0x10 are the generic sia81x9/sia8152x/sia917x entries whose
 * sub-lists in sipa_compat_table confirm the indexing). It says: 8-bit register
 * addresses, 16-bit values, identity register 0x06, valid IDs 0x5d80..0x5d90.
 *
 * Confirmed against the hardware on a Redmi K80 (zorn), which carries two of
 * them at 0x6c and 0x6d: register 0x06 reads 0x5d90 big-endian on both.
 *
 * The DAI description likewise comes from the vendor module's sia91xx_dai
 * (.rodata+0x2e28): "sia91xx-aif", S16/S24/S32, 11.025-64 kHz, 1-2 channels,
 * playback plus a capture side for the amplifier's I/V sense.
 *
 * The power-on sequence comes from the vendor's parameter file, /vendor/firmware/
 * sipa.bin (magic 32be86c7), whose layout the module's own accessors describe: a
 * 188-byte file header followed by one 1968-byte block per channel, each block
 * starting with the 192-byte chip config that sipa_param_read_chip_cfg() hands
 * out. Inside a config, the descriptor at +64 is {offset, count, entry_size} for
 * the register table sipa_regmap_defaults() writes (25 x 32 bytes:
 * { u32 reg; u32 mask; u32 val[6]; }, one value per audio scene) and the one at
 * +76 describes the list sipa_regmap_set_chip_on() runs (44 bytes:
 * { u32 reg; u32 mask; u32 op; u32 pad; u32 delay_us; u32 val[6]; }).
 *
 * The op field indexes a four-entry jump table at .rodata+0x4998:
 *
 *   0  read the register and fold the masked bits back into val[scene]
 *   1  write val[scene]; masked read-modify-write unless the mask covers the
 *      whole register
 *   2  read and verify (val[scene] ^ read) & mask == 0
 *   3  delay only
 *
 * The scene names are audio_scene[] at .rodata+0x3bf0: Playback, Voice, Voip,
 * Receiver, Factory, FM. This driver uses scene 0, and the tables below are
 * generated straight from zorn's sipa.bin, so they are the vendor's numbers
 * rather than an interpretation of them.
 *
 * Register 0x01's low nibble is the state field the vendor's lists check: 0 when
 * the part is idle, 5 once it is on, which is also how sipa_regmap_get_chip_en()
 * reads back an enabled sia9187. Register 0x13 carries the enable: 0x0384 on,
 * 0x0381 off. Register 0x15 is the only difference between the two channels'
 * power-on lists (0x0a0a against 0x0b0b), so it is where the part is told which
 * half of the I2S frame belongs to it.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

/*
 * What to drive on the vendor's spksw-gpio at power-on: 1 or 0, or -1 to claim
 * the pin and leave it alone. A running Android reads 1 on the part that has the
 * pin, but it reads that with the amplifiers off, so the value that belongs with
 * playback is not actually known -- hence a knob rather than a constant.
 */
static int sia91xx_spksw = 1;
module_param_named(spksw, sia91xx_spksw, int, 0644);
MODULE_PARM_DESC(spksw, "value driven on spksw-gpio at power-on (-1 leaves it)");

#define SIA91XX_REG_STATE	0x01
#define SIA91XX_REG_CHIPID	0x06
#define SIA91XX_REG_ENABLE	0x13

/* Register 0x01's low nibble, as the vendor's own verify steps read it. */
#define SIA91XX_STATE		GENMASK(3, 0)
#define SIA91XX_STATE_IDLE	0x0
#define SIA91XX_STATE_ON	0x5

#define SIA9187_DISABLE		0x0381

/* From reg_map_info_table entry 0x13. */
#define SIA9187_ID_FIRST	0x5d80
#define SIA9187_ID_LAST		0x5d90

/*
 * Register 0x14's bits [7:5] are the serial-interface format. The vendor's value
 * (0xa3c8, field 110) describes the frame their own link sends -- the one
 * Qualcomm calls TDM-LPAIF-RX-SECONDARY: 4 slots x 16 bit. This is the value
 * Android's driver programs, and it is what the part actually needs to latch and
 * drive its boost (regs 0x03/0x04/0x05 fill with live VBAT/temp/current only
 * when the framing matches).
 *
 * Earlier this driver used 0x9328 (the same register with that field back at 001
 * = plain I2S) to pair with AudioReach's I2S sink, since AudioReach could not
 * produce TDM framing. But at 16-bit I2S the frame is too short for the part to
 * latch (silent), and at 32-bit I2S the part latches but the boost ADCs stay
 * zero at every amplitude (no sound) and full volume browns out the PMIC -- the
 * frame alignment never matches what the part expects.
 *
 * The backend now sends real TDM (4 slot x 16 bit) via the SECONDARY_TDM_RX_0
 * port and AudioReach's TDM sink, so this returns to the vendor's 0xa3c8 to
 * match it.
 */
#define SIA9187_R14_FACTORY	0xa3c8

/* zorn wires channel 0 to 0x6c and channel 1 to 0x6d. */
#define SIA9187_FIRST_ADDR	0x6c

struct sia91xx_reg {
	u8 reg;
	u16 val;
};

struct sia91xx_setup {
	const struct sia91xx_reg *defaults;
	unsigned int num_defaults;
	const struct sia91xx_reg *power_on;
	unsigned int num_power_on;
};

struct sia91xx {
	const struct sia91xx_setup *setup;
	struct regmap *regmap;
	struct gpio_desc *reset;
	struct gpio_desc *spksw;
	struct device *dev;
	unsigned int chipid;
};

/* Generated from vendor sipa.bin (magic 32be86c7), scene 0 = Playback. */

static const struct sia91xx_reg sia9187_defaults_ch0[] = {
	{ 0x17, 0x0418 },
	{ 0x18, 0x1ab3 },
	{ 0x19, 0x3e28 },
	{ 0x1a, 0x6c00 },
	{ 0x1b, 0x83ee },
	{ 0x1c, 0x8b40 },
	{ 0x1d, 0xee80 },
	{ 0x1e, 0x8000 },
	{ 0x20, 0x8122 },
	{ 0x23, 0x0288 },
	{ 0x24, 0x4800 },
	{ 0x25, 0x0800 },
	{ 0x26, 0x4000 },
	{ 0x27, 0x0004 },
	{ 0x28, 0x2408 },
	{ 0x29, 0x6000 },
	{ 0x2a, 0x9664 },
	{ 0x2b, 0x910a },
	{ 0x2c, 0x0014 },
	{ 0x2d, 0x12fc },
	{ 0x2e, 0xc4bf },
	{ 0x2f, 0x0000 },
	{ 0x30, 0x0000 },
	{ 0x31, 0x000b },
	{ 0x77, 0x100c },
};

static const struct sia91xx_reg sia9187_defaults_ch1[] = {
	{ 0x17, 0x0418 },
	{ 0x18, 0x1ab3 },
	{ 0x19, 0x3e28 },
	{ 0x1a, 0x6c00 },
	{ 0x1b, 0x83ee },
	{ 0x1c, 0x8b40 },
	{ 0x1d, 0xee80 },
	{ 0x1e, 0x8000 },
	{ 0x20, 0x8122 },
	{ 0x23, 0x0288 },
	{ 0x24, 0x4800 },
	{ 0x25, 0x0800 },
	{ 0x26, 0x4000 },
	{ 0x27, 0x0004 },
	{ 0x28, 0x2408 },
	{ 0x29, 0x6000 },
	{ 0x2a, 0x9664 },
	{ 0x2b, 0x90f0 },
	{ 0x2c, 0x0014 },
	{ 0x2d, 0x12fc },
	{ 0x2e, 0xc4bf },
	{ 0x2f, 0x0000 },
	{ 0x30, 0x0000 },
	{ 0x31, 0x000b },
	{ 0x77, 0x100c },
};

static const struct sia91xx_reg sia9187_power_on_ch0[] = {
	{ 0x09, 0xffff },
	{ 0x14, SIA9187_R14_FACTORY },
	{ 0x15, 0x0a0a },
	{ 0x16, 0x101b },
	{ 0x12, 0x9d60 },
	{ 0x13, 0x0384 },
	{ 0x17, 0x0418 },
};

static const struct sia91xx_reg sia9187_power_on_ch1[] = {
	{ 0x09, 0xffff },
	{ 0x14, SIA9187_R14_FACTORY },
	{ 0x15, 0x0b0b },
	{ 0x16, 0x101b },
	{ 0x12, 0x9d60 },
	{ 0x13, 0x0384 },
	{ 0x17, 0x0418 },
};

#define SIA9187_CHANNEL(n) {						\
	.defaults	= sia9187_defaults_##n,				\
	.num_defaults	= ARRAY_SIZE(sia9187_defaults_##n),		\
	.power_on	= sia9187_power_on_##n,				\
	.num_power_on	= ARRAY_SIZE(sia9187_power_on_##n),		\
}

/*
 * One entry per channel, in the order sipa.bin stores them, which is the order
 * the vendor's channel_num indexes. zorn wires channel 0 to 0x6c and channel 1
 * to 0x6d, and the only register that differs is 0x15 -- the I2S slot -- so
 * getting this backwards would swap the speakers rather than break them.
 */
static const struct sia91xx_setup sia9187_setup[] = {
	SIA9187_CHANNEL(ch0),
	SIA9187_CHANNEL(ch1),
};

/*
 * 8-bit register address, 16-bit big-endian value. The register space is sparse
 * -- reading 0x07 on a real SIA9187 NAKs -- so no cache: a regcache would have
 * to know the holes, and the vendor driver does not describe them beyond the
 * identity range.
 */
/*
 * Which registers answer at all, measured on the hardware rather than taken from
 * anywhere: everything else NAKs. The vendor's tables only ever touch registers
 * inside these runs, which is the corroboration.
 */
static const struct regmap_range sia91xx_readable_ranges[] = {
	regmap_reg_range(0x00, 0x06),
	regmap_reg_range(0x08, 0x0a),
	regmap_reg_range(0x11, 0x31),
	regmap_reg_range(0x50, 0x57),
	regmap_reg_range(0x6e, 0x77),
	regmap_reg_range(0xfe, 0xff),
};

static const struct regmap_access_table sia91xx_readable_table = {
	.yes_ranges = sia91xx_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(sia91xx_readable_ranges),
};

static const struct regmap_config sia91xx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
	.max_register = 0xff,
	.rd_table = &sia91xx_readable_table,
};

static int sia91xx_read_chipid(struct sia91xx *sia)
{
	unsigned int val;
	int ret;

	ret = regmap_read(sia->regmap, SIA91XX_REG_CHIPID, &val);
	if (ret)
		return dev_err_probe(sia->dev, ret, "cannot read the identity register\n");

	if (val < SIA9187_ID_FIRST || val > SIA9187_ID_LAST)
		return dev_err_probe(sia->dev, -ENODEV,
				     "identity %#x is outside the SIA9187 range %#x..%#x\n",
				     val, SIA9187_ID_FIRST, SIA9187_ID_LAST);

	sia->chipid = val;
	dev_info(sia->dev, "SIA9187, identity %#x\n", val);

	return 0;
}

static int sia91xx_write_seq(struct sia91xx *sia, const struct sia91xx_reg *seq,
			    unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = regmap_write(sia->regmap, seq[i].reg, seq[i].val);
		if (ret) {
			dev_err(sia->dev, "cannot write %#04x = %#06x: %d\n",
				seq[i].reg, seq[i].val, ret);
			return ret;
		}
	}

	return 0;
}

static int sia91xx_state(struct sia91xx *sia, unsigned int *state)
{
	unsigned int val;
	int ret;

	ret = regmap_read(sia->regmap, SIA91XX_REG_STATE, &val);
	if (ret) {
		dev_err(sia->dev, "cannot read the state register: %d\n", ret);
		return ret;
	}

	*state = FIELD_GET(SIA91XX_STATE, val);

	return 0;
}

/*
 * sipa_resume(): the defaults for the scene, a re-read of the identity register,
 * then the chip-on list. The vendor checks the state field first and gives up if
 * the part is not idle, so this does too -- writing the enable to a part that is
 * already running is not something the sequence was ever asked to do.
 */
/* One pass of the vendor's chip-on list, then wait for the part to latch. */
static int sia91xx_try_on(struct sia91xx *sia)
{
	unsigned int state, discard;
	int ret, i;

	ret = sia91xx_state(sia, &state);
	if (ret)
		return ret;

	/*
	 * The list starts by verifying the part is idle. If it is anywhere else
	 * -- including stuck at 1, which is where it lands when the enable was
	 * written before the I2S clock was running -- put it back first.
	 */
	if (state != SIA91XX_STATE_IDLE) {
		ret = regmap_write(sia->regmap, SIA91XX_REG_ENABLE,
				   SIA9187_DISABLE);
		if (ret)
			return ret;
		usleep_range(1000, 2000);
	}

	ret = sia91xx_write_seq(sia, sia->setup->defaults,
				sia->setup->num_defaults);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	/* The vendor's list reads 0x08 here and throws the value away. */
	ret = regmap_read(sia->regmap, 0x08, &discard);
	if (ret)
		return ret;

	ret = sia91xx_write_seq(sia, sia->setup->power_on,
				sia->setup->num_power_on);
	if (ret)
		return ret;

	for (i = 0; i < 20; i++) {
		usleep_range(1000, 2000);

		ret = sia91xx_state(sia, &state);
		if (ret)
			return ret;

		if (state == SIA91XX_STATE_ON)
			return 0;
	}

	dev_dbg(sia->dev, "did not latch on: state %#x\n", state);

	return -EAGAIN;
}

/*
 * sipa_resume(): the defaults for the scene, then the chip-on list.
 *
 * The part reaches its "on" state (0x01 low nibble 5) only if the I2S bit clock
 * is already running when the enable is written; without one it sits at 1 and
 * stays there, enabled but silent. Nothing in ASoC guarantees the ADSP has the
 * port up at DAPM time -- PipeWire holds the PCM open across suspends, so the
 * widget powers up once, long before the graph starts -- which is why this is
 * driven from the DAI's prepare and is idempotent and retried rather than done
 * once and trusted.
 */
static int sia91xx_power_on(struct sia91xx *sia)
{
	unsigned int state;
	int ret, attempt;

	ret = sia91xx_state(sia, &state);
	if (ret)
		return ret;

	if (state == SIA91XX_STATE_ON)
		return 0;

	if (sia->spksw && sia91xx_spksw >= 0)
		gpiod_set_value_cansleep(sia->spksw, !!sia91xx_spksw);

	for (attempt = 0; attempt < 3; attempt++) {
		ret = sia91xx_try_on(sia);
		if (ret != -EAGAIN)
			return ret;
	}

	dev_err(sia->dev, "did not come on after %d attempts\n", attempt);

	return -EIO;
}

/* sipa_suspend(): clear the enable bit, then the 1 ms the vendor list carries. */
static int sia91xx_power_off(struct sia91xx *sia)
{
	unsigned int state;
	int ret;

	ret = sia91xx_state(sia, &state);
	if (ret)
		return ret;

	if (state != SIA91XX_STATE_ON)
		dev_warn(sia->dev, "not on before power-off: state %#x\n", state);

	ret = regmap_write(sia->regmap, SIA91XX_REG_ENABLE, SIA9187_DISABLE);
	if (ret) {
		dev_err(sia->dev, "cannot clear the enable: %d\n", ret);
		return ret;
	}

	usleep_range(1000, 2000);

	return 0;
}

/*
 * For a DPCM backend, snd_soc_pcm_dai_prepare() walks the cpu DAI before the
 * codec DAIs, so by the time this runs q6apm has already started the graph and
 * the LPASS port is clocking. That is the earliest point at which the enable
 * write can actually latch, and ALSA calls it again on every resume from
 * suspend, which is exactly when a PipeWire sink comes back.
 */
static int sia91xx_dai_prepare(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct sia91xx *sia = dev_get_drvdata(dai->dev);

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	return sia91xx_power_on(sia);
}

static const struct snd_soc_dai_ops sia91xx_dai_ops = {
	.prepare = sia91xx_dai_prepare,
};

static int sia91xx_pa_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct sia91xx *sia = dev_get_drvdata(component->dev);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/*
		 * Try here too: on a plain aplay the clock is already up by now,
		 * so the part comes on before the first sample. If it does not,
		 * the DAI prepare above picks it up.
		 */
		sia91xx_power_on(sia);
		return 0;
	case SND_SOC_DAPM_PRE_PMD:
		return sia91xx_power_off(sia);
	}

	return 0;
}

/*
 * Playback and capture both carry the amplifier's own numbers -- the capture
 * side is its I/V sense, which is why the vendor exposes a full duplex DAI on a
 * part that only drives a speaker.
 */
static struct snd_soc_dai_driver sia91xx_dai = {
	.name = "sia91xx-aif",
	.id = 1,
	.ops = &sia91xx_dai_ops,
	.playback = {
		.stream_name = "AIF Playback",
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
		.rates = SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 |
			 SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
			 SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
			 SNDRV_PCM_RATE_64000,
		.channels_min = 1,
		.channels_max = 2,
	},
	.capture = {
		.stream_name = "AIF Capture",
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
		.rates = SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 |
			 SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_32000 |
			 SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
			 SNDRV_PCM_RATE_64000,
		.channels_min = 1,
		.channels_max = 2,
	},
};

static const struct snd_soc_dapm_widget sia91xx_widgets[] = {
	SND_SOC_DAPM_OUT_DRV_E("PA", SND_SOC_NOPM, 0, 0, NULL, 0,
			       sia91xx_pa_event,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_INPUT("IV"),
};

static const struct snd_soc_dapm_route sia91xx_routes[] = {
	{ "PA", NULL, "AIF Playback" },
	{ "OUT", NULL, "PA" },
	{ "AIF Capture", NULL, "IV" },
};

static const struct snd_soc_component_driver sia91xx_component = {
	.dapm_widgets		= sia91xx_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sia91xx_widgets),
	.dapm_routes		= sia91xx_routes,
	.num_dapm_routes	= ARRAY_SIZE(sia91xx_routes),
	.idle_bias_on		= 1,
	.endianness		= 1,
};

static int sia91xx_i2c_probe(struct i2c_client *i2c)
{
	unsigned int channel;
	struct sia91xx *sia;
	int ret;

	sia = devm_kzalloc(&i2c->dev, sizeof(*sia), GFP_KERNEL);
	if (!sia)
		return -ENOMEM;

	sia->dev = &i2c->dev;
	i2c_set_clientdata(i2c, sia);

	sia->regmap = devm_regmap_init_i2c(i2c, &sia91xx_regmap_config);
	if (IS_ERR(sia->regmap))
		return dev_err_probe(sia->dev, PTR_ERR(sia->regmap),
				     "cannot init regmap\n");

	/*
	 * Claimed but left deasserted. Both amplifiers answer on the bus in this
	 * state, and driving the line the vendor calls si,si_pa_reset high stops
	 * the one at 0x6d from answering at all -- so whatever that pin does on
	 * this board, asserting it is not the way to bring the part up. Getting
	 * it as OUT_LOW is enough to stop anything else claiming the pin.
	 */
	sia->reset = devm_gpiod_get_optional(sia->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sia->reset))
		return dev_err_probe(sia->dev, PTR_ERR(sia->reset),
				     "cannot get the reset gpio\n");

	/*
	 * The vendor's spksw-gpio, which only the left-hand part has. Its driver
	 * never touches it -- it is exposed as the sia91xx_spk_sw_gpio_switch
	 * kcontrol and the HAL sets it -- and on a running Android it is asserted
	 * while the right-hand part, which has no such pin, reads 0. So assert it
	 * once here and leave it, which is what Android's value amounts to.
	 */
	sia->spksw = devm_gpiod_get_optional(sia->dev, "spksw",
					    sia91xx_spksw > 0 ? GPIOD_OUT_HIGH
							      : GPIOD_OUT_LOW);
	if (IS_ERR(sia->spksw))
		return dev_err_probe(sia->dev, PTR_ERR(sia->spksw),
				     "cannot get the speaker-switch gpio\n");

	usleep_range(1000, 2000);

	ret = sia91xx_read_chipid(sia);
	if (ret)
		return ret;

	/*
	 * Which half of the I2S frame this part takes. The vendor carries it as
	 * si,channel_num in its own binding; on this board the two addresses are
	 * consecutive and in the same order as sipa.bin's blocks, so the address
	 * is enough and no new property has to be invented for it.
	 */
	channel = i2c->addr - SIA9187_FIRST_ADDR;
	if (channel >= ARRAY_SIZE(sia9187_setup))
		return dev_err_probe(sia->dev, -EINVAL,
				     "address %#x is not one of the two the sipa parameters describe\n",
				     i2c->addr);
	sia->setup = &sia9187_setup[channel];

	return devm_snd_soc_register_component(sia->dev, &sia91xx_component,
					       &sia91xx_dai, 1);
}

static const struct i2c_device_id sia91xx_i2c_id[] = {
	{ "sia91xx" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sia91xx_i2c_id);

static const struct of_device_id sia91xx_of_match[] = {
	{ .compatible = "si,sia91xx" },
	{ .compatible = "si,sia9187" },
	{ }
};
MODULE_DEVICE_TABLE(of, sia91xx_of_match);

static struct i2c_driver sia91xx_i2c_driver = {
	.driver = {
		.name = "sia91xx",
		.of_match_table = sia91xx_of_match,
	},
	.probe = sia91xx_i2c_probe,
	.id_table = sia91xx_i2c_id,
};
module_i2c_driver(sia91xx_i2c_driver);

MODULE_DESCRIPTION("Silicon Integrated SIA91xx smart speaker amplifier");
MODULE_LICENSE("GPL");
