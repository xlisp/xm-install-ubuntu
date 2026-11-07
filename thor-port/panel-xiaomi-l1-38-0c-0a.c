// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi 12S Ultra (thor) "38 0c 0a" 1440x3200 LTPO AMOLED panel
 *
 * DSI command mode, 4 lanes, DSC 1.1 (10 bpc, 8 bpp, 2 slices of 720x50).
 * The init sequences (60 Hz and 120 Hz) were generated from the vendor DT
 * (/sys/firmware/fdt of a 2203121C, panel node
 * qcom,mdss_dsi_l1_38_0c_0a_dsc_cmd selected via msm_drm.dsi_display0 in the
 * kernel command line) with the same packet format as
 * linux-mdss-dsi-panel-driver-generator. Supplies, reset timing
 * (qcom,mdss-dsi-reset-sequence <0 1 1 10>) and the DCS backlight
 * (bl_ctrl_dcs, 16-bit big-endian DBV, levels 4..2047,
 * qcom,mdss-dsi-bl-inverted-dbv) follow the vendor data.
 *
 * Local Thor bring-up patch, not submitted upstream.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

struct xiaomi_l1 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vddio;
	struct regulator *vci;
	struct gpio_desc *reset_gpio;
	struct drm_connector *connector;
	struct drm_dsc_config dsc;
};

static inline struct xiaomi_l1 *to_xiaomi_l1(struct drm_panel *panel)
{
	return container_of(panel, struct xiaomi_l1, panel);
}

/*
 * Command mode: the porches only stand for transfer overhead. With the
 * vendor porches (hsync 16, hbp 32, hfp 32; vsync 8, vbp 24, vfp 24) the
 * msm host derives a ~544 Mbit/s link at 60 Hz and ~1.09 Gbit/s at 120 Hz,
 * matching the vendor qcom,mdss-dsi-panel-clockrate (0x510ff400).
 */
#define XIAOMI_L1_HDISPLAY	1440
#define XIAOMI_L1_VDISPLAY	3200
#define XIAOMI_L1_HTOTAL	(1440 + 16 + 32 + 32)
#define XIAOMI_L1_VTOTAL	(3200 + 8 + 24 + 24)

#define XIAOMI_L1_MODE(fps, preferred) {					\
	.clock = XIAOMI_L1_HTOTAL * XIAOMI_L1_VTOTAL * (fps) / 1000,		\
	.hdisplay = 1440,							\
	.hsync_start = 1440 + 32,						\
	.hsync_end = 1440 + 32 + 16,						\
	.htotal = XIAOMI_L1_HTOTAL,						\
	.vdisplay = 3200,							\
	.vsync_start = 3200 + 24,						\
	.vsync_end = 3200 + 24 + 8,						\
	.vtotal = XIAOMI_L1_VTOTAL,						\
	.width_mm = 70,								\
	.height_mm = 155,							\
	.type = DRM_MODE_TYPE_DRIVER | (preferred),				\
}

enum { XIAOMI_L1_60HZ, XIAOMI_L1_120HZ };

static const struct drm_display_mode xiaomi_l1_modes[] = {
	XIAOMI_L1_MODE(60, DRM_MODE_TYPE_PREFERRED),
	XIAOMI_L1_MODE(120, 0),
};

static int xiaomi_l1_power_on(struct xiaomi_l1 *ctx)
{
	int ret;

	ret = regulator_enable(ctx->vddio);
	if (ret)
		return ret;
	usleep_range(10000, 11000);

	ret = regulator_enable(ctx->vci);
	if (ret)
		goto err_vddio;
	usleep_range(5000, 6000);

	return 0;

err_vddio:
	regulator_disable(ctx->vddio);
	return ret;
}

static void xiaomi_l1_power_off(struct xiaomi_l1 *ctx)
{
	regulator_disable(ctx->vci);
	usleep_range(10000, 11000);
	regulator_disable(ctx->vddio);
	usleep_range(10000, 11000);
}

/* Vendor qcom,mdss-dsi-reset-sequence = <0 1 1 10>: low 1 ms, then high */
static void xiaomi_l1_reset(struct xiaomi_l1 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);	/* physical low */
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);	/* physical high */
	usleep_range(10000, 11000);
}

/*
 * Vendor timing@wqhd_60hz_index_00 qcom,mdss-dsi-on-command. The first
 * (0x9e) write is the DDIC-side DSC configuration.
 */
