#include "board.h"
#include <string.h>
#include <stdlib.h>

/* ══════════════════════════════════════════════
 *  Look-up tables globali
 * ══════════════════════════════════════════════ */
int sq_adj[NUM_SQ][4];
int sq_ray[NUM_SQ][4][7];
int sq_ray_len[NUM_SQ][4];

void sq_to_rc(int sq, int *row, int *col) {
    *row = sq >> 2;                          /* sq / 4           */
    int idx = sq & 3;                        /* sq % 4           */
    /* righe pari: col  1,3,5,7  → col = 2*idx+1
       righe dispari: col 0,2,4,6 → col = 2*idx   */
    *col = ((*row & 1) == 0) ? (2*idx + 1) : (2*idx);
}

int rc_to_sq(int row, int col) {
    if (row < 0 || row > 7 || col < 0 || col > 7) return -1;
    if ((row + col) % 2 == 0) return -1;   /* casa chiara */
    if ((row & 1) == 0) {
        if ((col & 1) == 0) return -1;
        return row * 4 + col / 2;
    } else {
        if ((col & 1) != 0) return -1;
        return row * 4 + col / 2;
    }
}

void board_lookup_init(void) {
    static const int DR[4] = {-1, -1, +1, +1};
    static const int DC[4] = {-1, +1, -1, +1};

    for (int sq = 0; sq < NUM_SQ; sq++) {
        int row, col;
        sq_to_rc(sq, &row, &col);
        for (int d = 0; d < 4; d++) {
            int r1 = row + DR[d], c1 = col + DC[d];
            sq_adj[sq][d] = rc_to_sq(r1, c1);

            sq_ray_len[sq][d] = 0;
            for (int k = 1; k <= 7; k++) {
                int rr = row + DR[d]*k, cc = col + DC[d]*k;
                int s = rc_to_sq(rr, cc);
                if (s < 0) break;
                sq_ray[sq][d][sq_ray_len[sq][d]++] = s;
            }
        }
    }
}

/* ══════════════════════════════════════════════
 *  board_init
 * ══════════════════════════════════════════════ */
void board_init(Board *b) {
    b->bm   = 0x00000FFF;   /* righe 0-2  : bit  0-11  */
    b->bk   = 0;
    b->wm   = 0xFFF00000;   /* righe 5-7  : bit 20-31  */
    b->wk   = 0;
    b->stm  = BLACK;
    b->nocap = 0;
}

/* ══════════════════════════════════════════════
 *  Generatore di catture – pedine
 *
 *  REGOLA DAMA ITALIANA:
 *   - Le pedine catturano SOLO in avanti (diagonale in avanti)
 *   - Le pedine NON possono catturare le dame avversarie
 * ══════════════════════════════════════════════ */
static void man_cap_rec(const Board *b, int sq, int side,
                        Move *cur, Move *out, int *cnt) {
    /* pezzi nemici che una pedina PUÒ catturare (solo pedine avversarie) */
    uint32_t opp_men = (side == BLACK) ? b->wm : b->bm;
    /* occupazione effettiva escludendo i pezzi già catturati in questa sequenza */
    uint32_t eff_occ = (b->bm | b->bk | b->wm | b->wk) & ~cur->cap_mask;
    bool extended = false;

    /* BUG 1 FIX: le pedine catturano SOLO nelle due direzioni in avanti.
     * NERO avanza verso righe crescenti → DIR_DL e DIR_DR.
     * BIANCO avanza verso righe decrescenti → DIR_UL e DIR_UR.
     * Le direzioni all'indietro sono escluse. */
    int fwd[2] = {
        (side == BLACK) ? DIR_DL : DIR_UL,
        (side == BLACK) ? DIR_DR : DIR_UR
    };

    for (int k = 0; k < 2; k++) {
        int d   = fwd[k];
        int mid = sq_adj[sq][d];
        if (mid < 0) continue;
        /* la casa intermedia deve avere una pedina avversaria non ancora catturata */
        if (!((opp_men >> mid) & 1) || ((cur->cap_mask >> mid) & 1)) continue;
        int land = sq_adj[mid][d];
        if (land < 0) continue;
        /* la casa di atterraggio deve essere libera */
        if ((eff_occ >> land) & 1) continue;

        extended = true;
        Move next  = *cur;
        next.to    = (int8_t)land;
        next.cap_mask |= (1u << mid);
        next.num_caps++;
        /* le pedine non catturano dame → num_kcaps invariato */

        bool prom = (side == BLACK) ? (land >= 28) : (land < 4);
        if (prom) {
            next.promotes = true;
            out[(*cnt)++] = next;   /* la promozione interrompe la sequenza */
        } else {
            man_cap_rec(b, land, side, &next, out, cnt);
        }
    }

    if (!extended && cur->num_caps > 0) {
        out[(*cnt)++] = *cur;
    }
}

