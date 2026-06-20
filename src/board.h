#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/*
 * DAMA ITALIANA – Bitboard engine
 *
 * Numerazione delle 32 case scure (0-31):
 *      col: 0   1   2   3   4   5   6   7
 * riga 0: [  ][0 ][  ][1 ][  ][2 ][  ][3 ]  <- NERO parte da qui (righe 0-2)
 * riga 1: [4 ][  ][5 ][  ][6 ][  ][7 ][  ]
 * riga 2: [  ][8 ][  ][9 ][  ][10][  ][11]
 * riga 3: [12][  ][13][  ][14][  ][15][  ]
 * riga 4: [  ][16][  ][17][  ][18][  ][19]
 * riga 5: [20][  ][21][  ][22][  ][23][  ]
 * riga 6: [  ][24][  ][25][  ][26][  ][27]
 * riga 7: [28][  ][29][  ][30][  ][31][  ]  <- BIANCO parte da qui (righe 5-7)
 *
 * NERO si muove VERSO il basso (indici crescenti)
 * BIANCO si muove VERSO l'alto (indici decrescenti)
 *
Regole Dama Italiana:
 *  - Le pedine si muovono solo in avanti
 *  - Le pedine prendono in tutte le direzioni MA NON possono prendere le dame
 *  - Le dame possono muoversi avanti e indietro e tutte le duirezioni e mangiare in tutte le direzioni
 *  - Le catture sono obbligatorie
 *  - Se ci sono più catture possibili, si sceglie quella che cattura il maggior numero di pezzi (pedine + dame)
 *  - Presa obbligatoria; massimo numero di pezzi; priorità della dama
 *  - A parità di pezzi tra dame: si prende con la dama; poi max dame catturate
 */

#define BLACK  0
#define WHITE  1
#define DRAW   2

#define NUM_SQ    32
#define MAX_MOVES 80   /* limite superiore generoso */
#define MAX_CAPS  16

/* ──── Maschere bitboard ──── */
#define MASK_ALL        0xFFFFFFFFu
/* Righe pari (0,2,4,6): case 0-3, 8-11, 16-19, 24-27 */
#define MASK_EVEN_ROW   0x0F0F0F0Fu
/* Righe dispari (1,3,5,7): case 4-7, 12-15, 20-23, 28-31 */
#define MASK_ODD_ROW    0xF0F0F0F0u
/* Bordo destro nelle righe pari (col 7): case 3,11,19,27 */
#define MASK_RLEDGE     0x08080808u
/* Bordo sinistro nelle righe dispari (col 0): case 4,12,20,28 */
#define MASK_LLEDGE     0x10101010u
/* Righe di promozione */
#define MASK_BLACK_PROM 0xF0000000u  /* riga 7, case 28-31 */
#define MASK_WHITE_PROM 0x0000000Fu  /* riga 0, case 0-3   */

/* ──── Tipi ──── */
typedef struct {
    int8_t   from;          /* casella di partenza (-1 se non definita) */
    int8_t   to;            /* casella di arrivo */
    int8_t   num_caps;      /* numero pezzi catturati */
    int8_t   num_kcaps;     /* dame catturate (priorità italiana) */
    uint32_t cap_mask;      /* bitmask pezzi catturati */
    bool     promotes;      /* promozione a dama */
    bool     king_move;     /* il pezzo che si muove era già dama */
} Move;

typedef struct {
    uint32_t bm;    /* pedine nere  */
    uint32_t bk;    /* dame nere    */
    uint32_t wm;    /* pedine bianche */
    uint32_t wk;    /* dame bianche */
    int8_t   stm;   /* chi muove: BLACK o WHITE */
    int16_t  nocap; /* semi-mosse senza cattura (pareggio a 80) */
} Board;

/* ──── Funzioni ──── */

/* Inizializza a posizione iniziale */
void board_init(Board *b);

/* Genera tutte le mosse legali; restituisce il conteggio */
int  board_gen_moves(const Board *b, Move *out);

/* Applica una mossa in-place */
void board_do_move(Board *b, const Move *m);

/* Valuta la partita:
 *   1  = Nero vince,  -1 = Bianco vince,  0 = in corso / pareggio */
int  board_result(const Board *b, int n_moves_avail);

/* Coordinate <-> casella */
void sq_to_rc(int sq, int *row, int *col);
int  rc_to_sq(int row, int col);   /* -1 se casella chiara o fuori tavola */

/* Stampa a terminale (debug) */
void board_print(const Board *b);

/* Inizializza le look-up table (da chiamare una volta all'avvio) */
void board_lookup_init(void);

/* Tabelle esportate per l'uso negli altri moduli */
extern int sq_adj[NUM_SQ][4];          /* adiacente in dir d, -1 = fuori */

/* Indici direzioni */
#define DIR_UL 0   /* su-sinistra  */
#define DIR_UR 1   /* su-destra    */
#define DIR_DL 2   /* giù-sinistra */
#define DIR_DR 3   /* giù-destra   */
