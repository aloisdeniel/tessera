/*
 * text.h — font atlas baking + glyph metrics for 3D text labels.
 *
 * A font def is baked once at registration: ASCII + Latin-1 glyphs rasterized
 * with stb_truetype into one RGBA atlas (white RGB, alpha = coverage) so the
 * text pass can tint them per label. Metrics are kept in bake-time pixels; the
 * renderer scales them by (label size / pixel_height) into world units.
 */
#ifndef TESSERA_TEXT_H
#define TESSERA_TEXT_H

#include "core/core.h"
#include "gpu/gpu.h"
#include "tessera.h"

/* Baked codepoints: ASCII 32..126 plus Latin-1 160..255 (the gap is control
 * chars). Indexed as cp - TS_FONT_FIRST over one dense table. */
#define TS_FONT_FIRST  32
#define TS_FONT_LAST   255
#define TS_FONT_GLYPHS (TS_FONT_LAST - TS_FONT_FIRST + 1)

typedef struct {
    float u0, v0, u1, v1;      /* atlas sub-rect (normalized UV)             */
    float x0, y0, x1, y1;      /* quad rect rel. to the baseline cursor (px,
                                * y down; y0 is usually negative = above)    */
    float xadvance;            /* cursor advance (px)                        */
    bool  present;
} TsGlyph;

typedef struct {
    TsTexture tex;             /* glyph atlas (white RGB, alpha coverage)    */
    float     pixel_height;    /* bake size (px)                             */
    float     ascent, descent; /* px at bake size; descent is negative       */
    float     line_gap;
    TsGlyph   glyphs[TS_FONT_GLYPHS];
    bool      valid;
} TsFontDef;

/* Bake `ttf` (raw font file bytes) at `pixel_height` into *out. */
bool ts_font_build(TsGpu* gpu, const TsLog* log, const void* ttf, size_t size,
                   float pixel_height, TsFontDef* out, char* err, size_t err_sz);
void ts_font_free(TsGpu* gpu, TsFontDef* f);

/* Decode the next UTF-8 codepoint from *p (advances it). Invalid bytes decode
 * as U+FFFD and advance one byte. Returns 0 at NUL (without advancing). */
uint32_t ts_utf8_next(const char** p);

/* Glyph for a codepoint (NULL when absent / outside the baked range). */
const TsGlyph* ts_font_glyph(const TsFontDef* f, uint32_t cp);

/* Width of a NUL-terminated UTF-8 string in bake-time pixels. */
float ts_font_text_width(const TsFontDef* f, const char* text);

#endif /* TESSERA_TEXT_H */
