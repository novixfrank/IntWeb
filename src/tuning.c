#include "tuning.h"
#include "board.h"
#include "mcts.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <time.h>

/* ══════════════════════════════════════════════
 *  Utilità randomiche
 * ══════════════════════════════════════════════ */

/* LCG semplice per uso interno (thread-safe tramite seed esplicito) */
static inline uint32_t lcg_next(unsigned int *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

/* Float uniforme in [lo, hi] */
static float rand_uniform(float lo, float hi, unsigned int *seed) {
    return lo + (hi - lo) * ((float)(lcg_next(seed) >> 8) / (float)(1 << 24));
}

/* Gaussiana approssimata via Box-Muller */
static float rand_gaussian(float mean, float sigma, unsigned int *seed) {
    float u1 = rand_uniform(1e-6f, 1.0f, seed);
    float u2 = rand_uniform(0.0f,  1.0f, seed);
    float z  = sqrtf(-2.0f * logf(u1)) * cosf(6.283185f * u2);
    return mean + sigma * z;
}

/* Clamp */
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ══════════════════════════════════════════════
 *  individual_to_params
 * ══════════════════════════════════════════════ */
MCTSParams individual_to_params(const Individual *ind, SelectModel model,
                                double time_limit) {
    MCTSParams p = mcts_default_params();
    p.model        = model;
    p.C            = ind->C;
    p.c_puct       = ind->c_puct;
    p.prior_cap    = ind->prior_cap;
    p.prior_promo  = ind->prior_promo;
    p.time_limit   = time_limit;
    p.rollout_depth = 150;
    return p;
}

/* ══════════════════════════════════════════════
 *  tuning_init_population
 * ══════════════════════════════════════════════ */
void tuning_init_population(Individual *pop, int n, unsigned int seed) {
    for (int i = 0; i < n; i++) {
        pop[i].C           = rand_uniform(0.5f,  3.0f, &seed);
        pop[i].c_puct      = rand_uniform(0.5f,  5.0f, &seed);
        pop[i].prior_cap   = rand_uniform(0.0f,  1.5f, &seed);
        pop[i].prior_promo = rand_uniform(0.0f,  0.8f, &seed);
        pop[i].fitness     = 0.0f;
        pop[i].wins   = 0;
        pop[i].losses = 0;
        pop[i].draws  = 0;
    }
    /* Include i valori di default nella popolazione iniziale come "ancora" */
    if (n >= 2) {
        pop[0].C = 1.4142f; pop[0].c_puct = 2.0f;
        pop[0].prior_cap = 0.7f; pop[0].prior_promo = 0.3f;
        pop[1].C = 1.0f;    pop[1].c_puct = 1.5f;
        pop[1].prior_cap = 0.5f; pop[1].prior_promo = 0.2f;
    }
}

/* ══════════════════════════════════════════════
 *  play_game_between
 *  Gioca una partita tra ind_black (NERO) e ind_white (BIANCO)
 *  Restituisce: 1 = NERO vince, -1 = BIANCO vince, 0 = pareggio
 * ══════════════════════════════════════════════ */
static int play_game_between(const Individual *black, const Individual *white,
                              SelectModel model, double tlimit,
                              MCTSPool *pool_b, MCTSPool *pool_w) {
    Board b;
    board_init(&b);

    MCTSParams p_black = individual_to_params(black, model, tlimit);
    MCTSParams p_white = individual_to_params(white, model, tlimit);

    /* Limite mosse per evitare partite infinite durante il tuning */
    int max_plies = 200;

    for (int ply = 0; ply < max_plies; ply++) {
        Move moves[MAX_MOVES];
        int  nmv = board_gen_moves(&b, moves);
        int  res = board_result(&b, nmv);
        if (res != 0 || nmv == 0) return res;

        Move chosen;
        if (b.stm == BLACK) {
            chosen = mcts_best_move(&b, &p_black, pool_b);
        } else {
            chosen = mcts_best_move(&b, &p_white, pool_w);
        }

        /* Validazione della mossa: se non valida, prendi la prima legale */
        bool valid = false;
        for (int i = 0; i < nmv; i++) {
            if (moves[i].from == chosen.from &&
                moves[i].to   == chosen.to   &&
                moves[i].cap_mask == chosen.cap_mask) {
                chosen = moves[i];
                valid  = true;
                break;
            }
        }
        if (!valid) chosen = moves[0];

        board_do_move(&b, &chosen);
    }

    /* Timeout → considera pareggio */
    return 0;
}

/* ══════════════════════════════════════════════
 *  tuning_round_robin
 * ══════════════════════════════════════════════ */
void tuning_round_robin(Individual *pop, int n, const TuningParams *tp,
                        SelectModel model,
                        MCTSPool *pool_a, MCTSPool *pool_b) {
    /* Reset statistiche */
    for (int i = 0; i < n; i++) {
        pop[i].wins = pop[i].losses = pop[i].draws = 0;
        pop[i].fitness = 0.0f;
    }

    /* Il modello di selezione usato nelle partite di valutazione viene
       passato dal chiamante: deve coincidere con il modello per cui la
       popolazione è stata definita (SEL_UCB1 per pop_ucb1, SEL_PUCT per
       pop_puct), altrimenti i parametri specifici di un modello (es.
       c_puct) verrebbero valutati giocando partite con l'altro modello. */

    int total_pairs = n * (n - 1);
    int done = 0;

    for (int i = 0; i < n; i++) {
        for (int j = i+1; j < n; j++) {
            if (i == j) continue;

            /* Gioca tp->games_per_pair partite con colori alternati */
            int g2 = tp->games_per_pair / 2;
            if (g2 < 1) g2 = 1;

            for (int g = 0; g < g2; g++) {
                /* i = NERO, j = BIANCO */
                int r = play_game_between(&pop[i], &pop[j], model,
                                          tp->time_limit, pool_a, pool_b);
                if (r > 0)  { pop[i].wins++;  pop[j].losses++; }
                else if (r < 0) { pop[i].losses++; pop[j].wins++; }
                else        { pop[i].draws++; pop[j].draws++; }

                /* i = BIANCO, j = NERO */
                int r2 = play_game_between(&pop[j], &pop[i], model,
                                            tp->time_limit, pool_a, pool_b);
                if (r2 > 0)  { pop[j].wins++;  pop[i].losses++; }
                else if (r2 < 0) { pop[j].losses++; pop[i].wins++; }
                else         { pop[j].draws++; pop[i].draws++; }
            }

            done++;
            if (tp->verbose) {
                printf("\r  Tournament: %d/%d ", done, total_pairs);
                fflush(stdout);
            }
        }
    }
    if (tp->verbose) printf("\n");

    /* Calcola fitness: W - L + 0.5*D (come score chess) */
    for (int i = 0; i < n; i++) {
        pop[i].fitness = (float)pop[i].wins
                       - (float)pop[i].losses
                       + 0.5f * (float)pop[i].draws;
    }
}

/* ══════════════════════════════════════════════
 *  Ordinamento decrescente per fitness (bubble sort, pop piccola)
 * ══════════════════════════════════════════════ */
static void sort_by_fitness(Individual *pop, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (pop[j].fitness > pop[i].fitness) {
                Individual tmp = pop[i]; pop[i] = pop[j]; pop[j] = tmp;
            }
        }
    }
}

