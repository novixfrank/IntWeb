#include "gui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ══════════════════════════════════════════════
 *  Costanti colori
 * ══════════════════════════════════════════════ */
#define C_DARK_SQ    0x6B,0x3A,0x0F
#define C_LIGHT_SQ   0xF0,0xD9,0xB4
#define C_BG         0x28,0x28,0x28
#define C_SIDEBAR    0x1E,0x1E,0x2A
#define C_BLACK_PC   0x20,0x20,0x20
#define C_WHITE_PC   0xF8,0xF8,0xF0
#define C_KING_RING  0xFF,0xD7,0x00
#define C_HINT       0x00,0xCC,0x44
#define C_SEL        0xFF,0xAA,0x00
#define C_LAST       0x66,0xAA,0xFF
#define C_BTN        0x3A,0x3A,0x5A
#define C_BTN_ACT    0x55,0x88,0xFF
#define C_TEXT       255,255,255
#define C_TEXT_DIM   150,150,150
#define C_BORDER     0x44,0x44,0x44

/* ══════════════════════════════════════════════
 *  Struttura argomenti thread AI
 * ══════════════════════════════════════════════ */
typedef struct {
    GUI   *g;
    Board  board;
    MCTSParams params;
} AIThreadArg;

/* ══════════════════════════════════════════════
 *  gui_init
 * ══════════════════════════════════════════════ */
int gui_init(GUI *g) {
    memset(g, 0, sizeof(*g));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        SDL_Quit();
        return -1;
    }

    g->window = SDL_CreateWindow(
        "Dama Italiana – MCTS AI",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!g->window) {
        fprintf(stderr, "CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    g->renderer = SDL_CreateRenderer(g->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g->renderer) {
        fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(g->renderer, SDL_BLENDMODE_BLEND);

    /* Font: cerca percorsi comuni su Linux/macOS/Windows */
    const char *font_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
        NULL
    };
    for (int i = 0; font_paths[i]; i++) {
        g->font_lg = TTF_OpenFont(font_paths[i], 18);
        if (g->font_lg) {
            g->font_sm = TTF_OpenFont(font_paths[i], 12);
            break;
        }
    }
    if (!g->font_lg) {
        fprintf(stderr, "Avviso: font non trovato – testo non disponibile\n");
    }

    /* Pool MCTS */
    board_lookup_init();
    g->pool = mcts_pool_create();

    /* Parametri default */
    g->params_ucb1 = mcts_default_params();
    g->params_ucb1.model = SEL_UCB1;
    g->params_puct = mcts_default_params();
    g->params_puct.model = SEL_PUCT;

    /* Carica parametri tunati se presenti */
    tuning_load("dama_params.txt", &g->params_ucb1, &g->params_puct);

    /* Stato iniziale */
    g->state      = GS_MENU;
    g->sel_sq     = -1;
    g->last_from  = -1;
    g->last_to    = -1;
    g->menu_model = 0;
    g->menu_time  = 1;    /* default 1s */
    g->menu_color = 0;    /* default NERO = umano */
    g->menu_hints = true;
    SDL_AtomicSet(&g->ai_done, 0);

    return 0;
}

/* ══════════════════════════════════════════════
 *  gui_destroy
 * ══════════════════════════════════════════════ */
void gui_destroy(GUI *g) {
    if (g->ai_thread) {
        SDL_WaitThread(g->ai_thread, NULL);
        g->ai_thread = NULL;
    }
    if (g->font_sm)   TTF_CloseFont(g->font_sm);
    if (g->font_lg)   TTF_CloseFont(g->font_lg);
    if (g->renderer)  SDL_DestroyRenderer(g->renderer);
    if (g->window)    SDL_DestroyWindow(g->window);
    mcts_pool_free(g->pool);
    TTF_Quit();
    SDL_Quit();
}

/* ══════════════════════════════════════════════
 *  Helpers coordinate
 * ══════════════════════════════════════════════ */
int gui_px_to_sq(const GUI *g, int px, int py) {
    (void)g;
    int bx = px - BOARD_X, by = py - BOARD_Y;
    if (bx < 0 || bx >= BOARD_SIZE || by < 0 || by >= BOARD_SIZE) return -1;
    int col = bx / CELL_SIZE;
    int row = by / CELL_SIZE;
    return rc_to_sq(row, col);
}

void gui_sq_to_px(const GUI *g, int sq, int *cx, int *cy) {
    (void)g;
    int row, col;
    sq_to_rc(sq, &row, &col);
    *cx = BOARD_X + col * CELL_SIZE + CELL_SIZE / 2;
    *cy = BOARD_Y + row * CELL_SIZE + CELL_SIZE / 2;
}

/* ══════════════════════════════════════════════
 *  Disegno primitivi
 * ══════════════════════════════════════════════ */
