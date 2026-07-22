/* examples/dice — procedural dice thrown onto a felt table.
 *
 * Registers several dice definitions (a two-sided token, a d6 cube, and a
 * couple of many-sided barrel dice), each built by the engine from a set of
 * per-face sprites generated here on the fly. Throwing a die tumbles it along a
 * precomputed trajectory that settles on a requested face at a floating point
 * above the table; removing one fades it out.
 *
 *   Interactive:  ./example_dice
 *       SPACE / R   re-throw all dice (new random faces)
 *       C           clear the dice (fade out)
 *       arrows      orbit the camera
 *       ESC         quit
 *   Headless:     ./example_dice --demo <dir> [--frames N]
 *       writes <dir>/dice_000.png .. capturing the throw + settle.
 */
#include "tessera.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logfn(void* ud, int level, const char* msg) {
    (void)ud;
    static const char* n[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    fprintf(stderr, "[%s] %s\n", (level >= 0 && level <= 4) ? n[level] : "?", msg);
}

/* ------------------------------------------------------------------ sprites */
/* A compact 5x7 bitmap font for the digits 0-9 (one byte per row, low 5 bits). */
static const unsigned char FONT5x7[10][7] = {
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
};

/* Encode RGBA pixels as a 32-bit uncompressed TGA (top-left origin). The engine
 * decodes dice sprites with stb_image, which reads TGA — so no image-write
 * dependency is needed here, and this links cleanly in both the shared and the
 * static (ASan) builds. Returns malloc'd bytes. */
static unsigned char* encode_tga32(const unsigned char* rgba, int w, int h, int* out_len) {
    int sz = 18 + w * h * 4;
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    memset(d, 0, 18);
    d[2] = 2;                                   /* uncompressed true-colour */
    d[12] = w & 0xFF; d[13] = (w >> 8) & 0xFF;
    d[14] = h & 0xFF; d[15] = (h >> 8) & 0xFF;
    d[16] = 32;                                 /* bits per pixel           */
    d[17] = 0x28;                               /* top-left origin, 8-bit α  */
    for (int i = 0; i < w * h; ++i) {           /* store BGRA               */
        d[18 + i*4 + 0] = rgba[i*4 + 2];
        d[18 + i*4 + 1] = rgba[i*4 + 1];
        d[18 + i*4 + 2] = rgba[i*4 + 0];
        d[18 + i*4 + 3] = rgba[i*4 + 3];
    }
    *out_len = sz;
    return d;
}

#define FACE_PX 160

static void put_px(unsigned char* img, int x, int y, const unsigned char c[4]) {
    if (x < 0 || y < 0 || x >= FACE_PX || y >= FACE_PX) return;
    unsigned char* p = &img[(y * FACE_PX + x) * 4];
    p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
}

/* Draw a filled scaled digit glyph with its top-left at (ox,oy). */
static void draw_digit(unsigned char* img, int d, int ox, int oy, int sc,
                       const unsigned char fg[4]) {
    if (d < 0 || d > 9) return;
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            if (FONT5x7[d][row] & (1 << (4 - col)))
                for (int yy = 0; yy < sc; ++yy)
                    for (int xx = 0; xx < sc; ++xx)
                        put_px(img, ox + col * sc + xx, oy + row * sc + yy, fg);
}

/* Render one die face: a rounded, bordered tile of colour `bg` with the value
 * printed large in `fg`, encoded to a PNG in memory (*out / *out_len). */
