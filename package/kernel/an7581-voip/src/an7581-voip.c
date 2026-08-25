// SPDX-License-Identifier: GPL-2.0-only
/*
 * Diagnostic driver for the Airoha AN7581 telephony PCM/ISI block.
 *
 * This intentionally implements only the low-risk part of SLIC bring-up:
 * select ISI, pulse reset, and read the Si32280 identification registers.
 * It never initializes ProSLIC RAM, the DC/DC converter, linefeed, or ringing.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>

/* Network-processor SCU registers. */
#define AN7581_SCU_IFACE_SEL		0x0094
#define AN7581_SCU_IFACE_SEL_MASK	GENMASK(3, 0)
#define AN7581_SCU_IFACE_ISI		0xf
/* Chip-SCU clock-source register used by the vendor set_gpio_clocksrc(). */
#define AN7581_CHIP_SCU_PCM_CLK		0x01d0
#define AN7581_CHIP_SCU_PCM1_CLK_MASK	GENMASK(11, 10)

/* ISI wrapper 0, inside the telephony PCM register window. */
#define AN7581_ISI_TX			0x1004
#define AN7581_ISI_STATUS		0x1008
#define AN7581_ISI_RX			0x100c
#define AN7581_ISI_CTRL			0x1010
#define AN7581_ISI_PORT			0x1014

#define AN7581_ISI_RX_START		BIT(0)
#define AN7581_ISI_TX_DONE		BIT(1)
#define AN7581_ISI_RX_DONE		BIT(2)
#define AN7581_ISI_CTRL_VENDOR_INIT	0x19

#define AN7581_ISI_POLL_US		1
#define AN7581_ISI_TIMEOUT_US		10000
#define AN7581_SLIC_READY_RETRIES	200
#define AN7581_SLIC_READY_DELAY_MS	10

#define SI3228X_REG_ID			0
#define SI3228X_REG_RESET_STATUS		3
#define SI3228X_RESET_STATUS_MASK	GENMASK(4, 0)
#define SI3228X_RESET_STATUS_READY	0x1f
#define SI32280_REVISION			3
#define SI32280_PART_NUMBER		1

struct an7581_voip {
	struct device *dev;
	void __iomem *base;
	struct regmap *scu;
	struct regmap *chip_scu;
	struct reset_control *spi_reset;
	struct reset_control *isi_reset;
	u8 reset_status[2];
	u8 id[2];
	bool ready[2];
};

static int an7581_isi_write_byte(struct an7581_voip *voip, u8 byte)
{
	u32 status;
	int ret;

	writel(byte, voip->base + AN7581_ISI_TX);

	ret = readl_poll_timeout(voip->base + AN7581_ISI_STATUS, status,
				 status & AN7581_ISI_TX_DONE,
				 AN7581_ISI_POLL_US, AN7581_ISI_TIMEOUT_US);
	if (ret) {
		dev_err(voip->dev,
			"ISI TX timeout: byte=0x%02x status=0x%08x\n",
			byte, status);
		return ret;
	}

	/* The vendor driver acknowledges completion by writing the bit back. */
	writel(status | AN7581_ISI_TX_DONE,
	       voip->base + AN7581_ISI_STATUS);

	return 0;
}

static int an7581_isi_read_byte(struct an7581_voip *voip, u8 *byte)
{
	u32 status;
	int ret;

	status = readl(voip->base + AN7581_ISI_STATUS);
	writel(status | AN7581_ISI_RX_START,
	       voip->base + AN7581_ISI_STATUS);

	ret = readl_poll_timeout(voip->base + AN7581_ISI_STATUS, status,
				 status & AN7581_ISI_RX_DONE,
				 AN7581_ISI_POLL_US, AN7581_ISI_TIMEOUT_US);
	if (ret) {
		dev_err(voip->dev, "ISI RX timeout: status=0x%08x\n",
			status);
		return ret;
	}

	*byte = readl(voip->base + AN7581_ISI_RX) & 0xff;

	status = readl(voip->base + AN7581_ISI_STATUS);
	writel(status | AN7581_ISI_RX_DONE,
	       voip->base + AN7581_ISI_STATUS);

	return 0;
}