static int xiaomi_l1_on(struct xiaomi_l1 *ctx, int timing)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9e,
		0x11, 0x00, 0x00, 0xab, 0x30, 0x80, 0x0c, 0x80, 0x05, 0xa0,
		0x00, 0x19, 0x02, 0xd0, 0x02, 0xd0, 0x02, 0x00, 0x02, 0x68,
		0x00, 0x20, 0x02, 0xbe, 0x00, 0x0a, 0x00, 0x0c, 0x04, 0x00,
		0x03, 0x0d, 0x18, 0x00, 0x10, 0xf0, 0x07, 0x10, 0x20, 0x00,
		0x06, 0x0f, 0x0f, 0x33, 0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54,
		0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02,
		0x22, 0x00, 0x2a, 0x40, 0x2a, 0xbe, 0x3a, 0xfc, 0x3a, 0xfa,
		0x3a, 0xf8, 0x3b, 0x38, 0x3b, 0x78, 0x3b, 0xb6, 0x4b, 0xf6,
		0x4c, 0x34, 0x4c, 0x74, 0x5c, 0x74, 0x8c, 0xf4, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9d, 0x01);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x00, 0x00, 0x05, 0x9f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x00, 0x00, 0x0c, 0x7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);

	if (timing == XIAOMI_L1_120HZ) {
		/* Vendor timing@wqhd_120hz_index_01: fast PLL + LP tables */
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x41);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x06, 0xb9);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9,
			0x0c, 0x9e, 0x0c, 0x9e, 0x00, 0x1d);
	} else {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x00);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x06, 0xb9);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	}
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x08, 0xcb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x27);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x21, 0x82);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x10, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x21, 0xbd);

	if (timing == XIAOMI_L1_120HZ) {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd,
			0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
			0x00, 0x07, 0x00, 0x0b, 0x00);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x61, 0xbd);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd,
			0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x1d);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x12, 0xbd);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0x00);
	} else {
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd,
			0x03, 0x00, 0x06, 0x00, 0x09, 0x00, 0x0c, 0x00, 0x0f,
			0x00, 0x15, 0x00, 0x21, 0x00);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x61, 0xbd);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd,
			0x04, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x10, 0x00, 0x74);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x12, 0xbd);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x21);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0x01);
	}
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x16, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x0d, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x0c, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x28);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x47, 0xf4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x47, 0x05, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x22, 0xb9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0xb1, 0xa1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x05, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x52);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x02, 0x67, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1d, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x52, 0x1f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x53, 0x1f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f,
		0x07, 0x01, 0x00, 0x02, 0xb9, 0x02, 0xba, 0x07, 0xa2, 0x07,
		0xaa, 0x12, 0xc0, 0x12, 0xc1, 0x1a, 0xba, 0x1b, 0xc0, 0x1f,
		0xe0, 0x00, 0x53, 0x00, 0xc3, 0x03, 0x43, 0x07, 0xa0, 0x07,
		0xa1, 0x07, 0xa2, 0x07, 0xa6, 0x07, 0xa8, 0x07, 0xaa, 0x07,
		0xac, 0x07, 0xae, 0x07, 0xb0, 0x07, 0xb2, 0x07, 0xb4, 0x07,
		0xb6, 0x00, 0x03, 0x00, 0xa4, 0x00, 0xc0, 0x00, 0xd1, 0x01,
		0x86, 0x03, 0xff, 0x07, 0xff, 0x30, 0x30, 0x35, 0x40, 0x35,
		0x30, 0x40, 0x40, 0x35, 0x80, 0x80, 0x80, 0x0a, 0x0f, 0x14,
		0x18, 0x1e, 0x3a, 0x78, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
		0x80, 0x80, 0x01, 0xb1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x78, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	if (timing == XIAOMI_L1_120HZ) {
		/* 120 Hz low-power oscillator trim (vendor tail of on-command) */
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x23, 0x02);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x14, 0xbd);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
		mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	}

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int xiaomi_l1_off(struct xiaomi_l1 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	return dsi_ctx.accum_err;
}

static int xiaomi_l1_current_timing(struct xiaomi_l1 *ctx)
{
	struct drm_connector *connector = ctx->connector;
	struct drm_crtc_state *crtc_state;

	if (!connector || !connector->state || !connector->state->crtc)
		return XIAOMI_L1_60HZ;

	crtc_state = connector->state->crtc->state;
	if (crtc_state->mode.vrefresh > 90)
		return XIAOMI_L1_120HZ;

	return XIAOMI_L1_60HZ;
}