/* ══════════════════════════════════════════════
 *  tuning_evolve
 *  Una generazione: crossover + mutazione
 * ══════════════════════════════════════════════ */
void tuning_evolve(Individual *pop, int n, const TuningParams *tp,
                   unsigned int *seed) {
    /* 1. Ordina per fitness decrescente */
    sort_by_fitness(pop, n);

    /* 2. Élite: i primi tp->elite_count vengono copiati invariati */
    int elite = tp->elite_count;
    if (elite >= n) elite = n / 2;

    /* 3. Riempi il resto con figli generati da crossover + mutazione */
    Individual new_pop[64];  /* max pop_size supportato */
    if (n > 64) n = 64;

    /* Copia élite */
    for (int i = 0; i < elite; i++) {
        new_pop[i] = pop[i];
        new_pop[i].fitness = 0.0f;
        new_pop[i].wins = new_pop[i].losses = new_pop[i].draws = 0;
    }

    /* Genera figli */
    for (int i = elite; i < n; i++) {
        /* Selezione torneo (2 candidati) per i genitori */
        int a = (int)(lcg_next(seed) % (uint32_t)elite);
        int b_idx = (int)(lcg_next(seed) % (uint32_t)elite);
        const Individual *pa = &pop[a];
        const Individual *pb = &pop[b_idx];

        /* Crossover uniforme */
        Individual child;
        child.C           = (lcg_next(seed) & 1) ? pa->C           : pb->C;
        child.c_puct      = (lcg_next(seed) & 1) ? pa->c_puct      : pb->c_puct;
        child.prior_cap   = (lcg_next(seed) & 1) ? pa->prior_cap   : pb->prior_cap;
        child.prior_promo = (lcg_next(seed) & 1) ? pa->prior_promo : pb->prior_promo;

        /* Mutazione gaussiana */
        if (rand_uniform(0.0f, 1.0f, seed) < tp->mutation_rate)
            child.C = clampf(rand_gaussian(child.C, tp->mutation_sigma * 2.0f, seed), 0.1f, 4.0f);
        if (rand_uniform(0.0f, 1.0f, seed) < tp->mutation_rate)
            child.c_puct = clampf(rand_gaussian(child.c_puct, tp->mutation_sigma * 2.0f, seed), 0.1f, 5.0f);
        if (rand_uniform(0.0f, 1.0f, seed) < tp->mutation_rate)
            child.prior_cap = clampf(rand_gaussian(child.prior_cap, tp->mutation_sigma, seed), 0.0f, 2.0f);
        if (rand_uniform(0.0f, 1.0f, seed) < tp->mutation_rate)
            child.prior_promo = clampf(rand_gaussian(child.prior_promo, tp->mutation_sigma, seed), 0.0f, 1.0f);

        child.fitness = 0.0f;
        child.wins = child.losses = child.draws = 0;
        new_pop[i] = child;
    }

    memcpy(pop, new_pop, (size_t)n * sizeof(Individual));
}