static void an7581_voip_log_hw_state(struct an7581_voip *voip)
{
	unsigned int iface = 0;
	unsigned int pcm_clk = 0;

	regmap_read(voip->scu, AN7581_SCU_IFACE_SEL, &iface);
	regmap_read(voip->chip_scu, AN7581_CHIP_SCU_PCM_CLK, &pcm_clk);

	dev_info(voip->dev,
		 "hardware state: scu[0x094]=0x%08x chip-scu[0x1d0]=0x%08x isi-ctrl=0x%08x isi-status=0x%08x\n",
		 iface, pcm_clk,
		 readl(voip->base + AN7581_ISI_CTRL),
		 readl(voip->base + AN7581_ISI_STATUS));
}

static int an7581_voip_pulse_reset(struct an7581_voip *voip,
				    struct reset_control *reset,
				    const char *name)
{
	int ret;

	ret = reset_control_assert(reset);
	if (ret) {
		dev_err(voip->dev, "failed to assert %s reset: %d\n",
			name, ret);
		return ret;
	}

	usleep_range(5000, 6000);

	ret = reset_control_deassert(reset);
	if (ret) {
		dev_err(voip->dev, "failed to deassert %s reset: %d\n",
			name, ret);
		return ret;
	}

	usleep_range(5000, 6000);

	return 0;
}

static u8 si32280_read_control_byte(unsigned int channel)
{
	/* SiLabs uses bit-reversed, five-bit channel addressing. */
	return 0x60 | (channel ? BIT(4) : 0);
}

static int an7581_si32280_read_reg(struct an7581_voip *voip,
				   unsigned int channel, u8 reg, u8 *value)
{
	int ret;

	if (channel > 1)
		return -EINVAL;

	ret = an7581_isi_write_byte(voip,
				    si32280_read_control_byte(channel));
	if (ret)
		return ret;

	ret = an7581_isi_write_byte(voip, reg);
	if (ret)
		return ret;

	return an7581_isi_read_byte(voip, value);
}

static int an7581_voip_hw_init(struct an7581_voip *voip)
{
	u32 val;
	int ret;

	/*
	 * PCM1 is selected by the pinctrl state. Match the common clock-source
	 * operation used by the vendor HIR E/F/10 tables without changing the
	 * unrelated PCM2 bit, which differs between silicon revisions.
	 */
	ret = regmap_update_bits(voip->chip_scu, AN7581_CHIP_SCU_PCM_CLK,
				 AN7581_CHIP_SCU_PCM1_CLK_MASK, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(voip->scu, AN7581_SCU_IFACE_SEL,
				 AN7581_SCU_IFACE_SEL_MASK,
				 AN7581_SCU_IFACE_ISI);
	if (ret)
		return ret;

	/* Match spi.ko: reset the SPI wrapper first, then the ISI block. */
	ret = an7581_voip_pulse_reset(voip, voip->spi_reset, "spiwp");
	if (ret)
		return ret;

	ret = an7581_voip_pulse_reset(voip, voip->isi_reset, "isi");
	if (ret)
		return ret;

	/* Configure wrapper 0 exactly as SPI_cfg() does on recent HIRs. */
	val = readl(voip->base + AN7581_ISI_CTRL);
	writel(val | AN7581_ISI_CTRL_VENDOR_INIT,
	       voip->base + AN7581_ISI_CTRL);
	writel(0, voip->base + AN7581_ISI_PORT);

	return 0;
}

static int an7581_voip_identify_channel(struct an7581_voip *voip,
					unsigned int channel)
{
	u8 status = 0;
	unsigned int i;
	int ret;

	for (i = 0; i < AN7581_SLIC_READY_RETRIES; i++) {
		ret = an7581_si32280_read_reg(voip, channel,
					     SI3228X_REG_RESET_STATUS, &status);
		if (ret)
			return ret;

		if ((status & SI3228X_RESET_STATUS_MASK) ==
		    SI3228X_RESET_STATUS_READY) {
			voip->ready[channel] = true;
			break;
		}

		msleep(AN7581_SLIC_READY_DELAY_MS);
	}

	voip->reset_status[channel] = status;

	ret = an7581_si32280_read_reg(voip, channel, SI3228X_REG_ID,
				      &voip->id[channel]);
	if (ret)
		return ret;

	return 0;
}

static bool an7581_voip_is_si32280(u8 id)
{
	return FIELD_GET(GENMASK(2, 0), id) == SI32280_REVISION &&
	       FIELD_GET(GENMASK(5, 3), id) == SI32280_PART_NUMBER;
}

static ssize_t identity_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct an7581_voip *voip = dev_get_drvdata(dev);

	return sysfs_emit(buf,
		"ch0 ready=%u reg3=0x%02x reg0=0x%02x revision=%lu part=%lu\n"
		"ch1 ready=%u reg3=0x%02x reg0=0x%02x revision=%lu part=%lu\n",
		voip->ready[0], voip->reset_status[0], voip->id[0],
		FIELD_GET(GENMASK(2, 0), voip->id[0]),
		FIELD_GET(GENMASK(5, 3), voip->id[0]),
		voip->ready[1], voip->reset_status[1], voip->id[1],
		FIELD_GET(GENMASK(2, 0), voip->id[1]),
		FIELD_GET(GENMASK(5, 3), voip->id[1]));
}
static DEVICE_ATTR_RO(identity);