void gui_draw_text(GUI *g, TTF_Font *font, const char *txt,
                   int x, int y, SDL_Color col) {
    if (!font || !txt || !*txt) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g->renderer, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(g->renderer, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

static void draw_text_c(GUI *g, TTF_Font *font, const char *txt,
                         int x, int y, int w, SDL_Color col) {
    if (!font || !txt || !*txt) return;
    int tw, th;
    TTF_SizeUTF8(font, txt, &tw, &th);
    gui_draw_text(g, font, txt, x + (w - tw)/2, y, col);
}

void gui_draw_circle_filled(GUI *g, int cx, int cy, int radius,
                             Uint8 r, Uint8 gr, Uint8 b, Uint8 a) {
    SDL_SetRenderDrawColor(g->renderer, r, gr, b, a);
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrtf((float)(radius*radius - dy*dy));
        SDL_RenderDrawLine(g->renderer,
                           cx - dx, cy + dy,
                           cx + dx, cy + dy);
    }
}

static void draw_circle_outline(GUI *g, int cx, int cy, int radius,
                                  Uint8 r, Uint8 gr, Uint8 b, Uint8 a, int thickness) {
    SDL_SetRenderDrawColor(g->renderer, r, gr, b, a);
    for (int t = 0; t < thickness; t++) {
        int rad = radius - t;
        if (rad <= 0) break;
        /* Bresenham circle */
        int x = rad, y = 0, err = 0;
        while (x >= y) {
            SDL_RenderDrawPoint(g->renderer, cx+x, cy+y);
            SDL_RenderDrawPoint(g->renderer, cx+y, cy+x);
            SDL_RenderDrawPoint(g->renderer, cx-y, cy+x);
            SDL_RenderDrawPoint(g->renderer, cx-x, cy+y);
            SDL_RenderDrawPoint(g->renderer, cx-x, cy-y);
            SDL_RenderDrawPoint(g->renderer, cx-y, cy-x);
            SDL_RenderDrawPoint(g->renderer, cx+y, cy-x);
            SDL_RenderDrawPoint(g->renderer, cx+x, cy-y);
            y++;
            err += 1 + 2*y;
            if (2*(err-x) + 1 > 0) { x--; err += 1 - 2*x; }
        }
    }
}

void gui_draw_button(GUI *g, const SDL_Rect *r, const char *lbl, bool active) {
    Uint8 br = active ? 0x55 : 0x3A;
    Uint8 bg = active ? 0x88 : 0x3A;
    Uint8 bb = active ? 0xFF : 0x5A;

    /* Ombra */
    SDL_SetRenderDrawColor(g->renderer, 0, 0, 0, 120);
    SDL_Rect shadow = {r->x+2, r->y+2, r->w, r->h};
    SDL_RenderFillRect(g->renderer, &shadow);

    /* Corpo */
    SDL_SetRenderDrawColor(g->renderer, br, bg, bb, 255);
    SDL_RenderFillRect(g->renderer, r);

    /* Bordo */
    SDL_SetRenderDrawColor(g->renderer, 200, 200, 255, 200);
    SDL_RenderDrawRect(g->renderer, r);

    /* Testo */
    if (g->font_sm && lbl) {
        SDL_Color tc = {255, 255, 255, 255};
        draw_text_c(g, g->font_sm, lbl, r->x, r->y + (r->h - 13)/2, r->w, tc);
    }
}

/* ══════════════════════════════════════════════
 *  Aggiorna mosse legali per casella sel_sq
 * ══════════════════════════════════════════════ */
void gui_update_legal(GUI *g, int sq) {
    Move all[MAX_MOVES];
    int n = board_gen_moves(&g->board, all);
    g->n_legal = 0;
    for (int i = 0; i < n; i++) {
        if (all[i].from == sq) {
            g->legal[g->n_legal++] = all[i];
        }
    }
}

/* ══════════════════════════════════════════════
 *  Avvia nuova partita
 * ══════════════════════════════════════════════ */
void gui_start_new_game(GUI *g) {
    board_init(&g->board);
    g->sel_sq    = -1;
    g->dragging  = false;
    g->last_from = -1;
    g->last_to   = -1;
    g->result    = 0;
    g->result_msg[0] = '\0';
    SDL_AtomicSet(&g->ai_done, 0);

    /* Configura parametri in base al menu */
    double tl_map[] = {0.2, 1.0, 3.0};
    double tl = tl_map[g->menu_time < 3 ? g->menu_time : 1];

    if (g->menu_model == 0) {
        g->cfg.params = g->params_ucb1;
    } else {
        g->cfg.params = g->params_puct;
    }
    g->cfg.params.time_limit = tl;
    g->cfg.human_color = g->menu_color;
    g->cfg.show_hints  = g->menu_hints;
    g->cfg.show_stats  = true;

    g->state = GS_PLAYING;

    /* Se AI deve iniziare (umano = BIANCO, AI = NERO muove per primo) */
    if (g->board.stm != g->cfg.human_color) {
        gui_start_ai(g);
    }
}

/* ══════════════════════════════════════════════
 *  Thread AI
 * ══════════════════════════════════════════════ */
static int ai_thread_fn(void *data) {
    AIThreadArg *arg = (AIThreadArg *)data;
    GUI *g = arg->g;

    double t0 = now_sec();
    Move mv = mcts_best_move(&arg->board, &arg->params, g->pool);
    g->ai_elapsed = now_sec() - t0;
    g->ai_stats   = mcts_get_stats(g->pool);
    g->ai_move    = mv;

    SDL_AtomicSet(&g->ai_done, 1);
    free(arg);
    return 0;
}

void gui_start_ai(GUI *g) {
    g->state = GS_AI_THINK;
    SDL_AtomicSet(&g->ai_done, 0);

    AIThreadArg *arg = malloc(sizeof(AIThreadArg));
    arg->g      = g;
    arg->board  = g->board;
    arg->params = g->cfg.params;

    g->ai_thread = SDL_CreateThread(ai_thread_fn, "AI", arg);
    if (!g->ai_thread) {
        /* fallback sincrono */
        Move mv = mcts_best_move(&g->board, &g->cfg.params, g->pool);
        g->ai_move = mv;
        SDL_AtomicSet(&g->ai_done, 1);
    }
}

/* ══════════════════════════════════════════════
 *  Applica mossa (umano o AI) e controlla fine
 * ══════════════════════════════════════════════ */
void gui_finish_move(GUI *g, const Move *m) {
    g->last_from = m->from;
    g->last_to   = m->to;
    board_do_move(&g->board, m);
    g->sel_sq   = -1;
    g->dragging = false;
    g->n_legal  = 0;
    gui_check_gameover(g);
}

void gui_check_gameover(GUI *g) {
    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&g->board, mvs);
    int r = board_result(&g->board, n);

    if (r != 0 || n == 0) {
        int res = (n == 0) ? ((g->board.stm == BLACK) ? -1 : 1) : r;
        g->result = res;
        g->state  = GS_GAMEOVER;
        if (res == 0)
            snprintf(g->result_msg, sizeof(g->result_msg), "Pareggio!");
        else if (res == 1)
            snprintf(g->result_msg, sizeof(g->result_msg), "NERO vince!");
        else
            snprintf(g->result_msg, sizeof(g->result_msg), "BIANCO vince!");
    } else {
        g->state = GS_PLAYING;
        /* Se tocca all'AI */
        if (g->board.stm != g->cfg.human_color) {
            gui_start_ai(g);
        }
    }
}

