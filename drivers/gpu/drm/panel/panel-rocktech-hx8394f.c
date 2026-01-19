// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for panels based on Himax HX8394 controller, such as:
 *
 * - Rocktech RK055MHD091A0-CTG 5.5" MIPI-DSI panel
 *
 * Copyright (C) 2024 Calixto Systems pvt ltd
 *
 * Based on drivers/gpu/drm/panel/panel-rocktech-hx8394f.c
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define DRV_NAME "panel-rocktech-hx8394f"

/* Manufacturer specific commands sent via DSI, listed in HX8394-F datasheet */
#define HX8394_CMD_SETSEQUENCE	  0xb0
#define HX8394_CMD_SETPOWER	  0xb1
#define HX8394_CMD_SETDISP	  0xb2
#define HX8394_CMD_SETCYC	  0xb4
#define HX8394_CMD_SETVCOM	  0xb6
#define HX8394_CMD_SETTE	  0xb7
#define HX8394_CMD_SETSENSOR	  0xb8
#define HX8394_CMD_SETEXTC	  0xb9
#define HX8394_CMD_SETMIPI	  0xba
#define HX8394_CMD_SETOTP	  0xbb
#define HX8394_CMD_SETREGBANK	  0xbd
#define HX8394_CMD_UNKNOWN1	  0xc0
#define HX8394_CMD_SETDGCLUT	  0xc1
#define HX8394_CMD_SETID	  0xc3
#define HX8394_CMD_SETDDB	  0xc4
#define HX8394_CMD_UNKNOWN2	  0xc6
#define HX8394_CMD_SETCABC	  0xc9
#define HX8394_CMD_SETCABCGAIN	  0xca
#define HX8394_CMD_SETPANEL	  0xcc
#define HX8394_CMD_SETOFFSET	  0xd2
#define HX8394_CMD_SETGIP0	  0xd3
#define HX8394_CMD_UNKNOWN3	  0xd4
#define HX8394_CMD_SETGIP1	  0xd5
#define HX8394_CMD_SETGIP2	  0xd6
#define HX8394_CMD_SETGPO	  0xd6
#define HX8394_CMD_SETSCALING	  0xdd
#define HX8394_CMD_SETIDLE	  0xdf
#define HX8394_CMD_SETGAMMA	  0xe0
#define HX8394_CMD_SETCHEMODE_DYN 0xe4
#define HX8394_CMD_SETCHE	  0xe5
#define HX8394_CMD_SETCESEL	  0xe6
#define HX8394_CMD_SET_SP_CMD	  0xe9
#define HX8394_CMD_SETREADINDEX	  0xfe
#define HX8394_CMD_GETSPIREAD	  0xff

/* User Define command set */
#define UD_SETADDRESSMODE	0x36 /* Set address mode */
#define UD_SETSEQUENCE		0xB0 /* Set sequence */
#define UD_SETPOWER		0xB1 /* Set power */
#define UD_SETDISP		0xB2 /* Set display related register */
#define UD_SETCYC		0xB4 /* Set display waveform cycles */
#define UD_SETVCOM		0xB6 /* Set VCOM voltage */
#define UD_SETTE		0xB7 /* Set internal TE function */
#define UD_SETSENSOR		0xB8 /* Set temperature sensor */
#define UD_SETEXTC		0xB9 /* Set extension command */
#define UD_SETMIPI		0xBA /* Set MIPI control */
#define UD_SETOTP		0xBB /* Set OTP */
#define UD_SETREGBANK		0xBD /* Set register bank */
#define UD_SETDGCLUT		0xC1 /* Set DGC LUT */
#define UD_SETID		0xC3 /* Set ID */
#define UD_SETDDB		0xC4 /* Set DDB */
#define UD_SETCABC		0xC9 /* Set CABC control */
#define UD_SETCABCGAIN		0xCA
#define UD_SETPANEL		0xCC
#define UD_SETOFFSET		0xD2
#define UD_SETGIP0		0xD3 /* Set GIP Option0 */
#define UD_SETGIP1		0xD5 /* Set GIP Option1 */
#define UD_SETGIP2		0xD6 /* Set GIP Option2 */
#define UD_SETGPO		0xD9
#define UD_SETSCALING		0xDD
#define UD_SETIDLE		0xDF
#define UD_SETGAMMA		0xE0 /* Set gamma curve related setting */
#define UD_SETCHEMODE_DYN	0xE4
#define UD_SETCHE		0xE5
#define UD_SETCESEL		0xE6 /* Enable color enhance */
#define UD_SET_SP_CMD		0xE9
#define UD_SETREADINDEX		0xFE /* Set SPI Read Index */
#define UD_GETSPIREAD		0xFF /* SPI Read Command Data */