/* ══════════════════════════════════════════════
 *  tuning_bai
 *  Best Arm Identification:
 *  ogni individuo sfida il campione corrente;
 *  il campione viene aggiornato se perde.
 * ══════════════════════════════════════════════ */
int tuning_bai(Individual *pop, int n, int budget,
               const TuningParams *tp, SelectModel model,
               MCTSPool *pool_a, MCTSPool *pool_b) {
    /* Il campione è l'individuo con fitness più alta */
    sort_by_fitness(pop, n);
    int champion = 0;
    int games_per_challenger = (budget / n) + 1;

    for (int i = 1; i < n; i++) {
        if (i == champion) continue;
        int champ_score = 0;
        int chal_score  = 0;

        for (int g = 0; g < games_per_challenger; g++) {
            int r = play_game_between(&pop[champion], &pop[i],
                                      model, tp->time_limit,
                                      pool_a, pool_b);
            if (r > 0) champ_score++;
            else if (r < 0) chal_score++;

            int r2 = play_game_between(&pop[i], &pop[champion],
                                        model, tp->time_limit,
                                        pool_a, pool_b);
            if (r2 > 0) chal_score++;
            else if (r2 < 0) champ_score++;
        }
        if (chal_score > champ_score) {
            champion = i;
            if (tp->verbose)
                printf("  BAI: nuovo campione: individuo %d (%.0f vs %.0f)\n",
                       i, (float)chal_score, (float)champ_score);
        }
    }
    return champion;
}

/* ══════════════════════════════════════════════
 *  tuning_run
 *  Esegue il ciclo genetico completo per UCB1 e PUCT.
 * ══════════════════════════════════════════════ */