static struct attribute *an7581_voip_attrs[] = {
	&dev_attr_identity.attr,
	NULL,
};

static const struct attribute_group an7581_voip_attr_group = {
	.attrs = an7581_voip_attrs,
};

static int an7581_voip_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct an7581_voip *voip;
	unsigned int channel;
	bool found = false;
	int ret;

	voip = devm_kzalloc(dev, sizeof(*voip), GFP_KERNEL);
	if (!voip)
		return -ENOMEM;

	voip->dev = dev;
	voip->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(voip->base))
		return PTR_ERR(voip->base);

	voip->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "airoha,scu");
	if (IS_ERR(voip->scu))
		return dev_err_probe(dev, PTR_ERR(voip->scu),
				     "failed to get NP-SCU syscon\n");

	voip->chip_scu = syscon_regmap_lookup_by_phandle(dev->of_node,
							 "airoha,chip-scu");
	if (IS_ERR(voip->chip_scu))
		return dev_err_probe(dev, PTR_ERR(voip->chip_scu),
				     "failed to get Chip-SCU syscon\n");

	voip->spi_reset = devm_reset_control_get_exclusive(dev, "spiwp");
	if (IS_ERR(voip->spi_reset))
		return dev_err_probe(dev, PTR_ERR(voip->spi_reset),
				     "failed to get SPI wrapper reset\n");

	voip->isi_reset = devm_reset_control_get_exclusive(dev, "isi");
	if (IS_ERR(voip->isi_reset))
		return dev_err_probe(dev, PTR_ERR(voip->isi_reset),
				     "failed to get ISI reset\n");

	platform_set_drvdata(pdev, voip);

	ret = an7581_voip_hw_init(voip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize ISI wrapper\n");
	an7581_voip_log_hw_state(voip);

	for (channel = 0; channel < 2; channel++) {
		ret = an7581_voip_identify_channel(voip, channel);
		if (ret) {
			dev_err(dev, "channel %u ISI transaction failed: %d\n",
				channel, ret);
			continue;
		}

		dev_info(dev,
			 "channel %u: ready=%u reg3=0x%02x reg0=0x%02x revision=%lu part=%lu%s\n",
			 channel, voip->ready[channel],
			 voip->reset_status[channel], voip->id[channel],
			 FIELD_GET(GENMASK(2, 0), voip->id[channel]),
			 FIELD_GET(GENMASK(5, 3), voip->id[channel]),
			 an7581_voip_is_si32280(voip->id[channel]) ?
			 " (Si32280)" : " (unexpected ID)");

		found |= an7581_voip_is_si32280(voip->id[channel]);
	}

	ret = devm_device_add_group(dev, &an7581_voip_attr_group);
	if (ret)
		return ret;

	if (!found)
		dev_warn(dev,
			 "Si32280 not identified; inspect the logged values before changing the driver\n");

	dev_info(dev,
		 "diagnostic mode only; DC/DC, linefeed and ringing remain untouched\n");

	return 0;
}

static const struct of_device_id an7581_voip_of_match[] = {
	{ .compatible = "airoha,an7581-voip" },
	{ }
};
MODULE_DEVICE_TABLE(of, an7581_voip_of_match);

static struct platform_driver an7581_voip_driver = {
	.probe = an7581_voip_probe,
	.driver = {
		.name = "an7581-voip",
		.of_match_table = an7581_voip_of_match,
	},
};
module_platform_driver(an7581_voip_driver);

MODULE_AUTHOR("OpenWrt community");
MODULE_DESCRIPTION("Airoha AN7581 VoIP/ISI diagnostic driver");
MODULE_LICENSE("GPL");