/* ══════════════════════════════════════════════
 *  RENDER – Scacchiera + pezzi
 * ══════════════════════════════════════════════ */
static void render_board(GUI *g) {
    /* Caselle */
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            SDL_Rect cell = {
                BOARD_X + col * CELL_SIZE,
                BOARD_Y + row * CELL_SIZE,
                CELL_SIZE, CELL_SIZE
            };
            bool dark = (row + col) % 2 == 1;
            if (dark)
                SDL_SetRenderDrawColor(g->renderer, C_DARK_SQ, 255);
            else
                SDL_SetRenderDrawColor(g->renderer, C_LIGHT_SQ, 255);
            SDL_RenderFillRect(g->renderer, &cell);
        }
    }

    /* Highlight ultima mossa AI */
    if (g->last_from >= 0) {
        int sqs[2] = {g->last_from, g->last_to};
        for (int k = 0; k < 2; k++) {
            int row, col;
            sq_to_rc(sqs[k], &row, &col);
            SDL_Rect cell = {
                BOARD_X + col * CELL_SIZE,
                BOARD_Y + row * CELL_SIZE,
                CELL_SIZE, CELL_SIZE
            };
            SDL_SetRenderDrawColor(g->renderer, C_LAST, 80);
            SDL_RenderFillRect(g->renderer, &cell);
        }
    }

    /* Highlight mosse legali */
    if (g->cfg.show_hints && g->sel_sq >= 0) {
        for (int i = 0; i < g->n_legal; i++) {
            int row, col;
            sq_to_rc(g->legal[i].to, &row, &col);
            int cx = BOARD_X + col * CELL_SIZE + CELL_SIZE/2;
            int cy = BOARD_Y + row * CELL_SIZE + CELL_SIZE/2;
            gui_draw_circle_filled(g, cx, cy, CELL_SIZE/5, C_HINT, 200);
        }
    }

    /* Bordo scacchiera */
    SDL_Rect border = {BOARD_X - 2, BOARD_Y - 2, BOARD_SIZE + 4, BOARD_SIZE + 4};
    SDL_SetRenderDrawColor(g->renderer, C_BORDER, 255);
    SDL_RenderDrawRect(g->renderer, &border);

    /* Coordinata righe/colonne */
    if (g->font_sm) {
        SDL_Color dim = {C_TEXT_DIM, 200};
        char buf[4];
        for (int i = 0; i < 8; i++) {
            snprintf(buf, sizeof(buf), "%d", i);
            gui_draw_text(g, g->font_sm, buf,
                          BOARD_X - 18,
                          BOARD_Y + i * CELL_SIZE + CELL_SIZE/2 - 6,
                          dim);
            gui_draw_text(g, g->font_sm, buf,
                          BOARD_X + i * CELL_SIZE + CELL_SIZE/2 - 4,
                          BOARD_Y + BOARD_SIZE + 4,
                          dim);
        }
    }
}