/* ══════════════════════════════════════════════
 *  Generatore di catture – dame
 *
 *  REGOLA DAMA ITALIANA:
 *   - La dama si muove di UNA sola casella per volta (non è "volante")
 *   - La dama cattura pezzi avversari adiacenti in tutte e 4 le direzioni
 *   - La dama PUÒ catturare sia pedine che dame avversarie
 * ══════════════════════════════════════════════ */
static void king_cap_rec(const Board *b, int sq, int side,
                         uint32_t vis_mask,   /* caselle già visitate dalla dama */
                         Move *cur, Move *out, int *cnt) {
    uint32_t opp       = (side == BLACK) ? (b->wm | b->wk) : (b->bm | b->bk);
    uint32_t opp_kings = (side == BLACK) ? b->wk : b->bk;
    uint32_t eff_occ   = (b->bm | b->bk | b->wm | b->wk) & ~cur->cap_mask;
    bool extended = false;

    /* BUG 2 FIX: la dama cattura SOLO pezzi adiacenti (una casella),
     * non scorre su raggi interi. Si muove in tutte le 4 direzioni diagonali. */
    for (int d = 0; d < 4; d++) {
        int mid = sq_adj[sq][d];
        if (mid < 0) continue;
        /* il pezzo adiacente deve essere avversario e non ancora catturato */
        if (!((opp >> mid) & 1) || ((cur->cap_mask >> mid) & 1)) continue;
        int land = sq_adj[mid][d];
        if (land < 0) continue;
        /* la casella di atterraggio deve essere libera */
        if ((eff_occ >> land) & 1) continue;
        /* non si può atterrare su una casella già visitata in questa cattura */
        if ((vis_mask >> land) & 1) continue;

        extended = true;
        Move next = *cur;
        next.to       = (int8_t)land;
        next.cap_mask |= (1u << mid);
        next.num_caps++;
        next.num_kcaps += (int8_t)((opp_kings >> mid) & 1);

        uint32_t new_vis = vis_mask | (1u << land);
        king_cap_rec(b, land, side, new_vis, &next, out, cnt);
    }

    if (!extended && cur->num_caps > 0) {
        out[(*cnt)++] = *cur;
    }
}

/* ══════════════════════════════════════════════
 *  Filtro priorità italiana
 *  1. Max pezzi catturati
 *  2. A parità: dama > pedina
 *  3. Tra dame a parità: max dame catturate
 * ══════════════════════════════════════════════ */
static int filter_captures(Move *m, int n) {
    if (n == 0) return 0;

    /* 1. max totale */
    int mx = 0;
    for (int i = 0; i < n; i++) if (m[i].num_caps > mx) mx = m[i].num_caps;
    int j = 0;
    for (int i = 0; i < n; i++) if (m[i].num_caps == mx) m[j++] = m[i];
    n = j;

    /* 2. preferenza dama */
    bool has_king = false;
    for (int i = 0; i < n; i++) if (m[i].king_move) { has_king = true; break; }
    if (has_king) {
        j = 0;
        for (int i = 0; i < n; i++) if (m[i].king_move) m[j++] = m[i];
        n = j;

        /* 3. max dame catturate (solo tra mosse di dame) */
        int mk = 0;
        for (int i = 0; i < n; i++) if (m[i].num_kcaps > mk) mk = m[i].num_kcaps;
        j = 0;
        for (int i = 0; i < n; i++) if (m[i].num_kcaps == mk) m[j++] = m[i];
        n = j;
    }
    return n;
}

/* ══════════════════════════════════════════════
 *  board_gen_moves  –  punto di ingresso principale
 * ══════════════════════════════════════════════ */
