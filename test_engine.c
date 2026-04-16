/*
 * test_engine.c – test standalone (senza SDL) per:
 *   1. Generatore di mosse / regole Dama Italiana
 *   2. Correttezza board_do_move / board_result
 *   3. Correttezza MCTS (mossa valida, non crash)
 *   4. Statistiche rapide di performance
 *
 * Compilare con:
 *   gcc -std=c11 -O2 -D_POSIX_C_SOURCE=200809L -I src src/board.c src/mcts.c test_engine.c -lm -o test_engine
 */

#include "board.h"
#include "mcts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

/* ─── Contatori test ─── */
static int g_tests = 0, g_passed = 0;

#define CHECK(expr, msg) do { \
    g_tests++; \
    if (!(expr)) { printf("  FAIL [%d]: %s\n", __LINE__, msg); } \
    else         { g_passed++; printf("  OK   [%d]: %s\n", __LINE__, msg); } \
} while(0)

/* ══════════════════════════════════════════════
 *  Test 1 – Layout bitboard e conversioni
 * ══════════════════════════════════════════════ */
static void test_layout(void) {
    printf("\n=== Test 1: Layout bitboard ===\n");

    /* sq_to_rc / rc_to_sq devono essere inverse */
    for (int sq = 0; sq < NUM_SQ; sq++) {
        int row, col;
        sq_to_rc(sq, &row, &col);
        int sq2 = rc_to_sq(row, col);
        CHECK(sq2 == sq, "sq→rc→sq round-trip");
    }

    /* Righe di partenza */
    Board b; board_init(&b);

    int bn = __builtin_popcount(b.bm);
    int wn = __builtin_popcount(b.wm);
    CHECK(bn == 12, "NERO: 12 pedine iniziali");
    CHECK(wn == 12, "BIANCO: 12 pedine iniziali");
    CHECK(b.bk == 0, "NERO: 0 dame iniziali");
    CHECK(b.wk == 0, "BIANCO: 0 dame iniziali");
    CHECK(b.stm == BLACK, "Muove il NERO per primo");

    /* Le pedine nere devono stare nelle righe 0-2 (bit 0-11) */
    CHECK((b.bm & 0xFFFFF000u) == 0, "Pedine nere nelle righe 0-2");
    /* Le pedine bianche nelle righe 5-7 (bit 20-31) */
    CHECK((b.wm & 0x000FFFFFu) == 0, "Pedine bianche nelle righe 5-7");
}

/* ══════════════════════════════════════════════
 *  Test 2 – Generazione mosse posizione iniziale
 * ══════════════════════════════════════════════ */
static void test_movegen_initial(void) {
    printf("\n=== Test 2: Mosse posizione iniziale ===\n");
    Board b; board_init(&b);
    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    CHECK(n > 0,   "Posizione iniziale: almeno una mossa");
    CHECK(n <= 20, "Posizione iniziale: non più di 20 mosse");
    printf("    Mosse iniziali NERO: %d\n", n);

    /* Nessuna mossa deve essere una cattura (board iniziale) */
    for (int i = 0; i < n; i++) {
        CHECK(mvs[i].num_caps == 0, "Nessuna cattura in posizione iniziale");
    }

    /* Tutte le mosse devono essere verso il basso (righe crescenti) per il NERO */
    for (int i = 0; i < n; i++) {
        int rf, cf, rt, ct;
        sq_to_rc(mvs[i].from, &rf, &cf);
        sq_to_rc(mvs[i].to,   &rt, &ct);
        CHECK(rt > rf, "NERO muove verso il basso");
    }
}

/* ══════════════════════════════════════════════
 *  Test 3 – Presa obbligatoria (solo in avanti per le pedine)
 *
 *  FIX BUG 1: la posizione usa una pedina bianca AVANTI rispetto
 *  al nero, non dietro. La cattura avviene in direzione DIR_DL
 *  (avanti-sinistra per il NERO).
 *
 *  Layout:
 *    sq=9  (NERO, riga 2, col 3)
 *     ↘ avanti-sinistra (DIR_DL)
 *    sq=13 (BIANCO, riga 3, col 2)  ← pezzo da catturare
 *     ↘
 *    sq=16 (atterraggio, riga 4, col 1)
 * ══════════════════════════════════════════════ */
