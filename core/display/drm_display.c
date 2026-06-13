#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_display.h"

#define DRM_DEVICE "sunxi-drm"

/* Aligne sur une frontière puissance de 2 — certains GPU ARM exigent des dimensions alignées sur 16 px. */
#define ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))

/* ─────────────────────────────────────────────────────────────
 * Libère toutes les ressources DRM dans l'ordre inverse d'alloc.
 * ───────────────────────────────────────────────────────────── */
static void drm_display_cleanup(struct drm_display *d) {
  if (!d || d->fd < 0)
    return;

  if (d->map && d->map != MAP_FAILED)
    munmap(d->map, d->buf_size);

  if (d->fb_id)
    drmModeRmFB(d->fd, d->fb_id);

  if (d->buf_handle) {
    struct drm_mode_destroy_dumb dreq = {.handle = d->buf_handle};
    ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  }

  drmDropMaster(d->fd);
  close(d->fd);
  d->fd = -1;
}

/* ─────────────────────────────────────────────────────────────
 * Remonte la chaîne Connecteur → Encodeur → CRTC pour trouver
 * le premier connecteur branché et son CRTC associé.
 * ───────────────────────────────────────────────────────────── */
static int find_connector_and_mode(int fd, drmModeRes *res, uint32_t *out_conn_id, uint32_t *out_crtc_id, drmModeModeInfo *out_mode) {
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
    if (!conn)
      continue;

    if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
      drmModeFreeConnector(conn);
      continue;
    }

    *out_conn_id = conn->connector_id;
    *out_mode = conn->modes[0];

    if (conn->encoder_id) {
      drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
      if (enc && enc->crtc_id) {
        *out_crtc_id = enc->crtc_id;
        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        return 0;
      }
      if (enc)
        drmModeFreeEncoder(enc);
    }

    for (int j = 0; j < conn->count_encoders; j++) {
      drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[j]);
      if (!enc)
        continue;
      for (int k = 0; k < res->count_crtcs; k++) {
        if (enc->possible_crtcs & (1 << k)) {
          *out_crtc_id = res->crtcs[k];
          drmModeFreeEncoder(enc);
          drmModeFreeConnector(conn);
          return 0;
        }
      }
      drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(conn);
  }
  return -1;
}

/* ─────────────────────────────────────────────────────────────
 * Initialise le pipeline DRM : ouvre le nœud /dev/dri/cardN,
 * alloue un Dumb Buffer en VRAM, l'enregistre comme Framebuffer
 * et le mappe en espace utilisateur.
 * ───────────────────────────────────────────────────────────── */
int drm_display_init(struct drm_display *d, uint32_t *out_w, uint32_t *out_h, uint32_t *out_stride) {
  if (!d)
    return -1;
  memset(d, 0, sizeof(*d));
  d->fd = -1;

  const char *dev_names[] = {DRM_DEVICE, "/dev/dri/card0", "/dev/dri/card1"};
  drmModeRes *res = NULL;
  int found = 0;

  for (int i = 0; i < 3 && !found; i++) {
    if (d->fd >= 0) {
      if (res)
        drmModeFreeResources(res);
      drmDropMaster(d->fd);
      close(d->fd);
      d->fd = -1;
    }

    d->fd = (i == 0) ? drmOpen(dev_names[i], NULL) : open(dev_names[i], O_RDWR);
    if (d->fd < 0)
      continue;

    if (drmSetMaster(d->fd) < 0) {
      close(d->fd);
      d->fd = -1;
      continue;
    }

    res = drmModeGetResources(d->fd);
    if (!res) {
      close(d->fd);
      d->fd = -1;
      continue;
    }

    if (find_connector_and_mode(d->fd, res, &d->conn_id, &d->crtc_id, &d->mode) < 0)
      continue;

    found = 1;
  }

  if (!found) {
    if (res)
      drmModeFreeResources(res);
    fprintf(stderr, "drm_display_init: aucun device DRM utilisable.\n");
    return -1;
  }

  uint32_t w = d->mode.hdisplay;
  uint32_t h = d->mode.vdisplay;

  struct drm_mode_create_dumb creq;
  memset(&creq, 0, sizeof(creq));
  creq.width = w;
  creq.height = h;
  creq.bpp = 32;

  if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
    creq.width = ALIGN(w, 16);
    creq.height = ALIGN(h, 16);
    creq.bpp = 32;
    if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
      fprintf(stderr, "DRM_IOCTL_MODE_CREATE_DUMB: %s\n", strerror(errno));
      drmModeFreeResources(res);
      drm_display_cleanup(d);
      return -1;
    }
  }

  d->buf_handle = creq.handle;
  d->buf_size = creq.size;
  d->width = w;
  d->height = h;
  d->pitch = creq.pitch;
  d->format = DRM_FORMAT_XRGB8888;

  uint32_t handles[4] = {creq.handle, 0, 0, 0};
  uint32_t strides[4] = {creq.pitch, 0, 0, 0};
  uint32_t offsets[4] = {0};

  if (drmModeAddFB2(d->fd, creq.width, creq.height, d->format, handles, strides, offsets, &d->fb_id, 0) < 0) {
    perror("drmModeAddFB2");
    drmModeFreeResources(res);
    drm_display_cleanup(d);
    return -1;
  }

  struct drm_mode_map_dumb mreq = {.handle = creq.handle};
  if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
    perror("DRM_IOCTL_MODE_MAP_DUMB");
    drmModeFreeResources(res);
    drm_display_cleanup(d);
    return -1;
  }

  d->map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, mreq.offset);
  if (d->map == MAP_FAILED) {
    perror("mmap");
    drmModeFreeResources(res);
    drm_display_cleanup(d);
    return -1;
  }

  if (out_w)     *out_w = w;
  if (out_h)     *out_h = h;
  if (out_stride) *out_stride = d->pitch / 4;

  drmModeFreeResources(res);
  return 0;
}

/* ─────────────────────────────────────────────────────────────
 * Copie ligne par ligne un buffer ARGB8888 vers le Dumb Buffer.
 * La barrière mémoire garantit la visibilité par le GPU/CRTC.
 * ───────────────────────────────────────────────────────────── */
int drm_display_blit_argb32(struct drm_display *d, const uint32_t *pixels, uint32_t stride) {
  if (!d || d->fd < 0 || !d->map || !pixels)
    return -1;

  uint32_t w = d->width;
  uint32_t h = d->height;

  for (uint32_t y = 0; y < h; y++) {
    uint32_t *dst = (uint32_t *)((uint8_t *)d->map + y * d->pitch);
    const uint32_t *src = pixels + y * stride;
    memcpy(dst, src, w * sizeof(uint32_t));
  }
  __asm__ __volatile__("dmb ishst" ::: "memory");
  return 0;
}

/* ─────────────────────────────────────────────────────────────
 * Programme le CRTC pour scanner le framebuffer vers le connecteur.
 * ───────────────────────────────────────────────────────────── */
int drm_display_present(struct drm_display *d) {
  if (!d || d->fd < 0)
    return -1;

  if (drmModeSetCrtc(d->fd, d->crtc_id, d->fb_id, 0, 0, &d->conn_id, 1, &d->mode) < 0) {
    perror("drmModeSetCrtc");
    return -1;
  }
  return 0;
}

/* ─────────────────────────────────────────────────────────────
 * Libère toutes les ressources DRM/mémoire.
 * ───────────────────────────────────────────────────────────── */
void drm_display_shutdown(struct drm_display *d) {
  if (!d)
    return;
  drm_display_cleanup(d);
}