int board_gen_moves(const Board *b, Move *out) {
    int side = b->stm;
    uint32_t my_men  = (side == BLACK) ? b->bm : b->wm;
    uint32_t my_kings= (side == BLACK) ? b->bk : b->wk;
    uint32_t all_occ = b->bm | b->bk | b->wm | b->wk;
    Move tmp[MAX_MOVES * 4];
    int  tcnt = 0;

    /* ── Catture pedine ── */
    for (uint32_t bits = my_men; bits; bits &= bits-1) {
        int sq = __builtin_ctz(bits);
        Move base = { .from=sq, .to=sq, .num_caps=0, .num_kcaps=0,
                      .cap_mask=0, .promotes=false, .king_move=false };
        man_cap_rec(b, sq, side, &base, tmp, &tcnt);
    }

    /* ── Catture dame ── */
    for (uint32_t bits = my_kings; bits; bits &= bits-1) {
        int sq = __builtin_ctz(bits);
        Move base = { .from=sq, .to=sq, .num_caps=0, .num_kcaps=0,
                      .cap_mask=0, .promotes=false, .king_move=true };
        king_cap_rec(b, sq, side, (1u<<sq), &base, tmp, &tcnt);
    }

    /* Se ci sono catture, applica filtro priorità e restituisci */
    if (tcnt > 0) {
        tcnt = filter_captures(tmp, tcnt);
        memcpy(out, tmp, (size_t)tcnt * sizeof(Move));
        return tcnt;
    }

    /* ── Mosse semplici (nessuna cattura disponibile) ── */
    int cnt = 0;
    uint32_t empty = ~all_occ & MASK_ALL;

    /* Pedine: solo avanti (già corretto nella versione originale) */
    int fwd[2] = { (side == BLACK) ? DIR_DL : DIR_UL,
                   (side == BLACK) ? DIR_DR : DIR_UR };
    for (uint32_t bits = my_men; bits; bits &= bits-1) {
        int sq = __builtin_ctz(bits);
        for (int k = 0; k < 2; k++) {
            int to = sq_adj[sq][fwd[k]];
            if (to < 0 || !((empty >> to) & 1)) continue;
            Move m = { .from=sq, .to=to, .num_caps=0, .num_kcaps=0,
                       .cap_mask=0, .promotes=false, .king_move=false };
            m.promotes = (side == BLACK) ? (to >= 28) : (to < 4);
            out[cnt++] = m;
        }
    }

    /* BUG 2 FIX: Dame – si muovono di UNA sola casella alla volta
     * in tutte e 4 le direzioni (avanti e indietro).
     * NON scorrono su raggi interi (comportamento precedente rimosso). */
    for (uint32_t bits = my_kings; bits; bits &= bits-1) {
        int sq = __builtin_ctz(bits);
        for (int d = 0; d < 4; d++) {
            int to = sq_adj[sq][d];
            if (to < 0 || !((empty >> to) & 1)) continue;
            Move m = { .from=sq, .to=to, .num_caps=0, .num_kcaps=0,
                       .cap_mask=0, .promotes=false, .king_move=true };
            out[cnt++] = m;
        }
    }

    return cnt;
}

/* ══════════════════════════════════════════════
 *  board_do_move
 * ══════════════════════════════════════════════ */
void board_do_move(Board *b, const Move *m) {
    uint32_t fb  = 1u << m->from;
    uint32_t tb  = 1u << m->to;
    uint32_t cap = m->cap_mask;

    if (b->stm == BLACK) {
        if (m->king_move) {
            b->bk = (b->bk & ~fb) | tb;
        } else {
            b->bm &= ~fb;
            if (m->promotes) b->bk |= tb;
            else             b->bm |= tb;
        }
        b->wm &= ~cap;
        b->wk &= ~cap;
    } else {
        if (m->king_move) {
            b->wk = (b->wk & ~fb) | tb;
        } else {
            b->wm &= ~fb;
            if (m->promotes) b->wk |= tb;
            else             b->wm |= tb;
        }
        b->bm &= ~cap;
        b->bk &= ~cap;
    }

    b->nocap = (cap != 0) ? 0 : (int16_t)(b->nocap + 1);
    b->stm  ^= 1;
}

/* ══════════════════════════════════════════════
 *  board_result
 * ══════════════════════════════════════════════ */
int board_result(const Board *b, int n_moves_avail) {
    /* Pareggio per 40 mosse senza cattura (80 semi-mosse) */
    if (b->nocap >= 80) return 0;

    /* Chi non ha pezzi perde */
    if (!(b->bm | b->bk)) return -1;   /* nero non ha pezzi → bianco vince */
    if (!(b->wm | b->wk)) return  1;   /* bianco non ha pezzi → nero vince */

    /* Chi non ha mosse perde */
    if (n_moves_avail == 0) {
        return (b->stm == BLACK) ? -1 : 1;
    }

    return 0;   /* partita in corso */
}

/* ══════════════════════════════════════════════
 *  Stampa di debug
 * ══════════════════════════════════════════════ */
void board_print(const Board *b) {
    printf("  +-----------------+\n");
    for (int row = 0; row < 8; row++) {
        printf("%d |", row);
        for (int col = 0; col < 8; col++) {
            int sq = rc_to_sq(row, col);
            char ch = '.';
            if (sq >= 0) {
                if ((b->bm >> sq) & 1) ch = 'b';
                else if ((b->bk >> sq) & 1) ch = 'B';
                else if ((b->wm >> sq) & 1) ch = 'w';
                else if ((b->wk >> sq) & 1) ch = 'W';
                else ch = '+';
            }
            printf(" %c", ch);
        }
        printf(" |\n");
    }
    printf("  +-----------------+\n");
    printf("  Muove: %s  NoCapMoves: %d\n",
           b->stm == BLACK ? "NERO" : "BIANCO", b->nocap);
}