struct hx8394 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *vcc;
	struct regulator *iovcc;
	bool prepared;
        bool enabled;
};

static const struct drm_display_mode default_mode = {
	.hdisplay    = 720,
	.hsync_start = 720 + 40,
	.hsync_end   = 720 + 40 + 46,
	.htotal	     = 720 + 40 + 46 + 40,
	.vdisplay    = 1280,
	.vsync_start = 1280 + 9,
	.vsync_end   = 1280 + 9 + 7,
	.vtotal	     = 1280 + 9 + 7 + 7,
	.clock	     = 52582,
	.flags 	     = 0,
	.width_mm    = 68,
	.height_mm   = 136,
};

static inline struct hx8394 *panel_to_hx8394(struct drm_panel *panel)
{
	return container_of(panel, struct hx8394, panel);
}

static void hx8394_dcs_write_buf(struct hx8394 *ctx, const void *data,
				  size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int err;

	err = mipi_dsi_dcs_write_buffer(dsi, data, len);
	if (err < 0)
		dev_err_ratelimited(ctx->dev, "MIPI DSI DCS write buffer failed: %d\n", err);
}

static void hx8394_dcs_write_cmd(struct hx8394 *ctx, u8 cmd, u8 value)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int err;

	err = mipi_dsi_dcs_write(dsi, cmd, &value, 1);
	if (err < 0)
		dev_err_ratelimited(ctx->dev, "MIPI DSI DCS write failed: %d\n", err);
}

#define dcs_write_seq(ctx, seq...)				\
({								\
	static const u8 d[] = { seq };				\
								\
	hx8394_dcs_write_buf(ctx, d, ARRAY_SIZE(d));		\
})

/*
 * This panel is not able to auto-increment all cmd addresses so for some of
 * them, we need to send them one by one...
 */
#define dcs_write_cmd_seq(ctx, cmd, seq...)			\
({								\
	static const u8 d[] = { seq };				\
	unsigned int i;						\
								\
	for (i = 0; i < ARRAY_SIZE(d) ; i++)			\
		hx8394_dcs_write_cmd(ctx, cmd + i, d[i]);	\
})


static void hx8394f_init_sequence(struct hx8394 *ctx)
{

	u8 mipi_data[] = {UD_SETMIPI, 0x60, 0x03, 0x68, 0x6B, 0xB2, 0xC0};

	dcs_write_seq(ctx, UD_SETADDRESSMODE, 0x02);
	dcs_write_seq(ctx, UD_SETEXTC, 0xFF, 0x83, 0x94);

	/* SETMIPI */
	mipi_data[1] = 0x60 | 3 ;
	hx8394_dcs_write_buf(ctx, mipi_data, ARRAY_SIZE(mipi_data));

	dcs_write_seq(ctx, UD_SETPOWER, 0x48, 0x12, 0x72, 0x09, 0x32, 0x54,
		      0x71, 0x71, 0x57, 0x47);

	dcs_write_seq(ctx, UD_SETDISP, 0x00, 0x80, 0x64, 0x15, 0x0E, 0x11);

	dcs_write_seq(ctx, UD_SETCYC, 0x73, 0x74, 0x73, 0x74, 0x73, 0x74, 0x01,
		      0x0C, 0x86, 0x75, 0x00, 0x3F, 0x73, 0x74, 0x73, 0x74,
		      0x73, 0x74, 0x01, 0x0C, 0x86);

	dcs_write_seq(ctx, UD_SETGIP0, 0x00, 0x00, 0x07, 0x07, 0x40, 0x07, 0x0C,
		      0x00, 0x08, 0x10, 0x08, 0x00, 0x08, 0x54, 0x15, 0x0A,
		      0x05, 0x0A, 0x02, 0x15, 0x06, 0x05, 0x06, 0x47, 0x44,
		      0x0A, 0x0A, 0x4B, 0x10, 0x07, 0x07, 0x0C, 0x40);

	dcs_write_seq(ctx, UD_SETGIP1, 0x1C, 0x1C, 0x1D, 0x1D, 0x00, 0x01, 0x02,
		      0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
		      0x24, 0x25, 0x18, 0x18, 0x26, 0x27, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x20, 0x21, 0x18, 0x18, 0x18,
		      0x18);

	dcs_write_seq(ctx, UD_SETGIP2, 0x1C, 0x1C, 0x1D, 0x1D, 0x07, 0x06, 0x05,
		      0x04, 0x03, 0x02, 0x01, 0x00, 0x0B, 0x0A, 0x09, 0x08,
		      0x21, 0x20, 0x18, 0x18, 0x27, 0x26, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		      0x18, 0x18, 0x18, 0x18, 0x25, 0x24, 0x18, 0x18, 0x18,
		      0x18);

	dcs_write_seq(ctx, UD_SETVCOM, 0x92, 0x92);

	dcs_write_seq(ctx, UD_SETGAMMA, 0x00, 0x0A, 0x15, 0x1B, 0x1E, 0x21,
		      0x24, 0x22, 0x47, 0x56, 0x65, 0x66, 0x6E, 0x82, 0x88,
		      0x8B, 0x9A, 0x9D, 0x98, 0xA8, 0xB9, 0x5D, 0x5C, 0x61,
		      0x66, 0x6A, 0x6F, 0x7F, 0x7F, 0x00, 0x0A, 0x15, 0x1B,
		      0x1E, 0x21, 0x24, 0x22, 0x47, 0x56, 0x65, 0x65, 0x6E,
		      0x81, 0x87, 0x8B, 0x98, 0x9D, 0x99, 0xA8, 0xBA, 0x5D,
		      0x5D, 0x62, 0x67, 0x6B, 0x72, 0x7F, 0x7F);
	dcs_write_seq(ctx, 0xC0, 0x1F, 0x31);
	dcs_write_seq(ctx, UD_SETPANEL, 0x03);
	dcs_write_seq(ctx, 0xD4, 0x02);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x02);
	dcs_write_seq(ctx, 0xD8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		      0xFF, 0xFF, 0xFF, 0xFF);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x00);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x01);
	dcs_write_seq(ctx, UD_SETPOWER, 0x00);
	dcs_write_seq(ctx, UD_SETREGBANK, 0x00);
	dcs_write_seq(ctx, 0xBF, 0x40, 0x81, 0x50, 0x00, 0x1A, 0xFC, 0x01);
	dcs_write_seq(ctx, 0xC6, 0xED);

	//return 0;
}