static void render_piece(GUI *g, int sq, bool selected, bool ghost) {
    int cx, cy;
    gui_sq_to_px(g, sq, &cx, &cy);
    const Board *b = &g->board;
    int radius = CELL_SIZE / 2 - 5;

    bool is_black_man  = (b->bm >> sq) & 1;
    bool is_black_king = (b->bk >> sq) & 1;
    bool is_white_man  = (b->wm >> sq) & 1;
    bool is_white_king = (b->wk >> sq) & 1;
    bool is_black = is_black_man || is_black_king;
    bool is_king  = is_black_king || is_white_king;

    if (!is_black_man && !is_black_king && !is_white_man && !is_white_king) return;

    Uint8 alpha = ghost ? 100 : 255;

    /* Ombra */
    gui_draw_circle_filled(g, cx+3, cy+3, radius, 0, 0, 0, ghost ? 40 : 80);

    /* Corpo */
    if (is_black) {
        /* Gradiente simulato: cerchio più chiaro interno */
        gui_draw_circle_filled(g, cx, cy, radius, 0x30, 0x30, 0x30, alpha);
        gui_draw_circle_filled(g, cx-2, cy-2, radius-4, 0x55, 0x55, 0x55, alpha);
    } else {
        gui_draw_circle_filled(g, cx, cy, radius, 0xE8, 0xE8, 0xE0, alpha);
        gui_draw_circle_filled(g, cx-2, cy-2, radius-4, 0xFF, 0xFF, 0xFF, alpha);
    }

    /* Bordo selezione */
    if (selected) {
        draw_circle_outline(g, cx, cy, radius+1, C_SEL, 255, 3);
    } else {
        Uint8 bc = is_black ? 0x80 : 0xA0;
        draw_circle_outline(g, cx, cy, radius, bc, bc, bc, alpha, 2);
    }

    /* Corona per le dame */
    if (is_king) {
        draw_circle_outline(g, cx, cy, radius-6, C_KING_RING, alpha, 3);
        /* Stella a 6 punte semplificata: tre linee */
        SDL_SetRenderDrawColor(g->renderer, C_KING_RING, alpha);
        int kr = radius - 10;
        for (int k = 0; k < 3; k++) {
            float angle = (float)k * 3.14159f / 3.0f;
            int ax = (int)(kr * cosf(angle));
            int ay = (int)(kr * sinf(angle));
            SDL_RenderDrawLine(g->renderer, cx-ax, cy-ay, cx+ax, cy+ay);
        }
    }
}

static void render_pieces(GUI *g) {
    for (int sq = 0; sq < NUM_SQ; sq++) {
        if (g->dragging && sq == g->sel_sq) continue; /* renderato sotto il cursore */
        render_piece(g, sq,
                     sq == g->sel_sq && !g->dragging,
                     false);
    }

    /* Pezzo in drag: disegnato nella posizione del mouse */
    if (g->dragging && g->sel_sq >= 0) {
        int sq = g->sel_sq;
        const Board *b = &g->board;
        bool is_black = ((b->bm | b->bk) >> sq) & 1;
        bool is_king  = ((b->bk | b->wk) >> sq) & 1;
        int  radius   = CELL_SIZE / 2 - 5;
        int  cx = g->drag_x, cy = g->drag_y;

        gui_draw_circle_filled(g, cx+3, cy+3, radius, 0, 0, 0, 60);
        if (is_black) {
            gui_draw_circle_filled(g, cx, cy, radius, 0x30, 0x30, 0x30, 230);
            gui_draw_circle_filled(g, cx-2, cy-2, radius-4, 0x55, 0x55, 0x55, 230);
        } else {
            gui_draw_circle_filled(g, cx, cy, radius, 0xE8, 0xE8, 0xE0, 230);
            gui_draw_circle_filled(g, cx-2, cy-2, radius-4, 0xFF, 0xFF, 0xFF, 230);
        }
        draw_circle_outline(g, cx, cy, radius, C_SEL, 230, 3);
        if (is_king) {
            draw_circle_outline(g, cx, cy, radius-6, C_KING_RING, 230, 3);
        }
    }
}

/* ══════════════════════════════════════════════
 *  RENDER – Sidebar
 * ══════════════════════════════════════════════ */
