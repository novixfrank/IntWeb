#include "board.h"
#include "mcts.h"
#include "tuning.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ══════════════════════════════════════════════
 *  Modalità terminale: AI vs AI per analisi
 * ══════════════════════════════════════════════ */
static void run_selfplay(int n_games, const MCTSParams *p1, const MCTSParams *p2,
                          const char *name1, const char *name2) {
    MCTSPool *pool_a = mcts_pool_create();
    MCTSPool *pool_b = mcts_pool_create();

    int w1 = 0, w2 = 0, draws = 0;
    printf("\n[Self-play] %s vs %s  (%d partite)\n", name1, name2, n_games);

    for (int g = 0; g < n_games; g++) {
        Board b;
        board_init(&b);
        int max_ply = 400;

        for (int ply = 0; ply < max_ply; ply++) {
            Move mvs[MAX_MOVES];
            int nm = board_gen_moves(&b, mvs);
            int r  = board_result(&b, nm);
            if (r != 0 || nm == 0) {
                int res = (nm == 0) ? ((b.stm == BLACK) ? -1 : 1) : r;
                if (res > 0)       { if (g%2==0) w1++; else w2++; }
                else if (res < 0)  { if (g%2==0) w2++; else w1++; }
                else               draws++;
                goto next_game;
            }

            const MCTSParams *cp;
            MCTSPool *cp_pool;
            if (g % 2 == 0) {
                cp = (b.stm == BLACK) ? p1 : p2;
                cp_pool = (b.stm == BLACK) ? pool_a : pool_b;
            } else {
                cp = (b.stm == BLACK) ? p2 : p1;
                cp_pool = (b.stm == BLACK) ? pool_b : pool_a;
            }
            Move mv = mcts_best_move(&b, cp, cp_pool);

            bool ok = false;
            for (int i = 0; i < nm; i++) {
                if (mvs[i].from==mv.from && mvs[i].to==mv.to &&
                    mvs[i].cap_mask==mv.cap_mask) {
                    mv = mvs[i]; ok = true; break;
                }
            }
            if (!ok) mv = mvs[0];
            board_do_move(&b, &mv);
        }
        draws++; /* timeout */
        next_game:
        printf("  Partita %d: %s W=%d D=%d %s W=%d\n",
               g+1, name1, w1, draws, name2, w2);
        fflush(stdout);
    }

    printf("Risultato finale: %s=%d  %s=%d  Pareggi=%d\n\n",
           name1, w1, name2, w2, draws);
    mcts_pool_free(pool_a);
    mcts_pool_free(pool_b);
}

/* ══════════════════════════════════════════════
 *  Analisi sperimentale da terminale
 * ══════════════════════════════════════════════ */
static void run_analysis(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║    ANALISI SPERIMENTALE – DAMA ITALIANA MCTS             ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    MCTSPool *pool = mcts_pool_create();

    /* Carica o usa default */
    MCTSParams ucb1 = mcts_default_params(); ucb1.model = SEL_UCB1;
    MCTSParams puct = mcts_default_params(); puct.model = SEL_PUCT;
    tuning_load("dama_params.txt", &ucb1, &puct);

    /* Tabella simulazioni/sec */
    tuning_print_report(&ucb1, &puct, pool);

    /* Match a diversi budget */
    double budgets[] = {0.2, 1.0, 3.0};
    const char *bl[]  = {"0.2s", "1.0s", "3.0s"};
    for (int bi = 0; bi < 3; bi++) {
        MCTSParams u = ucb1; u.time_limit = budgets[bi];
        MCTSParams p = puct; p.time_limit = budgets[bi];
        char n1[32], n2[32];
        snprintf(n1, sizeof(n1), "UCB1(%s)", bl[bi]);
        snprintf(n2, sizeof(n2), "PUCT(%s)", bl[bi]);
        run_selfplay(6, &u, &p, n1, n2);
    }

    mcts_pool_free(pool);
    printf("Analisi completata.\n\n");
}

/* ══════════════════════════════════════════════
 *  Stampa aiuto
 * ══════════════════════════════════════════════ */
static void print_usage(const char *prog) {
    printf("Utilizzo: %s [OPZIONE]\n", prog);
    printf("  (nessuna)    Avvia la GUI SDL2\n");
    printf("  --tuning     Esegue il tuning genetico e salva i parametri\n");
    printf("  --clop       Tuning sequenziale CLOP (più preciso per param singoli)\n");
    printf("  --analysis   Analisi sperimentale (self-play)\n");
    printf("  --benchmark  Benchmark velocità MCTS\n");
    printf("  --help       Mostra questo messaggio\n\n");
    printf("Dama Italiana – MCTS AI\n");
    printf("Regole italiane complete, bitboard, UCB1 + PUCT\n\n");
}

/* ══════════════════════════════════════════════
 *  Benchmark velocità
 * ══════════════════════════════════════════════ */