static int hx8394_disable(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);

	if (!ctx->enabled)
		return 0;

	ctx->enabled = false;

	return 0;
}

static int hx8394_unprepare(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret)
		dev_warn(panel->dev, "failed to set display off: %d\n", ret);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret)
		dev_warn(panel->dev, "failed to enter sleep mode: %d\n", ret);

	msleep(120);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
	}

	regulator_disable(ctx->vcc);

	ctx->prepared = false;

	return 0;
}

static int hx8394_prepare(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->vcc);
	if (ret < 0) {
		dev_err(ctx->dev, "failed to enable supply: %d\n", ret);
		return ret;
	}

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		msleep(20);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(100);
	}

	hx8394f_init_sequence(ctx);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret)
		return ret;

	msleep(125);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret)
		return ret;

	msleep(20);

	ctx->prepared = true;

	return 0;
}


static int hx8394_enable(struct drm_panel *panel)
{
	struct hx8394 *ctx = panel_to_hx8394(panel);

	if (ctx->enabled)
		return 0;

	ctx->enabled = true;

	return 0;
}

static int hx8394_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs hx8394_drm_funcs = {
	.disable = hx8394_disable,
	.unprepare = hx8394_unprepare,
	.prepare = hx8394_prepare,
	.enable = hx8394_enable,
	.get_modes = hx8394_get_modes,
};

static int hx8394_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hx8394 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		dev_err(dev, "cannot get reset GPIO: %d\n", ret);
		return ret;
	}

	ctx->vcc = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->vcc)) {
		ret = PTR_ERR(ctx->vcc);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "cannot get regulator: %d\n", ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &hx8394_drm_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static int hx8394_remove(struct mipi_dsi_device *dsi)
{
	struct hx8394 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id rocktech_hx8394_of_match[] = {
	{ .compatible = "rocktech,hx8394f" },
	{ }
};
MODULE_DEVICE_TABLE(of, rocktech_hx8394_of_match);

static struct mipi_dsi_driver rocktech_hx8394_driver = {
	.probe = hx8394_probe,
	.remove = hx8394_remove,
	.driver = {
		.name = "panel-rocktech-hx8394f",
		.of_match_table = rocktech_hx8394_of_match,
	},
};
module_mipi_dsi_driver(rocktech_hx8394_driver);

MODULE_AUTHOR("Vipin Vijayan <vipin.v@calixto.co.in>");
MODULE_DESCRIPTION("DRM driver for Himax HX8394 based MIPI DSI panels");
MODULE_LICENSE("GPL v2");