static void render_sidebar(GUI *g) {
    SDL_Rect sidebar = {SIDE_X - 5, 10, SIDE_W + 5, WIN_H - 20};
    SDL_SetRenderDrawColor(g->renderer, C_SIDEBAR, 255);
    SDL_RenderFillRect(g->renderer, &sidebar);
    SDL_SetRenderDrawColor(g->renderer, C_BORDER, 255);
    SDL_RenderDrawRect(g->renderer, &sidebar);

    if (!g->font_sm && !g->font_lg) return;

    SDL_Color white  = {C_TEXT, 255};
    SDL_Color dim    = {C_TEXT_DIM, 255};
    SDL_Color yellow = {255, 200, 50, 255};
    SDL_Color green  = {80, 220, 100, 255};

    int x = SIDE_X + 4, y = 20;
    TTF_Font *fl = g->font_lg ? g->font_lg : g->font_sm;
    TTF_Font *fs = g->font_sm ? g->font_sm : g->font_lg;

    /* Titolo */
    gui_draw_text(g, fl, "DAMA ITALIANA", x, y, yellow); y += 26;
    gui_draw_text(g, fs, "MCTS AI", x+2, y, dim); y += 28;

    /* Separatore */
    SDL_SetRenderDrawColor(g->renderer, C_BORDER, 200);
    SDL_RenderDrawLine(g->renderer, SIDE_X, y, WIN_W - 5, y); y += 8;

    if (g->state == GS_PLAYING || g->state == GS_AI_THINK || g->state == GS_GAMEOVER) {
        /* Chi muove */
        const char *mover = (g->board.stm == BLACK) ? "● NERO" : "○ BIANCO";
        SDL_Color mc = (g->board.stm == BLACK) ? dim : white;
        gui_draw_text(g, fl, mover, x, y, mc); y += 24;

        /* Pezzi in campo */
        int bn = __builtin_popcount(g->board.bm);
        int bk = __builtin_popcount(g->board.bk);
        int wn = __builtin_popcount(g->board.wm);
        int wk = __builtin_popcount(g->board.wk);
        char buf[64];
        snprintf(buf, sizeof(buf), "Nero:  %d ped  %d dame", bn, bk);
        gui_draw_text(g, fs, buf, x, y, dim); y += 16;
        snprintf(buf, sizeof(buf), "Bianco:%d ped  %d dame", wn, wk);
        gui_draw_text(g, fs, buf, x, y, dim); y += 20;

        snprintf(buf, sizeof(buf), "Mosso senza cap: %d/80", g->board.nocap);
        gui_draw_text(g, fs, buf, x, y, dim); y += 20;

        SDL_RenderDrawLine(g->renderer, SIDE_X, y, WIN_W - 5, y); y += 8;

        /* Configurazione AI */
        const char *model_name = (g->cfg.params.model == SEL_UCB1) ? "UCB1" : "PUCT";
        double tl = g->cfg.params.time_limit;
        snprintf(buf, sizeof(buf), "AI: %s  %.1fs", model_name, tl);
        gui_draw_text(g, fs, buf, x, y, green); y += 18;

        if (g->cfg.params.model == SEL_UCB1) {
            snprintf(buf, sizeof(buf), "  C = %.3f", g->cfg.params.C);
        } else {
            snprintf(buf, sizeof(buf), "  c_puct = %.3f", g->cfg.params.c_puct);
        }
        gui_draw_text(g, fs, buf, x, y, dim); y += 16;
        snprintf(buf, sizeof(buf), "  prior_cap = %.2f", g->cfg.params.prior_cap);
        gui_draw_text(g, fs, buf, x, y, dim); y += 16;

        SDL_RenderDrawLine(g->renderer, SIDE_X, y, WIN_W - 5, y); y += 8;

        /* Stats ultima mossa AI */
        if (g->ai_stats.total_sims > 0) {
            gui_draw_text(g, fs, "Ultima mossa AI:", x, y, yellow); y += 16;
            snprintf(buf, sizeof(buf), "  Sims: %lld", (long long)g->ai_stats.total_sims);
            gui_draw_text(g, fs, buf, x, y, dim); y += 14;
            snprintf(buf, sizeof(buf), "  Nodi: %d", g->ai_stats.tree_size);
            gui_draw_text(g, fs, buf, x, y, dim); y += 14;
            snprintf(buf, sizeof(buf), "  Tempo: %.2fs", g->ai_elapsed);
            gui_draw_text(g, fs, buf, x, y, dim); y += 14;
            double sps = g->ai_elapsed > 0
                ? (double)g->ai_stats.total_sims / g->ai_elapsed : 0;
            snprintf(buf, sizeof(buf), "  S/s: %.0f", sps);
            gui_draw_text(g, fs, buf, x, y, dim); y += 14;
            float wr = g->ai_stats.best_win_rate;
            snprintf(buf, sizeof(buf), "  Win rate: %.1f%%", wr * 50 + 50);
            gui_draw_text(g, fs, buf, x, y, dim); y += 18;
        }

        if (g->state == GS_AI_THINK) {
            SDL_RenderDrawLine(g->renderer, SIDE_X, y, WIN_W - 5, y); y += 8;
            gui_draw_text(g, fl, "AI pensa...", x, y, yellow); y += 22;
            /* Animazione spinner semplice */
            static int spin = 0;
            spin = (spin + 1) % 8;
            const char *frames[] = {"|","/"," -","\\","|","/"," -","\\"};
            gui_draw_text(g, fl, frames[spin], x + 80, y - 22, white);
        }
    }

    /* Pulsante Nuova Partita */
    y = WIN_H - 90;
    SDL_Rect btn1 = {SIDE_X + 4, y, SIDE_W - 8, 30};
    gui_draw_button(g, &btn1, "Nuova Partita", false); y += 38;

    SDL_Rect btn2 = {SIDE_X + 4, y, SIDE_W - 8, 30};
    gui_draw_button(g, &btn2, "Menu", false);

    /* Gameover overlay in sidebar */
    if (g->state == GS_GAMEOVER) {
        y = WIN_H / 2 - 50;
        SDL_Rect go_bg = {SIDE_X - 5, y - 10, SIDE_W + 10, 70};
        SDL_SetRenderDrawColor(g->renderer, 10, 10, 30, 230);
        SDL_RenderFillRect(g->renderer, &go_bg);
        SDL_SetRenderDrawColor(g->renderer, C_KING_RING, 255);
        SDL_RenderDrawRect(g->renderer, &go_bg);
        SDL_Color win_col = {C_KING_RING, 255};
        draw_text_c(g, fl, "FINE PARTITA", SIDE_X, y, SIDE_W, win_col); y += 28;
        draw_text_c(g, fl, g->result_msg, SIDE_X, y, SIDE_W, white);
    }
}

