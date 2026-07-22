/* examples/cards — a felt table with cards, a draw pile, and a fanned hand.
 *
 * Shows the card mechanic end to end: cards placed flat on the table (some face
 * up, some concealed), a draw pile whose thickness tracks its card count, and a
 * hand that fans its cards in world space. A second state flips a concealed card
 * face up (texture crossfade), slides a card across the table, and grows the
 * draw pile — all animated by the engine from the state diff.
 *
 *   Interactive:  ./example_cards
 *       SPACE       toggle between the two states (flip / move / grow)
 *       arrows      orbit the camera
 *       ESC         quit
 *   Headless:     ./example_cards --demo <dir> [--frames N]
 *       writes <dir>/cards_000.png .. capturing the transition.
 */
#include "tessera.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logfn(void* ud, int level, const char* msg) {
    (void)ud;
    static const char* n[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    fprintf(stderr, "[%s] %s\n", (level >= 0 && level <= 4) ? n[level] : "?", msg);
}

/* ------------------------------------------------------------------ art */
/* 5x7 digit font (shared with the dice example's style). */
static const unsigned char FONT5x7[13][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C=10 */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H=11 */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T=12 */
};

#define CW 200   /* card texture width  */
#define CH 300   /* card texture height (2:3) */

static unsigned char* encode_tga32(const unsigned char* rgba, int w, int h, int* out_len) {
    int sz = 18 + w * h * 4;
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    memset(d, 0, 18);
    d[2] = 2;
    d[12] = w & 0xFF; d[13] = (w >> 8) & 0xFF;
    d[14] = h & 0xFF; d[15] = (h >> 8) & 0xFF;
    d[16] = 32; d[17] = 0x28;
    for (int i = 0; i < w * h; ++i) {
        d[18 + i*4 + 0] = rgba[i*4 + 2];
        d[18 + i*4 + 1] = rgba[i*4 + 1];
        d[18 + i*4 + 2] = rgba[i*4 + 0];
        d[18 + i*4 + 3] = rgba[i*4 + 3];
    }
    *out_len = sz;
    return d;
}

static void put_px(unsigned char* img, int x, int y, const unsigned char c[4]) {
    if (x < 0 || y < 0 || x >= CW || y >= CH) return;
    unsigned char* p = &img[(y * CW + x) * 4];
    p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
}

static void draw_glyph(unsigned char* img, int g, int ox, int oy, int sc,
                       const unsigned char fg[4]) {
    if (g < 0 || g > 12) return;
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            if (FONT5x7[g][row] & (1 << (4 - col)))
                for (int yy = 0; yy < sc; ++yy)
                    for (int xx = 0; xx < sc; ++xx)
                        put_px(img, ox + col * sc + xx, oy + row * sc + yy, fg);
}

/* Fill the card with a rounded background of colour `bg`, bordered. */
static void fill_card(unsigned char* img, const unsigned char bg[3]) {
    const int R = 26;
    unsigned char border[3] = { (unsigned char)(bg[0]*0.55f),
                                (unsigned char)(bg[1]*0.55f),
                                (unsigned char)(bg[2]*0.55f) };
    for (int y = 0; y < CH; ++y)
        for (int x = 0; x < CW; ++x) {
            int dx = x < R ? R - x : (x >= CW - R ? x - (CW - R - 1) : 0);
            int dy = y < R ? R - y : (y >= CH - R ? y - (CH - R - 1) : 0);
            bool inside = (dx * dx + dy * dy) <= R * R;
            unsigned char c[4];
            if (!inside) { c[0]=c[1]=c[2]=c[3]=0; }
            else {
                bool edge = (dx*dx + dy*dy) >= (R-6)*(R-6) ||
                            x < 6 || y < 6 || x >= CW-6 || y >= CH-6;
                if (edge) { c[0]=border[0]; c[1]=border[1]; c[2]=border[2]; c[3]=255; }
                else      { c[0]=bg[0]; c[1]=bg[1]; c[2]=bg[2]; c[3]=255; }
            }
            put_px(img, x, y, c);
        }
}

/* A front face: rank glyph in the corners + big centre glyph, tinted by suit. */
static TesseraDefId make_front(TesseraEngine* e, int glyph, const unsigned char suit[3]) {
    unsigned char* img = (unsigned char*)malloc(CW * CH * 4);
    const unsigned char cream[3] = {242, 240, 232};
    fill_card(img, cream);
    unsigned char fg[4] = { suit[0], suit[1], suit[2], 255 };
    draw_glyph(img, glyph, 16, 16, 4, fg);                 /* top-left  */
    draw_glyph(img, glyph, CW - 16 - 5*4, CH - 16 - 7*4, 4, fg); /* bot-right */
    draw_glyph(img, glyph, (CW - 5*14)/2, (CH - 7*14)/2, 14, fg); /* centre */
    int len = 0; unsigned char* tga = encode_tga32(img, CW, CH, &len);
    free(img);
    TesseraBytes b = { .data = tga, .size = (size_t)len };
    TesseraDefId id = tessera_register_atlas(e, &b);
    free(tga);
    return id;
}

