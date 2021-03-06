/*
 * Copyright (C) 2002 Jindrich Makovicka <makovick@gmail.com>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* A very simple tv station logo remover */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <math.h>

#include "common/msg.h"
#include "common/cpudetect.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "vf.h"
#include "vf_lavfi.h"
#include "video/memcpy_pic.h"

#include "options/m_option.h"

//===========================================================================//

static struct vf_priv_s {
    int xoff, yoff, lw, lh, band, show;
    char *file;
    struct timed_rectangle {
        int ts, x, y, w, h, b;
    } *timed_rect;
    int n_timed_rect;
    int cur_timed_rect;
    struct vf_lw_opts *lw_opts;
} const vf_priv_dflt = {
    .band = 1,
};

#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#define MAX(a,b) (((a) > (b)) ? (a) : (b))

/**
 * Adjust the coordinates to suit the band width
 * Also print a notice in verbose mode
 */
static void fix_band(struct vf_priv_s *p)
{
    if (p->band < 0) {
        p->band = 4;
        p->show = 1;
    }
    p->lw += p->band*2;
    p->lh += p->band*2;
    p->xoff -= p->band;
    p->yoff -= p->band;
    mp_msg(MSGT_VFILTER, MSGL_V, "delogo: %d x %d, %d x %d, band = %d\n",
           p->xoff, p->yoff, p->lw, p->lh, p->band);
}

static void update_sub(struct vf_priv_s *p, double pts)
{
    int ipts = pts * 1000;
    int tr = p->cur_timed_rect;
    while (tr < p->n_timed_rect - 1 && ipts >= p->timed_rect[tr + 1].ts)
        tr++;
    while (tr >= 0 && ipts < p->timed_rect[tr].ts)
        tr--;
    if (tr == p->cur_timed_rect)
        return;
    p->cur_timed_rect = tr;
    if (tr >= 0) {
        p->xoff = p->timed_rect[tr].x;
        p->yoff = p->timed_rect[tr].y;
        p->lw   = p->timed_rect[tr].w;
        p->lh   = p->timed_rect[tr].h;
        p->band = p->timed_rect[tr].b;
    } else {
        p->xoff = p->yoff = p->lw = p->lh = p->band = 0;
    }
    fix_band(p);
}

