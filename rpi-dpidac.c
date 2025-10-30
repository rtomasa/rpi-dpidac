/*
 * Copyright (C) 2018 Hugh Cole-Baker
 *
 * Hugh Cole-Baker <sigmaris@gmail.com>
 * cpasjuste
 * Ruben Tomas Alonso (RTA) <ruben.tomas.alonso@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/media-bus-format.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_edid.h>

#define DRV_NAME "rpi-dpidac"

/* --- module params ---
 * e.g.: modprobe rpi-dpidac force_mode
 */
static char force_mode[128]; /* accepts the same format as kernel cmdline "video=" e.g.: "1024x768@60 -hsync -vsync */
module_param_string(force_mode, force_mode, sizeof(force_mode), 0644);
MODULE_PARM_DESC(force_mode, "video=-style mode string for DPI connector");

static char pref_mode[32]; /* accepts: 1080p60,1080p50,720p60,720p50 */
module_param_string(pref_mode, pref_mode, sizeof(pref_mode), 0644);
MODULE_PARM_DESC(pref_mode, "Preferred mode string. Accepts WxH@R (e.g. 1024x768@60), WxH, or CEA like 1080p50/720p60");

static char busfmt_param[32]; /* accepts names below */
module_param_string(busfmt, busfmt_param, sizeof(busfmt_param), 0644);
MODULE_PARM_DESC(busfmt, "RGB bus format: rgb565,rgb565-padhi,bgr666,bgr666-padhi,rgb666-padhi,bgr888,rgb888");

struct dpidac
{
    struct drm_bridge bridge;
    struct drm_connector connector;
    struct edid *fake_edid; /* may include extensions; allocated as 128*N */
    bool pi5_interlace_fix;
    u32 bus_format;
    u32 bus_flags;
    char preferred_mode[16]; /* e.g. "1080p60" */
};

/* Pi5-only helper */
static inline bool dpidac_is_pi5(void)
{
#ifdef CONFIG_ARM64
    return of_machine_is_compatible("brcm,bcm2712");
#else
    return false;
#endif
}

/* map string to MEDIA_BUS_FMT_* */
static bool dpidac_parse_busfmt(const char *s, u32 *fmt_out)
{
    if (!s || !*s)
        return false;
    if (!strcmp(s, "rgb565"))
    {
        *fmt_out = MEDIA_BUS_FMT_RGB565_1X16;
        return true;
    }
    if (!strcmp(s, "rgb565-padhi"))
    {
        *fmt_out = MEDIA_BUS_FMT_RGB565_1X24_CPADHI;
        return true;
    }
    if (!strcmp(s, "bgr666"))
    {
        *fmt_out = MEDIA_BUS_FMT_BGR666_1X18;
        return true;
    }
    if (!strcmp(s, "bgr666-padhi"))
    {
        *fmt_out = MEDIA_BUS_FMT_BGR666_1X24_CPADHI;
        return true;
    }
    if (!strcmp(s, "rgb666-padhi"))
    {
        *fmt_out = MEDIA_BUS_FMT_RGB666_1X24_CPADHI;
        return true;
    }
    if (!strcmp(s, "bgr888"))
    {
        *fmt_out = MEDIA_BUS_FMT_BGR888_1X24;
        return true;
    }
    if (!strcmp(s, "rgb888"))
    {
        *fmt_out = MEDIA_BUS_FMT_RGB888_1X24;
        return true;
    }
    return false;
}

static inline struct dpidac *drm_bridge_to_dpidac(struct drm_bridge *bridge)
{
    return container_of(bridge, struct dpidac, bridge);
}

static inline struct dpidac *drm_connector_to_dpidac(struct drm_connector *connector)
{
    return container_of(connector, struct dpidac, connector);
}