void tuning_run(MCTSParams *best_ucb1, MCTSParams *best_puct,
                const TuningParams *tp) {
    /* Allocazione pool MCTS — due pool separati per i due giocatori */
    MCTSPool *pool_a = mcts_pool_create();
    MCTSPool *pool_b = mcts_pool_secondary();

    unsigned int seed = (unsigned int)time(NULL);

    /* ── UCB1 ── */
    if (tp->verbose) printf("\n=== Tuning UCB1 (%d generazioni x %d individui) ===\n",
                             tp->generations, tp->pop_size);

    Individual pop_ucb1[64];
    int n = tp->pop_size;
    if (n > 64) n = 64;
    tuning_init_population(pop_ucb1, n, seed);

    for (int gen = 0; gen < tp->generations; gen++) {
        if (tp->verbose) printf("\n[UCB1] Generazione %d/%d\n", gen+1, tp->generations);
        tuning_round_robin(pop_ucb1, n, tp, SEL_UCB1, pool_a, pool_b);
        if (tp->verbose) tuning_print_population(pop_ucb1, n);
        if (gen < tp->generations - 1)
            tuning_evolve(pop_ucb1, n, tp, &seed);
    }

    /* BAI finale */
    int bai_budget = tp->games_per_pair * n;
    int best_idx = tuning_bai(pop_ucb1, n, bai_budget, tp, SEL_UCB1, pool_a, pool_b);
    *best_ucb1 = individual_to_params(&pop_ucb1[best_idx], SEL_UCB1, 1.0);

    if (tp->verbose) {
        printf("\n[UCB1] Migliori parametri: C=%.3f\n", best_ucb1->C);
    }

    /* ── PUCT ── */
    if (tp->verbose) printf("\n=== Tuning PUCT (%d generazioni x %d individui) ===\n",
                             tp->generations, tp->pop_size);

    Individual pop_puct[64];
    seed ^= 0xDEADBEEFu;
    tuning_init_population(pop_puct, n, seed);

    for (int gen = 0; gen < tp->generations; gen++) {
        if (tp->verbose) printf("\n[PUCT] Generazione %d/%d\n", gen+1, tp->generations);

        /* A differenza di pop_ucb1, qui le partite di valutazione devono
           essere giocate con il modello PUCT (model = SEL_PUCT), in modo
           che c_puct e le prior abbiano davvero un effetto sull'esito
           delle partite usate per calcolare la fitness. */
        tuning_round_robin(pop_puct, n, tp, SEL_PUCT, pool_a, pool_b);
        if (tp->verbose) tuning_print_population(pop_puct, n);
        if (gen < tp->generations - 1)
            tuning_evolve(pop_puct, n, tp, &seed);
    }

    best_idx = tuning_bai(pop_puct, n, bai_budget, tp, SEL_PUCT, pool_a, pool_b);
    *best_puct = individual_to_params(&pop_puct[best_idx], SEL_PUCT, 1.0);

    if (tp->verbose) {
        printf("\n[PUCT] Migliori parametri: c_puct=%.3f, cap=%.3f, promo=%.3f\n",
               best_puct->c_puct, best_puct->prior_cap, best_puct->prior_promo);
    }
}

/* ══════════════════════════════════════════════
 *  tuning_save / tuning_load
 * ══════════════════════════════════════════════ */
int tuning_save(const char *path, const MCTSParams *ucb1, const MCTSParams *puct) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "ucb1_C %.6f\n",           ucb1->C);
    fprintf(f, "puct_c_puct %.6f\n",      puct->c_puct);
    fprintf(f, "puct_prior_cap %.6f\n",   puct->prior_cap);
    fprintf(f, "puct_prior_promo %.6f\n", puct->prior_promo);
    fclose(f);
    return 0;
}

int tuning_load(const char *path, MCTSParams *ucb1, MCTSParams *puct) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* Inizializza con default */
    *ucb1 = mcts_default_params(); ucb1->model = SEL_UCB1;
    *puct = mcts_default_params(); puct->model = SEL_PUCT;

    char key[64];
    float val;
    while (fscanf(f, "%63s %f\n", key, &val) == 2) {
        if (key[0] == '#') { /* commento */ continue; }
        if      (strcmp(key, "ucb1_C")           == 0) ucb1->C           = val;
        else if (strcmp(key, "puct_c_puct")      == 0) puct->c_puct      = val;
        else if (strcmp(key, "puct_prior_cap")   == 0) puct->prior_cap   = val;
        else if (strcmp(key, "puct_prior_promo") == 0) puct->prior_promo = val;
    }
    fclose(f);
    return 0;
}

/* ══════════════════════════════════════════════
 *  tuning_print_population
 * ══════════════════════════════════════════════ */
