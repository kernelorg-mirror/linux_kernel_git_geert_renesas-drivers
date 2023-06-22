// SPDX-License-Identifier: GPL-2.0+
/*
 * shmob_drm_plane.c  --  SH Mobile DRM Planes
 *
 * Copyright (C) 2012 Renesas Electronics Corporation
 *
 * Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane_helper.h>

#include "shmob_drm_drv.h"
#include "shmob_drm_kms.h"
#include "shmob_drm_plane.h"
#include "shmob_drm_regs.h"

struct shmob_drm_plane {
	struct drm_plane base;
	unsigned int index;
};

struct shmob_drm_plane_state {
	struct drm_plane_state base;

	const struct shmob_drm_format_info *format;
	u32 dma[2];
};

static inline struct shmob_drm_plane *to_shmob_plane(struct drm_plane *plane)
{
	return container_of(plane, struct shmob_drm_plane, base);
}

static inline struct shmob_drm_plane_state *to_shmob_plane_state(struct drm_plane_state *state)
{
	return container_of(state, struct shmob_drm_plane_state, base);
}

static void shmob_drm_plane_compute_base(struct shmob_drm_plane_state *sstate)
{
	struct drm_framebuffer *fb = sstate->base.fb;
	unsigned int x = sstate->base.src_x >> 16;
	unsigned int y = sstate->base.src_y >> 16;
	struct drm_gem_dma_object *gem;
	unsigned int bpp;

	bpp = shmob_drm_format_is_yuv(sstate->format) ? 8 : sstate->format->bpp;
	gem = drm_fb_dma_get_gem_obj(fb, 0);
	sstate->dma[0] = gem->dma_addr + fb->offsets[0]
		       + y * fb->pitches[0] + x * bpp / 8;

	if (shmob_drm_format_is_yuv(sstate->format)) {
		bpp = sstate->format->bpp - 8;
		gem = drm_fb_dma_get_gem_obj(fb, 1);
		sstate->dma[1] = gem->dma_addr + fb->offsets[1]
			       + y / (bpp == 4 ? 2 : 1) * fb->pitches[1]
			       + x * (bpp == 16 ? 2 : 1);
	}
}

static void shmob_drm_primary_plane_setup(struct shmob_drm_plane *splane,
					  struct drm_plane_state *state)
{
	struct shmob_drm_plane_state *sstate = to_shmob_plane_state(state);
	struct shmob_drm_device *sdev = to_shmob_device(splane->base.dev);
	struct drm_framebuffer *fb = state->fb;

	/* TODO: Handle YUV colorspaces. Hardcode REC709 for now. */
	lcdc_write(sdev, LDDFR, sstate->format->lddfr | LDDFR_CF1);
	lcdc_write(sdev, LDMLSR, fb->pitches[0]);

	/* Word and long word swap. */
	lcdc_write(sdev, LDDDSR, sstate->format->ldddsr);

	lcdc_write_mirror(sdev, LDSA1R, sstate->dma[0]);
	if (shmob_drm_format_is_yuv(sstate->format))
		lcdc_write_mirror(sdev, LDSA2R, sstate->dma[1]);

	lcdc_write(sdev, LDRCNTR, lcdc_read(sdev, LDRCNTR) ^ LDRCNTR_MRS);
}

static void shmob_drm_overlay_plane_setup(struct shmob_drm_plane *splane,
					  struct drm_plane_state *state)
{
	struct shmob_drm_plane_state *sstate = to_shmob_plane_state(state);
	struct shmob_drm_device *sdev = to_shmob_device(splane->base.dev);
	unsigned int ovl_idx = splane->index - 1;
	struct drm_framebuffer *fb = state->fb;
	u32 format;

	/* TODO: Support ROP3 mode */
	format = LDBBSIFR_EN | ((state->alpha >> 8) << LDBBSIFR_LAY_SHIFT) |
		 sstate->format->ldbbsifr;

#define plane_reg_dump(sdev, ovl_idx, reg) \
	dev_dbg(sdev->ddev.dev, "%s(%u): %s 0x%08x 0x%08x\n", __func__, \
		ovl_idx, #reg, \
		lcdc_read(sdev, reg(ovl_idx)), \
		lcdc_read(sdev, reg(ovl_idx) + LCDC_SIDE_B_OFFSET))

	plane_reg_dump(sdev, ovl_idx, LDBnBSIFR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSSZR);
	plane_reg_dump(sdev, ovl_idx, LDBnBLOCR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSMWR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSAYR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSACR);

	lcdc_write(sdev, LDBCR, LDBCR_UPC(ovl_idx));
	dev_dbg(sdev->ddev.dev, "%s(%u): %s 0x%08x\n", __func__, ovl_idx,
		"LDBCR", lcdc_read(sdev, LDBCR));

	lcdc_write(sdev, LDBnBSIFR(ovl_idx), format);

	lcdc_write(sdev, LDBnBSSZR(ovl_idx),
		   (state->crtc_h << LDBBSSZR_BVSS_SHIFT) |
		   (state->crtc_w << LDBBSSZR_BHSS_SHIFT));
	lcdc_write(sdev, LDBnBLOCR(ovl_idx),
		   (state->crtc_y << LDBBLOCR_CVLC_SHIFT) |
		   (state->crtc_x << LDBBLOCR_CHLC_SHIFT));
	lcdc_write(sdev, LDBnBSMWR(ovl_idx),
		   fb->pitches[0] << LDBBSMWR_BSMW_SHIFT);