/* ══════════════════════════════════════════════
 *  RENDER – Menu principale
 * ══════════════════════════════════════════════ */
static void render_menu(GUI *g) {
    /* Sfondo gradiente simulato */
    for (int y = 0; y < WIN_H; y++) {
        Uint8 v = (Uint8)(20 + y * 30 / WIN_H);
        SDL_SetRenderDrawColor(g->renderer, v/3, v/3, v, 255);
        SDL_RenderDrawLine(g->renderer, 0, y, WIN_W, y);
    }

    TTF_Font *fl = g->font_lg;
    TTF_Font *fs = g->font_sm;
    if (!fl && !fs) return;

    SDL_Color white  = {255,255,255,255};
    SDL_Color yellow = {255,210,60,255};
    SDL_Color dim    = {150,150,200,255};

    /* Titolo */
    int cx = WIN_W / 2;
    if (fl) draw_text_c(g, fl, "DAMA ITALIANA", 0, 60, WIN_W, yellow);
    if (fs) draw_text_c(g, fs, "Monte Carlo Tree Search AI", 0, 90, WIN_W, dim);

    /* Box menu */
    int box_x = WIN_W/2 - 160, box_y = 140, box_w = 320, box_h = 390;
    SDL_Rect box = {box_x, box_y, box_w, box_h};
    SDL_SetRenderDrawColor(g->renderer, 15, 15, 40, 220);
    SDL_RenderFillRect(g->renderer, &box);
    SDL_SetRenderDrawColor(g->renderer, 80, 80, 160, 255);
    SDL_RenderDrawRect(g->renderer, &box);

    int x = box_x + 20, y = box_y + 16;

    /* ── Modello ── */
    if (fs) gui_draw_text(g, fs, "MODELLO AI:", x, y, yellow); 
    y += 18;
    {
        const char *models[] = {"UCB1", "PUCT (AlphaGo)"};
        for (int i = 0; i < 2; i++) {
            SDL_Rect r = {x, y, box_w - 40, 26};
            gui_draw_button(g, &r, models[i], g->menu_model == i);
            y += 30;
        }
    }
    y += 6;

    /* ── Tempo ── */
    if (fs) gui_draw_text(g, fs, "TEMPO PER MOSSA:", x, y, yellow); 
    y += 18;
    {
        const char *times[] = {"0.2 sec", "1.0 sec", "3.0 sec"};
        for (int i = 0; i < 3; i++) {
            SDL_Rect r = {x, y, box_w - 40, 26};
            gui_draw_button(g, &r, times[i], g->menu_time == i);
            y += 30;
        }
    }
    y += 6;

    /* ── Colore umano ── */
    if (fs) gui_draw_text(g, fs, "GIOCHI CON:", x, y, yellow); 
    y += 18;
    {
        const char *colors[] = {"NERO (muove per primo)", "BIANCO"};
        for (int i = 0; i < 2; i++) {
            SDL_Rect r = {x, y, box_w - 40, 26};
            gui_draw_button(g, &r, colors[i], g->menu_color == i);
            y += 30;
        }
    }
    y += 10;

    /* ── Suggerimenti ── */
    {
        SDL_Rect r = {x, y, box_w - 40, 26};
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "Suggerimenti: %s",
                 g->menu_hints ? "ON" : "OFF");
        gui_draw_button(g, &r, lbl, g->menu_hints);
        y += 36;
    }

    /* Pulsante START */
    y = box_y + box_h + 20;
    SDL_Rect btn_start = {cx - 100, y, 200, 44};
    SDL_SetRenderDrawColor(g->renderer, 40, 160, 60, 255);
    SDL_RenderFillRect(g->renderer, &btn_start);
    SDL_SetRenderDrawColor(g->renderer, 100, 255, 120, 255);
    SDL_RenderDrawRect(g->renderer, &btn_start);
    if (fl) draw_text_c(g, fl, "INIZIA PARTITA", cx - 100, y + 11, 200, white);

    /* Pulsante TUNING */
    SDL_Rect btn_tune = {cx - 80, y + 54, 160, 30};
    SDL_SetRenderDrawColor(g->renderer, 60, 40, 100, 255);
    SDL_RenderFillRect(g->renderer, &btn_tune);
    SDL_SetRenderDrawColor(g->renderer, 140, 100, 220, 255);
    SDL_RenderDrawRect(g->renderer, &btn_tune);
    if (fs) draw_text_c(g, fs, "TUNING PARAMETRI", cx - 80, y + 54 + 8, 160, dim);

    /* Footer */
    if (fs) {
        draw_text_c(g, fs, "Drag-and-drop per muovere le pedine",
                    0, WIN_H - 20, WIN_W, dim);
    }

    (void)cx;
}