/* The concealing front + the back: woven lattice patterns so nothing leaks. */
static TesseraDefId make_pattern(TesseraEngine* e, const unsigned char base[3],
                                 const unsigned char line[3]) {
    unsigned char* img = (unsigned char*)malloc(CW * CH * 4);
    fill_card(img, base);
    unsigned char l4[4] = { line[0], line[1], line[2], 255 };
    for (int y = 12; y < CH - 12; ++y)
        for (int x = 12; x < CW - 12; ++x)
            if (((x + y) % 18 < 2) || ((x - y + 4096) % 18 < 2))
                put_px(img, x, y, l4);
    int len = 0; unsigned char* tga = encode_tga32(img, CW, CH, &len);
    free(img);
    TesseraBytes b = { .data = tga, .size = (size_t)len };
    TesseraDefId id = tessera_register_atlas(e, &b);
    free(tga);
    return id;
}

/* ------------------------------------------------------------------ scene */
static TesseraDefId g_felt, g_hidden_atlas, g_back_atlas;
static TesseraDefId g_card_defs[8];   /* a few card types */
static int g_ndefs = 0;

static TesseraTilePlacement g_tiles[13 * 11];
static size_t g_ntiles = 0;

/* Quaternion (xyzw) from axis + angle. */
static void quat_axis(float x, float y, float z, float a, float out[4]) {
    float s = sinf(a * 0.5f);
    out[0] = x * s; out[1] = y * s; out[2] = z * s; out[3] = cosf(a * 0.5f);
}

static void build_defs(TesseraEngine* e) {
    TesseraTileDef felt = {0};
    felt.tint[0] = 0.09f; felt.tint[1] = 0.40f; felt.tint[2] = 0.24f; felt.tint[3] = 1.0f;
    felt.thickness = 0.2f;
    g_felt = tessera_register_tile_def(e, &felt);

    g_ntiles = 0;
    for (int z = -5; z <= 5; ++z)
        for (int x = -6; x <= 6; ++x) {
            g_tiles[g_ntiles].coord.x = x; g_tiles[g_ntiles].coord.y = z;
            g_tiles[g_ntiles].tile_def = g_felt; g_tiles[g_ntiles].variant = 0;
            g_tiles[g_ntiles].id = 0; g_ntiles++;
        }

    const unsigned char slate[3] = {44, 52, 74}, wire[3] = {90, 110, 150};
    const unsigned char maroon[3] = {96, 26, 34}, gold[3] = {198, 160, 78};
    g_hidden_atlas = make_pattern(e, slate, wire);
    g_back_atlas   = make_pattern(e, maroon, gold);

    const unsigned char red[3] = {176, 40, 44}, ink[3] = {32, 40, 60};
    int glyphs[6] = { 10 /*C*/, 11 /*H*/, 12 /*T*/, 7, 8, 9 };
    for (int i = 0; i < 6; ++i) {
        const unsigned char* suit = (i % 2) ? red : ink;
        TesseraDefId front = make_front(e, glyphs[i], suit);
        TesseraCardDef cd = {0};
        cd.visible_atlas = front;
        cd.hidden_atlas  = g_hidden_atlas;
        cd.back_atlas    = g_back_atlas;
        cd.tint[0] = cd.tint[1] = cd.tint[2] = cd.tint[3] = 1.0f;
        g_card_defs[g_ndefs++] = tessera_register_card_def(e, &cd);
        if (!g_card_defs[g_ndefs - 1])
            fprintf(stderr, "register_card failed: %s\n", tessera_last_error(e));
    }
}

