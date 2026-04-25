#pragma once
#include "board.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ══════════════════════════════════════════════
 *  Modelli di selezione
 * ══════════════════════════════════════════════ */
typedef enum {
    SEL_UCB1  = 0,   /* UCB1 classico                              */
    SEL_PUCT  = 1    /* PUCT come AlphaGo (con prior euristico)    */
} SelectModel;

/* ══════════════════════════════════════════════
 *  Parametri MCTS (iper-parametri da tuning)
 * ══════════════════════════════════════════════ */
typedef struct {
    SelectModel model;
    float  C;           /* UCB1: costante di esplorazione (default √2) */
    float  c_puct;      /* PUCT: costante (default 2.0)                */
    float  prior_cap;   /* peso prior per mosse di cattura (PUCT)      */
    float  prior_promo; /* peso prior per promozioni (PUCT)            */
    double time_limit;  /* secondi per mossa: 0.2 / 1.0 / 3.0         */
    int    rollout_depth; /* profondità max rollout (default 150)       */
} MCTSParams;

/* Valori predefiniti */
static inline MCTSParams mcts_default_params(void) {
    MCTSParams p;
    p.model        = SEL_UCB1;
    p.C            = 1.414213562f;   /* √2 */
    p.c_puct       = 2.0f;
    p.prior_cap    = 0.7f;
    p.prior_promo  = 0.3f;
    p.time_limit   = 1.0;
    p.rollout_depth = 150;
    return p;
}

/* ══════════════════════════════════════════════
 *  Nodo MCTS
 * ══════════════════════════════════════════════ */
#define MCTS_POOL_SIZE 2200000   /* ~88 MB */

typedef struct {
    float    w;           /* reward accumulato                             */
    int32_t  n;           /* visite                                        */
    int32_t  parent;      /* indice padre (-1 = radice)                    */
    int32_t  first_child; /* primo figlio (-1)                             */
    int32_t  next_sib;    /* prossimo fratello (-1)                        */
    int16_t  num_children;/* figli creati finora                           */
    int16_t  num_legal;   /* mosse legali totali (-1 = non calcolato)      */
    /* Mossa che ha portato a questo nodo */
    int8_t   from;
    int8_t   to;
    int8_t   num_caps;
    int8_t   num_kcaps;
    uint32_t cap_mask;
    uint8_t  flags;       /* bit0=promotes, bit1=king_move                 */
    float    prior;       /* prior per PUCT                                */
} MCTSNode;              /* 40 byte / nodo */

/* ══════════════════════════════════════════════
 *  Pool di memoria per i nodi
 * ══════════════════════════════════════════════ */
typedef struct {
    MCTSNode nodes[MCTS_POOL_SIZE];
    int32_t  next;      /* prossimo indice libero */
    int64_t  sims;      /* simulazioni effettuate */
} MCTSPool;

/* Alloca/libera pool (statici, inizializzati una volta) */
MCTSPool *mcts_pool_create(void);      /* pool primario */
MCTSPool *mcts_pool_secondary(void);   /* pool secondario (tuning/analisi) */
void      mcts_pool_free(MCTSPool *pool);
void      mcts_pool_reset(MCTSPool *pool);

/* ══════════════════════════════════════════════
 *  API principale
 * ══════════════════════════════════════════════ */

/* Inizializza l'albero con la board corrente come radice */
void  mcts_init(MCTSPool *pool, const Board *root_board);

/* Esegue simulazioni per time_limit secondi; restituisce la mossa migliore */
Move  mcts_search(MCTSPool *pool, const Board *b, const MCTSParams *p);

/* Versione che accetta direttamente il nodo radice (per compatibilità GUI) */
Move  mcts_best_move(const Board *b, const MCTSParams *p, MCTSPool *pool);

/* ══════════════════════════════════════════════
 *  Utilità per analisi sperimentale
 * ══════════════════════════════════════════════ */
typedef struct {
    int64_t total_sims;    /* simulazioni eseguite */
    double  sims_per_sec;
    float   best_win_rate; /* tasso vittoria del best child */
    int     tree_size;     /* nodi nell'albero */
} MCTSStats;

MCTSStats mcts_get_stats(const MCTSPool *pool);
void      mcts_print_pv(const MCTSPool *pool, const Board *b, int depth);

/* ══════════════════════════════════════════════
 *  Controllo log su terminale
 *  Default: disabilitato. Abilitare con mcts_set_log(1)
 *  (chiamato da main.c quando viene passato --log).
 * ══════════════════════════════════════════════ */
void mcts_set_log(int enabled);   /* 1 = log attivo, 0 = silenzioso */

/* Clock utility */
static inline double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