static void make_face(int value, const unsigned char bg[3], const unsigned char fg[3],
                      unsigned char** out, int* out_len) {
    unsigned char* img = (unsigned char*)malloc(FACE_PX * FACE_PX * 4);
    const int R = 22;                 /* corner radius */
    unsigned char border[4] = { (unsigned char)(bg[0]*0.6f), (unsigned char)(bg[1]*0.6f),
                                (unsigned char)(bg[2]*0.6f), 255 };
    for (int y = 0; y < FACE_PX; ++y)
        for (int x = 0; x < FACE_PX; ++x) {
            /* rounded-rect coverage + transparent outside */
            int dx = x < R ? R - x : (x >= FACE_PX - R ? x - (FACE_PX - R - 1) : 0);
            int dy = y < R ? R - y : (y >= FACE_PX - R ? y - (FACE_PX - R - 1) : 0);
            bool inside = (dx * dx + dy * dy) <= R * R;
            unsigned char c[4];
            if (!inside) { c[0]=c[1]=c[2]=c[3]=0; }
            else {
                bool edge = (dx * dx + dy * dy) >= (R - 5) * (R - 5) ||
                            x < 5 || y < 5 || x >= FACE_PX - 5 || y >= FACE_PX - 5;
                if (edge) { c[0]=border[0]; c[1]=border[1]; c[2]=border[2]; c[3]=255; }
                else      { c[0]=bg[0]; c[1]=bg[1]; c[2]=bg[2]; c[3]=255; }
            }
            put_px(img, x, y, c);
        }

    /* draw the value centred */
    char num[8]; snprintf(num, sizeof num, "%d", value);
    int ndig = (int)strlen(num);
    int sc = ndig >= 2 ? 12 : 16;
    int gw = 5 * sc, gap = sc;
    int total = ndig * gw + (ndig - 1) * gap;
    int ox = (FACE_PX - total) / 2, oy = (FACE_PX - 7 * sc) / 2;
    unsigned char fg4[4] = { fg[0], fg[1], fg[2], 255 };
    for (int i = 0; i < ndig; ++i)
        draw_digit(img, num[i] - '0', ox + i * (gw + gap), oy, sc, fg4);

    *out = encode_tga32(img, FACE_PX, FACE_PX, out_len);
    free(img);
}

/* ------------------------------------------------------------------ dice defs */
typedef struct {
    TesseraDefId def;
    uint32_t     faces;
    float        size;
    float        x;         /* rest position on the table (z = 0) */
} DieType;

static DieType g_types[8];
static int     g_ntypes = 0;

/* Register one die def with `faces` faces, `bg`/`fg` colours, freeing the
 * generated in-memory sprites after the engine has copied them. */
static void register_die(TesseraEngine* e, uint32_t faces, float size, float x,
                         const unsigned char bg[3], const unsigned char fg[3]) {
    TesseraDiceFace* fdefs = (TesseraDiceFace*)calloc(faces, sizeof *fdefs);
    unsigned char** blobs = (unsigned char**)calloc(faces, sizeof *blobs);
    for (uint32_t f = 0; f < faces; ++f) {
        int len = 0;
        make_face((int)f + 1, bg, fg, &blobs[f], &len);
        fdefs[f].sprite.data = blobs[f];
        fdefs[f].sprite.size = (size_t)len;
    }
    TesseraDiceDef dd = {0};
    dd.faces = fdefs;
    dd.face_count = faces;
    dd.size = size;
    dd.tint[0] = dd.tint[1] = dd.tint[2] = dd.tint[3] = 1.0f;
    TesseraDefId id = tessera_register_dice_def(e, &dd);
    if (!id) fprintf(stderr, "register_dice(%u) failed: %s\n", faces, tessera_last_error(e));

    for (uint32_t f = 0; f < faces; ++f) free(blobs[f]);
    free(blobs); free(fdefs);

    g_types[g_ntypes].def = id;
    g_types[g_ntypes].faces = faces;
    g_types[g_ntypes].size = size;
    g_types[g_ntypes].x = x;
    g_ntypes++;
}

/* Throw every die type to a fresh random face + trajectory. */
static void throw_all(TesseraEngine* e, unsigned* rng) {
    for (int i = 0; i < g_ntypes; ++i) {
        if (!g_types[i].def) continue;
        *rng = *rng * 1664525u + 1013904223u;
        uint32_t face = (*rng >> 8) % g_types[i].faces;
        uint32_t seed = (*rng >> 3);
        TesseraDiceThrow t = {0};
        t.id = (TesseraDiceId)(i + 1);
        t.def = g_types[i].def;
        t.face = face;
        t.position[0] = g_types[i].x;
        t.position[1] = 0.55f;           /* float above the felt */
        t.position[2] = 0.0f;
        t.seed = seed;
        t.throw_s = 1.2f;
        tessera_add_dice(e, &t);
    }
}

/* ------------------------------------------------------------------ scene */
static TesseraDefId g_felt = 0;

