#pragma once
#include "mcts.h"
#include <stdint.h>

/*
 * ══════════════════════════════════════════════════════════════════
 *  DAMA ITALIANA – Hyperparameter Tuning
 *
 *  Implementa:
 *   1. Round-Robin Tournament tra istanze di MCTSParams
 *   2. Algoritmo Genetico (selezione + crossover + mutazione)
 *   3. BAI (Best Arm Identification) per raffinamento finale
 *   4. CLOP (Confident Linear Optimization of Parameters)
 * ══════════════════════════════════════════════════════════════════
 */

/* ── Individuo dell'algoritmo genetico ── */
typedef struct {
    float  C;           /* UCB1: costante esplorazione   [0.1 – 4.0] */
    float  c_puct;      /* PUCT: costante               [0.5 – 5.0] */
    float  prior_cap;   /* peso prior cattura           [0.0 – 2.0] */
    float  prior_promo; /* peso prior promozione        [0.0 – 1.0] */
    float  fitness;     /* punteggio round-robin (wins - losses)    */
    int    wins;
    int    losses;
    int    draws;
} Individual;

/* ── Parametri del tuning ── */
typedef struct {
    int    pop_size;          /* dimensione popolazione (default: 12)      */
    int    generations;       /* numero generazioni (default: 6)           */
    int    games_per_pair;    /* partite per accoppiamento (default: 4)    */
    double time_limit;        /* budget per mossa durante tuning (0.2s)   */
    float  mutation_rate;     /* probabilità mutazione (default: 0.20)     */
    float  mutation_sigma;    /* std mutazione gaussiana (default: 0.15)   */
    int    elite_count;       /* élite da preservare (default: 2)          */
    int    verbose;           /* 0 = silenzioso, 1 = progress              */
} TuningParams;

/* Valori predefiniti */
static inline TuningParams tuning_default_params(void) {
    TuningParams p;
    p.pop_size        = 12;
    p.generations     = 6;
    p.games_per_pair  = 4;
    p.time_limit      = 0.2;
    p.mutation_rate   = 0.20f;
    p.mutation_sigma  = 0.15f;
    p.elite_count     = 2;
    p.verbose         = 1;
    return p;
}

/* ── API ── */

/* Inizializza popolazione casuale */
void tuning_init_population(Individual *pop, int n, unsigned int seed);

/* Valuta tutti gli individui via round-robin tournament.
   Usa due pool MCTS passati (uno per giocatore bianco, uno per nero).
   Il parametro `model` indica con quale modello di selezione (UCB1 o
   PUCT) vengono effettivamente giocate le partite di valutazione: è
   fondamentale passare SEL_PUCT quando si valuta una popolazione di
   individui pensata per PUCT, altrimenti c_puct verrebbe "misurato"
   in partite giocate con la logica UCB1, rendendo il tuning inutile. */
void tuning_round_robin(Individual *pop, int n, const TuningParams *tp,
                        SelectModel model,
                        MCTSPool *pool_a, MCTSPool *pool_b);

/* Una generazione genetica: selezione + crossover + mutazione */
void tuning_evolve(Individual *pop, int n, const TuningParams *tp,
                   unsigned int *seed);

/* BAI: Best Arm Identification con budget fisso.
   Anche qui `model` determina con quale algoritmo di selezione vengono
   giocate le partite di confronto (deve coincidere con il modello della
   popolazione passata in pop, esattamente come per tuning_round_robin).
   Restituisce l'indice del migliore individuo. */
int  tuning_bai(Individual *pop, int n, int budget,
                const TuningParams *tp, SelectModel model,
                MCTSPool *pool_a, MCTSPool *pool_b);

/* Converte un Individual in MCTSParams */
MCTSParams individual_to_params(const Individual *ind, SelectModel model,
                                double time_limit);

/* Lancia il ciclo genetico completo e restituisce i migliori parametri
   per UCB1 e PUCT.  Può essere usato direttamente dal main. */
void tuning_run(MCTSParams *best_ucb1, MCTSParams *best_puct,
                const TuningParams *tp);

/* Carica/Salva i migliori parametri su file (formato semplice testo) */
int  tuning_save(const char *path, const MCTSParams *ucb1, const MCTSParams *puct);
int  tuning_load(const char *path,       MCTSParams *ucb1,       MCTSParams *puct);

/* Stampa tabella riassuntiva della popolazione */
void tuning_print_population(const Individual *pop, int n);

/* Stampa analisi sperimentale finale (per la relazione) */
void tuning_print_report(const MCTSParams *ucb1, const MCTSParams *puct,
                         MCTSPool *pool);

/* ══════════════════════════════════════════════════════════════════
 *  CLOP – Confident Linear Optimization of Parameters
 *
 *  Adatta CLOP (Rémi Coulom, 2011) al contesto della Dama Italiana.
 *  Ottimizza un singolo parametro scalare `theta` tramite regressione
 *  logistica sequenziale su partite a coppie (param+ vs param-).
 *
 *  Ogni round:
 *    1. Sceglie due valori del parametro simmetrici attorno all'ottimo
 *       corrente (con passo adattivo `sigma`)
 *    2. Gioca n_games partite tra le due configurazioni
 *    3. Aggiorna la stima del massimo con gradient ascent
 * ══════════════════════════════════════════════════════════════════ */

/* Parametro ottimizzabile */
typedef enum {
    CLOP_PARAM_C           = 0,  /* UCB1: costante esplorazione */
    CLOP_PARAM_C_PUCT      = 1,  /* PUCT: c_puct               */
    CLOP_PARAM_PRIOR_CAP   = 2,  /* prior cattura              */
    CLOP_PARAM_PRIOR_PROMO = 3   /* prior promozione           */
} CLOPParam;

/* Stato CLOP per un singolo parametro */
typedef struct {
    CLOPParam  param;        /* quale parametro si sta ottimizzando */
    float      theta;        /* stima corrente del valore ottimale  */
    float      sigma;        /* passo di esplorazione               */
    float      lo, hi;       /* limiti del parametro                */
    int        n_rounds;     /* numero di round effettuati          */
    int        n_games;      /* partite per round                   */
    double     time_limit;   /* budget per mossa durante i test     */
    int        verbose;
} CLOPState;

/* Inizializza uno stato CLOP per il parametro `param`
   con valore iniziale `init`, passo `sigma`, limiti [lo, hi] */
void clop_init(CLOPState *st, CLOPParam param,
               float init, float sigma, float lo, float hi,
               int n_games, double time_limit);

/* Esegue un round CLOP:
 *   - gioca n_games partite tra theta-sigma e theta+sigma
 *   - aggiorna theta con gradient step
 *   - riduce sigma (annealing)
 *   Ritorna il nuovo theta. */
float clop_run_round(CLOPState *st,
                     const MCTSParams *base,
                     MCTSPool *pool_a, MCTSPool *pool_b);

/* Esegue max_rounds round e restituisce il parametro ottimale */
float clop_optimize(CLOPState *st, int max_rounds,
                    const MCTSParams *base,
                    MCTSPool *pool_a, MCTSPool *pool_b);

/* Applica il risultato CLOP a un MCTSParams */
void clop_apply(const CLOPState *st, MCTSParams *p);

/* Stampa storia convergenza */
void clop_print(const CLOPState *st);
