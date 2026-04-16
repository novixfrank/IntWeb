#pragma once
#include "board.h"
#include "mcts.h"
#include "tuning.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

/* ─── Layout ─── */
#define WIN_W        900
#define WIN_H        700
#define BOARD_X       30
#define BOARD_Y       30
#define BOARD_SIZE   640
#define CELL_SIZE    (BOARD_SIZE / 8)
#define SIDE_X       (BOARD_X + BOARD_SIZE + 10)
#define SIDE_W       (WIN_W - SIDE_X - 10)

typedef enum {
    GS_MENU, GS_PLAYING, GS_AI_THINK, GS_GAMEOVER, GS_TUNING
} GState;

typedef struct {
    int         human_color;
    MCTSParams  params;
    bool        show_hints;
    bool        show_stats;
} GConfig;

typedef struct {
    SDL_Window    *window;
    SDL_Renderer  *renderer;
    TTF_Font      *font_lg;
    TTF_Font      *font_sm;
    Board          board;
    GState         state;
    GConfig        cfg;
    MCTSPool      *pool;
    MCTSParams     params_ucb1;
    MCTSParams     params_puct;
    int            sel_sq;
    int            drag_x, drag_y;
    bool           dragging;
    Move           legal[MAX_MOVES];
    int            n_legal;
    int            last_from, last_to;
    SDL_Thread    *ai_thread;
    SDL_atomic_t   ai_done;
    Move           ai_move;
    double         ai_elapsed;
    MCTSStats      ai_stats;
    int            menu_model;
    int            menu_time;
    int            menu_color;
    bool           menu_hints;
    int            result;
    char           result_msg[64];
} GUI;

int   gui_init(GUI *g);
void  gui_destroy(GUI *g);
void  gui_run(GUI *g);
void  gui_render(GUI *g);
void  gui_handle_event(GUI *g, const SDL_Event *e);
void  gui_start_new_game(GUI *g);
void  gui_start_ai(GUI *g);
void  gui_finish_move(GUI *g, const Move *m);
void  gui_check_gameover(GUI *g);
void  gui_update_legal(GUI *g, int sq);
int   gui_px_to_sq(const GUI *g, int px, int py);
void  gui_sq_to_px(const GUI *g, int sq, int *cx, int *cy);
void  gui_draw_text(GUI *g, TTF_Font *font, const char *txt,
                    int x, int y, SDL_Color col);
void  gui_draw_circle_filled(GUI *g, int cx, int cy, int radius,
                              Uint8 r, Uint8 gr, Uint8 b, Uint8 a);
void  gui_draw_button(GUI *g, const SDL_Rect *r, const char *lbl, bool active);