static bool dpidac_parse_want(const char *s, int *wh, int *wv, int *wr)
{
    if (!s || !*s)
        return false;
    /* WxH@R */
    if (sscanf(s, "%dx%d@%d", wh, wv, wr) == 3)
        return true;
    /* WxH (no refresh) */
    if (sscanf(s, "%dx%d", wh, wv) == 2)
    {
        *wr = 0;
        return true;
    }
    /* 1080p60 / 720p50 */
    if (sscanf(s, "%dp%d", wh, wr) == 2)
    {
        switch (*wh)
        {
        case 480:
            *wv = 480;
            *wh = 720;
            break;
        case 576:
            *wv = 576;
            *wh = 720;
            break;
        case 720:
            *wv = 720;
            *wh = 1280;
            break;
        case 1080:
            *wv = 1080;
            *wh = 1920;
            break;
        default:
            return false;
        }
        return true;
    }
    return false;
}

static void dpidac_set_preferred(struct drm_connector *conn, const char *want)
{
    int wh = 0, wv = 0, wr = 0, tol = 1;
    struct drm_display_mode *m, *pref = NULL;

    if (!dpidac_parse_want(want, &wh, &wv, &wr))
        want = NULL;

    list_for_each_entry(m, &conn->probed_modes, head)
    {
        bool match = false;
        if (want)
        {
            if (m->hdisplay == wh && m->vdisplay == wv)
            {
                int vr = drm_mode_vrefresh(m);
                match = (wr == 0) || (abs(vr - wr) <= tol);
            }
        }
        if (match)
        {
            m->type |= DRM_MODE_TYPE_PREFERRED;
            pref = m;
        }
        else
        {
            m->type &= ~DRM_MODE_TYPE_PREFERRED;
        }
    }

    if (!pref && !list_empty(&conn->probed_modes))
    {
        pref = list_first_entry(&conn->probed_modes, struct drm_display_mode, head);
        pref->type |= DRM_MODE_TYPE_PREFERRED;
    }
}

static bool has_mode(struct drm_connector *c, int h, int v, int r)
{
    struct drm_display_mode *m;
    list_for_each_entry(m, &c->probed_modes, head)
    {
        int vr = drm_mode_vrefresh(m);
        if (m->hdisplay == h && m->vdisplay == v && abs(vr - r) <= 1)
            return true;
    }
    return false;
}