static void run_benchmark(void) {
    printf("\n=== BENCHMARK MCTS ===\n\n");
    board_lookup_init();
    MCTSPool *pool = mcts_pool_create();

    MCTSParams ucb1 = mcts_default_params(); ucb1.model = SEL_UCB1;
    MCTSParams puct = mcts_default_params(); puct.model = SEL_PUCT;
    tuning_load("dama_params.txt", &ucb1, &puct);

    Board b; board_init(&b);

    printf("%-12s  %-8s  %-12s  %-10s  %-10s\n",
           "Modello", "Budget", "Sims", "Sims/sec", "Nodi");
    printf("------------------------------------------------------\n");

    double budgets[] = {0.2, 1.0, 3.0};
    const char *blabels[] = {"0.2s", "1.0s", "3.0s"};

    for (int bi = 0; bi < 3; bi++) {
        /* UCB1 */
        ucb1.time_limit = budgets[bi];
        double t0 = now_sec();
        mcts_best_move(&b, &ucb1, pool);
        double el = now_sec() - t0;
        MCTSStats s = mcts_get_stats(pool);
        printf("%-12s  %-8s  %-12lld  %-10.0f  %-10d\n",
               "UCB1", blabels[bi],
               (long long)s.total_sims,
               el > 0 ? (double)s.total_sims/el : 0.0,
               s.tree_size);

        /* PUCT */
        puct.time_limit = budgets[bi];
        t0 = now_sec();
        mcts_best_move(&b, &puct, pool);
        el = now_sec() - t0;
        s = mcts_get_stats(pool);
        printf("%-12s  %-8s  %-12lld  %-10.0f  %-10d\n",
               "PUCT", blabels[bi],
               (long long)s.total_sims,
               el > 0 ? (double)s.total_sims/el : 0.0,
               s.tree_size);
    }
    printf("\n");

    /* Test generatore mosse */
    printf("=== BENCHMARK Generatore Mosse ===\n");
    {
        Move mvs[MAX_MOVES];
        int reps = 1000000;
        double t0 = now_sec();
        int total = 0;
        for (int i = 0; i < reps; i++) total += board_gen_moves(&b, mvs);
        double el = now_sec() - t0;
        printf("board_gen_moves: %.0f k-calls/sec  (%.0f mosse medie)\n",
               (double)reps / el / 1000.0,
               (double)total / reps);
    }

    mcts_pool_free(pool);
    printf("\nBenchmark completato.\n\n");
}

/* ══════════════════════════════════════════════
 *  MAIN
 * ══════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    srand((unsigned int)time(NULL));

    /* Inizializza lookup table (obbligatorio prima di qualsiasi operazione) */
    board_lookup_init();

    /* ── Modalità riga di comando ── */
    if (argc >= 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "--tuning") == 0) {
            printf("\n=== TUNING IPERPARAMETRI (Algoritmo Genetico) ===\n");
            printf("Questo processo richiede diversi minuti.\n\n");

            TuningParams tp = tuning_default_params();
            tp.verbose = 1;
            /* Parametri rapidi se specificato --fast */
            if (argc >= 3 && strcmp(argv[2], "--fast") == 0) {
                tp.generations = 3;
                tp.pop_size    = 6;
                tp.games_per_pair = 2;
                printf("[Modalità fast: %d gen, %d individui]\n\n",
                       tp.generations, tp.pop_size);
            }

            MCTSParams best_ucb1, best_puct;
            tuning_run(&best_ucb1, &best_puct, &tp);
            tuning_save("dama_params.txt", &best_ucb1, &best_puct);

            printf("\nParametri salvati in dama_params.txt\n");
            printf("UCB1: C=%.4f  prior_cap=%.4f  prior_promo=%.4f\n",
                   best_ucb1.C, best_ucb1.prior_cap, best_ucb1.prior_promo);
            printf("PUCT: c_puct=%.4f  prior_cap=%.4f  prior_promo=%.4f\n\n",
                   best_puct.c_puct, best_puct.prior_cap, best_puct.prior_promo);
            return 0;
        }
        if (strcmp(argv[1], "--analysis") == 0) {
            run_analysis();
            return 0;
        }
        if (strcmp(argv[1], "--benchmark") == 0) {
            run_benchmark();
            return 0;
        }
        if (strcmp(argv[1], "--clop") == 0) {
            printf("\n=== CLOP – Ottimizzazione sequenziale parametri ===\n\n");
            MCTSPool *pool = mcts_pool_create();
            MCTSPool *pool2 = mcts_pool_secondary();
            MCTSParams base = mcts_default_params();
            tuning_load("dama_params.txt", &base, &base);

            /* Ottimizza C per UCB1 */
            CLOPState st_C;
            clop_init(&st_C, CLOP_PARAM_C,
                      base.C, 0.4f, 0.1f, 4.0f, 4, 0.2);
            clop_optimize(&st_C, 12, &base, pool, pool2);
            base.C = st_C.theta;

            /* Ottimizza c_puct per PUCT */
            MCTSParams base_puct = base;
            base_puct.model = SEL_PUCT;
            CLOPState st_puct;
            clop_init(&st_puct, CLOP_PARAM_C_PUCT,
                      base_puct.c_puct, 0.5f, 0.1f, 5.0f, 4, 0.2);
            clop_optimize(&st_puct, 12, &base_puct, pool, pool2);
            base_puct.c_puct = st_puct.theta;

            MCTSParams best_ucb1 = base;
            MCTSParams best_puct = base_puct;
            tuning_save("dama_params.txt", &best_ucb1, &best_puct);
            printf("Parametri CLOP salvati in dama_params.txt\n");
            mcts_pool_free(pool);
            mcts_pool_free(pool2);
            return 0;
        }
        fprintf(stderr, "Opzione sconosciuta: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    /* ── Modalità GUI (default) ── */
    GUI g;
    if (gui_init(&g) != 0) {
        fprintf(stderr, "Errore inizializzazione GUI.\n");
        return 1;
    }
    gui_run(&g);
    gui_destroy(&g);
    return 0;
}
