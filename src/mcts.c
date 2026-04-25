/**
 * @file mcts.c
 * @brief Implementazione dell'algoritmo Monte Carlo Tree Search (MCTS) per la Dama Italiana.
 * * Il file contiene la logica per la selezione, espansione, simulazione (rollout) 
 * e backpropagation. Utilizza un approccio Negamax con Depth Decay per ottimizzare 
 * la ricerca della vittoria e la riduzione del danno.
 */

#include "mcts.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════════════
 * GESTIONE MEMORIA (POOL)
 * ══════════════════════════════════════════════════════════════════════════════ */
static MCTSPool g_pool;           /* pool primario (AI in partita e ricerche singole) */
static MCTSPool g_pool_secondary; /* pool secondario (usato in tuning per il secondo giocatore) */

/* Flag log: 0 = silenzioso (default), 1 = stampa ragionamento su terminale */
static int g_mcts_log_enabled = 0;

void mcts_set_log(int enabled) {
    g_mcts_log_enabled = enabled;
}

/**
 * @brief Inizializza e restituisce il puntatore al pool di memoria primario.
 * @return MCTSPool* Puntatore al pool globale statico.
 */
MCTSPool *mcts_pool_create(void) {
    mcts_pool_reset(&g_pool);
    return &g_pool;
}

/**
 * @brief Restituisce un pool secondario per analisi o tuning parallelo.
 * @return MCTSPool* Puntatore al pool secondario.
 */
MCTSPool *mcts_pool_secondary(void) {
    mcts_pool_reset(&g_pool_secondary);
    return &g_pool_secondary;
}

/**
 * @brief Libera le risorse del pool (operazione fittizia in questa implementazione statica).
 * @param p Puntatore al pool da "liberare".
 */
void mcts_pool_free(MCTSPool *p) { (void)p; }

/**
 * @brief Resetta gli indici del pool per iniziare una nuova ricerca.
 * @param pool Puntatore al pool da resettare.
 */
void mcts_pool_reset(MCTSPool *pool) {
    pool->next = 0;
    pool->sims = 0;
}

/**
 * @brief Alloca un nuovo nodo dal pool pre-allocato.
 * @param pool Il pool da cui allocare.
 * @return int32_t Indice del nuovo nodo o -1 se il pool è pieno.
 * @note L'allocazione è O(1) in quanto incrementa solo un contatore.
 */
static inline int32_t pool_alloc(MCTSPool *pool) {
    if (pool->next >= MCTS_POOL_SIZE - 1) return -1;
    return pool->next++;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CODIFICA E LOGICA EURISTICA
 * ══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Salva i dati essenziali di una mossa all'interno di un nodo MCTS.
 * @param nd Puntatore al nodo di destinazione.
 * @param m Puntatore alla mossa da codificare.
 */
static inline void node_encode_move(MCTSNode *nd, const Move *m) {
    nd->from      = m->from;
    nd->to        = m->to;
    nd->num_caps  = m->num_caps;
    nd->num_kcaps = m->num_kcaps;
    nd->cap_mask  = m->cap_mask;
    nd->flags     = (uint8_t)((m->promotes ? 1 : 0) | (m->king_move ? 2 : 0));
}

/**
 * @brief Ricostruisce una mossa completa partendo dai dati compressi nel nodo.
 * @param nd Puntatore al nodo contenente la mossa.
 * @return Move La mossa decodificata pronta per essere applicata alla board.
 */
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

/**
 * @brief Calcola il valore di probabilità a priori (prior) per il modello PUCT.
 * @details Favorisce mosse che effettuano catture o promozioni per guidare l'esplorazione iniziale.
 * @param m Mossa da valutare.
 * @param p Parametri MCTS (pesi euristici).
 * @param num_legal Numero totale di mosse legali nel nodo padre.
 * @return float Valore prior normalizzato.
 */
static inline float compute_prior_puct(const Move *m, const MCTSParams *p, int num_legal) {
    float pr = 1.0f;
    if (m->num_caps > 0) pr += p->prior_cap  * (float)m->num_caps;
    if (m->promotes)     pr += p->prior_promo;
    if (m->king_move)    pr += 0.1f;
    return pr / (float)(num_legal > 0 ? num_legal * 2 : 1);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CORE ALGORITHM: SELEZIONE E SIMULAZIONE
 * ══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Seleziona il miglior figlio di un nodo usando UCB1 o PUCT.
 * @details Bilancia Exploitation (valore medio w/n) ed Exploration (incertezza).
 * @param pool Pool di memoria contenente l'albero.
 * @param node_idx Indice del nodo padre.
 * @param p Parametri di configurazione (modello e costanti).
 * @return int32_t Indice del miglior nodo figlio trovato.
 */
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
            // Formula UCB1 standard
            float expl = (ch->n > 0) ? p->C * sqrtf(2.0f * log_n / (float)ch->n) : 1e10f;
            score = q + expl;
        } else {
            // Formula PUCT (Predictor + Upper Confidence Bound applied to Trees)
            score = q + p->c_puct * ch->prior * sqrt_n / (1.0f + ch->n);
        }
        if (score > best) { best = score; best_child = c; }
    }
    return best_child;
}