static int xiaomi_l1_prepare(struct drm_panel *panel)
{
	struct xiaomi_l1 *ctx = to_xiaomi_l1(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = xiaomi_l1_power_on(ctx);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	xiaomi_l1_reset(ctx);

	ret = xiaomi_l1_on(ctx, xiaomi_l1_current_timing(ctx));
	if (ret < 0) {
		dev_err(dev, "failed to initialize panel: %d\n", ret);
		goto err_off;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(dev, "failed to transmit PPS: %d\n", ret);
		goto err_off;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		goto err_off;
	}

	msleep(28);

	return 0;

err_off:
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	xiaomi_l1_power_off(ctx);
	return ret;
}

static int xiaomi_l1_unprepare(struct drm_panel *panel)
{
	struct xiaomi_l1 *ctx = to_xiaomi_l1(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = xiaomi_l1_off(ctx);
	if (ret < 0)
		dev_err(dev, "failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	xiaomi_l1_power_off(ctx);

	return 0;
}

static int xiaomi_l1_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct xiaomi_l1 *ctx = to_xiaomi_l1(panel);
	struct drm_display_mode *mode;
	int i;

	ctx->connector = connector;

	for (i = 0; i < ARRAY_SIZE(xiaomi_l1_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &xiaomi_l1_modes[i]);
		if (!mode)
			return -ENOMEM;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = xiaomi_l1_modes[0].width_mm;
	connector->display_info.height_mm = xiaomi_l1_modes[0].height_mm;

	return ARRAY_SIZE(xiaomi_l1_modes);
}

static const struct drm_panel_funcs xiaomi_l1_panel_funcs = {
	.prepare = xiaomi_l1_prepare,
	.unprepare = xiaomi_l1_unprepare,
	.get_modes = xiaomi_l1_get_modes,
};

#define XIAOMI_L1_MAX_BRIGHTNESS	2047	/* vendor bl-max-level 0x7ff */

/* 16-bit DBV, big-endian (vendor qcom,mdss-dsi-bl-inverted-dbv) */
static int xiaomi_l1_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops xiaomi_l1_bl_ops = {
	.update_status = xiaomi_l1_bl_update_status,
};

static struct backlight_device *
xiaomi_l1_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = XIAOMI_L1_MAX_BRIGHTNESS / 2,
		.max_brightness = XIAOMI_L1_MAX_BRIGHTNESS,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &xiaomi_l1_bl_ops, &props);
}

/*
 * The DDIC decodes with fixed rate-control parameters (the vendor 0x9e
 * configuration embeds initial_xmit_delay 512 and initial_offset 6144,
 * matching the Novatek-class tables used by the other SM8475 Xiaomi
 * panels). The host encoder has to use them as well.
 */
static void xiaomi_l1_set_rc_params(struct drm_dsc_config *dsc)
{
	static const u8 min_qp[DSC_NUM_BUF_RANGES] = {
		0, 4, 5, 5, 7, 7, 7, 7, 7, 8, 9, 9, 9, 12, 16
	};
	static const u8 max_qp[DSC_NUM_BUF_RANGES] = {
		8, 8, 9, 10, 11, 11, 11, 12, 13, 14, 14, 15, 15, 16, 17
	};
	static const s8 bpg_offset[DSC_NUM_BUF_RANGES] = {
		2, 0, 0, -2, -4, -6, -8, -8, -8, -10, -10, -10, -12, -12, -12
	};
	int i;

	for (i = 0; i < DSC_NUM_BUF_RANGES; i++) {
		dsc->rc_range_params[i].range_min_qp = min_qp[i];
		dsc->rc_range_params[i].range_max_qp = max_qp[i];
		/* 6-bit two's complement, as in the PPS */
		dsc->rc_range_params[i].range_bpg_offset =
			bpg_offset[i] & DSC_RANGE_BPG_OFFSET_MASK;
	}
	dsc->first_line_bpg_offset = 13;
	dsc->initial_offset = 6144;
	dsc->initial_xmit_delay = 512;
	dsc->flatness_min_qp = 7;
	dsc->flatness_max_qp = 16;
	dsc->rc_quant_incr_limit0 = 15;
	dsc->rc_quant_incr_limit1 = 15;
}

static int xiaomi_l1_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct xiaomi_l1 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct xiaomi_l1, panel,
				   &xiaomi_l1_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio),
				     "failed to get vddio regulator\n");

	ctx->vci = devm_regulator_get(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci),
				     "failed to get vci regulator\n");

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	ctx->dsi = dsi;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = xiaomi_l1_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "failed to create backlight\n");

	ret = devm_drm_panel_add(dev, &ctx->panel);
	if (ret)
		return ret;

	/* This panel only supports DSC */
	dsi->dsc = &ctx->dsc;
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 50;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 1440 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;
	xiaomi_l1_set_rc_params(&ctx->dsc);

	return devm_mipi_dsi_attach(dev, dsi);
}

static const struct of_device_id xiaomi_l1_of_match[] = {
	{ .compatible = "xiaomi,l1-38-0c-0a" },
	{ }
};
MODULE_DEVICE_TABLE(of, xiaomi_l1_of_match);

static struct mipi_dsi_driver xiaomi_l1_driver = {
	.probe = xiaomi_l1_probe,
	.driver = {
		.name = "panel-xiaomi-l1-38-0c-0a",
		.of_match_table = xiaomi_l1_of_match,
	},
};
module_mipi_dsi_driver(xiaomi_l1_driver);

MODULE_DESCRIPTION("DRM driver for the Xiaomi 12S Ultra 38 0c 0a DSC command mode panel");
MODULE_LICENSE("GPL");