static void test_mandatory_capture(void) {
    printf("\n=== Test 3: Presa obbligatoria (avanti) ===\n");
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm  = (1u << 9);    /* nero a sq=9  (riga 2, col 3) */
    b.wm  = (1u << 13);   /* bianco a sq=13 (riga 3, col 2) – in avanti */
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    CHECK(n > 0, "Deve esserci almeno una mossa");
    bool all_caps = true;
    for (int i = 0; i < n; i++) {
        if (mvs[i].num_caps == 0) { all_caps = false; break; }
    }
    CHECK(all_caps, "Con catture disponibili, tutte le mosse devono essere catture");

    /* Applica la cattura e verifica gli effetti */
    board_do_move(&b, &mvs[0]);
    CHECK(!(b.wm & (1u<<13)), "Pedina bianca rimossa dopo cattura (sq=13)");
    CHECK(b.stm == WHITE,     "Dopo mossa del NERO, tocca al BIANCO");
}

/* ══════════════════════════════════════════════
 *  Test 4 – Promozione a dama
 * ══════════════════════════════════════════════ */
static void test_promotion(void) {
    printf("\n=== Test 4: Promozione ===\n");
    /*
     *  Pedina nera a sq=24 (riga 6) può muoversi a sq=28 (riga 7 = promozione).
     *  sq=28 deve essere libera.
     */
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm  = (1u << 24);
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);
    CHECK(n > 0, "Pedina riga 6: almeno una mossa disponibile");

    bool found_promo = false;
    for (int i = 0; i < n; i++) {
        if (mvs[i].promotes) { found_promo = true; break; }
    }
    CHECK(found_promo, "Deve esserci una mossa di promozione");

    /* Applica la promozione */
    Move promo_mv;
    for (int i = 0; i < n; i++) {
        if (mvs[i].promotes) { promo_mv = mvs[i]; break; }
    }
    board_do_move(&b, &promo_mv);
    CHECK((b.bk & ~b.bm) != 0, "Dopo promozione NERO: c'è una dama nera");
    CHECK(!(b.bm & (1u << promo_mv.to)), "Casella arrivo non è pedina ma dama");
}

/* ══════════════════════════════════════════════
 *  Test 5 – Regola italiana: pedine non catturano dame
 *
 *  FIX BUG 1: la dama bianca è posizionata AVANTI rispetto alla
 *  pedina nera (stessa posizione del test 3), così la pedina la
 *  "vede" in direzione avanti ma non può catturarla.
 *  Cross-check: una PEDINA bianca nella stessa posizione È catturabile.
 * ══════════════════════════════════════════════ */
static void test_men_cannot_capture_kings(void) {
    printf("\n=== Test 5: Pedine non catturano dame avversarie ===\n");
    /*
     *  Pedina nera a sq=9 (riga 2, col 3).
     *  Dama bianca a sq=13 (riga 3, col 2) → avanti-sinistra (DIR_DL) per il NERO.
     *  La pedina NON può catturare la dama (regola italiana).
     */
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm  = (1u << 9);
    b.wk  = (1u << 13);   /* DAMA bianca in posizione avanti */
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    /* Non deve esistere alcuna cattura della dama (sq=13 non compare in cap_mask) */
    bool cap_king = false;
    for (int i = 0; i < n; i++) {
        if (mvs[i].cap_mask & (1u << 13)) { cap_king = true; break; }
    }
    CHECK(!cap_king, "Pedina nera NON cattura dama bianca (regola italiana)");

    /* Cross-check: con una PEDINA bianca nella stessa posizione, la cattura è possibile */
    Board b2;
    memset(&b2, 0, sizeof(b2));
    b2.bm  = (1u << 9);
    b2.wm  = (1u << 13);   /* PEDINA bianca, stessa posizione */
    b2.stm = BLACK;
    Move mvs2[MAX_MOVES];
    int n2 = board_gen_moves(&b2, mvs2);
    bool cap_pawn = false;
    for (int i = 0; i < n2; i++) {
        if (mvs2[i].cap_mask & (1u << 13)) { cap_pawn = true; break; }
    }
    CHECK(cap_pawn, "Pedina nera PUÒ catturare pedina bianca in avanti (cross-check)");
}