static void build_scene(TesseraEngine* e) {
    TesseraTileDef felt = {0};
    felt.tint[0] = 0.10f; felt.tint[1] = 0.42f; felt.tint[2] = 0.24f; felt.tint[3] = 1.0f;
    felt.thickness = 0.2f;
    g_felt = tessera_register_tile_def(e, &felt);

    static TesseraTilePlacement tiles[15 * 9];
    size_t n = 0;
    for (int z = -4; z <= 4; ++z)
        for (int x = -7; x <= 7; ++x) {
            tiles[n].coord.x = x; tiles[n].coord.y = z;
            tiles[n].tile_def = g_felt;
            tiles[n].variant = 0;
            tiles[n].id = 0;
            n++;
        }

    TesseraState st = {0};
    st.tiles = tiles; st.tile_count = n;
    st.camera.focus.x = 0.0f; st.camera.focus.y = 0.0f;
    st.camera.distance = 13.5f;
    st.camera.yaw = 0.28f;
    st.camera.pitch = 0.80f;
    st.camera.fov = 0.9f;
    tessera_set_state(e, &st);
}

/* ------------------------------------------------------------------ demo */
static int run_demo(TesseraEngine* e, const char* dir, int frames) {
    build_scene(e);
    /* settle the table / camera in */
    for (int i = 0; i < 30; ++i) tessera_tick(e, 1.0 / 60.0);

    unsigned rng = 0xD1CE5EEDu;
    throw_all(e, &rng);

    int W = 1280, H = 800;
    unsigned char* buf = (unsigned char*)malloc((size_t)W * H * 4);
    char path[1024];
    for (int f = 0; f < frames; ++f) {
        /* render_rgba advances the animation (offscreen, headless); capture_png
         * then encodes the frame to disk via the library. */
        if (!tessera_render_rgba(e, 1.0 / 60.0, W, H, buf, (size_t)W * H * 4)) {
            fprintf(stderr, "render_rgba failed: %s\n", tessera_last_error(e));
            free(buf); return 3;
        }
        snprintf(path, sizeof path, "%s/dice_%03d.png", dir, f);
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

    /* the classic dice set: a two-sided token plus the five Platonic solids,
     * spread along x and floating above the felt. Platonic solids are sized by
     * their bounding sphere, so they get a slightly larger `size` than the cube
     * (which is sized face-to-face) to look comparable on the table. */
    const unsigned char gold[3]  = {214, 176, 74},  brown[3] = {60, 44, 20};
    const unsigned char teal[3]  = {58, 168, 156},  ink[3]   = {30, 44, 44};
    const unsigned char ivory[3] = {235, 232, 220}, dark[3]  = {40, 40, 48};
    const unsigned char blue[3]  = {70, 110, 200},  white[3] = {245, 245, 250};
    const unsigned char amber[3] = {224, 150, 60},  cocoa[3] = {50, 30, 12};
    const unsigned char red[3]   = {196, 66, 66},   cream[3] = {245, 240, 235};
    register_die(e, 2,  1.05f, -5.0f, gold,  brown);  /* d2 coin / token   */
    register_die(e, 4,  1.45f, -3.0f, teal,  ink);    /* d4 tetrahedron    */
    register_die(e, 6,  1.55f, -1.0f, ivory, dark);   /* d6 cube           */
    register_die(e, 8,  1.30f,  1.0f, blue,  white);  /* d8 octahedron     */
    register_die(e, 12, 1.30f,  3.0f, amber, cocoa);  /* d12 dodecahedron  */
    register_die(e, 20, 1.30f,  5.0f, red,   cream);  /* d20 icosahedron   */

    if (demo_dir) {
        int rc = run_demo(e, demo_dir, frames);
        tessera_destroy(e);
        return rc;
    }

    build_scene(e);
    unsigned rng = (unsigned)SDL_GetTicks() | 1u;
    for (int i = 0; i < 20; ++i) tessera_tick(e, 1.0 / 60.0);
    throw_all(e, &rng);

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
                case SDLK_SPACE:
                case SDLK_R: throw_all(e, &rng); break;
                case SDLK_C: tessera_clear_dice(e); break;
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
