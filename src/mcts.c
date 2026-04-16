#include "mcts.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ══════════════════════════════════════════════
 *  Pool
 * ══════════════════════════════════════════════ */
static MCTSPool g_pool;          /* pool primario (AI in partita e ricerche singole) */
static MCTSPool g_pool_secondary; /* pool secondario (usato in tuning per il secondo giocatore) */

MCTSPool *mcts_pool_create(void) {
    mcts_pool_reset(&g_pool);
    return &g_pool;
}

/* Secondo pool separato per tuning/analisi dove due ricerche devono coesistere */
MCTSPool *mcts_pool_secondary(void) {
    mcts_pool_reset(&g_pool_secondary);
    return &g_pool_secondary;
}

void mcts_pool_free(MCTSPool *p) { (void)p; }

void mcts_pool_reset(MCTSPool *pool) {
    pool->next = 0;
    pool->sims = 0;
}

static inline int32_t pool_alloc(MCTSPool *pool) {
    if (pool->next >= MCTS_POOL_SIZE - 1) return -1;   /* pool pieno */
    return pool->next++;
}

/* ══════════════════════════════════════════════
 *  Codifica / decodifica mossa nel nodo
 * ══════════════════════════════════════════════ */
static inline void node_encode_move(MCTSNode *nd, const Move *m) {
    nd->from      = m->from;
    nd->to        = m->to;
    nd->num_caps  = m->num_caps;
    nd->num_kcaps = m->num_kcaps;
    nd->cap_mask  = m->cap_mask;
    nd->flags     = (uint8_t)((m->promotes ? 1 : 0) | (m->king_move ? 2 : 0));
}

static inline Move node_decode_move(const MCTSNode *nd) {
    Move m;
    m.from       = nd->from;
    m.to         = nd->to;
    m.num_caps   = nd->num_caps;
    m.num_kcaps  = nd->num_kcaps;
    m.cap_mask   = nd->cap_mask;
    m.promotes   = (nd->flags & 1) != 0;
    m.king_move  = (nd->flags & 2) != 0;
    return m;
}

/* ══════════════════════════════════════════════
 *  Prior euristico per PUCT (singola mossa, normalizzazione approssimata)
 *  Viene chiamato durante l'espansione di ogni figlio.
 *  Formula: base=1.0 + catture * prior_cap + promozione * prior_promo
 *  Dividiamo per (num_legal * 2) come stima della somma totale.
 * ══════════════════════════════════════════════ */
static inline float compute_prior_puct(const Move *m, const MCTSParams *p,
                                        int num_legal) {
    float pr = 1.0f;
    if (m->num_caps > 0) pr += p->prior_cap  * (float)m->num_caps;
    if (m->promotes)     pr += p->prior_promo;
    if (m->king_move)    pr += 0.1f;
    return pr / (float)(num_legal > 0 ? num_legal * 2 : 1);
}

/* ══════════════════════════════════════════════
 *  Selezione UCB1 / PUCT
 * ══════════════════════════════════════════════ */
static int32_t select_child(const MCTSPool *pool, int32_t node_idx,
                             const MCTSParams *p) {
    const MCTSNode *nd   = &pool->nodes[node_idx];
    float  log_n  = logf((float)(nd->n + 1));
    float  sqrt_n = sqrtf((float)nd->n);
    float  best   = -1e30f;
    int32_t best_child = -1;

    for (int32_t c = nd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
        const MCTSNode *ch = &pool->nodes[c];
        float score;
        float q = (ch->n > 0) ? ch->w / (float)ch->n : 0.0f;

        if (p->model == SEL_UCB1) {
            float expl = (ch->n > 0) ? p->C * sqrtf(2.0f * log_n / (float)ch->n)
                                      : 1e10f;
            score = q + expl;
        } else {
            /* PUCT: Q(a) + c_puct * P(a) * sqrt(N) / (1 + n(a)) */
            score = q + p->c_puct * ch->prior * sqrt_n / (1.0f + ch->n);
        }
        if (score > best) { best = score; best_child = c; }
    }
    return best_child;
}

/* ══════════════════════════════════════════════
 *  Rollout (simulazione rapida)
 *  Politica semi-casuale:
 *    - catture obbligatorie (regola Dama Italiana)
 *    - preferenza per mosse di promozione
 *    - altrimenti casuale uniforme
 * ══════════════════════════════════════════════ */
