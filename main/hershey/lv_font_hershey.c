// =====================================================================
//  Hershey vector font, as an LVGL 9 font
// ---------------------------------------------------------------------
//  LVGL wants a box of alpha per glyph; Hershey gives strokes. So each
//  glyph is rasterized once, into an A8 mask that the font keeps, and
//  every later draw of that character at that size is a memcpy of a
//  mask the size of the letter. The first label to use a character pays
//  for the strokes; everything after it is as fast as a bitmap font,
//  and the letters stay sharp at any size.
//
//  Glyphs come from the same tables SynthEngine3D draws with, so the
//  widths agree character for character.
// =====================================================================

#include <math.h>
#include <string.h>

#include "lvgl.h"

#include "hershey_glyph.h"
#include "hershey_icons.h"
#include "lv_font_hershey.h"

// ceil(units * px / cap_height), in font units -> pixels
#define HERSHEY_PX(px, units) (((units) * (px) + HERSHEY_CAP_H - 1) / HERSHEY_CAP_H)

// A codepoint this font has never seen gets a box the width of one.
static int8_t const HersheyTofu[] = { 2, 0, 2, 14, 10, 14, 10, 0, 2, 0 };

// Codepoints below 0x80 (all of ASCII, where the UI lives) get a slot of
// their own; the rest share a small ring, which is plenty for the
// accented letters and the handful of LVGL symbols.
#define HERSHEY_EXTRA_SLOTS 48

typedef struct {
    uint32_t      cp;
    lv_draw_buf_t* buf;
} glyph_entry_t;

typedef struct {
    int32_t       size;   // cap height in pixels
    glyph_entry_t ascii[128];
    glyph_entry_t extra[HERSHEY_EXTRA_SLOTS];
    uint16_t      extra_next;
} hershey_ctx_t;

typedef struct {
    int adv_w;
    int box_w;
    int box_h;
    int ofs_x;
    int ofs_y;
    int top;   // rows above the baseline, i.e. the row index the cap line lands on
} glyph_metrics_t;

typedef struct {
    int16_t min_x, max_x, min_y, max_y;
} bbox_t;

// --- glyphs -------------------------------------------------------------------

static void fetch_glyph(uint32_t cp, hershey_glyph_t* g) {
    hershey_icon_t const* const icon = hershey_icon_lookup(cp);
    if(icon != NULL) {
        g->pts    = icon->pts;
        g->n      = icon->n;
        g->adv    = icon->adv;
        g->accent = SE_ACCENT_NONE;
        return;
    }
    if(hershey_glyph(cp, g)) return;
    g->pts    = HersheyTofu;
    g->n      = sizeof(HersheyTofu) / 2;
    g->adv    = HERSHEY_TOFU_ADV;
    g->accent = SE_ACCENT_NONE;
}

static void bbox_add(bbox_t* b, int x, int y) {
    if(x < b->min_x) b->min_x = (int16_t)x;
    if(x > b->max_x) b->max_x = (int16_t)x;
    if(y < b->min_y) b->min_y = (int16_t)y;
    if(y > b->max_y) b->max_y = (int16_t)y;
}

static void bbox_walk(bbox_t* b, int8_t const* pts, int n, int fx, int fy) {
    for(int i = 0; i < n; i++) {
        int const vx = pts[2 * i], vy = pts[2 * i + 1];
        if(vx == -1 && vy == -1) continue;
        bbox_add(b, vx + fx, vy + fy);
    }
}

static void bbox_accent(hershey_glyph_t const* g, bbox_t* b) {
    if(g->accent == SE_ACCENT_NONE) return;
    if(g->accent == SE_ACCENT_SLASH) {
        int const top = (g->accent_y > HERSHEY_X_H + 2) ? HERSHEY_CAP_H : HERSHEY_X_H + 2;
        bbox_add(b, 0, -2);
        bbox_add(b, g->adv, top);
        return;
    }
    bbox_walk(b, &SE_HERSHEY_ACCENT_PTS[2 * SE_HERSHEY_ACCENT[g->accent].off],
              SE_HERSHEY_ACCENT[g->accent].n, g->accent_x, g->accent_y);
}