static void add_fixed_mode(struct drm_connector *c,
                           u32 pclk, int hact, int hfp, int hs, int hbp,
                           int vact, int vfp, int vs, int vbp, bool ph, bool pv)
{
    struct drm_display_mode *m = drm_mode_create(c->dev);
    if (!m)
        return;
    m->clock = pclk;
    m->hdisplay = hact;
    m->hsync_start = hact + hfp;
    m->hsync_end = m->hsync_start + hs;
    m->htotal = hact + hfp + hs + hbp;
    m->vdisplay = vact;
    m->vsync_start = vact + vfp;
    m->vsync_end = m->vsync_start + vs;
    m->vtotal = vact + vfp + vs + vbp;
    m->type = DRM_MODE_TYPE_DRIVER;
    m->flags = (ph ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
               (pv ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);
    drm_mode_set_name(m);
    drm_mode_probed_add(c, m);
}

static void dpidac_add_cea_defaults(struct drm_connector *c)
{
    struct
    {
        int h, v, r;
        u32 pclk;
        int hfp, hs, hbp;
        int vfp, vs, vbp;
    } x[] = {
        {1920, 1080, 60, 148500, 88, 44, 148, 4, 5, 36},  /* CEA 16 */
        {1920, 1080, 50, 148500, 528, 44, 148, 4, 5, 36}, /* CEA 31 */
        {1280, 720, 60, 74250, 110, 40, 220, 5, 5, 20},   /* CEA  4 */
        {1280, 720, 50, 74250, 440, 40, 220, 5, 5, 20},   /* CEA 19 */
    };
    for (int i = 0; i < 4; i++)
        if (!has_mode(c, x[i].h, x[i].v, x[i].r))
            add_fixed_mode(c, x[i].pclk, x[i].h, x[i].hfp, x[i].hs, x[i].hbp,
                           x[i].v, x[i].vfp, x[i].vs, x[i].vbp, true, true);
}

static int dpidac_get_modes(struct drm_connector *connector)
{
    struct dpidac *dpi = drm_connector_to_dpidac(connector);
    int n = 0;

    /* 1) Highest priority: explicit force */
    if (force_mode[0])
    {
        struct drm_cmdline_mode cmd = {0};
        struct drm_display_mode *m;

        if (drm_mode_parse_command_line_for_connector(force_mode, connector, &cmd))
        {
            m = drm_mode_create_from_cmdline_mode(connector->dev, &cmd);
            if (m)
            {
                m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
                drm_mode_set_name(m);
                drm_mode_probed_add(connector, m);
                DRM_INFO(DRV_NAME ": Forced mode: %s\n", m->name);
                return 1;
            }
        }
        DRM_WARN(DRV_NAME ": Bad force_mode string, falling back\n");
    }

    /* 2) Use our synthetic EDID if present */
    if (dpi->fake_edid)
        n = drm_add_edid_modes(connector, dpi->fake_edid);

    /* ensure 1080/720 exist even on VGA sinks */
    dpidac_add_cea_defaults(connector);

    /* 3) Optionally reflag a preferred mode by name */
    dpidac_set_preferred(connector, pref_mode[0] ? pref_mode : dpi->preferred_mode);

    DRM_INFO(DRV_NAME ": %d modes exposed\n", n);
    return n;
}

static const struct drm_connector_helper_funcs dpidac_con_helper_funcs = {
    .get_modes = dpidac_get_modes,
};

static enum drm_connector_status dpidac_connector_detect(struct drm_connector *connector, bool force)
{
    return connector_status_connected;
}

static const struct drm_connector_funcs dpidac_con_funcs = {
    .detect = dpidac_connector_detect,
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = drm_connector_cleanup,
    .reset = drm_atomic_helper_connector_reset,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
    .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* Encode 3-letter vendor into EDID mfg ID (big-endian 5-bit chars). */
static inline __be16 dpidac_mfg_be16(const char vend[3])
{
    u16 id = (((vend[0] - '@') & 0x1f) << 10) |
             (((vend[1] - '@') & 0x1f) << 5) |
             (((vend[2] - '@') & 0x1f) << 0);
    return cpu_to_be16(id);
}

static void dpidac_edid_set_checksum(struct edid *e)
{
    u8 *b = (u8 *)e;
    int i;
    u8 sum = 0;
    e->checksum = 0;
    for (i = 0; i < 127; i++)
        sum += b[i];
    e->checksum = (u8)(256 - sum);
}

/* write Established Timings to base EDID (bytes 0x23..0x25) */
static void dpidac_fill_established_timings(u8 *b)
{
    /* byte 0x23 */
    u8 b0 = 0;
    b0 |= 1 << 7; /* 720x400@70 */
    /* 640x480@60,75 */
    b0 |= 1 << 5; /* 640x480@60 */
    b0 |= 1 << 2; /* 640x480@75 */
    /* 800x600@60 */
    b0 |= 1 << 0;

    /* byte 0x24 */
    u8 b1 = 0;
    b1 |= 1 << 6; /* 800x600@75 */
    /* 1024x768@60/70/75 */
    b1 |= 1 << 3; /* 1024x768@60 */
    b1 |= 1 << 2; /* 1024x768@70 */
    b1 |= 1 << 1; /* 1024x768@75 */

    /* byte 0x25 manufacturer reserved (leave 0) */

    b[0x23] = b0;
    b[0x24] = b1;
    b[0x25] = 0x00;
}

/* Build a minimal base EDID + one CTA-861 extension carrying VICs for 1080p/720p at 50/60.
   Also sets Established Timings for the classic VESA modes. */
static struct edid *dpidac_build_edid(const char vend[3], u16 product_le,
                                      u32 serial_le, u8 week, u8 year_from_1990,
                                      const char *name, const char *pref_mode_str)
{
    const size_t sz = 256; /* base + one CTA ext */
    u8 *raw = kzalloc(sz, GFP_KERNEL);
    struct edid *e = (struct edid *)raw;
    u8 *d;
    int nlen;

    if (!raw)
        return NULL;

    /* --- base block --- */
    e->header[0] = 0x00;
    e->header[1] = 0xff;
    e->header[2] = 0xff;
    e->header[3] = 0xff;
    e->header[4] = 0xff;
    e->header[5] = 0xff;
    e->header[6] = 0xff;
    e->header[7] = 0x00;

    e->product_id.manufacturer_name = dpidac_mfg_be16(vend);
    e->product_id.product_code = cpu_to_le16(product_le);
    e->product_id.serial_number = cpu_to_le32(serial_le);
    e->product_id.week_of_manufacture = week;
    e->product_id.year_of_manufacture = year_from_1990;

    e->version = 1;
    e->revision = 4;

    /* Base EDID input byte: analog, 0.7/0.3/1.0 V, blank=black, separate H/V sync supported */
    e->input = 0x00;    /* analog VGA */
    e->input |= BIT(3); /* separate sync supported */
    e->width_cm = 0;
    e->height_cm = 0;
    e->gamma = 120; /* 2.2 */
    e->features = 0x00;

    /* Established timings */
    dpidac_fill_established_timings((u8 *)e);

    /* Standard Timings (8 entries) -> leave zero, we rely on established + CTA */

    /* Detailed Descriptor #1: Monitor name (0xFC) */
    d = (u8 *)&e->detailed_timings[0];
    memset(d, 0, 18);
    d[3] = 0xFC;
    d[4] = 0x00;
    memset(d + 5, 0x20, 13);
    nlen = min_t(int, 13, name ? (int)strlen(name) : 0);
    if (nlen)
        memcpy(d + 5, name, nlen);
    if (nlen < 13)
        d[5 + nlen] = 0x0A;

    /* Detailed Descriptor #2..#4: leave as display descriptors filled with zeros */

    /* one extension follows */
    e->extensions = 1;
    dpidac_edid_set_checksum(e);

    /* --- CTA-861 extension --- */
    u8 *cta = raw + 128;
    memset(cta, 0, 128);
    cta[0] = 0x02; /* CTA */
    cta[1] = 0x03; /* revision */
    /* Build Video Data Block with VICs: 16(1080p60),31(1080p50),4(720p60),19(720p50) */
    u8 svd[4] = {16, 31, 4, 19};
    /* mark preferred as native (bit7) */
    if (pref_mode_str && !strcmp(pref_mode_str, "1080p60"))
        svd[0] |= 0x80;
    else if (pref_mode_str && !strcmp(pref_mode_str, "1080p50"))
        svd[1] |= 0x80;
    else if (pref_mode_str && !strcmp(pref_mode_str, "720p60"))
        svd[2] |= 0x80;
    else if (pref_mode_str && !strcmp(pref_mode_str, "720p50"))
        svd[3] |= 0x80;
    else
        svd[0] |= 0x80; /* default native 1080p60 */

    /* data block collection starts at byte 4 */
    u8 *dbc = cta + 4;
    u8 vdb_len = sizeof(svd);
    dbc[0] = (2 << 5) | vdb_len; /* tag=2 video, length */
    memcpy(dbc + 1, svd, vdb_len);

    /* DTD offset just after data blocks */
    cta[2] = 4 + 1 + vdb_len;
    /* cta[3] header flags left zero */

    /* zero pad to checksum and set checksum */
    u8 sum = 0;
    for (int i = 0; i < 127; i++)
        sum += cta[i];
    cta[127] = (u8)(256 - sum);

    return e;
}

static int dpidac_attach(struct drm_bridge *bridge, enum drm_bridge_attach_flags flags)
{
    struct dpidac *dpi = drm_bridge_to_dpidac(bridge);
    u32 bus_format = MEDIA_BUS_FMT_RGB666_1X24_CPADHI;
    u32 bus_flags = 0;
    const char *pref_from_dt = NULL;
    int ret;

    if (!bridge->encoder)
    {
        DRM_ERROR(DRV_NAME ": Missing encoder\n");
        return -ENODEV;
    }

    /* Read optional DT properties */
    of_property_read_u32(bridge->of_node, "raspberrypi,bus-format", &bus_format);
    of_property_read_u32(bridge->of_node, "bus-flags", &bus_flags);
    of_property_read_string(bridge->of_node, "raspberrypi,preferred-mode", &pref_from_dt);

    /* Apply module param overrides if given */
    if (busfmt_param[0])
        dpidac_parse_busfmt(busfmt_param, &bus_format);
    if (pref_mode[0])
        strscpy(dpi->preferred_mode, pref_mode, sizeof(dpi->preferred_mode));
    else if (pref_from_dt)
        strscpy(dpi->preferred_mode, pref_from_dt, sizeof(dpi->preferred_mode));
    else
        strscpy(dpi->preferred_mode, "1080p60", sizeof(dpi->preferred_mode));

    dpi->bus_format = bus_format;
    dpi->bus_flags = bus_flags;

    drm_connector_helper_add(&dpi->connector, &dpidac_con_helper_funcs);
    ret = drm_connector_init(bridge->dev, &dpi->connector, &dpidac_con_funcs, DRM_MODE_CONNECTOR_VGA);
    if (ret)
        return ret;

    ret = drm_display_info_set_bus_formats(&dpi->connector.display_info, &bus_format, 1);
    if (ret)
        return ret;
    dpi->connector.display_info.bus_flags = bus_flags;

    dpi->connector.interlace_allowed = 1;
    dpi->connector.doublescan_allowed = 0;
    drm_connector_attach_encoder(&dpi->connector, bridge->encoder);

    /* Build EDID with established timings + CTA VDB for 1080p/720p 50/60, with preferred marked native.
       Base vs CTA roles and data block contents match standard practice for HDMI/VGA EDIDs. :contentReference[oaicite:0]{index=0} :contentReference[oaicite:1]{index=1} */
    dpi->fake_edid = dpidac_build_edid("RTA", 0x0000, 0x00000000, 1, 36, "RPI-DPI-VGA", dpi->preferred_mode);
    if (dpi->fake_edid)
        drm_connector_update_edid_property(&dpi->connector, dpi->fake_edid);

    dpi->pi5_interlace_fix = dpidac_is_pi5();
    if (dpi->pi5_interlace_fix)
    {
        DRM_INFO(DRV_NAME ": Pi5 detected. Expect RP1 PIO interlace fixer to be loaded.\n");
        /* Background: RP1 PIO generates half-line VSYNC from HS/DE for interlace. :contentReference[oaicite:2]{index=2} */
    }

    return 0;
}

static void dpidac_detach(struct drm_bridge *bridge)
{
    struct dpidac *dpi = drm_bridge_to_dpidac(bridge);

    drm_connector_cleanup(&dpi->connector);
    if (dpi->fake_edid)
    {
        drm_connector_update_edid_property(&dpi->connector, NULL);
        kfree(dpi->fake_edid);
        dpi->fake_edid = NULL;
    }
}

static const struct drm_bridge_funcs dpidac_bridge_funcs = {
    .attach = dpidac_attach,
    .detach = dpidac_detach,
};

static int dpidac_probe(struct platform_device *pdev)
{
    struct dpidac *dpi;

    dpi = devm_kzalloc(&pdev->dev, sizeof(*dpi), GFP_KERNEL);
    if (!dpi)
        return -ENOMEM;
    platform_set_drvdata(pdev, dpi);

    dpi->bridge.funcs = &dpidac_bridge_funcs;
    dpi->bridge.of_node = pdev->dev.of_node;

    drm_bridge_add(&dpi->bridge);

    printk(KERN_INFO "[RPI-DPIDAC]: module probed\n");

    return 0;
}

static void dpidac_remove(struct platform_device *pdev)
{
    struct dpidac *dpi = platform_get_drvdata(pdev);
    drm_bridge_remove(&dpi->bridge);

    if (dpi->fake_edid)
    {
        drm_connector_update_edid_property(&dpi->connector, NULL);
        kfree(dpi->fake_edid);
        dpi->fake_edid = NULL;
    }

    printk(KERN_INFO "[RPI-DPIDAC]: module removed\n");
}

static const struct of_device_id dpidac_match[] = {
    {.compatible = "raspberrypi,dpidac"},
    {},
};
MODULE_DEVICE_TABLE(of, dpidac_match);

static struct platform_driver dpidac_driver = {
    .probe = dpidac_probe,
    .remove = dpidac_remove,
    .driver = {
        .name = "rpi-dpidac",
        .of_match_table = dpidac_match,
    },
};

module_platform_driver(dpidac_driver);

MODULE_DESCRIPTION("Raspberry Pi DPI DAC Bridge driver");
MODULE_AUTHOR("Ruben Tomas Alonso (RTA)");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