void tuning_print_population(const Individual *pop, int n) {
    printf("  %-4s  %-6s  %-6s  %-7s  %-7s  %-6s  %-6s\n",
           "Idx", "C", "c_puct", "cap", "promo", "W-L-D", "Fitness");
    printf("  ----------------------------------------------------------\n");

    /* Copia e ordina per stampa */
    Individual sorted[64];
    if (n > 64) n = 64;
    memcpy(sorted, pop, (size_t)n * sizeof(Individual));
    /* bubble sort */
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (sorted[j].fitness > sorted[i].fitness) {
                Individual t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }

    for (int i = 0; i < n; i++) {
        printf("  %-4d  %6.3f  %6.3f  %7.3f  %7.3f  %d-%d-%d  %6.1f\n",
               i,
               sorted[i].C, sorted[i].c_puct,
               sorted[i].prior_cap, sorted[i].prior_promo,
               sorted[i].wins, sorted[i].losses, sorted[i].draws,
               sorted[i].fitness);
    }
}

/* ══════════════════════════════════════════════
 *  tuning_print_report
 *  Analisi sperimentale: confronta UCB1 vs PUCT a 0.2/1.0/3.0 sec
 * ══════════════════════════════════════════════ */
void tuning_print_report(const MCTSParams *ucb1, const MCTSParams *puct,
                         MCTSPool *pool) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║         ANALISI SPERIMENTALE – DAMA ITALIANA MCTS        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    double budgets[] = {0.2, 1.0, 3.0};
    const char *labels[] = {"0.2s", "1.0s", "3.0s"};
    int n_budgets = 3;

    printf("%-10s  %-10s  %-12s  %-12s  %-12s\n",
           "Modello", "Tempo", "Sims/sec", "Nodi", "BestWR");
    printf("--------------------------------------------------------------\n");

    Board b;
    board_init(&b);

    for (int bi = 0; bi < n_budgets; bi++) {
        /* UCB1 */
        {
            MCTSParams p = *ucb1;
            p.time_limit = budgets[bi];
            double t0 = now_sec();
            mcts_best_move(&b, &p, pool);
            double elapsed = now_sec() - t0;
            MCTSStats s = mcts_get_stats(pool);
            printf("%-10s  %-10s  %-12.0f  %-12d  %-12.3f\n",
                   "UCB1", labels[bi],
                   elapsed > 0 ? (double)s.total_sims / elapsed : 0.0,
                   s.tree_size,
                   s.best_win_rate);
        }
        /* PUCT */
        {
            MCTSParams p = *puct;
            p.time_limit = budgets[bi];
            double t0 = now_sec();
            mcts_best_move(&b, &p, pool);
            double elapsed = now_sec() - t0;
            MCTSStats s = mcts_get_stats(pool);
            printf("%-10s  %-10s  %-12.0f  %-12d  %-12.3f\n",
                   "PUCT", labels[bi],
                   elapsed > 0 ? (double)s.total_sims / elapsed : 0.0,
                   s.tree_size,
                   s.best_win_rate);
        }
    }

    printf("\nParametri ottimali UCB1: C=%.3f\n", ucb1->C);
    printf("Parametri ottimali PUCT: c_puct=%.3f, prior_cap=%.3f, prior_promo=%.3f\n\n", puct->c_puct, puct->prior_cap, puct->prior_promo);

    /* Match UCB1 vs PUCT a 0.2s: 10 partite */
    printf("Match UCB1 vs PUCT (10 partite ciascuno, budget 0.2s):\n");
    int ucb1_wins = 0, puct_wins = 0, draws = 0;

    MCTSParams pu = *ucb1; pu.time_limit = 0.2;
    MCTSParams pp = *puct; pp.time_limit = 0.2;

    for (int g = 0; g < 10; g++) {
        /* UCB1=NERO, PUCT=BIANCO */
        Board bg; board_init(&bg);
        int max_ply = 300;
        for (int ply = 0; ply < max_ply; ply++) {
            Move mvs[MAX_MOVES];
            int nm = board_gen_moves(&bg, mvs);
            int r  = board_result(&bg, nm);
            if (r != 0 || nm == 0) {
                if (r > 0) ucb1_wins++;
                else if (r < 0) puct_wins++;
                else draws++;
                goto next_game;
            }
            MCTSParams *cur = (bg.stm == BLACK) ? &pu : &pp;
            Move mv = mcts_best_move(&bg, cur, pool);
            /* valida */
            bool ok = false;
            for (int i = 0; i < nm; i++)
                if (mvs[i].from==mv.from && mvs[i].to==mv.to && mvs[i].cap_mask==mv.cap_mask)
                    { mv=mvs[i]; ok=true; break; }
            if (!ok) mv = mvs[0];
            board_do_move(&bg, &mv);
        }
        draws++;
        next_game:;
    }

    printf("  UCB1 vittorie: %d  PUCT vittorie: %d  Pareggi: %d\n\n",
           ucb1_wins, puct_wins, draws);
}