// The box a glyph is drawn into, and where it sits relative to the pen
// and the baseline. Everything below rounds outward, so no stroke can
// fall outside the box.
static void glyph_metrics(hershey_ctx_t const* ctx, uint32_t cp, glyph_metrics_t* m) {
    hershey_glyph_t g;
    fetch_glyph(cp, &g);

    float const scale = (float)ctx->size / (float)HERSHEY_CAP_H;
    m->adv_w = (int)lroundf(g.adv * scale);

    if(g.n == 0) {
        m->box_w = 0;
        m->box_h = 0;
        m->ofs_x = 0;
        m->ofs_y = 0;
        m->top   = 0;
        return;
    }

    bbox_t b = { 32767, -32768, 32767, -32768 };
    bbox_walk(&b, g.pts, g.n, 0, 0);
    bbox_accent(&g, &b);

    m->ofs_x = (int)floorf(b.min_x * scale);
    m->box_w = (int)ceilf(b.max_x * scale) - m->ofs_x + 1;
    m->top   = (int)ceilf(b.max_y * scale);
    int const bot = (int)ceilf(-b.min_y * scale);
    m->box_h = m->top + bot + 1;
    m->ofs_y = -bot;
}

// --- rasterizing ----------------------------------------------------------------

static void mask_line(uint8_t* mask, int w, int h, int stride, int x0, int y0, int x1, int y1) {
    int const dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int const dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for(;;) {
        if(x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) mask[y0 * stride + x0] = 0xFF;
        if(x0 == x1 && y0 == y1) break;
        int const e2 = 2 * err;
        if(e2 >= dy) { err += dy; x0 += sx; }
        if(e2 <= dx) { err += dx; y0 += sy; }
    }
}

// One stroke. At display sizes a single pixel reads thin, so the bigger
// sizes draw the same stroke once more, nudged across the stroke's short
// axis: two pixels wide for a vertical line, two tall for a horizontal one.
static void mask_stroke(uint8_t* mask, int w, int h, int stride, int x0, int y0, int x1, int y1, bool thick) {
    mask_line(mask, w, h, stride, x0, y0, x1, y1);
    if(!thick) return;
    if(abs(y1 - y0) > abs(x1 - x0)) mask_line(mask, w, h, stride, x0 + 1, y0, x1 + 1, y1);
    else                             mask_line(mask, w, h, stride, x0, y0 + 1, x1, y1);
}

static void raster_walk(uint8_t* mask, int w, int h, int stride, float scale, glyph_metrics_t const* m,
                        int8_t const* pts, int n, int fx, int fy, bool thick) {
    int pen_down = 0;
    int prev_x = 0, prev_y = 0;

    for(int i = 0; i < n; i++) {
        int const vx = pts[2 * i], vy = pts[2 * i + 1];
        if(vx == -1 && vy == -1) {
            pen_down = 0;
            continue;
        }
        int const cx = (int)lroundf((vx + fx) * scale) - m->ofs_x;
        int const cy = m->top - (int)lroundf((vy + fy) * scale);
        if(pen_down) mask_stroke(mask, w, h, stride, prev_x, prev_y, cx, cy, thick);
        prev_x   = cx;
        prev_y   = cy;
        pen_down = 1;
    }
}

static lv_draw_buf_t* rasterize(hershey_ctx_t const* ctx, uint32_t cp, glyph_metrics_t const* m) {
    int const w = m->box_w, h = m->box_h;
    lv_draw_buf_t* buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if(buf == NULL) return NULL;

    int const stride = (int)buf->header.stride;
    memset(buf->data, 0, (size_t)stride * (size_t)h);

    hershey_glyph_t g;
    fetch_glyph(cp, &g);

    float const scale = (float)ctx->size / (float)HERSHEY_CAP_H;
    bool  const thick = ctx->size >= 24;

    raster_walk(buf->data, w, h, stride, scale, m, g.pts, g.n, 0, 0, thick);

    if(g.accent != SE_ACCENT_NONE) {
        if(g.accent == SE_ACCENT_SLASH) {
            int const top = (g.adv > HERSHEY_X_H + 2) ? HERSHEY_CAP_H : HERSHEY_X_H + 2;
            int8_t const bar[4] = { 0, -2, (int8_t)g.adv, (int8_t)top };
            raster_walk(buf->data, w, h, stride, scale, m, bar, 2, 0, 0, thick);
        }
        else {
            raster_walk(buf->data, w, h, stride, scale, m,
                        &SE_HERSHEY_ACCENT_PTS[2 * SE_HERSHEY_ACCENT[g.accent].off],
                        SE_HERSHEY_ACCENT[g.accent].n, g.accent_x, g.accent_y, thick);
        }
    }

    return buf;
}

// --- glyph cache ----------------------------------------------------------------