static float rollout(Board b, const MCTSParams *p) {
    int depth = 0;
    Move moves[MAX_MOVES];

    for (;;) {
        int n = board_gen_moves(&b, moves);

        /* Condizioni terminali:
         *   - n == 0 → chi deve muovere non ha mosse (perde)
         *   - board_result != 0 → qualcuno ha perso tutti i pezzi
         *   - b.nocap >= 80 → pareggio per regola delle 40 mosse
         *   - depth >= rollout_depth → taglio per tempo
         */
        if (n == 0) {
            /* Chi deve muovere non ha mosse: perde */
            return (b.stm == BLACK) ? -1.0f : 1.0f;
        }
        if (b.nocap >= 80) {
            return 0.0f;   /* pareggio */
        }
        int res = board_result(&b, n);
        if (res != 0) {
            return (float)res;
        }
        if (depth >= p->rollout_depth) {
            return 0.0f;   /* taglio: considera pareggio */
        }
        /* Sceglie la mossa */
        int chosen = 0;
        if (n > 1) {
            /* Piccola euristica: preferisce mosse con più catture o promozioni */
            int best_caps  = -1;
            bool best_prom = false;
            int  best_idx  = 0;
            for (int i = 0; i < n; i++) {
                if (moves[i].num_caps > best_caps ||
                    (moves[i].num_caps == best_caps && moves[i].promotes && !best_prom)) {
                    best_caps = moves[i].num_caps;
                    best_prom = moves[i].promotes;
                    best_idx  = i;
                }
            }
            /* Con 30% di probabilità prende comunque una mossa casuale
               (diversità / evita loop locali) */
            if (best_caps > 0 || (rand() % 100) < 70) {
                chosen = best_idx;
            } else {
                chosen = rand() % n;
            }
        }
        board_do_move(&b, &moves[chosen]);
        depth++;
    }
}

/* ══════════════════════════════════════════════
 *  Espansione e ricorsione MCTS (negamax)
 *  Ritorna il reward dal punto di vista del giocatore
 *  che ha MOSSO per arrivare a questo nodo.
 * ══════════════════════════════════════════════ */
static float mcts_rec(MCTSPool *pool, int32_t node_idx, Board board,
                      const MCTSParams *p, int depth) {
    if (node_idx < 0) return 0.0f;
    MCTSNode *nd = &pool->nodes[node_idx];
    nd->n++;

    /* ── Terminale: numero mosse calcolato al livello superiore ── */
    /* Calcoliamo comunque le mosse per verificare lo stato */
    Move moves[MAX_MOVES];
    int  nmoves;

    if (nd->num_legal < 0) {
        /* Prima visita: calcola mosse legali */
        nmoves = board_gen_moves(&board, moves);
        nd->num_legal = (int16_t)nmoves;
    } else {
        nmoves = nd->num_legal;
        if (nmoves > 0) board_gen_moves(&board, moves); /* ri-genera per usarle */
    }

    int res = board_result(&board, nmoves);

    /* Posizione terminale:
     *   nmoves == 0  → chi muove non ha mosse (perde)
     *   res != 0     → qualcuno senza pezzi
     *   nocap >= 80  → pareggio regola 40 mosse (board_result restituisce 0,
     *                  ma il gioco deve terminare lo stesso)
     */
    if (nmoves == 0 || res != 0 || board.nocap >= 80) {
        float r;
        if (nmoves == 0)       r = -1.0f;  /* chi muove perde */
        else if (board.nocap >= 80) r = 0.0f;  /* pareggio */
        else r = (board.stm == BLACK) ? (float)res : -(float)res;
        nd->w += r;
        return -r;
    }

    float result;

    if (nd->num_children < nd->num_legal) {
        /* ── Espansione: creo il prossimo figlio ── */
        int ci = nd->num_children;   /* indice della mossa da espandere */
        /* ri-genera se necessario (già fatto sopra) */
        const Move *m = &moves[ci];

        int32_t child_idx = pool_alloc(pool);
        if (child_idx < 0) {
            /* Pool pieno: rollout dal nodo corrente */
            Board b2 = board;
            board_do_move(&b2, m);
            result = rollout(b2, p);
            nd->w += result;
            return -result;
        }

        MCTSNode *ch = &pool->nodes[child_idx];
        memset(ch, 0, sizeof(*ch));
        ch->parent       = node_idx;
        ch->first_child  = -1;
        ch->next_sib     = nd->first_child;
        ch->num_legal    = -1;   /* non ancora calcolato */
        ch->num_children = 0;
        ch->prior        = 1.0f / nd->num_legal;
        node_encode_move(ch, m);

        /* Inserisce in testa alla lista figli */
        nd->first_child = child_idx;
        nd->num_children++;

        /* Prior PUCT: assegnato con funzione inlining */
        if (p->model == SEL_PUCT) {
            ch->prior = compute_prior_puct(m, p, nd->num_legal);
        }

        /* Applica mossa e fai rollout */
        Board b2 = board;
        board_do_move(&b2, m);
        result = rollout(b2, p);
        ch->n++;
        ch->w += result;
        result = -result;
    } else {
        /* ── Selezione: sceglie il figlio migliore via UCB/PUCT ── */
        int32_t best = select_child(pool, node_idx, p);
        if (best < 0) {
            /* Nessun figlio (strano): rollout */
            result = rollout(board, p);
        } else {
            Move m = node_decode_move(&pool->nodes[best]);
            Board b2 = board;
            board_do_move(&b2, &m);
            result = mcts_rec(pool, best, b2, p, depth + 1);
        }
    }

    nd->w += result;
    return -result;
}