/* ══════════════════════════════════════════════
 *  Test 6 – Dame: movimento di una sola casella (FIX BUG 2)
 *
 *  La dama si muove di UNA sola casella per volta in tutte le
 *  direzioni disponibili, non su raggi interi.
 *  Da sq=0 (angolo riga 0), solo le direzioni verso il basso
 *  sono valide: DIR_DL → sq=4, DIR_DR → sq=5.
 * ══════════════════════════════════════════════ */
static void test_king_single_step(void) {
    printf("\n=== Test 6: Dame – una casella per volta (no scorrimento) ===\n");
    Board b;
    memset(&b, 0, sizeof(b));
    b.bk  = (1u << 0);   /* dama nera in angolo (riga 0, col 1) */
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    /* Da sq=0 con singolo passo: DIR_DL→sq=4, DIR_DR→sq=5. Totale: 2 mosse. */
    CHECK(n >= 2, "Dama ha almeno 2 mosse disponibili dall'angolo");
    printf("    Mosse dama da sq=0 (board vuota): %d\n", n);

    /* Tutte devono essere king_move */
    bool all_king = true;
    for (int i = 0; i < n; i++) {
        if (!mvs[i].king_move) { all_king = false; break; }
    }
    CHECK(all_king, "Tutte le mosse della dama hanno king_move=true");

    /* Con movimento a una sola casella, da sq=0 le uniche destinazioni
     * valide sono sq=4 (DIR_DL) e sq=5 (DIR_DR). Nessuna altra. */
    bool only_single_step = true;
    for (int i = 0; i < n; i++) {
        if (mvs[i].to != 4 && mvs[i].to != 5) {
            only_single_step = false; break;
        }
    }
    CHECK(only_single_step, "Dama muove di una sola casella (to = 4 o 5)");
}

/* ══════════════════════════════════════════════
 *  Test 7 – Fine partita: pareggio 80 semi-mosse
 * ══════════════════════════════════════════════ */
static void test_draw_rule(void) {
    printf("\n=== Test 7: Pareggio (80 semi-mosse senza cattura) ===\n");
    Board b; board_init(&b);
    b.nocap = 80;
    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);
    int r = board_result(&b, n);
    CHECK(r == 0, "board_result=0 con nocap=80 (pareggio)");
}

/* ══════════════════════════════════════════════
 *  Test 8 – Fine partita: nessun pezzo
 * ══════════════════════════════════════════════ */
static void test_no_pieces(void) {
    printf("\n=== Test 8: Vittoria per assenza pezzi ===\n");
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm = (1u << 0);  /* solo nero */
    b.stm = WHITE;
    int r = board_result(&b, 5);
    CHECK(r == 1, "NERO vince se BIANCO non ha pezzi (r=1)");
}

/* ══════════════════════════════════════════════
 *  Test 9 – MCTS: mossa valida
 * ══════════════════════════════════════════════ */
static void test_mcts_valid_move(void) {
    printf("\n=== Test 9: MCTS restituisce mossa valida ===\n");
    Board b; board_init(&b);
    MCTSParams p = mcts_default_params();
    p.time_limit = 0.1;
    MCTSPool *pool = mcts_pool_create();

    Move mv = mcts_best_move(&b, &p, pool);
    MCTSStats s = mcts_get_stats(pool);

    printf("    Sims: %lld  Nodi: %d\n", (long long)s.total_sims, s.tree_size);
    CHECK(s.total_sims > 0, "Almeno una simulazione eseguita");

    Move legal[MAX_MOVES];
    int nl = board_gen_moves(&b, legal);
    bool found = false;
    for (int i = 0; i < nl; i++) {
        if (legal[i].from == mv.from &&
            legal[i].to   == mv.to   &&
            legal[i].cap_mask == mv.cap_mask) {
            found = true; break;
        }
    }
    CHECK(found, "Mossa MCTS è legale");
    mcts_pool_free(pool);
}

/* ══════════════════════════════════════════════
 *  Test 10 – Partita completa AI vs AI (terminazione)
 * ══════════════════════════════════════════════ */