/* ══════════════════════════════════════════════
 *  Render TUNING overlay
 * ══════════════════════════════════════════════ */
static void render_tuning(GUI *g) {
    SDL_SetRenderDrawColor(g->renderer, 10, 10, 30, 255);
    SDL_RenderClear(g->renderer);

    TTF_Font *fl = g->font_lg;
    TTF_Font *fs = g->font_sm;

    SDL_Color yellow = {255,210,60,255};
    SDL_Color white  = {255,255,255,255};
    SDL_Color dim    = {150,150,200,255};

    if (fl) draw_text_c(g, fl, "TUNING IPERPARAMETRI", 0, 60, WIN_W, yellow);
    if (fs) {
        draw_text_c(g, fs, "Algoritmo Genetico + Round-Robin Tournament",
                    0, 92, WIN_W, dim);
        draw_text_c(g, fs, "Attenzione: il processo richiede diversi minuti.",
                    0, 114, WIN_W, dim);
        draw_text_c(g, fs, "Risultati salvati in dama_params.txt",
                    0, 134, WIN_W, dim);
    }

    if (SDL_AtomicGet(&g->ai_done) == 1) {
        /* tuning terminato */
        if (fl) draw_text_c(g, fl, "COMPLETATO!", 0, WIN_H/2, WIN_W, yellow);
        if (fs) draw_text_c(g, fs, "Premi un tasto per tornare al menu",
                             0, WIN_H/2 + 30, WIN_W, white);
    } else {
        /* in corso */
        static int spin = 0; spin = (spin+1) % 4;
        const char *sp[] = {"●  ○  ○","○  ●  ○","○  ○  ●","○  ●  ○"};
        if (fl) draw_text_c(g, fl, sp[spin], 0, WIN_H/2, WIN_W, yellow);
        if (fs) draw_text_c(g, fs, "Tuning in corso...", 0, WIN_H/2+30, WIN_W, white);
    }
}

/* ══════════════════════════════════════════════
 *  gui_render  – punto d'ingresso del rendering
 * ══════════════════════════════════════════════ */
void gui_render(GUI *g) {
    SDL_SetRenderDrawColor(g->renderer, C_BG, 255);
    SDL_RenderClear(g->renderer);

    switch (g->state) {
    case GS_MENU:
        render_menu(g);
        break;
    case GS_TUNING:
        render_tuning(g);
        break;
    default:
        render_board(g);
        render_pieces(g);
        render_sidebar(g);
        break;
    }

    SDL_RenderPresent(g->renderer);
}

/* ══════════════════════════════════════════════
 *  Thread TUNING
 * ══════════════════════════════════════════════ */
static int tuning_thread_fn(void *data) {
    GUI *g = (GUI *)data;
    TuningParams tp = tuning_default_params();
    tp.verbose = 1;
    tp.generations = 5;
    tp.pop_size    = 8;
    tuning_run(&g->params_ucb1, &g->params_puct, &tp);
    tuning_save("dama_params.txt", &g->params_ucb1, &g->params_puct);
    SDL_AtomicSet(&g->ai_done, 1);
    return 0;
}

/* ══════════════════════════════════════════════
 *  Gestione eventi MENU
 * ══════════════════════════════════════════════ */
static void handle_menu_click(GUI *g, int mx, int my) {
    int cx = WIN_W / 2;
    int box_x = WIN_W/2 - 160, box_y = 140, box_w = 320;
    int x = box_x + 20, y = box_y + 16;

    /* --- Modello --- */
    y += 18;
    for (int i = 0; i < 2; i++) {
        if (mx >= x && mx <= x + box_w - 40 && my >= y && my <= y + 26)
            g->menu_model = i;
        y += 30;
    }
    y += 6;

    /* --- Tempo --- */
    y += 18;
    for (int i = 0; i < 3; i++) {
        if (mx >= x && mx <= x + box_w - 40 && my >= y && my <= y + 26)
            g->menu_time = i;
        y += 30;
    }
    y += 6;

    /* --- Colore --- */
    y += 18;
    for (int i = 0; i < 2; i++) {
        if (mx >= x && mx <= x + box_w - 40 && my >= y && my <= y + 26)
            g->menu_color = i;
        y += 30;
    }
    y += 10;

    /* --- Hints --- */
    if (mx >= x && mx <= x + box_w - 40 && my >= y && my <= y + 26)
        g->menu_hints = !g->menu_hints;
    y += 36;

    /* Bottone START */
    int btn_y = box_y + 390 + 20;
    if (mx >= cx-100 && mx <= cx+100 && my >= btn_y && my <= btn_y + 44) {
        gui_start_new_game(g);
        return;
    }

    /* Bottone TUNING */
    if (mx >= cx-80 && mx <= cx+80 && my >= btn_y+54 && my <= btn_y+84) {
        g->state = GS_TUNING;
        SDL_AtomicSet(&g->ai_done, 0);
        SDL_Thread *tt = SDL_CreateThread(tuning_thread_fn, "Tuning", g);
        if (tt) SDL_DetachThread(tt);
    }
}