/**
 * @brief Esegue una simulazione rapida (Rollout) fino a fine partita o limite di profondità.
 * @details La politica è semi-casuale: priorità a catture/promozioni, altrimenti random.
 * @param b Copia della board da cui partire.
 * @param p Parametri MCTS.
 * @return float Risultato assoluto: +1.0 (vince Nero), -1.0 (vince Bianco), 0.0 (pareggio).
 */
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
            // Semplice euristica per rollout più realistici
            int best_caps = -1; int best_idx = 0;
            for (int i = 0; i < n; i++) {
                if (moves[i].num_caps > best_caps) {
                    best_caps = moves[i].num_caps;
                    best_idx = i;
                }
            }
            chosen = (best_caps > 0 || (rand() % 100) < 70) ? best_idx : (rand() % n);
        }
        board_do_move(&b, &moves[chosen]);
        depth++;
    }
}

/**
 * @brief Funzione ricorsiva core di MCTS (Selection, Expansion, Backpropagation).
 * @details Implementa la logica Negamax: il reward viene invertito ad ogni livello.
 * Include il "Depth Decay" per spingere l'IA a vincere velocemente.
 * @param pool Pool di memoria dell'albero.
 * @param node_idx Indice del nodo corrente.
 * @param board Stato attuale della scacchiera.
 * @param p Parametri MCTS.
 * @param depth Profondità corrente nell'albero.
 * @return float Reward relativo per il giocatore che deve muovere al livello precedente.
 */