	lcdc_write(sdev, LDBnBSAYR(ovl_idx), sstate->dma[0]);
	if (shmob_drm_format_is_yuv(sstate->format))
		lcdc_write(sdev, LDBnBSACR(ovl_idx), sstate->dma[1]);

	lcdc_write(sdev, LDBCR, LDBCR_UPF(ovl_idx) | LDBCR_UPD(ovl_idx));
	dev_dbg(sdev->ddev.dev, "%s(%u): %s 0x%08x\n", __func__, ovl_idx,
		"LDBCR", lcdc_read(sdev, LDBCR));

	plane_reg_dump(sdev, ovl_idx, LDBnBSIFR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSSZR);
	plane_reg_dump(sdev, ovl_idx, LDBnBLOCR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSMWR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSAYR);
	plane_reg_dump(sdev, ovl_idx, LDBnBSACR);
}

static int shmob_drm_plane_atomic_check(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct shmob_drm_plane_state *sstate = to_shmob_plane_state(new_plane_state);
	struct drm_crtc_state *crtc_state;
	bool is_primary = plane->type == DRM_PLANE_TYPE_PRIMARY;
	int ret;

	if (!new_plane_state->crtc) {
		/*
		 * The visible field is not reset by the DRM core but only
		 * updated by drm_atomic_helper_check_plane_state(), set it
		 * manually.
		 */
		new_plane_state->visible = false;
		sstate->format = NULL;
		return 0;
	}

	crtc_state = drm_atomic_get_crtc_state(state, new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  !is_primary, true);
	if (ret < 0)
		return ret;

	if (!new_plane_state->visible) {
		sstate->format = NULL;
		return 0;
	}

	sstate->format = shmob_drm_format_info(new_plane_state->fb->format->format);
	if (!sstate->format) {
		dev_dbg(plane->dev->dev,
			"plane_atomic_check: unsupported format %p4cc\n",
			&new_plane_state->fb->format->format);
		return -EINVAL;
	}

	shmob_drm_plane_compute_base(sstate);

	return 0;
}

static void shmob_drm_plane_atomic_update(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct shmob_drm_plane *splane = to_shmob_plane(plane);

	if (!new_plane_state->visible)
		return;

	if (plane->type == DRM_PLANE_TYPE_PRIMARY)
		shmob_drm_primary_plane_setup(splane, new_plane_state);
	else
		shmob_drm_overlay_plane_setup(splane, new_plane_state);
}

static void shmob_drm_plane_atomic_disable(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct shmob_drm_device *sdev = to_shmob_device(plane->dev);
	struct shmob_drm_plane *splane = to_shmob_plane(plane);
	unsigned int ovl_idx;

	if (!old_state->crtc)
		return;

	if (plane->type != DRM_PLANE_TYPE_OVERLAY)
		return;

	ovl_idx = splane->index - 1;
	lcdc_write(sdev, LDBCR, LDBCR_UPC(ovl_idx));
	lcdc_write(sdev, LDBnBSIFR(ovl_idx), 0);
	lcdc_write(sdev, LDBCR, LDBCR_UPF(ovl_idx) | LDBCR_UPD(ovl_idx));
}

static struct drm_plane_state *
shmob_drm_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct shmob_drm_plane_state *state;
	struct shmob_drm_plane_state *copy;

	if (WARN_ON(!plane->state))
		return NULL;

	state = to_shmob_plane_state(plane->state);
	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (copy == NULL)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->base);

	return &copy->base;
}

static void shmob_drm_plane_atomic_destroy_state(struct drm_plane *plane,
						 struct drm_plane_state *state)
{
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(to_shmob_plane_state(state));
}

static void shmob_drm_plane_reset(struct drm_plane *plane)
{
	struct shmob_drm_plane_state *state;

	if (plane->state) {
		shmob_drm_plane_atomic_destroy_state(plane, plane->state);
		plane->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return;

	__drm_atomic_helper_plane_reset(plane, &state->base);
}

static const struct drm_plane_helper_funcs shmob_drm_plane_helper_funcs = {
	.atomic_check = shmob_drm_plane_atomic_check,
	.atomic_update = shmob_drm_plane_atomic_update,
	.atomic_disable = shmob_drm_plane_atomic_disable,
};

static const struct drm_plane_funcs shmob_drm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = shmob_drm_plane_reset,
	.atomic_duplicate_state = shmob_drm_plane_atomic_duplicate_state,
	.atomic_destroy_state = shmob_drm_plane_atomic_destroy_state,
};

static const uint32_t formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV61,
	DRM_FORMAT_NV24,
	DRM_FORMAT_NV42,
};

struct drm_plane *shmob_drm_plane_create(struct shmob_drm_device *sdev,
					 unsigned int index)
{
	struct shmob_drm_plane *splane;
	enum drm_plane_type type;

	if (!index)
		type = DRM_PLANE_TYPE_PRIMARY;
	else
		type = DRM_PLANE_TYPE_OVERLAY;

	splane = drmm_universal_plane_alloc(&sdev->ddev,
					    struct shmob_drm_plane, base, 1,
					    &shmob_drm_plane_funcs, formats,
					    ARRAY_SIZE(formats),  NULL, type,
					    NULL);
	if (IS_ERR(splane))
		return ERR_CAST(splane);

	splane->index = index;

	drm_plane_helper_add(&splane->base, &shmob_drm_plane_helper_funcs);

	return &splane->base;
}