static void do_delogo(uint8_t *dst, uint8_t *src, int dstStride, int srcStride, int width, int height,
                      int logo_x, int logo_y, int logo_w, int logo_h, int band, int show) {
    int y, x;
    int interp, dist;
    uint8_t *xdst, *xsrc;

    uint8_t *topleft, *botleft, *topright;
    int xclipl, xclipr, yclipt, yclipb;
    int logo_x1, logo_x2, logo_y1, logo_y2;

    xclipl = MAX(-logo_x, 0);
    xclipr = MAX(logo_x+logo_w-width, 0);
    yclipt = MAX(-logo_y, 0);
    yclipb = MAX(logo_y+logo_h-height, 0);

    logo_x1 = logo_x + xclipl;
    logo_x2 = logo_x + logo_w - xclipr;
    logo_y1 = logo_y + yclipt;
    logo_y2 = logo_y + logo_h - yclipb;

    topleft = src+logo_y1*srcStride+logo_x1;
    topright = src+logo_y1*srcStride+logo_x2-1;
    botleft = src+(logo_y2-1)*srcStride+logo_x1;

    dst += (logo_y1+1)*dstStride;
    src += (logo_y1+1)*srcStride;

    for(y = logo_y1+1; y < logo_y2-1; y++)
    {
        for (x = logo_x1+1, xdst = dst+logo_x1+1, xsrc = src+logo_x1+1; x < logo_x2-1; x++, xdst++, xsrc++) {
            interp = ((topleft[srcStride*(y-logo_y-yclipt)]
                       + topleft[srcStride*(y-logo_y-1-yclipt)]
                       + topleft[srcStride*(y-logo_y+1-yclipt)])*(logo_w-(x-logo_x))/logo_w
                      + (topright[srcStride*(y-logo_y-yclipt)]
                         + topright[srcStride*(y-logo_y-1-yclipt)]
                         + topright[srcStride*(y-logo_y+1-yclipt)])*(x-logo_x)/logo_w
                      + (topleft[x-logo_x-xclipl]
                         + topleft[x-logo_x-1-xclipl]
                         + topleft[x-logo_x+1-xclipl])*(logo_h-(y-logo_y))/logo_h
                      + (botleft[x-logo_x-xclipl]
                         + botleft[x-logo_x-1-xclipl]
                         + botleft[x-logo_x+1-xclipl])*(y-logo_y)/logo_h
                )/6;
/*                interp = (topleft[srcStride*(y-logo_y)]*(logo_w-(x-logo_x))/logo_w
                          + topright[srcStride*(y-logo_y)]*(x-logo_x)/logo_w
                          + topleft[x-logo_x]*(logo_h-(y-logo_y))/logo_h
                          + botleft[x-logo_x]*(y-logo_y)/logo_h
                          )/2;*/
            if (y >= logo_y+band && y < logo_y+logo_h-band && x >= logo_x+band && x < logo_x+logo_w-band) {
                    *xdst = interp;
            } else {
                dist = 0;
                if (x < logo_x+band) dist = MAX(dist, logo_x-x+band);
                else if (x >= logo_x+logo_w-band) dist = MAX(dist, x-(logo_x+logo_w-1-band));
                if (y < logo_y+band) dist = MAX(dist, logo_y-y+band);
                else if (y >= logo_y+logo_h-band) dist = MAX(dist, y-(logo_y+logo_h-1-band));
                *xdst = (*xsrc*dist + interp*(band-dist))/band;
                if (show && (dist == band-1)) *xdst = 0;
            }
        }

        dst+= dstStride;
        src+= srcStride;
    }
}

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi)
{
    struct mp_image *dmpi = mpi;
    if (!mp_image_is_writeable(mpi)) {
        dmpi = vf_alloc_out_image(vf);
        mp_image_copy_attributes(dmpi, mpi);
        mp_image_copy(dmpi, mpi);
    }

    if (vf->priv->timed_rect)
        update_sub(vf->priv, dmpi->pts);
    do_delogo(dmpi->planes[0], mpi->planes[0], dmpi->stride[0], mpi->stride[0], mpi->w, mpi->h,
              vf->priv->xoff, vf->priv->yoff, vf->priv->lw, vf->priv->lh, vf->priv->band, vf->priv->show);
    do_delogo(dmpi->planes[1], mpi->planes[1], dmpi->stride[1], mpi->stride[1], mpi->w/2, mpi->h/2,
              vf->priv->xoff/2, vf->priv->yoff/2, vf->priv->lw/2, vf->priv->lh/2, vf->priv->band/2, vf->priv->show);
    do_delogo(dmpi->planes[2], mpi->planes[2], dmpi->stride[2], mpi->stride[2], mpi->w/2, mpi->h/2,
              vf->priv->xoff/2, vf->priv->yoff/2, vf->priv->lw/2, vf->priv->lh/2, vf->priv->band/2, vf->priv->show);

    if (dmpi != mpi)
        talloc_free(mpi);
    return dmpi;
}

//===========================================================================//

static int query_format(struct vf_instance *vf, unsigned int fmt){
    switch(fmt)
    {
    case IMGFMT_420P:
        return vf_next_query_format(vf,IMGFMT_420P);
    }
    return 0;
}