static float mcts_rec(MCTSPool *pool, int32_t node_idx, Board board, const MCTSParams *p, int depth) {
    if (node_idx < 0) return 0.0f;
    MCTSNode *nd = &pool->nodes[node_idx];
    nd->n++;

    Move moves[MAX_MOVES];
    int nmoves;

    // Recupero o generazione mosse legali
    if (nd->num_legal < 0) {
        nmoves = board_gen_moves(&board, moves);
        nd->num_legal = (int16_t)nmoves;
    } else {
        nmoves = nd->num_legal;
        if (nmoves > 0) board_gen_moves(&board, moves);
    }

    int res = board_result(&board, nmoves);
    float abs_result;

    /* 1. FASE TERMINALE: Se la partita finisce qui, ritorna il risultato */
    if (nmoves == 0 || res != 0 || board.nocap >= 80) {
        if (nmoves == 0) abs_result = (board.stm == BLACK) ? -1.0f : 1.0f;
        else if (board.nocap >= 80) abs_result = 0.0f;
        else abs_result = (float)res;
        
        // Backpropagation immediata per nodo foglia terminale
        float reward_for_parent = (board.stm == BLACK) ? -abs_result : abs_result;
        nd->w += reward_for_parent;
        return abs_result * 0.99f; // Depth decay
    }

    /* 2. FASE DI ESPANSIONE: Se il nodo non è pienamente esplorato, crea un nuovo figlio */
    if (nd->num_children < nd->num_legal) {
        int ci = nd->num_children;
        const Move *m = &moves[ci];

        int32_t child_idx = pool_alloc(pool);
        if (child_idx < 0) {
            // Fallback su rollout se pool pieno
            Board b2 = board;
            board_do_move(&b2, m);
            abs_result = rollout(b2, p);
        } else {
            MCTSNode *ch = &pool->nodes[child_idx];
            memset(ch, 0, sizeof(*ch));
            ch->parent = node_idx; ch->first_child = -1; ch->next_sib = nd->first_child;
            ch->num_legal = -1; ch->num_children = 0;
            node_encode_move(ch, m);

            nd->first_child = child_idx;
            nd->num_children++;
            if (p->model == SEL_PUCT) ch->prior = compute_prior_puct(m, p, nd->num_legal);

            // Simulazione (Rollout)
            Board b2 = board;
            board_do_move(&b2, m);
            abs_result = rollout(b2, p);
            
            ch->n++;
            float reward_for_ch = (board.stm == BLACK) ? abs_result : -abs_result;
            ch->w += reward_for_ch;
        }
    } 
    /* 3. FASE DI SELEZIONE: Se il nodo è già espanso, scendi nel miglior figlio */
    else {
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

    /* 4. BACKPROPAGATION: Aggiorna il valore w del nodo in base al risultato del sotto-albero */
    float reward_for_parent = (board.stm == BLACK) ? -abs_result : abs_result;
    nd->w += reward_for_parent;
    
    // Ritorna il risultato scalato per incoraggiare velocità di vittoria
    return abs_result * 0.99f;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * API PUBBLICA E LOGGING
 * ══════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Avvia la ricerca della mossa migliore e stampa il log del ragionamento.
 * @details Gestisce il ciclo "Anytime" rispettando il time_limit.
 * @param b Stato attuale della scacchiera.
 * @param p Parametri di ricerca.
 * @param pool Pool di memoria da utilizzare.
 * @return Move La mossa scelta dall'IA.
 */
Move mcts_best_move(const Board *b, const MCTSParams *p, MCTSPool *pool) {
    mcts_pool_reset(pool);

    // Creazione radice
    int32_t root = pool_alloc(pool);
    MCTSNode *rnd = &pool->nodes[root];
    memset(rnd, 0, sizeof(*rnd));
    rnd->parent = -1; rnd->first_child = -1; rnd->num_legal = -1; rnd->num_children = 0;

    double start = now_sec();
    pool->sims = 0;

    // Ciclo Anytime: continua finché c'è tempo
    while (now_sec() - start < p->time_limit) {
        for (int batch = 0; batch < 16; batch++) {
            Board brd = *b;
            mcts_rec(pool, root, brd, p, 0);
            pool->sims++;
        }
    }

    // Selezione finale: Robust Child (maggior numero di visite n)
    Move best_move; memset(&best_move, 0, sizeof(best_move));
    int best_n = -1; float best_w = -1e30f;

    for (int32_t c = rnd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
        const MCTSNode *ch = &pool->nodes[c];
        if (ch->n > best_n || (ch->n == best_n && (ch->w / (ch->n+1)) > best_w)) {
            best_n = ch->n;
            best_w = (ch->n > 0) ? ch->w / ch->n : 0.0f;
            best_move = node_decode_move(ch);
        }
    }

    // Fallback in caso di tempo zero
    if (best_n < 0) {
        Move mvs[MAX_MOVES];
        if (board_gen_moves(b, mvs) > 0) best_move = mvs[0];
    }


    /* ── LOGGING LOGICA DECISIONALE (solo se abilitato con --log) ── */
    if (g_mcts_log_enabled) {
        printf("\n==========================================================\n");
        printf("RAGIONAMENTO IA (MCTS) COMPLETATO\n");
        printf("==========================================================\n");
        printf("Turno: %s | Obiettivo: Vittoria Massima / Danno Minimo\n", b->stm == BLACK ? "NERO" : "BIANCO");
        printf("Simulazioni: %lld | Nodi creati: %d\n", (long long)pool->sims, pool->next);
        printf("\n-> MOSSA SCELTA: [%d -> %d]\n", best_move.from, best_move.to);
        printf("-> Win Rate IA stimata: %.1f%%\n", best_w * 50.0f + 50.0f);
        printf("\nGiustificazione rispetto alle alternative:\n");
        for (int32_t c = rnd->first_child; c >= 0; c = pool->nodes[c].next_sib) {
            const MCTSNode *ch = &pool->nodes[c];
            Move m = node_decode_move(ch);
            float wr = (ch->n > 0) ? ch->w / ch->n : 0.0f;
            printf(" - [%2d -> %2d]: WR %5.1f%% (%d visite)%s\n",
                   m.from, m.to, wr * 50.0f + 50.0f, ch->n,
                   (m.from == best_move.from && m.to == best_move.to) ? " [BEST]" : "");
        }
        printf("==========================================================\n\n");
    }

    return best_move;
}

/**
 * @brief Estrae le statistiche correnti dal pool.
 * @param pool Pool analizzato.
 * @return MCTSStats Struttura contenente sims, tree size e win rate.
 */
MCTSStats mcts_get_stats(const MCTSPool *pool) {
    MCTSStats s = {0};
    s.total_sims = pool->sims;
    s.tree_size = pool->next;
    const MCTSNode *root = &pool->nodes[0];
    if (root->n > 0) {
        int bn = -1;
        for (int32_t c = root->first_child; c >= 0; c = pool->nodes[c].next_sib) {
            if (pool->nodes[c].n > bn) {
                bn = pool->nodes[c].n;
                s.best_win_rate = (pool->nodes[c].n > 0) ? pool->nodes[c].w / pool->nodes[c].n : 0.0f;
            }
        }
    }
    return s;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * API DI COMPATIBILITÀ
 * ══════════════════════════════════════════════════════════════════════════════ */

void mcts_init(MCTSPool *pool, const Board *root_board) {
    mcts_pool_reset(pool);
    (void)root_board;
}

Move mcts_search(MCTSPool *pool, const Board *b, const MCTSParams *p) {
    return mcts_best_move(b, p, pool);
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