/* ══════════════════════════════════════════════════════════════════
 *  CLOP – Confident Linear Optimization of Parameters adattato per Dama Italiana MCTS
 * ══════════════════════════════════════════════════════════════════ */

/* Imposta il parametro `param` nel MCTSParams alla soglia `val` */
static void clop_set_param(MCTSParams *p, CLOPParam param, float val) {
    switch (param) {
    case CLOP_PARAM_C:           p->C           = val; break;
    case CLOP_PARAM_C_PUCT:      p->c_puct      = val; break;
    case CLOP_PARAM_PRIOR_CAP:   p->prior_cap   = val; break;
    case CLOP_PARAM_PRIOR_PROMO: p->prior_promo = val; break;
    }
}

static const char *clop_param_name(CLOPParam param) {
    switch (param) {
    case CLOP_PARAM_C:           return "C (UCB1)";
    case CLOP_PARAM_C_PUCT:      return "c_puct (PUCT)";
    case CLOP_PARAM_PRIOR_CAP:   return "prior_cap";
    case CLOP_PARAM_PRIOR_PROMO: return "prior_promo";
    default:                     return "unknown";
    }
}

/* ── clop_init ── */
void clop_init(CLOPState *st, CLOPParam param,
               float init, float sigma, float lo, float hi,
               int n_games, double time_limit) {
    st->param      = param;
    st->theta      = init;
    st->sigma      = sigma;
    st->lo         = lo;
    st->hi         = hi;
    st->n_rounds   = 0;
    st->n_games    = n_games;
    st->time_limit = time_limit;
    st->verbose    = 1;
}

/* ── clop_run_round ──
 *  Gioca n_games partite tra (base + theta-sigma) e (base + theta+sigma).
 *  Stima il gradiente della probabilità di vittoria come funzione di theta
 *  e aggiorna theta con un passo proporzionale al gradiente.
 *
 *  Modello: P(win | x) ≈ sigmoid((x - theta*) / scale)
 *  Gradient step: theta += eta * (wins_plus - wins_minus) / n_total
 */