/* Build a state. `phase` 0 = initial, 1 = after the flip/move/grow. */
static void push_state(TesseraEngine* e, int phase) {
    TesseraCardPlacement cards[12];
    int nc = 0;

    /* Two cards flat on the table (one concealed; in phase 1 it flips face up
     * and slides across). */
    memset(&cards[nc], 0, sizeof cards[nc]);
    cards[nc].id = 1; cards[nc].def = g_card_defs[0];
    cards[nc].position[0] = -2.5f; cards[nc].position[1] = 0.02f; cards[nc].position[2] = -1.0f;
    /* identity orientation => flat, face up */
    cards[nc].hidden = false;
    nc++;

    memset(&cards[nc], 0, sizeof cards[nc]);
    cards[nc].id = 2; cards[nc].def = g_card_defs[1];
    cards[nc].position[0] = phase ? 1.6f : -0.6f;
    cards[nc].position[1] = 0.02f;
    cards[nc].position[2] = phase ? -2.2f : -1.0f;
    quat_axis(0, 1, 0, phase ? 0.5f : 0.0f, cards[nc].orientation);   /* rotate a bit in phase 1 */
    cards[nc].hidden = phase ? false : true;    /* flips face up in phase 1 */
    nc++;

    /* A hand of five fanned cards at the near edge, tilted back toward camera. */
    float hand_q[4]; quat_axis(1, 0, 0, -0.9f, hand_q);   /* lean the fan back */
    TesseraHandPlacement hand = {0};
    hand.id = 100;
    hand.position[0] = 0.0f; hand.position[1] = 1.4f; hand.position[2] = 4.2f;
    memcpy(hand.orientation, hand_q, sizeof hand_q);
    hand.spread_deg = 44.0f; hand.radius = 3.2f; hand.card_spacing = 0.95f;

    for (int i = 0; i < 5; ++i) {
        memset(&cards[nc], 0, sizeof cards[nc]);
        cards[nc].id = (TesseraCardId)(10 + i);
        cards[nc].def = g_card_defs[i % g_ndefs];
        cards[nc].hidden = false;
        cards[nc].hand = 100;
        cards[nc].hand_slot = (uint32_t)i;
        nc++;
    }

    /* A draw pile off to the side; it grows from 12 to 26 cards in phase 1. */
    TesseraCardDrawPlacement draw = {0};
    draw.id = 200; draw.def = g_card_defs[2];
    draw.position[0] = 3.6f; draw.position[1] = 0.02f; draw.position[2] = 0.5f;
    draw.count = phase ? 26 : 12;
    draw.top_hidden = true;

    TesseraState st = {0};
    st.tiles = g_tiles; st.tile_count = g_ntiles;
    st.cards = cards; st.card_count = (size_t)nc;
    st.card_draws = &draw; st.card_draw_count = 1;
    st.hands = &hand; st.hand_count = 1;
    st.camera.focus.x = 0.0f; st.camera.focus.y = 0.0f;
    st.camera.distance = 12.5f;
    st.camera.yaw = 0.0f;
    st.camera.pitch = 0.72f;
    st.camera.fov = 0.85f;
    tessera_set_state(e, &st);
}

/* ------------------------------------------------------------------ demo */
static int run_demo(TesseraEngine* e, const char* dir, int frames) {
    build_defs(e);
    push_state(e, 0);
    for (int i = 0; i < 40; ++i) tessera_tick(e, 1.0 / 60.0);  /* settle in */

    int W = 1280, H = 800;
    unsigned char* buf = (unsigned char*)malloc((size_t)W * H * 4);
    char path[1024];
    for (int f = 0; f < frames; ++f) {
        if (f == frames / 3) push_state(e, 1);   /* trigger flip / move / grow */
        if (!tessera_render_rgba(e, 1.0 / 60.0, W, H, buf, (size_t)W * H * 4)) {
            fprintf(stderr, "render_rgba failed: %s\n", tessera_last_error(e));
            free(buf); return 3;
        }
        snprintf(path, sizeof path, "%s/cards_%03d.png", dir, f);
        tessera_capture_png(e, W, H, path);
    }
    fprintf(stderr, "wrote %d frames to %s\n", frames, dir);
    free(buf);
    return 0;
}

int main(int argc, char** argv) {
    const char* demo_dir = NULL;
    int frames = 150;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }

    TesseraConfig cfg = {0};
    cfg.width = 1280; cfg.height = 800; cfg.pixel_density = 1.0f;
    cfg.debug = true; cfg.log = logfn;

    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e)) > 0) {
        fprintf(stderr, "init error: %s\n", e ? tessera_last_error(e) : "null");
        if (e) tessera_destroy(e);
        return 1;
    }
    fprintf(stderr, "backend: %s\n", tessera_backend_name(e));

    if (demo_dir) {
        int rc = run_demo(e, demo_dir, frames);
        tessera_destroy(e);
        return rc;
    }

    build_defs(e);
    int phase = 0;
    push_state(e, phase);

    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_SPACE: phase ^= 1; push_state(e, phase); break;
                case SDLK_LEFT:  tessera__debug_orbit(e, -0.12f, 0, 0); break;
                case SDLK_RIGHT: tessera__debug_orbit(e,  0.12f, 0, 0); break;
                case SDLK_UP:    tessera__debug_orbit(e, 0, -0.08f, 0); break;
                case SDLK_DOWN:  tessera__debug_orbit(e, 0,  0.08f, 0); break;
                default: break;
                }
            }
            if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                tessera_resize(e, ev.window.data1, ev.window.data2, 1.0f);
        }
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / freq; prev = now;
        if (dt > 0.1) dt = 0.1;
        tessera_tick(e, dt);
    }

    tessera_destroy(e);
    return 0;
}