/* ══════════════════════════════════════════════
 *  Gestione click sidebar (Nuova Partita / Menu)
 * ══════════════════════════════════════════════ */
static void handle_sidebar_click(GUI *g, int mx, int my) {
    int y1 = WIN_H - 90;
    int y2 = WIN_H - 52;
    if (mx >= SIDE_X + 4 && mx <= WIN_W - 10) {
        if (my >= y1 && my <= y1 + 30) {
            /* Nuova partita con stessa config */
            gui_start_new_game(g);
        }
        if (my >= y2 && my <= y2 + 30) {
            /* Torna al menu */
            g->state = GS_MENU;
        }
    }
}

/* ══════════════════════════════════════════════
 *  gui_handle_event
 * ══════════════════════════════════════════════ */
void gui_handle_event(GUI *g, const SDL_Event *e) {
    switch (e->type) {

    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            int mx = e->button.x, my = e->button.y;
            if (g->state == GS_MENU) {
                handle_menu_click(g, mx, my);
            } else if (g->state == GS_TUNING) {
                /* nessun input durante tuning */
            } else if (g->state == GS_PLAYING) {
                /* Sidebar? */
                if (mx >= SIDE_X) { handle_sidebar_click(g, mx, my); break; }

                int sq = gui_px_to_sq(g, mx, my);
                if (sq < 0) break;

                const Board *b = &g->board;
                uint32_t my_pieces = (b->stm == BLACK)
                    ? (b->bm | b->bk) : (b->wm | b->wk);

                if ((my_pieces >> sq) & 1) {
                    /* Selezione pezzo */
                    g->sel_sq = sq;
                    g->dragging = true;
                    g->drag_x = mx; g->drag_y = my;
                    gui_update_legal(g, sq);
                } else if (g->sel_sq >= 0) {
                    /* Tentativo di mossa per click */
                    for (int i = 0; i < g->n_legal; i++) {
                        if (g->legal[i].to == sq) {
                            gui_finish_move(g, &g->legal[i]);
                            break;
                        }
                    }
                    if (g->state == GS_PLAYING) {
                        g->sel_sq = -1;
                        g->dragging = false;
                        g->n_legal = 0;
                    }
                }
            } else if (g->state == GS_GAMEOVER) {
                handle_sidebar_click(g, mx, my);
            }
        }
        break;

    case SDL_MOUSEMOTION:
        if (g->dragging && g->state == GS_PLAYING) {
            g->drag_x = e->motion.x;
            g->drag_y = e->motion.y;
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT && g->dragging) {
            int mx = e->button.x, my = e->button.y;
            int sq = gui_px_to_sq(g, mx, my);
            g->dragging = false;

            if (sq >= 0 && g->sel_sq >= 0 && g->state == GS_PLAYING) {
                for (int i = 0; i < g->n_legal; i++) {
                    if (g->legal[i].to == sq) {
                        gui_finish_move(g, &g->legal[i]);
                        goto done_drag;
                    }
                }
                /* Nessuna mossa valida: rimetti il pezzo */
                g->sel_sq = -1;
                g->n_legal = 0;
            } else {
                g->sel_sq = -1;
                g->n_legal = 0;
            }
            done_drag:;
        }
        break;

    case SDL_KEYDOWN:
        if (g->state == GS_TUNING && SDL_AtomicGet(&g->ai_done) == 1) {
            g->state = GS_MENU;
        }
        if (e->key.keysym.sym == SDLK_ESCAPE) {
            if (g->state == GS_PLAYING || g->state == GS_GAMEOVER)
                g->state = GS_MENU;
        }
        break;

    default: break;
    }
}

/* ══════════════════════════════════════════════
 *  gui_run  – loop principale
 * ══════════════════════════════════════════════ */
void gui_run(GUI *g) {
    const int TARGET_FPS = 60;
    const int FRAME_MS   = 1000 / TARGET_FPS;

    while (1) {
        Uint32 t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return;
            gui_handle_event(g, &e);
        }

        /* Controlla se l'AI ha finito */
        if (g->state == GS_AI_THINK && SDL_AtomicGet(&g->ai_done) == 1) {
            SDL_AtomicSet(&g->ai_done, 0);
            if (g->ai_thread) {
                SDL_WaitThread(g->ai_thread, NULL);
                g->ai_thread = NULL;
            }

            /* Valida la mossa AI */
            Move mvs[MAX_MOVES];
            int nm = board_gen_moves(&g->board, mvs);
            bool valid = false;
            for (int i = 0; i < nm; i++) {
                if (mvs[i].from == g->ai_move.from &&
                    mvs[i].to   == g->ai_move.to   &&
                    mvs[i].cap_mask == g->ai_move.cap_mask) {
                    g->ai_move = mvs[i];
                    valid = true;
                    break;
                }
            }
            if (!valid && nm > 0) g->ai_move = mvs[0];

            if (nm > 0) {
                gui_finish_move(g, &g->ai_move);
            } else {
                gui_check_gameover(g);
            }
        }

        gui_render(g);

        Uint32 elapsed = SDL_GetTicks() - t0;
        if (elapsed < (Uint32)FRAME_MS)
            SDL_Delay(FRAME_MS - elapsed);
    }
}