static void test_full_game(void) {
    printf("\n=== Test 10: Partita completa AI vs AI ===\n");
    Board b; board_init(&b);
    MCTSParams p = mcts_default_params();
    p.time_limit = 0.05;
    MCTSPool *pool = mcts_pool_create();

    int max_ply = 400;   /* aumentato: con dame a 1 passo le partite possono essere più lunghe */
    int ply;
    for (ply = 0; ply < max_ply; ply++) {
        Move mvs[MAX_MOVES];
        int nm = board_gen_moves(&b, mvs);
        int r  = board_result(&b, nm);
        /* board_result restituisce 0 sia per "in corso" che per "pareggio" (nocap>=80);
         * controlliamo nocap esplicitamente per non continuare a giocare dopo il pareggio */
        if (r != 0 || nm == 0 || b.nocap >= 80) {
            printf("    Partita terminata al ply %d  result=%d\n", ply, r);
            break;
        }
        Move mv = mcts_best_move(&b, &p, pool);
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

    CHECK(ply < max_ply, "La partita è terminata entro il limite di mosse");
    mcts_pool_free(pool);
}

/* ══════════════════════════════════════════════
 *  Test 11 – Priorità italiana (max catture, solo in avanti)
 *
 *  FIX BUG 1: la posizione usa catture in avanti.
 *
 *  Pedina nera a sq=1 (riga 0, col 3).
 *  Pedine bianche a sq=5, sq=6, sq=14.
 *
 *  Percorso A: sq=1 →DIR_DL→ cattura sq=5 → atterra sq=8  (1 cattura)
 *  Percorso B: sq=1 →DIR_DR→ cattura sq=6 → atterra sq=10
 *                         →DIR_DL→ cattura sq=14 → atterra sq=17 (2 catture)
 *
 *  La priorità italiana deve selezionare SOLO il percorso B (max catture=2).
 * ══════════════════════════════════════════════ */
static void test_italian_priority(void) {
    printf("\n=== Test 11: Priorità italiana (max catture, avanti) ===\n");
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm  = (1u << 1);                              /* nero a sq=1  (0,3) */
    b.wm  = (1u << 5) | (1u << 6) | (1u << 14);    /* bianchi a sq=5,6,14 */
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    CHECK(n > 0, "Ci sono mosse di cattura");

    if (n > 0) {
        int max_caps = 0;
        for (int i = 0; i < n; i++)
            if (mvs[i].num_caps > max_caps) max_caps = mvs[i].num_caps;

        bool all_max = true;
        for (int i = 0; i < n; i++)
            if (mvs[i].num_caps != max_caps) { all_max = false; break; }

        CHECK(all_max, "Tutte le mosse hanno il numero massimo di catture");
        CHECK(max_caps == 2, "Max catture = 2 (percorso B: sq6 + sq14)");
        printf("    Max catture: %d  mosse generate: %d\n", max_caps, n);
    }
}

/* ══════════════════════════════════════════════
 *  Test 12 – PUCT a 0.2s
 * ══════════════════════════════════════════════ */
static void test_puct(void) {
    printf("\n=== Test 12: PUCT a 0.2s ===\n");
    Board b; board_init(&b);
    MCTSParams p = mcts_default_params();
    p.model      = SEL_PUCT;
    p.time_limit = 0.2;
    MCTSPool *pool = mcts_pool_create();

    Move mv = mcts_best_move(&b, &p, pool);
    MCTSStats s = mcts_get_stats(pool);

    printf("    PUCT sims: %lld  nodi: %d  sims/s: %.0f\n",
           (long long)s.total_sims, s.tree_size,
           0.2 > 0 ? (double)s.total_sims / 0.2 : 0.0);
    CHECK(s.total_sims > 0, "PUCT: almeno una simulazione");

    Move legal[MAX_MOVES];
    int nl = board_gen_moves(&b, legal);
    bool found = false;
    for (int i = 0; i < nl; i++) {
        if (legal[i].from == mv.from && legal[i].to == mv.to &&
            legal[i].cap_mask == mv.cap_mask) {
            found = true; break;
        }
    }
    CHECK(found, "PUCT: mossa legale");
    mcts_pool_free(pool);
}

/* ══════════════════════════════════════════════
 *  Test 13 – Draw detection: nocap >= 80 termina il rollout
 * ══════════════════════════════════════════════ */
static void test_draw_detection(void) {
    printf("\n=== Test 13: Draw detection nel rollout (nocap >= 80) ===\n");
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm    = (1u << 0);
    b.wm    = (1u << 31);
    b.stm   = BLACK;
    b.nocap = 79;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);
    CHECK(n > 0, "Ci sono mosse disponibili con nocap=79");

    board_do_move(&b, &mvs[0]);
    CHECK(b.nocap == 80, "nocap == 80 dopo mossa senza cattura");

    int res = board_result(&b, 1);
    CHECK(res == 0, "board_result = 0 con nocap=80 (pareggio)");

    MCTSParams p = mcts_default_params();
    p.time_limit = 0.05;
    MCTSPool *pool = mcts_pool_create();
    Move mv2 = mcts_best_move(&b, &p, pool);
    CHECK(pool->sims >= 0, "MCTS non va in loop con nocap=80");
    (void)mv2;
    mcts_pool_free(pool);
}

