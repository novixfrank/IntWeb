#include "mcts.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ══════════════════════════════════════════════
 * Pool
 * ══════════════════════════════════════════════ */
static MCTSPool g_pool;
static MCTSPool g_pool_secondary;

MCTSPool *mcts_pool_create(void) {
    mcts_pool_reset(&g_pool);
    return &g_pool;
}

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
    if (pool->next >= MCTS_POOL_SIZE - 1) return -1;
    return pool->next++;
}

/* ══════════════════════════════════════════════
 * Codifica / decodifica mossa nel nodo
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

static inline float compute_prior_puct(const Move *m, const MCTSParams *p, int num_legal) {
    float pr = 1.0f;
    if (m->num_caps > 0) pr += p->prior_cap  * (float)m->num_caps;
    if (m->promotes)     pr += p->prior_promo;
    if (m->king_move)    pr += 0.1f;
    return pr / (float)(num_legal > 0 ? num_legal * 2 : 1);
}

/* ══════════════════════════════════════════════
 * Selezione UCB1 / PUCT
 * ══════════════════════════════════════════════ */
static int32_t select_child(const MCTSPool *pool, int32_t node_idx, const MCTSParams *p) {
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
            float expl = (ch->n > 0) ? p->C * sqrtf(2.0f * log_n / (float)ch->n) : 1e10f;
            score = q + expl;
        } else {
            score = q + p->c_puct * ch->prior * sqrt_n / (1.0f + ch->n);
        }
        if (score > best) { best = score; best_child = c; }
    }
    return best_child;
}

/* ══════════════════════════════════════════════
 * Rollout (simulazione rapida)
 * Restituisce il punteggio ASSOLUTO: +1 se vince NERO, -1 se vince BIANCO.
 * ══════════════════════════════════════════════ */