static int load_timed_rectangles(struct vf_priv_s *delogo)
{
    FILE *f;
    char line[2048];
    int lineno = 0, p;
    double ts, last_ts = 0;
    struct timed_rectangle *rect = NULL, *nr;
    int n_rect = 0, alloc_rect = 0;

    f = fopen(delogo->file, "r");
    if (!f) {
        mp_msg(MSGT_VFILTER, MSGL_ERR, "delogo: unable to load %s: %s\n",
               delogo->file, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (*line == '#' || *line == '\n')
            continue;
        if (n_rect == alloc_rect) {
            if (alloc_rect > INT_MAX / 2 / (int)sizeof(*rect)) {
                mp_msg(MSGT_VFILTER, MSGL_WARN,
                       "delogo: too many rectangles\n");
                goto load_error;
            }
            alloc_rect = alloc_rect ? 2 * alloc_rect : 256;
            nr = realloc(rect, alloc_rect * sizeof(*rect));
            if (!nr) {
                mp_msg(MSGT_VFILTER, MSGL_WARN, "delogo: out of memory\n");
                goto load_error;
            }
            rect = nr;
        }
        nr = rect + n_rect;
        memset(nr, 0, sizeof(*nr));
        p = sscanf(line, "%lf %d:%d:%d:%d:%d",
                   &ts, &nr->x, &nr->y, &nr->w, &nr->h, &nr->b);
        if ((p == 2 && !nr->x) || p == 5 || p == 6) {
            if (ts <= last_ts)
                mp_msg(MSGT_VFILTER, MSGL_WARN, "delogo: %s:%d: wrong time\n",
                       delogo->file, lineno);
            nr->ts = 1000 * ts + 0.5;
            n_rect++;
        } else {
            mp_msg(MSGT_VFILTER, MSGL_WARN, "delogo: %s:%d: syntax error\n",
                   delogo->file, lineno);
        }
    }
    fclose(f);
    if (!n_rect) {
        mp_msg(MSGT_VFILTER, MSGL_ERR, "delogo: %s: no rectangles found\n",
               delogo->file);
        free(rect);
        return -1;
    }
    nr = realloc(rect, n_rect * sizeof(*rect));
    if (nr)
        rect = nr;
    delogo->timed_rect   = rect;
    delogo->n_timed_rect = n_rect;
    return 0;

load_error:
    free(rect);
    fclose(f);
    return -1;
}

static int vf_open(vf_instance_t *vf){
    struct vf_priv_s *p = vf->priv;
    vf->filter=filter;
    vf->query_format=query_format;

    int band = p->band;
    int show = p->show;
    if (band < 0) {
        band = 4;
        show = 1;
    }
    if (vf_lw_set_graph(vf, p->lw_opts, "delogo", "%d:%d:%d:%d:%d:%d",
                        p->xoff, p->yoff, p->lw, p->lh, band, show) >= 0)
    {
        if (p->file && p->file[0])
            mp_msg(MSGT_VFILTER, MSGL_WARN, "delogo: file argument ignored\n");
        return 1;
    }

    if (vf->priv->file) {
        if (load_timed_rectangles(vf->priv))
            return 0;
        mp_msg(MSGT_VFILTER, MSGL_V, "delogo: %d from %s\n",
               vf->priv->n_timed_rect, vf->priv->file);
        vf->priv->cur_timed_rect = -1;
    }
    fix_band(vf->priv);

    return 1;
}

#define OPT_BASE_STRUCT struct vf_priv_s
static const m_option_t vf_opts_fields[] = {
    OPT_INT("x", xoff, 0),
    OPT_INT("y", yoff, 0),
    OPT_INT("w", lw, 0),
    OPT_INT("h", lh, 0),
    OPT_INT("t", band, 0),
    OPT_INT("band", band, 0), // alias
    OPT_STRING("file", file, 0),
    OPT_FLAG("show", show, 0),
    OPT_SUBSTRUCT("", lw_opts, vf_lw_conf, 0),
    {0}
};

const vf_info_t vf_info_delogo = {
    .description = "simple logo remover",
    .name = "delogo",
    .open = vf_open,
    .priv_size = sizeof(struct vf_priv_s),
    .priv_defaults = &vf_priv_dflt,
    .options = vf_opts_fields,
};

//===========================================================================//
