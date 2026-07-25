/* text.c — bake a TrueType font into a glyph atlas (stb_truetype pack API). */
#include "text/text.h"
#include "stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pack both ranges into a `dim`x`dim` 8-bit bitmap. Returns false if the
 * glyphs did not all fit (caller retries with a bigger atlas). */
static bool pack_at(const unsigned char* ttf, float px, int dim,
                    unsigned char* bitmap, stbtt_packedchar* ascii,
                    stbtt_packedchar* latin1) {
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, bitmap, dim, dim, 0, 2, NULL)) return false;
    stbtt_pack_range ranges[2] = {
        { px, 32,  NULL, 95, ascii,  0, 0 },   /* 32..126 */
        { px, 160, NULL, 96, latin1, 0, 0 },   /* 160..255 */
    };
    int ok = stbtt_PackFontRanges(&pc, ttf, 0, ranges, 2);
    stbtt_PackEnd(&pc);
    return ok != 0;
}

bool ts_font_build(TsGpu* gpu, const TsLog* log, const void* ttf, size_t size,
                   float pixel_height, TsFontDef* out, char* err, size_t err_sz) {
    (void)log;
    memset(out, 0, sizeof *out);
    if (!ttf || size < 12) {
        snprintf(err, err_sz, "register_font: empty font bytes");
        return false;
    }
    float px = pixel_height > 0.0f ? pixel_height : 48.0f;
    if (px > 160.0f) px = 160.0f;

    const unsigned char* data = (const unsigned char*)ttf;
    int font_off = stbtt_GetFontOffsetForIndex(data, 0);
    if (font_off < 0) {
        snprintf(err, err_sz, "register_font: not a TrueType/OpenType font");
        return false;
    }
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, font_off)) {
        snprintf(err, err_sz, "register_font: font parse failed");
        return false;
    }

    /* Grow the atlas until every glyph fits (191 glyphs; 512 covers px<=48). */
    stbtt_packedchar ascii[95], latin1[96];
    unsigned char* bitmap = NULL;
    int dim = 256;
    while ((size_t)dim * dim < (size_t)(px + 4.0f) * (px + 4.0f) * 200) dim *= 2;
    for (;; dim *= 2) {
        if (dim > 4096) {
            free(bitmap);
            snprintf(err, err_sz, "register_font: glyphs do not fit a 4096 atlas");
            return false;
        }
        unsigned char* nb = (unsigned char*)realloc(bitmap, (size_t)dim * dim);
        if (!nb) { free(bitmap); snprintf(err, err_sz, "register_font: out of memory"); return false; }
        bitmap = nb;
        if (pack_at(data, px, dim, bitmap, ascii, latin1)) break;
    }

    /* Coverage -> RGBA (white, alpha = coverage) so the shader just tints. */
    uint8_t* rgba = (uint8_t*)malloc((size_t)dim * dim * 4);
    if (!rgba) { free(bitmap); snprintf(err, err_sz, "register_font: out of memory"); return false; }
    for (size_t i = 0; i < (size_t)dim * dim; ++i) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = bitmap[i];
    }
    free(bitmap);
    bool ok = ts_gpu_upload_texture(gpu, rgba, (uint32_t)dim, (uint32_t)dim, &out->tex);
    free(rgba);
    if (!ok) { snprintf(err, err_sz, "register_font: atlas upload failed"); return false; }

    float inv = 1.0f / (float)dim;
    for (int r = 0; r < 2; ++r) {
        const stbtt_packedchar* pcs = r == 0 ? ascii : latin1;
        int first = r == 0 ? 32 : 160, count = r == 0 ? 95 : 96;
        for (int i = 0; i < count; ++i) {
            const stbtt_packedchar* c = &pcs[i];
            TsGlyph* g = &out->glyphs[first + i - TS_FONT_FIRST];
            g->u0 = c->x0 * inv; g->v0 = c->y0 * inv;
            g->u1 = c->x1 * inv; g->v1 = c->y1 * inv;
            g->x0 = c->xoff;  g->y0 = c->yoff;
            g->x1 = c->xoff2; g->y1 = c->yoff2;
            g->xadvance = c->xadvance;
            g->present = true;
        }
    }

    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    float scale = stbtt_ScaleForPixelHeight(&info, px);
    out->pixel_height = px;
    out->ascent  = (float)asc * scale;
    out->descent = (float)desc * scale;
    out->line_gap = (float)gap * scale;
    out->valid = true;
    return true;
}

void ts_font_free(TsGpu* gpu, TsFontDef* f) {
    if (!f || !f->valid) return;
    ts_gpu_free_texture(gpu, &f->tex);
    f->valid = false;
}

uint32_t ts_utf8_next(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    if (s[0] == 0) return 0;
    uint32_t cp; int len;
    if (s[0] < 0x80)      { cp = s[0]; len = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; len = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; len = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; len = 4; }
    else { *p += 1; return 0xFFFD; }
    for (int i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

const TsGlyph* ts_font_glyph(const TsFontDef* f, uint32_t cp) {
    if (!f || !f->valid || cp < TS_FONT_FIRST || cp > TS_FONT_LAST) return NULL;
    const TsGlyph* g = &f->glyphs[cp - TS_FONT_FIRST];
    return g->present ? g : NULL;
}

float ts_font_text_width(const TsFontDef* f, const char* text) {
    if (!f || !f->valid || !text) return 0.0f;
    float w = 0.0f;
    const char* p = text;
    for (;;) {
        uint32_t cp = ts_utf8_next(&p);
        if (cp == 0) break;
        const TsGlyph* g = ts_font_glyph(f, cp);
        if (g) w += g->xadvance;
    }
    return w;
}
