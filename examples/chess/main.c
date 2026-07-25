/* examples/chess — a full game of chess rendered with Tessera.
 *
 * Architecture — the classic reducer loop the brief asks for:
 *
 *     PrevState + Action  --game_apply-->  NewState
 *                                              |
 *                                        build_visual
 *                                              v
 *                                         VisualState  ==push==>  Renderer
 *
 *   * GameState  : the pure, immutable rules state (board, side, castling, ep).
 *   * Action     : a single move (from, to, promotion, castle/ep flags).
 *   * game_apply : (PrevState, Action) -> NewState, no side effects.
 *   * VisualState: a projection of GameState into Tessera entity placements +
 *                  the camera, which always sits on the side of the player to
 *                  move. Pushed to the engine, which diffs & animates it.
 *
 * Each side "picks an item and moves it" via a small heuristic player
 * (ai_pick_action) over the fully legal move list. No UI — just the board view.
 *
 *   Interactive:  click a piece of the side to move, then click a destination.
 *                 SPACE = let the AI play the side to move.  A = autoplay toggle.
 *                 R = new game.  ESC = quit.
 *   Headless:     --demo <dir>  plays a game and writes a PNG per ply.
 */
#include "tessera.h"
#include "chess_gen.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== *
 *  Rules state
 * ===================================================================== */

enum { WHITE = 0, BLACK = 1 };

/* Board square: kind in 1..6 (CHESS_*), sign carries color: +white, -black. */
typedef struct {
    int8_t   sq[64];     /* rank*8 + file */
    uint8_t  side;       /* side to move */
    uint8_t  castle;     /* bit0 WK, 1 WQ, 2 BK, 3 BQ */
    int8_t   ep;         /* en-passant target square, or -1 */
    uint16_t ply;
} GameState;

typedef struct {
    int8_t from, to;     /* squares */
    int8_t promo;        /* promoted kind, or 0 */
    uint8_t is_castle;   /* 1 king-side, 2 queen-side */
    uint8_t is_ep;       /* en-passant capture */
} Action;

static inline int sq_of(int f, int r) { return r * 8 + f; }
static inline int file_of(int s) { return s & 7; }
static inline int rank_of(int s) { return s >> 3; }
static inline int piece_kind(int8_t v) { return v < 0 ? -v : v; }
static inline int piece_color(int8_t v) { return v < 0 ? BLACK : WHITE; }

static void game_init(GameState* g) {
    memset(g, 0, sizeof *g);
    const int back[8] = { CHESS_ROOK, CHESS_KNIGHT, CHESS_BISHOP, CHESS_QUEEN,
                          CHESS_KING, CHESS_BISHOP, CHESS_KNIGHT, CHESS_ROOK };
    for (int f = 0; f < 8; ++f) {
        g->sq[sq_of(f, 0)] = (int8_t)back[f];          /* white back rank */
        g->sq[sq_of(f, 1)] = (int8_t)CHESS_PAWN;       /* white pawns      */
        g->sq[sq_of(f, 6)] = (int8_t)(-CHESS_PAWN);    /* black pawns      */
        g->sq[sq_of(f, 7)] = (int8_t)(-back[f]);       /* black back rank  */
    }
    g->side = WHITE;
    g->castle = 0x0F;
    g->ep = -1;
}