/* ══════════════════════════════════════════════
 *  Test 14 – mcts_search API wrapper
 * ══════════════════════════════════════════════ */
static void test_mcts_search_api(void) {
    printf("\n=== Test 14: mcts_search / mcts_init API ===\n");
    Board b; board_init(&b);
    MCTSParams p = mcts_default_params();
    p.time_limit = 0.05;
    MCTSPool *pool = mcts_pool_create();

    mcts_init(pool, &b);
    Move mv = mcts_search(pool, &b, &p);

    Move legal[MAX_MOVES];
    int nl = board_gen_moves(&b, legal);
    bool found = false;
    for (int i = 0; i < nl; i++) {
        if (legal[i].from == mv.from && legal[i].to == mv.to &&
            legal[i].cap_mask == mv.cap_mask) {
            found = true; break;
        }
    }
    CHECK(found, "mcts_search restituisce mossa legale");
    mcts_pool_free(pool);
}

/* ══════════════════════════════════════════════
 *  Test 15 – Pedine NON mangiano all'indietro (verifica BUG 1)
 * ══════════════════════════════════════════════ */
static void test_no_backward_capture(void) {
    printf("\n=== Test 15: Pedine NON catturano all'indietro ===\n");
    /*
     *  Pedina nera a sq=13 (riga 3, col 2).
     *  Pedina bianca a sq=9  (riga 2, col 3) → DIETRO rispetto al NERO.
     *  La pedina nera NON deve poter catturare sq=9 (è all'indietro).
     */
    Board b;
    memset(&b, 0, sizeof(b));
    b.bm  = (1u << 13);   /* nero a sq=13 (riga 3) */
    b.wm  = (1u << 9);    /* bianco a sq=9  (riga 2) – DIETRO il nero */
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    /* Non deve esistere nessuna cattura (sq=9 è indietro) */
    bool cap_found = false;
    for (int i = 0; i < n; i++) {
        if (mvs[i].num_caps > 0) { cap_found = true; break; }
    }
    CHECK(!cap_found, "Pedina nera NON cattura all'indietro (sq=9 è dietro)");

    /* Le mosse generate devono essere solo semplici in avanti */
    bool all_forward = true;
    for (int i = 0; i < n; i++) {
        int rf, cf, rt, ct;
        sq_to_rc(mvs[i].from, &rf, &cf);
        sq_to_rc(mvs[i].to,   &rt, &ct);
        if (rt <= rf) { all_forward = false; break; }
    }
    CHECK(all_forward, "Tutte le mosse della pedina sono in avanti");
}

/* ══════════════════════════════════════════════
 *  Test 16 – Dame mangiano in tutte le direzioni (incluso indietro)
 *  ma di una sola casella per volta (BUG 2 fix)
 * ══════════════════════════════════════════════ */