float clop_run_round(CLOPState *st,
                     const MCTSParams *base,
                     MCTSPool *pool_a, MCTSPool *pool_b) {
    float x_lo = st->theta - st->sigma;
    float x_hi = st->theta + st->sigma;

    /* Clampa ai limiti */
    if (x_lo < st->lo) x_lo = st->lo;
    if (x_hi > st->hi) x_hi = st->hi;
    if (x_lo >= x_hi)  { st->sigma *= 0.5f; return st->theta; }

    /* Configura due MCTSParams */
    MCTSParams p_lo = *base;
    MCTSParams p_hi = *base;
    clop_set_param(&p_lo, st->param, x_lo);
    clop_set_param(&p_hi, st->param, x_hi);
    p_lo.time_limit = st->time_limit;
    p_hi.time_limit = st->time_limit;

    int wins_hi = 0, wins_lo = 0, draws = 0;
    int half = st->n_games / 2;
    if (half < 1) half = 1;

    for (int g = 0; g < half; g++) {
        /* Partita 1: hi=NERO, lo=BIANCO */
        Board b; board_init(&b);
        int max_ply = 300;
        for (int ply = 0; ply < max_ply; ply++) {
            Move mvs[MAX_MOVES];
            int  nm = board_gen_moves(&b, mvs);
            int  r  = board_result(&b, nm);
            if (r != 0 || nm == 0 || b.nocap >= 80) {
                int res = (nm == 0) ? ((b.stm==BLACK)?-1:1) : r;
                if (res > 0)      wins_hi++;
                else if (res < 0) wins_lo++;
                else              draws++;
                goto next1;
            }
            MCTSParams *cp = (b.stm == BLACK) ? &p_hi : &p_lo;
            MCTSPool   *pp = (b.stm == BLACK) ? pool_a : pool_b;
            Move mv = mcts_best_move(&b, cp, pp);
            bool ok = false;
            for (int i = 0; i < nm; i++) {
                if (mvs[i].from==mv.from && mvs[i].to==mv.to &&
                    mvs[i].cap_mask==mv.cap_mask) { mv=mvs[i]; ok=true; break; }
            }
            if (!ok) mv = mvs[0];
            board_do_move(&b, &mv);
        }
        draws++;
        next1:;

        /* Partita 2: lo=NERO, hi=BIANCO */
        board_init(&b);
        for (int ply = 0; ply < max_ply; ply++) {
            Move mvs[MAX_MOVES];
            int  nm = board_gen_moves(&b, mvs);
            int  r  = board_result(&b, nm);
            if (r != 0 || nm == 0 || b.nocap >= 80) {
                int res = (nm == 0) ? ((b.stm==BLACK)?-1:1) : r;
                if (res > 0)      wins_lo++;
                else if (res < 0) wins_hi++;
                else              draws++;
                goto next2;
            }
            MCTSParams *cp = (b.stm == BLACK) ? &p_lo : &p_hi;
            MCTSPool   *pp = (b.stm == BLACK) ? pool_a : pool_b;
            Move mv = mcts_best_move(&b, cp, pp);
            bool ok = false;
            for (int i = 0; i < nm; i++) {
                if (mvs[i].from==mv.from && mvs[i].to==mv.to &&
                    mvs[i].cap_mask==mv.cap_mask) { mv=mvs[i]; ok=true; break; }
            }
            if (!ok) mv = mvs[0];
            board_do_move(&b, &mv);
        }
        draws++;
        next2:;
    }

    /* Gradient step: sposta theta verso il valore vincente */
    int total = wins_hi + wins_lo + draws;
    if (total > 0) {
        /* gradient ∝ (P(hi wins) - 0.5) */
        float score_hi = (wins_hi + 0.5f * draws) / (float)total;
        float eta      = st->sigma * 0.5f;  /* step size */
        float delta    = eta * (2.0f * score_hi - 1.0f);
        st->theta += delta;
    }

    /* Clampa theta ai limiti */
    if (st->theta < st->lo) st->theta = st->lo;
    if (st->theta > st->hi) st->theta = st->hi;

    /* Annealing sigma: riduci del 10% per round */
    st->sigma *= 0.90f;
    if (st->sigma < 0.01f) st->sigma = 0.01f;

    st->n_rounds++;

    if (st->verbose) {
        printf("  CLOP [%s] round %2d: x_lo=%.3f x_hi=%.3f  "
               "W+=%d D=%d W-=%d  → theta=%.4f (σ=%.4f)\n",
               clop_param_name(st->param), st->n_rounds,
               x_lo, x_hi, wins_hi, draws, wins_lo,
               st->theta, st->sigma);
        fflush(stdout);
    }

    return st->theta;
}

/* ── clop_optimize ── */
float clop_optimize(CLOPState *st, int max_rounds,
                    const MCTSParams *base,
                    MCTSPool *pool_a, MCTSPool *pool_b) {
    if (st->verbose)
        printf("\n[CLOP] Ottimizzazione di %s  (init=%.3f, sigma=%.3f)\n",
               clop_param_name(st->param), st->theta, st->sigma);

    for (int r = 0; r < max_rounds; r++) {
        clop_run_round(st, base, pool_a, pool_b);
        /* Early stop se sigma è molto piccola */
        if (st->sigma < 0.02f) break;
    }

    if (st->verbose)
        printf("  CLOP finale: %s = %.4f  (dopo %d round)\n\n",
               clop_param_name(st->param), st->theta, st->n_rounds);

    return st->theta;
}

/* ── clop_apply ── */
void clop_apply(const CLOPState *st, MCTSParams *p) {
    clop_set_param(p, st->param, st->theta);
}

/* ── clop_print ── */
void clop_print(const CLOPState *st) {
    printf("  CLOP %s: theta=%.4f  sigma=%.4f  rounds=%d\n",
           clop_param_name(st->param), st->theta, st->sigma, st->n_rounds);
}