/* --- attack test: is `s` attacked by `by` (WHITE/BLACK)? ----------------- */
static bool is_attacked(const GameState* g, int s, int by) {
    int tf = file_of(s), tr = rank_of(s);
    int sign = (by == WHITE) ? 1 : -1;

    /* pawns (attack "forward" from `by`'s perspective) */
    int pr = tr - (by == WHITE ? 1 : -1);            /* rank the pawn would sit on */
    if (pr >= 0 && pr < 8) {
        for (int df = -1; df <= 1; df += 2) {
            int pf = tf + df;
            if (pf < 0 || pf > 7) continue;
            if (g->sq[sq_of(pf, pr)] == (int8_t)(sign * CHESS_PAWN)) return true;
        }
    }
    /* knights */
    static const int kn[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    for (int i = 0; i < 8; ++i) {
        int f = tf + kn[i][0], r = tr + kn[i][1];
        if (f < 0 || f > 7 || r < 0 || r > 7) continue;
        if (g->sq[sq_of(f, r)] == (int8_t)(sign * CHESS_KNIGHT)) return true;
    }
    /* king */
    for (int df = -1; df <= 1; ++df) for (int dr = -1; dr <= 1; ++dr) {
        if (!df && !dr) continue;
        int f = tf + df, r = tr + dr;
        if (f < 0 || f > 7 || r < 0 || r > 7) continue;
        if (g->sq[sq_of(f, r)] == (int8_t)(sign * CHESS_KING)) return true;
    }
    /* sliders: rook/queen orthogonal, bishop/queen diagonal */
    static const int orth[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    static const int diag[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int d = 0; d < 4; ++d) {
        int f = tf, r = tr;
        for (;;) {
            f += orth[d][0]; r += orth[d][1];
            if (f < 0 || f > 7 || r < 0 || r > 7) break;
            int8_t v = g->sq[sq_of(f, r)];
            if (v == 0) continue;
            if (piece_color(v) == by &&
                (piece_kind(v) == CHESS_ROOK || piece_kind(v) == CHESS_QUEEN)) return true;
            break;
        }
    }
    for (int d = 0; d < 4; ++d) {
        int f = tf, r = tr;
        for (;;) {
            f += diag[d][0]; r += diag[d][1];
            if (f < 0 || f > 7 || r < 0 || r > 7) break;
            int8_t v = g->sq[sq_of(f, r)];
            if (v == 0) continue;
            if (piece_color(v) == by &&
                (piece_kind(v) == CHESS_BISHOP || piece_kind(v) == CHESS_QUEEN)) return true;
            break;
        }
    }
    return false;
}

static int king_square(const GameState* g, int color) {
    int8_t want = (int8_t)((color == WHITE ? 1 : -1) * CHESS_KING);
    for (int s = 0; s < 64; ++s) if (g->sq[s] == want) return s;
    return -1;
}

/* ===================================================================== *
 *  game_apply : (PrevState, Action) -> NewState   (pure)
 * ===================================================================== */
static void game_apply(const GameState* prev, Action a, GameState* next) {
    *next = *prev;
    int8_t moving = next->sq[a.from];
    int color = piece_color(moving);
    int kind = piece_kind(moving);

    next->ep = -1;

    /* en-passant: remove the pawn that was passed */
    if (a.is_ep) {
        int cap_r = rank_of(a.to) - (color == WHITE ? 1 : -1);
        next->sq[sq_of(file_of(a.to), cap_r)] = 0;
    }

    /* move the piece (with promotion) */
    next->sq[a.from] = 0;
    next->sq[a.to] = a.promo ? (int8_t)((color == WHITE ? 1 : -1) * a.promo) : moving;

    /* castling: shuffle the rook */
    if (a.is_castle) {
        int r = rank_of(a.from);
        if (a.is_castle == 1) {                       /* king-side */
            next->sq[sq_of(5, r)] = next->sq[sq_of(7, r)];
            next->sq[sq_of(7, r)] = 0;
        } else {                                      /* queen-side */
            next->sq[sq_of(3, r)] = next->sq[sq_of(0, r)];
            next->sq[sq_of(0, r)] = 0;
        }
    }

    /* double pawn push sets the ep target */
    if (kind == CHESS_PAWN && abs(rank_of(a.to) - rank_of(a.from)) == 2)
        next->ep = (int8_t)sq_of(file_of(a.from), (rank_of(a.from) + rank_of(a.to)) / 2);

    /* update castling rights */
    if (kind == CHESS_KING) next->castle &= (color == WHITE) ? ~0x03 : ~0x0C;
    if (a.from == sq_of(0,0) || a.to == sq_of(0,0)) next->castle &= ~0x02;
    if (a.from == sq_of(7,0) || a.to == sq_of(7,0)) next->castle &= ~0x01;
    if (a.from == sq_of(0,7) || a.to == sq_of(0,7)) next->castle &= ~0x08;
    if (a.from == sq_of(7,7) || a.to == sq_of(7,7)) next->castle &= ~0x04;

    next->side = (uint8_t)(color ^ 1);
    next->ply = (uint16_t)(prev->ply + 1);
}

/* ===================================================================== *
 *  Move generation (pseudo-legal, then filtered to fully legal)
 * ===================================================================== */
static void add_pawn(Action* list, int* n, int from, int to, bool promo, bool ep) {
    if (promo) {
        list[(*n)++] = (Action){ (int8_t)from, (int8_t)to, CHESS_QUEEN, 0, 0 };
    } else {
        list[(*n)++] = (Action){ (int8_t)from, (int8_t)to, 0, 0, (uint8_t)ep };
    }
}

static int gen_pseudo(const GameState* g, Action* list) {
    int n = 0, me = g->side, sign = (me == WHITE) ? 1 : -1;
    for (int s = 0; s < 64; ++s) {
        int8_t v = g->sq[s];
        if (v == 0 || piece_color(v) != me) continue;
        int f = file_of(s), r = rank_of(s), kind = piece_kind(v);

        if (kind == CHESS_PAWN) {
            int dr = (me == WHITE) ? 1 : -1;
            int start = (me == WHITE) ? 1 : 6, last = (me == WHITE) ? 7 : 0;
            int r1 = r + dr;
            if (r1 >= 0 && r1 < 8 && g->sq[sq_of(f, r1)] == 0) {
                add_pawn(list, &n, s, sq_of(f, r1), r1 == last, false);
                int r2 = r + 2 * dr;
                if (r == start && g->sq[sq_of(f, r2)] == 0)
                    add_pawn(list, &n, s, sq_of(f, r2), false, false);
            }
            for (int df = -1; df <= 1; df += 2) {
                int cf = f + df;
                if (cf < 0 || cf > 7 || r1 < 0 || r1 >= 8) continue;
                int ts = sq_of(cf, r1);
                int8_t tv = g->sq[ts];
                if (tv != 0 && piece_color(tv) != me)
                    add_pawn(list, &n, s, ts, r1 == last, false);
                else if (ts == g->ep)
                    add_pawn(list, &n, s, ts, false, true);
            }
        } else if (kind == CHESS_KNIGHT) {
            static const int kn[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
            for (int i = 0; i < 8; ++i) {
                int nf = f + kn[i][0], nr = r + kn[i][1];
                if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                int8_t tv = g->sq[sq_of(nf, nr)];
                if (tv == 0 || piece_color(tv) != me)
                    list[n++] = (Action){ (int8_t)s, (int8_t)sq_of(nf, nr), 0, 0, 0 };
            }
        } else if (kind == CHESS_KING) {
            for (int df = -1; df <= 1; ++df) for (int dr = -1; dr <= 1; ++dr) {
                if (!df && !dr) continue;
                int nf = f + df, nr = r + dr;
                if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                int8_t tv = g->sq[sq_of(nf, nr)];
                if (tv == 0 || piece_color(tv) != me)
                    list[n++] = (Action){ (int8_t)s, (int8_t)sq_of(nf, nr), 0, 0, 0 };
            }
            /* castling: rights set, squares empty, king not passing through check */
            int hr = (me == WHITE) ? 0 : 7;
            int kbit = (me == WHITE) ? 0x01 : 0x04, qbit = (me == WHITE) ? 0x02 : 0x08;
            if ((g->castle & kbit) && g->sq[sq_of(5,hr)] == 0 && g->sq[sq_of(6,hr)] == 0 &&
                !is_attacked(g, sq_of(4,hr), me^1) && !is_attacked(g, sq_of(5,hr), me^1) &&
                !is_attacked(g, sq_of(6,hr), me^1))
                list[n++] = (Action){ (int8_t)sq_of(4,hr), (int8_t)sq_of(6,hr), 0, 1, 0 };
            if ((g->castle & qbit) && g->sq[sq_of(3,hr)] == 0 && g->sq[sq_of(2,hr)] == 0 &&
                g->sq[sq_of(1,hr)] == 0 &&
                !is_attacked(g, sq_of(4,hr), me^1) && !is_attacked(g, sq_of(3,hr), me^1) &&
                !is_attacked(g, sq_of(2,hr), me^1))
                list[n++] = (Action){ (int8_t)sq_of(4,hr), (int8_t)sq_of(2,hr), 0, 2, 0 };
        } else {
            /* sliders */
            const int (*dirs)[2]; int nd;
            static const int orth[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            static const int diag[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
            static const int all[8][2]  = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
            if (kind == CHESS_ROOK)   { dirs = orth; nd = 4; }
            else if (kind == CHESS_BISHOP) { dirs = diag; nd = 4; }
            else { dirs = all; nd = 8; }             /* queen */
            for (int d = 0; d < nd; ++d) {
                int nf = f, nr = r;
                for (;;) {
                    nf += dirs[d][0]; nr += dirs[d][1];
                    if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
                    int8_t tv = g->sq[sq_of(nf, nr)];
                    if (tv == 0) { list[n++] = (Action){ (int8_t)s, (int8_t)sq_of(nf, nr), 0, 0, 0 }; continue; }
                    if (piece_color(tv) != me) list[n++] = (Action){ (int8_t)s, (int8_t)sq_of(nf, nr), 0, 0, 0 };
                    break;
                }
            }
        }
    }
    return n;
}

/* Fully legal moves: pseudo-legal minus those leaving our king in check. */
static int gen_legal(const GameState* g, Action* out) {
    Action pseudo[256];
    int np = gen_pseudo(g, pseudo), n = 0, me = g->side;
    for (int i = 0; i < np; ++i) {
        GameState nx;
        game_apply(g, pseudo[i], &nx);
        int ks = king_square(&nx, me);
        if (ks < 0 || !is_attacked(&nx, ks, me ^ 1)) out[n++] = pseudo[i];
    }
    return n;
}

/* ===================================================================== *
 *  The "player": pick an item and move it (heuristic over legal moves).
 * ===================================================================== */
static uint32_t g_rng = 0x1234abcdu;
static uint32_t xrand(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static int piece_value(int kind) {
    static const int val[7] = { 0, 1, 3, 3, 5, 9, 0 };
    return val[kind];
}

static bool ai_pick_action(const GameState* g, Action* out) {
    Action moves[256];
    int n = gen_legal(g, moves);
    if (n == 0) return false;

    int best = -1000000, best_i = 0;
    for (int i = 0; i < n; ++i) {
        Action a = moves[i];
        int score = 0;
        int8_t victim = g->sq[a.to];
        if (victim) score += 100 * piece_value(piece_kind(victim));   /* grab material */
        if (a.promo) score += 90;
        if (a.is_castle) score += 25;                                 /* king safety */
        /* centre control for the destination */
        int df = file_of(a.to), dr = rank_of(a.to);
        int cf = df < 4 ? df : 7 - df, cr = dr < 4 ? dr : 7 - dr;
        score += (cf + cr) * 2;
        /* develop minor pieces off the back rank early */
        int kind = piece_kind(g->sq[a.from]);
        int home = (g->side == WHITE) ? 0 : 7;
        if ((kind == CHESS_KNIGHT || kind == CHESS_BISHOP) && rank_of(a.from) == home) score += 14;
        if (kind == CHESS_PAWN) score += 4;
        /* don't shuffle the king around for no reason */
        if (kind == CHESS_KING && !a.is_castle) score -= 12;
        /* avoid moving onto a square attacked by an enemy pawn */
        GameState nx; game_apply(g, a, &nx);
        if (is_attacked(&nx, a.to, g->side ^ 1)) score -= 8 * piece_value(kind);
        score += (int)(xrand() % 6);                                  /* variety */
        if (score > best) { best = score; best_i = i; }
    }
    *out = moves[best_i];
    return true;
}

/* ===================================================================== *
 *  Controller: threads a persistent entity id through each square so the
 *  renderer animates a *move* (a piece sliding) rather than a teleport.
 * ===================================================================== */
typedef struct {
    GameState state;
    uint64_t  ids[64];   /* stable entity id occupying each square (0 = none) */
    uint64_t  next_id;
    Action    last;      /* last action applied (for logging) */
    bool      has_last;
    bool      game_over;
} Game;

static void ctrl_init(Game* c) {
    game_init(&c->state);
    memset(c->ids, 0, sizeof c->ids);
    uint64_t id = 1;
    for (int s = 0; s < 64; ++s) if (c->state.sq[s]) c->ids[s] = id++;
    c->next_id = id;
    c->has_last = false;
    c->game_over = false;
}

/* Apply an action to BOTH the rules state and the id grid, in lockstep. */
static void ctrl_apply(Game* c, Action a) {
    int color = piece_color(c->state.sq[a.from]);

    if (a.is_ep) {
        int cap_r = rank_of(a.to) - (color == WHITE ? 1 : -1);
        c->ids[sq_of(file_of(a.to), cap_r)] = 0;      /* captured pawn vanishes */
    }
    c->ids[a.to] = c->ids[a.from];                    /* mover keeps its id (slides) */
    c->ids[a.from] = 0;
    if (a.is_castle) {
        int r = rank_of(a.from);
        if (a.is_castle == 1) { c->ids[sq_of(5,r)] = c->ids[sq_of(7,r)]; c->ids[sq_of(7,r)] = 0; }
        else                  { c->ids[sq_of(3,r)] = c->ids[sq_of(0,r)]; c->ids[sq_of(0,r)] = 0; }
    }

    GameState nx;
    game_apply(&c->state, a, &nx);
    c->state = nx;
    c->last = a; c->has_last = true;

    Action tmp[256];
    if (gen_legal(&c->state, tmp) == 0) c->game_over = true;
}

/* ===================================================================== *
 *  Definitions + visual projection
 * ===================================================================== */
#define BOARD_OFF (-4)                 /* file/rank 0..7 -> world -4..3 */

static TesseraDefId d_tile_light, d_tile_dark;
static TesseraDefId d_piece[2][7];     /* [color][kind] */
static TesseraDefId d_font;            /* 0 when no system font was found */
static const char* KIND_CHAR = " PNBRQK";

/* A TrueType font for the board labels (coordinates + turn banner). */
static const char* find_font_path(void) {
    static const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    const char* env = getenv("TESSERA_FONT");
    if (env && env[0]) return env;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; ++i) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

static void register_defs(TesseraEngine* e) {
    d_tile_light = tessera_register_tile_def(e, &(TesseraTileDef){
        .thickness = 0.22f, .tint = {0.82f, 0.78f, 0.68f, 1.0f} });
    d_tile_dark  = tessera_register_tile_def(e, &(TesseraTileDef){
        .thickness = 0.22f, .tint = {0.32f, 0.36f, 0.42f, 1.0f} });

    /* slightly warm ivory vs. cool charcoal, baked into each model's material */
    const float white_col[4] = { 0.90f, 0.87f, 0.80f, 1.0f };
    const float black_col[4] = { 0.16f, 0.17f, 0.20f, 1.0f };

    /* smoke poof played when a piece is captured (despawn) */
    TesseraParticleSpec poof = {
        .mode = TESSERA_EMIT_BURST, .count = 44, .lifetime_s = 0.7f, .lifetime_var = 0.25f,
        .speed = 1.6f, .speed_var = 0.6f, .spread_deg = 55.0f, .gravity = 0.8f,
        .size_start = 0.14f, .size_end = 0.5f,
        .color_start = {0.85f, 0.85f, 0.9f, 0.75f}, .color_end = {0.5f, 0.5f, 0.55f, 0.0f},
        .blend = TESSERA_BLEND_ALPHA };
    TesseraDefId d_poof = tessera_register_effect_def(e, &(TesseraEffectDef){ .on_remove = poof });

    const char* font_path = find_font_path();
    d_font = font_path
        ? tessera_register_font(e, &(TesseraBytes){ .path = font_path }, 56.0f)
        : 0;
    if (!d_font) fprintf(stderr, "chess: no font found; labels disabled\n");

    for (int color = 0; color < 2; ++color) {
        const float* col = (color == WHITE) ? white_col : black_col;
        int face = (color == WHITE) ? 1 : -1;      /* knights face the enemy */
        for (int kind = CHESS_PAWN; kind <= CHESS_KING; ++kind) {
            size_t sz = 0;
            uint8_t* glb = chess_build_piece_glb(kind, face, col, &sz);
            d_piece[color][kind] = tessera_register_entity_def(e, &(TesseraEntityDef){
                .gltf = { .data = glb, .size = sz, .debug_name = "chesspiece" },
                .scale = 1.1f, .on_despawn_effect = d_poof });
            free(glb);
        }
    }
}

/* Orbit distance (zoom); auto-fitted to the board corners on resize. */
static float g_cam_distance = 11.5f;

/* Stable per-tile instance id, so the corners can be projected / fitted. Board
 * square s is placed at coord (file+BOARD_OFF, rank+BOARD_OFF); id = s + 1 (1 =
 * a1 .. 64 = h8; 0 is reserved for "no id"). */
static TesseraTileId tile_id(int square) { return (TesseraTileId)(square + 1); }

/* Camera pose for the side to move: it swings behind that player. */
static TesseraCamera side_camera(int side) {
    /* yaw = PI puts the eye on white's side (-z); yaw = 0 on black's side (+z). */
    float yaw = (side == WHITE) ? 3.14159f : 0.0f;
    /* Board files/ranks 0..7 map to grid -4..3, so the true centre of the board
     * sits between tiles at (-0.5, -0.5) rather than on any single tile. */
    return (TesseraCamera){ .focus = { BOARD_OFF + 3.5f, BOARD_OFF + 3.5f }, .distance = g_cam_distance,
                            .yaw = yaw, .pitch = 0.82f, .fov = 0.72f };
}

/* Zoom so all four board corners stay on screen at the current window size. */
static void refit_camera(TesseraEngine* e) {
    TesseraTileId corners[4] = {
        tile_id(sq_of(0, 0)), tile_id(sq_of(7, 0)),
        tile_id(sq_of(0, 7)), tile_id(sq_of(7, 7)),
    };
    float d;
    if (tessera_camera_fit_distance(e, corners, 4, NULL, 0, 0.06f, &d))
        g_cam_distance = d;
}

/* Legal-move overlays for the piece on square `sel` (-1 = none): a pulsing
 * gold ring on the selected square, a green disc on each quiet destination and
 * a red ring on each capture. Returns the overlay count. */
static size_t build_overlays(const Game* c, int sel, TesseraOverlayPlacement* out) {
    if (sel < 0 || c->game_over) return 0;
    size_t n = 0;
    out[n++] = (TesseraOverlayPlacement){
        .coord = { file_of(sel) + BOARD_OFF, rank_of(sel) + BOARD_OFF },
        .shape = TESSERA_OVERLAY_RING,
        .tint = { 1.0f, 0.85f, 0.30f, 0.95f },
        .pulse_s = 1.2f, .pulse_alpha_min = 0.45f, .pulse_alpha_max = 1.0f,
        .pulse_scale_min = 0.90f, .pulse_scale_max = 1.0f };

    Action moves[256];
    int nm = gen_legal(&c->state, moves);
    for (int i = 0; i < nm && n < 64; ++i) {
        if (moves[i].from != sel) continue;
        int to = moves[i].to;
        bool capture = c->state.sq[to] != 0 || moves[i].is_ep;
        out[n++] = capture
            ? (TesseraOverlayPlacement){
                  .coord = { file_of(to) + BOARD_OFF, rank_of(to) + BOARD_OFF },
                  .shape = TESSERA_OVERLAY_RING,
                  .tint = { 1.0f, 0.28f, 0.22f, 0.85f } }
            : (TesseraOverlayPlacement){
                  .coord = { file_of(to) + BOARD_OFF, rank_of(to) + BOARD_OFF },
                  .shape = TESSERA_OVERLAY_DISC,
                  .tint = { 0.30f, 0.95f, 0.45f, 0.55f } };
    }
    return n;
}

/* Selection highlight riding on the selected piece's LIVE entity transform:
 * white selections get a crisp gold outline, black a cool additive glow, so
 * both post-pass styles show up in a demo game. Pulsing, like the ring decal. */
static size_t build_piece_highlights(const Game* c, int sel,
                                     TesseraHighlightPlacement* out) {
    if (sel < 0 || c->game_over || !c->ids[sel]) return 0;
    bool white = piece_color(c->state.sq[sel]) == WHITE;
    out[0] = (TesseraHighlightPlacement){
        .target_id = c->ids[sel],
        .kind  = TESSERA_HIGHLIGHT_ENTITY,
        .style = white ? TESSERA_HIGHLIGHT_OUTLINE : TESSERA_HIGHLIGHT_GLOW,
        .thickness = white ? 4.0f : 12.0f,
        .pulse_s = 1.2f, .pulse_min = 0.55f, .pulse_max = 1.0f };
    if (white)
        memcpy(out[0].color, (float[4]){ 1.0f, 0.85f, 0.30f, 0.95f }, sizeof out[0].color);
    else
        memcpy(out[0].color, (float[4]){ 0.35f, 0.75f, 1.0f, 0.85f }, sizeof out[0].color);
    return 1;
}

/* Text labels: board coordinates along two edges, a turn banner above the
 * board (its text change crossfades each ply), and a marker glued to each
 * king's LIVE entity transform (rides along through castling). */
static size_t build_labels(const Game* c, TesseraLabelPlacement* out) {
    if (!d_font) return 0;
    size_t n = 0;
    for (int f = 0; f < 8; ++f) {              /* files a..h on white's edge */
        TesseraLabelPlacement* l = &out[n++];
        *l = (TesseraLabelPlacement){
            .id = 200 + f, .font = d_font,
            .position = { (float)(f + BOARD_OFF), 0.30f, (float)(BOARD_OFF - 1) + 0.25f },
            .size = 0.42f, .color = { 0.85f, 0.82f, 0.72f, 0.9f }, .billboard = true };
        snprintf(l->text, sizeof l->text, "%c", 'a' + f);
    }
    for (int r = 0; r < 8; ++r) {              /* ranks 1..8 on the a-file edge */
        TesseraLabelPlacement* l = &out[n++];
        *l = (TesseraLabelPlacement){
            .id = 210 + r, .font = d_font,
            .position = { (float)(BOARD_OFF - 1) + 0.25f, 0.30f, (float)(r + BOARD_OFF) },
            .size = 0.42f, .color = { 0.85f, 0.82f, 0.72f, 0.9f }, .billboard = true };
        snprintf(l->text, sizeof l->text, "%d", r + 1);
    }
    {                                          /* turn banner above the board */
        TesseraLabelPlacement* l = &out[n++];
        *l = (TesseraLabelPlacement){
            .id = 100, .font = d_font,
            .position = { BOARD_OFF + 3.5f, 3.1f, BOARD_OFF + 3.5f },
            .size = 0.6f, .color = { 1.0f, 0.85f, 0.35f, 0.95f }, .billboard = true };
        if (c->game_over)
            snprintf(l->text, sizeof l->text, "Game over — ply %u", c->state.ply);
        else
            snprintf(l->text, sizeof l->text, "Ply %u · %s to move",
                     c->state.ply + 1, c->state.side == WHITE ? "White" : "Black");
    }
    for (int s = 0; s < 64; ++s) {             /* a tag riding on each king */
        if (piece_kind(c->state.sq[s]) != CHESS_KING || !c->ids[s]) continue;
        int color = piece_color(c->state.sq[s]);
        TesseraLabelPlacement* l = &out[n++];
        *l = (TesseraLabelPlacement){
            .id = 300 + (TesseraLabelId)color, .font = d_font,
            .anchor = TESSERA_LABEL_ANCHOR_ENTITY, .anchor_id = c->ids[s],
            .position = { 0.0f, 2.1f, 0.0f }, .size = 0.34f,
            .billboard = true };
        if (color == WHITE)
            (void)memcpy(l->color, (float[4]){ 0.95f, 0.93f, 0.85f, 0.85f }, sizeof l->color);
        else
            (void)memcpy(l->color, (float[4]){ 0.45f, 0.48f, 0.60f, 0.9f }, sizeof l->color);
        snprintf(l->text, sizeof l->text, "K");
    }
    return n;
}

/* Build a VisualState from the rules state + id grid (with the legal-move
 * overlays of the piece on `sel`, if any), and push it. */
static void build_and_push_sel(TesseraEngine* e, const Game* c, int sel) {
    TesseraTilePlacement tiles[64];
    TesseraEntityPlacement ents[32];
    TesseraOverlayPlacement ovls[64];
    TesseraLabelPlacement lbls[24];
    TesseraHighlightPlacement hls[1];
    size_t nt = 0, ne = 0;

    for (int r = 0; r < 8; ++r) for (int f = 0; f < 8; ++f) {
        TesseraDefId td = ((f + r) & 1) ? d_tile_light : d_tile_dark;
        tiles[nt++] = (TesseraTilePlacement){
            .coord = { f + BOARD_OFF, r + BOARD_OFF }, .tile_def = td,
            .id = tile_id(sq_of(f, r)) };
    }
    for (int s = 0; s < 64; ++s) {
        int8_t v = c->state.sq[s];
        if (!v || !c->ids[s]) continue;
        int color = piece_color(v), kind = piece_kind(v);
        ents[ne++] = (TesseraEntityPlacement){
            .id = c->ids[s], .def = d_piece[color][kind],
            .coord = { file_of(s) + BOARD_OFF, rank_of(s) + BOARD_OFF }, .facing = 0 };
    }

    size_t no = build_overlays(c, sel, ovls);
    size_t nl = build_labels(c, lbls);
    size_t nh = build_piece_highlights(c, sel, hls);

    TesseraState st = {
        .tiles = tiles, .tile_count = nt,
        .entities = ents, .entity_count = ne,
        .camera = side_camera(c->state.side),
        .epoch = c->state.ply,
        .overlays = no ? ovls : NULL, .overlay_count = no,
        .labels = nl ? lbls : NULL, .label_count = nl,
        .highlights = nh ? hls : NULL, .highlight_count = nh,
    };
    tessera_set_state(e, &st);
}

static void build_and_push(TesseraEngine* e, const Game* c) {
    build_and_push_sel(e, c, -1);
}

/* ===================================================================== *
 *  Helpers
 * ===================================================================== */
static void logfn(void* ud, int level, const char* msg) {
    (void)ud; (void)level; fprintf(stderr, "  %s\n", msg);
}
static void settle(TesseraEngine* e, double secs) {
    const double step = 1.0 / 120.0;
    for (double t = 0; t < secs; t += step) tessera_tick(e, step);
}
static void move_str(Action a, char out[8]) {
    snprintf(out, 8, "%c%d%c%d", 'a' + file_of(a.from), rank_of(a.from) + 1,
             'a' + file_of(a.to), rank_of(a.to) + 1);
}

/* Convert a picked tile coord back to a board square, or -1 if off-board. */
static int coord_to_square(TesseraCoord c) {
    int f = c.x - BOARD_OFF, r = c.y - BOARD_OFF;
    if (f < 0 || f > 7 || r < 0 || r > 7) return -1;
    return sq_of(f, r);
}

/* ===================================================================== *
 *  Headless demo: play a game and write a PNG per ply.
 * ===================================================================== */
static int run_demo(TesseraEngine* e, const char* dir, int max_plies) {
    char path[512];
    Game c; ctrl_init(&c);
    build_and_push(e, &c);
    settle(e, 1.2);
    snprintf(path, sizeof path, "%s/chess_000.png", dir);
    tessera_capture_png(e, 1280, 800, path);
    printf("wrote %s (start)\n", path);

    for (int i = 1; i <= max_plies && !c.game_over; ++i) {
        Action a;
        if (!ai_pick_action(&c.state, &a)) break;
        char ms[8]; move_str(a, ms);
        int mover = piece_kind(c.state.sq[a.from]);
        bool capture = c.state.sq[a.to] != 0 || a.is_ep;
        int side = c.state.side;

        /* "select" the mover: show its legal-move overlays before it moves */
        build_and_push_sel(e, &c, a.from);
        settle(e, 0.6);
        snprintf(path, sizeof path, "%s/chess_%03d_pick.png", dir, i);
        tessera_capture_png(e, 1280, 800, path);

        ctrl_apply(&c, a);
        build_and_push(e, &c);    /* overlays vanish (fade out) with the move */
        settle(e, 0.9);           /* let the slide + any capture poof play out */

        snprintf(path, sizeof path, "%s/chess_%03d.png", dir, i);
        tessera_capture_png(e, 1280, 800, path);
        printf("wrote %s  (%s %c %s%s)\n", path, side == WHITE ? "white" : "black",
               KIND_CHAR[mover], ms, capture ? " x" : "");
    }
    printf("game over after %u plies (%s)\n", c.state.ply,
           c.game_over ? "no legal moves" : "ply cap");
    return 0;
}

/* ===================================================================== *
 *  Interactive
 * ===================================================================== */
int main(int argc, char** argv) {
    const char* demo_dir = NULL;
    int max_plies = 40;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo_dir = argv[++i];
        else if (!strcmp(argv[i], "--plies") && i + 1 < argc) max_plies = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_rng = (uint32_t)strtoul(argv[++i], 0, 0);
    }

    TesseraConfig cfg = { .width = 1280, .height = 800, .pixel_density = 1.0f,
                          .debug = false, .log = logfn };
    TesseraEngine* e = tessera_create(&cfg);
    if (!e || strlen(tessera_last_error(e))) {
        fprintf(stderr, "init failed: %s\n", e ? tessera_last_error(e) : "null");
        return 1;
    }

    TesseraLight light = { .dir = {-0.4f, -1.0f, -0.5f}, .color = {1.0f, 0.97f, 0.9f},
                           .intensity = 1.15f, .ambient = {0.30f, 0.33f, 0.4f} };
    tessera_set_light(e, &light);
    tessera_set_quality(e, &(TesseraQuality){ .shadows = TESSERA_SHADOW_BLOB, .msaa = 1, .render_scale = 1.0f });
    tessera_set_timing(e, &(TesseraTiming){ .move_s = 0.5f, .add_s = 0.35f, .remove_s = 0.4f,
                                            .tile_s = 0.4f, .reflow_s = 0.4f, .camera_s = 0.9f,
                                            .speed_multiplier = 1.0f });
    register_defs(e);

    if (demo_dir) {
        int rc = run_demo(e, demo_dir, max_plies);
        tessera_destroy(e);
        return rc;
    }

    Game c; ctrl_init(&c);
    build_and_push(e, &c);

    int sel = -1;             /* selected source square, or -1 */
    bool autoplay = false;
    double ai_timer = 0.0;
    printf("chess: click a piece then a square to move.  "
           "SPACE=AI move  A=autoplay  R=new game  ESC=quit\n");

    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            else if (ev.type == SDL_EVENT_WINDOW_RESIZED) {
                tessera_resize(e, ev.window.data1, ev.window.data2, 1.0f);
                refit_camera(e);       /* zoom to keep the whole board on screen */
                build_and_push(e, &c); /* re-push so the new distance takes effect */
            }
            else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                     ev.button.button == SDL_BUTTON_LEFT && !c.game_over) {
                TesseraPick pk;
                if (!tessera_pick(e, ev.button.x, ev.button.y, &pk) || !pk.hit_tile) {
                    if (sel >= 0) { sel = -1; build_and_push(e, &c); }  /* clear overlays */
                    continue;
                }
                int s = coord_to_square(pk.tile);
                if (s < 0) {
                    if (sel >= 0) { sel = -1; build_and_push(e, &c); }
                    continue;
                }
                if (sel < 0) {
                    /* select one of our own pieces: show its legal-move overlays */
                    if (c.state.sq[s] && piece_color(c.state.sq[s]) == c.state.side) {
                        sel = s;
                        build_and_push_sel(e, &c, sel);
                    }
                } else {
                    /* try to move sel -> s if it is legal */
                    Action moves[256]; int n = gen_legal(&c.state, moves);
                    Action chosen; bool found = false;
                    for (int i = 0; i < n; ++i)
                        if (moves[i].from == sel && moves[i].to == s) { chosen = moves[i]; found = true; break; }
                    sel = -1;
                    if (found) {
                        char ms[8]; move_str(chosen, ms);
                        printf("%s plays %s\n", c.state.side == WHITE ? "white" : "black", ms);
                        ctrl_apply(&c, chosen);
                        if (c.game_over) printf("game over.\n");
                    }
                    build_and_push(e, &c);   /* move (or just deselect): overlays fade out */
                }
            }
            else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_A: autoplay = !autoplay; printf("autoplay %s\n", autoplay ? "on" : "off"); break;
                case SDLK_R: ctrl_init(&c); build_and_push(e, &c); sel = -1; printf("new game\n"); break;
                case SDLK_SPACE: {
                    Action a;
                    if (!c.game_over && ai_pick_action(&c.state, &a)) {
                        char ms[8]; move_str(a, ms);
                        printf("%s (AI) plays %s\n", c.state.side == WHITE ? "white" : "black", ms);
                        ctrl_apply(&c, a); build_and_push(e, &c); sel = -1;
                        if (c.game_over) printf("game over.\n");
                    }
                    break;
                }
                default: break;
                }
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / freq; prev = now;

        if (autoplay && !c.game_over && tessera_is_idle(e)) {
            ai_timer += dt;
            if (ai_timer > 0.6) {
                ai_timer = 0.0;
                Action a;
                if (ai_pick_action(&c.state, &a)) { ctrl_apply(&c, a); build_and_push(e, &c); }
            }
        }

        tessera_tick(e, dt);
    }

    tessera_destroy(e);
    return 0;
}