static void test_king_captures_all_directions(void) {
    printf("\n=== Test 16: Dame catturano in tutte le direzioni (1 passo) ===\n");
    /*
     *  Dama nera a sq=13 (riga 3, col 2).
     *  Pedine bianche a sq=9 (dietro-destra) e sq=17 (avanti-destra).
     *
     *  sq=13 a (3,2):
     *    DIR_UR (dietro-destra): (2,3)=sq=9  → cattura backward ✓
     *    DIR_DR (avanti-destra): (4,3)=sq=17 → cattura forward  ✓
     *
     *  La dama deve poter catturare entrambe.
     */
    Board b;
    memset(&b, 0, sizeof(b));
    b.bk  = (1u << 13);   /* dama nera a sq=13 */
    b.wm  = (1u << 9) | (1u << 17);   /* pedine bianche a sq=9 e sq=17 */
    b.stm = BLACK;

    Move mvs[MAX_MOVES];
    int n = board_gen_moves(&b, mvs);

    CHECK(n > 0, "La dama ha catture disponibili");

    /* Deve poter catturare sq=9 (indietro) */
    bool cap_back = false;
    for (int i = 0; i < n; i++) {
        if (mvs[i].cap_mask & (1u << 9)) { cap_back = true; break; }
    }
    CHECK(cap_back, "La dama cattura all'indietro (sq=9)");

    /* Deve poter catturare sq=17 (avanti) */
    bool cap_fwd = false;
    for (int i = 0; i < n; i++) {
        if (mvs[i].cap_mask & (1u << 17)) { cap_fwd = true; break; }
    }
    CHECK(cap_fwd, "La dama cattura in avanti (sq=17)");

    /* La dama si muove di una sola casella: nessun salto "volante" */
    bool single_step_only = true;
    for (int i = 0; i < n; i++) {
        /* Con singolo passo da sq=13:
         *   cattura sq=9  (DIR_UR) → atterra sq=6
         *   cattura sq=17 (DIR_DR) → atterra sq=22 */
        int to = mvs[i].to;
        if (to != 6 && to != 22) { single_step_only = false; break; }
    }
    CHECK(single_step_only, "La dama atterra sulla casella adiacente al pezzo catturato");
}

/* ══════════════════════════════════════════════
 *  Benchmark (non fa assert, solo stampa)
 * ══════════════════════════════════════════════ */
static void bench_movegen(void) {
    printf("\n=== Benchmark: board_gen_moves ===\n");
    Board b; board_init(&b);
    Move mvs[MAX_MOVES];

    int reps = 2000000;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile int total = 0;
    for (int i = 0; i < reps; i++) total += board_gen_moves(&b, mvs);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("    %d iterazioni in %.3f s → %.0f k-call/s  (%.0f mosse medie)\n",
           reps, el, (double)reps / el / 1000.0, (double)total / reps);
}

static void bench_mcts(void) {
    printf("\n=== Benchmark: MCTS a 1.0s ===\n");
    Board b; board_init(&b);
    MCTSPool *pool = mcts_pool_create();

    struct timespec t0, t1;

    /* UCB1 */
    {
        MCTSParams p = mcts_default_params();
        p.model = SEL_UCB1;
        p.time_limit = 1.0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        mcts_best_move(&b, &p, pool);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        MCTSStats s = mcts_get_stats(pool);
        printf("    UCB1 1.0s: sims=%lld  nodi=%d  sims/s=%.0f\n",
               (long long)s.total_sims, s.tree_size,
               el > 0 ? (double)s.total_sims / el : 0.0);
    }

    /* PUCT */
    {
        MCTSParams p = mcts_default_params();
        p.model = SEL_PUCT;
        p.time_limit = 1.0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        mcts_best_move(&b, &p, pool);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        MCTSStats s = mcts_get_stats(pool);
        printf("    PUCT 1.0s: sims=%lld  nodi=%d  sims/s=%.0f\n",
               (long long)s.total_sims, s.tree_size,
               el > 0 ? (double)s.total_sims / el : 0.0);
    }

    mcts_pool_free(pool);
}

/* ══════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║    DAMA ITALIANA – Test Suite Engine                 ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    board_lookup_init();
    srand((unsigned int)time(NULL));

    test_layout();
    test_movegen_initial();
    test_mandatory_capture();
    test_promotion();
    test_men_cannot_capture_kings();
    test_king_single_step();        /* ex test_flying_king – aggiornato per bug 2 */
    test_draw_rule();
    test_no_pieces();
    test_mcts_valid_move();
    test_full_game();
    test_italian_priority();
    test_puct();
    test_draw_detection();
    test_mcts_search_api();
    test_no_backward_capture();     /* nuovo: verifica bug 1 */
    test_king_captures_all_directions(); /* nuovo: verifica bug 2 */

    bench_movegen();
    bench_mcts();

    printf("\n══════════════════════════════════\n");
    printf("Risultato: %d/%d test superati", g_passed, g_tests);
    if (g_passed == g_tests)
        printf("  ✓  TUTTI OK\n");
    else
        printf("  ✗  %d FALLITI\n", g_tests - g_passed);
    printf("══════════════════════════════════\n\n");

    return (g_passed == g_tests) ? 0 : 1;
}