static glyph_entry_t* cache_slot(hershey_ctx_t* ctx, uint32_t cp) {
    if(cp < 128) return &ctx->ascii[cp];

    for(int i = 0; i < HERSHEY_EXTRA_SLOTS; i++) {
        if(ctx->extra[i].buf != NULL && ctx->extra[i].cp == cp) return &ctx->extra[i];
    }
    for(int i = 0; i < HERSHEY_EXTRA_SLOTS; i++) {
        if(ctx->extra[i].buf == NULL) {
            ctx->extra[i].cp = cp;
            return &ctx->extra[i];
        }
    }

    glyph_entry_t* const victim = &ctx->extra[ctx->extra_next];
    ctx->extra_next = (uint16_t)((ctx->extra_next + 1) % HERSHEY_EXTRA_SLOTS);
    lv_draw_buf_destroy(victim->buf);
    victim->buf = NULL;
    victim->cp  = cp;
    return victim;
}

// --- lv_font_t callbacks ---------------------------------------------------------

static bool hershey_get_glyph_dsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t letter,
                                  uint32_t letter_next) {
    LV_UNUSED(letter_next);   // Hershey spaces itself; there is nothing to kern

    hershey_ctx_t const* const ctx = (hershey_ctx_t const*)font->user_data;

    if(letter < 0x20 || letter == 0xf8ff || letter == 0x200c) {
        dsc_out->adv_w         = 0;
        dsc_out->box_w         = 0;
        dsc_out->box_h         = 0;
        dsc_out->ofs_x         = 0;
        dsc_out->ofs_y         = 0;
        dsc_out->stride        = 0;
        dsc_out->format        = LV_FONT_GLYPH_FORMAT_NONE;
        dsc_out->is_placeholder = 0;
        return true;
    }

    glyph_metrics_t m;
    glyph_metrics(ctx, letter, &m);

    dsc_out->adv_w          = (uint16_t)m.adv_w;
    dsc_out->box_w          = (uint16_t)m.box_w;
    dsc_out->box_h          = (uint16_t)m.box_h;
    dsc_out->ofs_x          = (int16_t)m.ofs_x;
    dsc_out->ofs_y          = (int16_t)m.ofs_y;
    dsc_out->stride         = 0;
    dsc_out->format         = (m.box_w > 0 && m.box_h > 0) ? LV_FONT_GLYPH_FORMAT_A8 : LV_FONT_GLYPH_FORMAT_NONE;
    dsc_out->is_placeholder = 0;
    dsc_out->gid.index      = letter;
    dsc_out->entry          = NULL;
    return true;
}

static const void* hershey_get_glyph_bitmap(lv_font_glyph_dsc_t* g_dsc, lv_draw_buf_t* draw_buf) {
    LV_UNUSED(draw_buf);   // the mask outlives this call, so it is worth keeping our own

    hershey_ctx_t* const ctx = (hershey_ctx_t*)g_dsc->resolved_font->user_data;
    uint32_t const     cp    = g_dsc->gid.index;
    glyph_entry_t* const slot = cache_slot(ctx, cp);

    if(slot->buf == NULL) {
        glyph_metrics_t m;
        glyph_metrics(ctx, cp, &m);
        if(m.box_w > 0 && m.box_h > 0) slot->buf = rasterize(ctx, cp, &m);
    }

    g_dsc->entry = NULL;   // the cache owns the mask, there is nothing to release
    return slot->buf;
}

// --- the fonts themselves ---------------------------------------------------------

#define HERSHEY_FONT_DEFINE(symbol, px)                                                        \
    static hershey_ctx_t symbol##_ctx = { .size = (px) };                                      \
    const lv_font_t symbol = {                                                                 \
        .dsc                = NULL,                                                            \
        .fallback           = NULL,                                                            \
        .user_data          = &symbol##_ctx,                                                   \
        .get_glyph_dsc      = hershey_get_glyph_dsc,                                           \
        .get_glyph_bitmap   = hershey_get_glyph_bitmap,                                        \
        .release_glyph      = NULL,                                                            \
        .line_height        = HERSHEY_PX(px, HERSHEY_ASCENT) + HERSHEY_PX(px, HERSHEY_DESCENT), \
        .base_line          = HERSHEY_PX(px, HERSHEY_DESCENT),                                 \
        .cap_height         = (px),                                                            \
        .x_height           = HERSHEY_PX(px, HERSHEY_X_H),                                     \
        .subpx              = LV_FONT_SUBPX_NONE,                                              \
        .kerning            = LV_FONT_KERNING_NONE,                                            \
        .static_bitmap      = 0,                                                               \
        .underline_position = -HERSHEY_PX(px, 3),                                              \
        .underline_thickness = HERSHEY_PX(px, 2),                                              \
    }

HERSHEY_FONT_DEFINE(lv_font_hershey_14, 14);
HERSHEY_FONT_DEFINE(lv_font_hershey_16, 16);
HERSHEY_FONT_DEFINE(lv_font_hershey_20, 20);
HERSHEY_FONT_DEFINE(lv_font_hershey_28, 28);