static float rollout(Board b, const MCTSParams *p) {
    int depth = 0;
    Move moves[MAX_MOVES];

    for (;;) {
        int n = board_gen_moves(&b, moves);

        if (n == 0) return (b.stm == BLACK) ? -1.0f : 1.0f;
        if (b.nocap >= 80) return 0.0f;
        int res = board_result(&b, n);
        if (res != 0) return (float)res;
        if (depth >= p->rollout_depth) return 0.0f;

        int chosen = 0;
        if (n > 1) {
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
 * MCTS Ricorsivo (Negamax)
 * Propaga correttamente il reward invertendolo in base al turno.
 * Introduce un DECADIMENTO (0.99) per premiare le vittorie veloci
 * e ritardare le sconfitte (riduzione del danno).
 * ══════════════════════════════════════════════ */
static float mcts_rec(MCTSPool *pool, int32_t node_idx, Board board, const MCTSParams *p, int depth) {
    if (node_idx < 0) return 0.0f;
    MCTSNode *nd = &pool->nodes[node_idx];
    nd->n++;

    Move moves[MAX_MOVES];
    int  nmoves;

    if (nd->num_legal < 0) {
        nmoves = board_gen_moves(&board, moves);
        nd->num_legal = (int16_t)nmoves;
    } else {
        nmoves = nd->num_legal;
        if (nmoves > 0) board_gen_moves(&board, moves);
    }

    int res = board_result(&board, nmoves);
    float abs_result;

    /* Nodo Terminale */
    if (nmoves == 0 || res != 0 || board.nocap >= 80) {
        if (nmoves == 0) abs_result = (board.stm == BLACK) ? -1.0f : 1.0f;
        else if (board.nocap >= 80) abs_result = 0.0f;
        else abs_result = (float)res;
        
        /* Reward relativo per chi ha MOSSO per arrivare qui */
        float reward_for_parent = (board.stm == BLACK) ? -abs_result : abs_result;
        nd->w += reward_for_parent;
        
        return abs_result * 0.99f; /* Decadimento per premiare win veloci */
    }

    if (nd->num_children < nd->num_legal) {
        /* Espansione */
        int ci = nd->num_children;
        const Move *m = &moves[ci];

        int32_t child_idx = pool_alloc(pool);
        if (child_idx < 0) {
            Board b2 = board;
            board_do_move(&b2, m);
            abs_result = rollout(b2, p);
        } else {
            MCTSNode *ch = &pool->nodes[child_idx];
            memset(ch, 0, sizeof(*ch));
            ch->parent       = node_idx;
            ch->first_child  = -1;
            ch->next_sib     = nd->first_child;
            ch->num_legal    = -1;
            ch->num_children = 0;
            ch->prior        = 1.0f / nd->num_legal;
            node_encode_move(ch, m);

            nd->first_child = child_idx;
            nd->num_children++;

            if (p->model == SEL_PUCT) {
                ch->prior = compute_prior_puct(m, p, nd->num_legal);
            }

            Board b2 = board;
            board_do_move(&b2, m);
            abs_result = rollout(b2, p);
            
            ch->n++;
            float reward_for_ch = (board.stm == BLACK) ? abs_result : -abs_result;
            ch->w += reward_for_ch;
        }
    } else {
        /* Selezione */
        int32_t best = select_child(pool, node_idx, p);
        if (best < 0) {
            abs_result = rollout(board, p);
        } else {
            Move m = node_decode_move(&pool->nodes[best]);
            Board b2 = board;
            board_do_move(&b2, &m);
            abs_result = mcts_rec(pool, best, b2, p, depth + 1);
        }
    }

    /* Backpropagation al padre */
    float reward_for_parent = (board.stm == BLACK) ? -abs_result : abs_result;
    nd->w += reward_for_parent;
    
    return abs_result * 0.99f; /* Applica il decadimento (Depth Decay) propagando a ritroso */
}

/* ══════════════════════════════════════════════
 * mcts_best_move  –  ricerca principale + LOG
 * ══════════════════════════════════════════════ */
Move mcts_best_move(const Board *b, const MCTSParams *p, MCTSPool *pool) {
    mcts_pool_reset(pool);

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

    while (now_sec() - start < p->time_limit) {
        for (int batch = 0; batch < 16; batch++) {
            Board brd = *b;
            mcts_rec(pool, root, brd, p, 0);
            pool->sims++;
        }
    }

    Move best_move;
    memset(&best_move, 0, sizeof(best_move));
    int   best_n = -1;
    float best_w = -1e30f;

    Move root_moves[MAX_MOVES];
    int  n_root = board_gen_moves(b, root_moves);

    /* Criterio: Robust Child (sceglie quello con più visite). Evita errori estremi. */
    for (int32_t c = rnd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
        const MCTSNode *ch = &pool->nodes[c];
        if (ch->n > best_n || (ch->n == best_n && (ch->w / (ch->n+1)) > best_w)) {
            best_n    = ch->n;
            best_w    = (ch->n > 0) ? ch->w / ch->n : 0.0f;
            best_move = node_decode_move(ch);
        }
    }

    if (best_n < 0 && n_root > 0) {
        best_move = root_moves[0];
    }

    /* ── LOG DEL RAGIONAMENTO A TERMINALE ── */
    printf("\n==========================================================\n");
    printf("🎯 RAGIONAMENTO IA (MCTS) COMPLETATO\n");
    printf("==========================================================\n");
    printf("Turno: %s (Obiettivo: Vincere o Minimizzare i Danni)\n", b->stm == BLACK ? "NERO" : "BIANCO");
    printf("Modello Utilizzato: %s\n", p->model == SEL_UCB1 ? "UCB1 Classico" : "PUCT (AlphaGo)");
    printf("Simulazioni Casuali (Rollout) Effettuate: %lld\n", (long long)pool->sims);
    printf("\n-> MOSSA SCELTA: Da Casella %d a Casella %d\n", best_move.from, best_move.to);
    printf("-> Probabilità di Vittoria stimata dell'IA (Win Rate): %.1f%%\n", best_w * 50.0f + 50.0f);
    printf("\nAnalisi e Giustificazione delle Alternative Valutate:\n");
    
    for (int32_t c = rnd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
        const MCTSNode *ch = &pool->nodes[c];
        Move m = node_decode_move(ch);
        float wr = (ch->n > 0) ? ch->w / ch->n : 0.0f;
        
        printf(" - Opzione [%2d -> %2d]: Testata %6d volte | WR: %5.1f%% ", m.from, m.to, ch->n, wr * 50.0f + 50.0f);
        if (m.from == best_move.from && m.to == best_move.to) {
            printf(" <<< PRESCELTA (Bilanciamento ottimale visite/profitto)");
        }
        printf("\n");
    }
    printf("==========================================================\n\n");

    return best_move;
}

/* ══════════════════════════════════════════════
 * Statistiche
 * ══════════════════════════════════════════════ */
MCTSStats mcts_get_stats(const MCTSPool *pool) {
    MCTSStats s;
    s.total_sims    = pool->sims;
    s.sims_per_sec  = 0;
    s.best_win_rate = 0;
    s.tree_size     = pool->next;

    const MCTSNode *root = &pool->nodes[0];
    if (root->n > 0) {
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
        printf("[%d->%d] ", m.from, m.to);
        board_do_move(&cur, &m);
        node = best;
    }
    printf("\n");
}

void mcts_init(MCTSPool *pool, const Board *root_board) {
    mcts_pool_reset(pool);
    (void)root_board;
}

Move mcts_search(MCTSPool *pool, const Board *b, const MCTSParams *p) {
    return mcts_best_move(b, p, pool);
}