/* ══════════════════════════════════════════════
 *  mcts_best_move  –  ricerca principale
 * ══════════════════════════════════════════════ */
Move mcts_best_move(const Board *b, const MCTSParams *p, MCTSPool *pool) {
    mcts_pool_reset(pool);

    /* Crea radice */
    int32_t root = pool_alloc(pool);
    MCTSNode *rnd = &pool->nodes[root];
    memset(rnd, 0, sizeof(*rnd));
    rnd->parent      = -1;
    rnd->first_child = -1;
    rnd->num_legal   = -1;
    rnd->num_children= 0;
    rnd->prior       = 1.0f;

    double start = now_sec();
    pool->sims = 0;

    /* Loop anytime: continua finché il tempo non scade */
    while (now_sec() - start < p->time_limit) {
        /* Batch di simulazioni per ridurre l'overhead di clock_gettime */
        for (int batch = 0; batch < 16; batch++) {
            Board brd = *b;
            mcts_rec(pool, root, brd, p, 0);
            pool->sims++;
        }
    }

    /* Sceglie il figlio della radice con più visite */
    Move best_move;
    memset(&best_move, 0, sizeof(best_move));
    int   best_n = -1;
    float best_w = -1e30f;

    /* Genera le mosse della radice per identificare il best move */
    Move root_moves[MAX_MOVES];
    int  n_root = board_gen_moves(b, root_moves);

    /* Trova il figlio più visitato */
    for (int32_t c = rnd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
        const MCTSNode *ch = &pool->nodes[c];
        if (ch->n > best_n || (ch->n == best_n && ch->w / (ch->n+1) > best_w)) {
            best_n    = ch->n;
            best_w    = (ch->n > 0) ? ch->w / ch->n : 0.0f;
            best_move = node_decode_move(ch);
        }
    }

    /* Se nessun figlio creato (timeout brevissimo), prendi la prima mossa */
    if (best_n < 0 && n_root > 0) {
        best_move = root_moves[0];
    }

    return best_move;
}

/* ══════════════════════════════════════════════
 *  Statistiche
 * ══════════════════════════════════════════════ */
MCTSStats mcts_get_stats(const MCTSPool *pool) {
    MCTSStats s;
    s.total_sims    = pool->sims;
    s.sims_per_sec  = 0;
    s.best_win_rate = 0;
    s.tree_size     = pool->next;

    const MCTSNode *root = &pool->nodes[0];
    if (root->n > 0) {
        /* Trova il figlio più visitato */
        int best_n = -1;
        for (int32_t c = root->first_child; c >= 0; c = pool->nodes[c].next_sib) {
            const MCTSNode *ch = &pool->nodes[c];
            if (ch->n > best_n) {
                best_n = ch->n;
                s.best_win_rate = (ch->n > 0) ? ch->w / ch->n : 0.0f;
            }
        }
    }
    return s;
}

void mcts_print_pv(const MCTSPool *pool, const Board *b, int depth) {
    Board cur = *b;
    int32_t node = 0;
    printf("PV: ");
    for (int d = 0; d < depth; d++) {
        if (node < 0) break;
        const MCTSNode *nd = &pool->nodes[node];
        int best_n = -1;
        int32_t best = -1;
        for (int32_t c = nd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
            if (pool->nodes[c].n > best_n) {
                best_n = pool->nodes[c].n;
                best   = c;
            }
        }
        if (best < 0) break;
        Move m = node_decode_move(&pool->nodes[best]);
        printf("[%d→%d] ", m.from, m.to);
        board_do_move(&cur, &m);
        node = best;
    }
    printf("\n");
}

/* ══════════════════════════════════════════════
 *  mcts_init / mcts_search
 *  API alternativa (dichiarata in mcts.h)
 *  Thin wrapper attorno a mcts_best_move.
 * ══════════════════════════════════════════════ */
void mcts_init(MCTSPool *pool, const Board *root_board) {
    /* Reset del pool e pre-espansione della radice. */
    mcts_pool_reset(pool);
    (void)root_board;   /* La board viene passata direttamente a mcts_search */
}

Move mcts_search(MCTSPool *pool, const Board *b, const MCTSParams *p) {
    /* Deleghiamo a mcts_best_move che gestisce reset + ricerca + best-child. */
    return mcts_best_move(b, p, pool);
}
