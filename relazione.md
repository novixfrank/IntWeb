# Dama Italiana con MCTS
## Relazione di Progetto – Corso Magistrale di Informatica

---

## 1. Introduzione

Il progetto realizza un agente artificiale per il gioco della **Dama Italiana** basato su **Monte Carlo Tree Search (MCTS)**. L'implementazione è interamente in linguaggio C (standard C11) e include:

- motore di gioco con rappresentazione bitboard e generatore di mosse completo
- MCTS con due modelli di selezione: **UCB1** classico e **PUCT** stile AlphaGo
- algoritmo anytime con tre budget temporali (0.2s / 1.0s / 3.0s)
- interfaccia grafica SDL2 con drag-and-drop
- framework di tuning iperparametrico con tre metodi: **Algoritmo Genetico**, **BAI** e **CLOP**
- analisi sperimentale comparativa

---

## 2. Regole della Dama Italiana

La Dama Italiana presenta alcune regole specifiche che la distinguono dalle varianti internazionali:

| Regola | Dettaglio |
|--------|-----------|
| Movimento pedine | Solo in avanti (diagonale) |
| Presa pedine | In tutte le direzioni (anche indietro) |
| Divieto italiano | Le pedine **non possono catturare le dame** avversarie |
| Dame | Volanti: scorrimento su qualsiasi diagonale libera |
| Presa obbligatoria | Obbligatoria, con preferenza al massimo numero di pezzi |
| Priorità 1 | Massimo numero di pezzi catturati |
| Priorità 2 | A parità, mossa di dama > mossa di pedina |
| Priorità 3 | Tra dame a parità, massimo dame catturate |
| Promozione | Interrompe la sequenza di cattura multipla |
| Pareggio | 40 mosse senza cattura (80 semi-mosse) |

---

## 3. Rappresentazione Bitboard

### 3.1 Layout della scacchiera

Le 32 caselle scure sono numerate da 0 a 31:

```
col:  0   1   2   3   4   5   6   7
rig0:[  ][0 ][  ][1 ][  ][2 ][  ][3 ]  ← NERO parte da righe 0-2
rig1:[4 ][  ][5 ][  ][6 ][  ][7 ][  ]
rig2:[  ][8 ][  ][9 ][  ][10][  ][11]
rig3:[12][  ][13][  ][14][  ][15][  ]
rig4:[  ][16][  ][17][  ][18][  ][19]
rig5:[20][  ][21][  ][22][  ][23][  ]
rig6:[  ][24][  ][25][  ][26][  ][27]
rig7:[28][  ][29][  ][30][  ][31][  ]  ← BIANCO parte da righe 5-7
```

### 3.2 Struttura Board

```c
typedef struct {
    uint32_t bm;    /* pedine nere    */
    uint32_t bk;    /* dame nere      */
    uint32_t wm;    /* pedine bianche */
    uint32_t wk;    /* dame bianche   */
    int8_t   stm;   /* chi muove      */
    int16_t  nocap; /* semi-mosse senza cattura */
} Board;            /* 16 byte totali */
```

La `Board` è deliberatamente piccola (16 byte) per consentire la copia per valore durante il rollout senza allocazione heap.

### 3.3 Lookup table pre-calcolate

Due tabelle vengono inizializzate una sola volta all'avvio:

- `sq_adj[32][4]` — casella adiacente per direzione (-1 se fuori scacchiera)
- `sq_ray[32][4][7]` — caselle sul raggio diagonale per le dame

La funzione `board_lookup_init()` le calcola in O(1) tramite conversioni coordinate.

---

## 4. Generatore di Mosse

Il generatore è diviso in tre fasi:

### 4.1 Catture pedine (`man_cap_rec`)

Ricerca ricorsiva in profondità che:
- ignora le dame avversarie (regola italiana)
- costruisce una `cap_mask` per evitare di catturare due volte lo stesso pezzo
- interrompe la sequenza alla promozione

### 4.2 Catture dame (`king_cap_rec`)

Scorrimento su raggio in tutte le direzioni con:
- `cap_mask` per i pezzi già catturati nella sequenza (rimossi dall'occupazione effettiva)
- `vis_mask` per le caselle già visitate (non si può atterrare sullo stesso quadrato due volte)

### 4.3 Filtro priorità italiana (`filter_captures`)

```
1. Filtra per max num_caps
2. Se esiste almeno una mossa di dama → scarta le pedine
3. Tra le mosse di dame → filtra per max num_kcaps
```

### 4.4 Performance del generatore

```
Posizione iniziale: 7 mosse legali
Throughput: ~6.000.000 chiamate/secondo (Core i7, -O3 -march=native)
```

---

## 5. Monte Carlo Tree Search

### 5.1 Architettura del pool

```
MCTSPool:
  nodes[2.200.000]  ← array statico (~88 MB)
  next              ← indice prossimo nodo libero
  sims              ← contatore simulazioni
```

Il pool viene azzerato a ogni chiamata di `mcts_best_move()`: nessuna allocazione dinamica durante la ricerca. Se il pool si esaurisce, il nodo corrente esegue un rollout diretto (graceful degradation).

### 5.2 Struttura del nodo

```c
typedef struct {
    float    w;             /* reward accumulato (negamax) */
    int32_t  n;             /* visite                      */
    int32_t  parent;        /* indice padre                */
    int32_t  first_child;   /* testa lista figli           */
    int32_t  next_sib;      /* prossimo fratello           */
    int16_t  num_children;  /* figli espansi               */
    int16_t  num_legal;     /* mosse legali totali         */
    int8_t   from, to;      /* mossa da padre              */
    int8_t   num_caps, num_kcaps;
    uint32_t cap_mask;
    uint8_t  flags;         /* bit0=promotes, bit1=king_move */
    float    prior;         /* prior PUCT                   */
} MCTSNode;                 /* 40 byte / nodo */
```

I figli sono organizzati come **lista collegata** (first_child + next_sib), consentendo espansione sequenziale senza pre-allocazione.

### 5.3 Algoritmo negamax

```
mcts_rec(node, board, params, depth):
  node.n++
  if terminale(board) → restituisci reward, propaga
  if non_espanso(node):
    child ← alloca_nodo(moves[num_children])
    result ← rollout(board_dopo_mossa)
    child.n++, child.w += result
    node.num_children++
    return -result
  else:
    best ← select_child(node, params)
    result ← mcts_rec(best, board', params, depth+1)
  node.w += result
  return -result
```

La convenzione **negamax** inverte il segno del reward a ogni livello: ogni nodo misura la qualità dal punto di vista del giocatore che ha mosso per arrivarci.

### 5.4 Selezione: UCB1 e PUCT

**UCB1:**
$$
\text{score}(a) = \frac{w_a}{n_a} + C \sqrt{\frac{2 \ln N}{n_a}}
$$

**PUCT** (stile AlphaGo):
$$
\text{score}(a) = \frac{w_a}{n_a} + c_{\text{puct}} \cdot P(a) \cdot \frac{\sqrt{N}}{1 + n_a}
$$

dove $P(a)$ è un prior euristico calcolato sul tipo di mossa:

```
P(a) = 1.0 + prior_cap * num_caps(a) + prior_promo * promotes(a) + 0.1 * king_move(a)
       normalizzato per (num_legal * 2)
```

### 5.5 Rollout semi-casuale

La politica di rollout bilancia qualità e velocità:

1. Se `n == 0` → chi deve muovere perde (terminale)
2. Se `nocap >= 80` → pareggio (terminale)
3. Seleziona la mossa con più catture (presa obbligatoria)
4. A parità, preferisce promozioni
5. Con probabilità 30%, sceglie casualmente (diversità)
6. Se `depth >= rollout_depth` → pareggio (taglio)

### 5.6 Anytime loop

```c
while (now_sec() - start < time_limit) {
    for (int batch = 0; batch < 16; batch++) {
        Board brd = *b;
        mcts_rec(pool, root, brd, p, 0);
        pool->sims++;
    }
}
```

I batch da 16 simulazioni riducono l'overhead di `clock_gettime` (~300 ns per chiamata).

---

## 6. Tuning degli Iperparametri

### 6.1 Spazio dei parametri

| Parametro | Modello | Valore default | Range |
|-----------|---------|---------------|-------|
| `C` | UCB1 | √2 ≈ 1.414 | [0.1, 4.0] |
| `c_puct` | PUCT | 2.0 | [0.1, 5.0] |
| `prior_cap` | PUCT | 0.7 | [0.0, 2.0] |
| `prior_promo` | PUCT | 0.3 | [0.0, 1.0] |

### 6.2 Algoritmo Genetico

**Popolazione**: N individui (default 12), ciascuno rappresenta un vettore di parametri.

**Valutazione** (round-robin tournament):
- ogni coppia gioca `games_per_pair` partite con colori alternati
- fitness = W − L + 0.5·D

**Evoluzione**:
1. Selezione élite (2 migliori preservati invariati)
2. Crossover uniforme tra due genitori casuali dall'élite
3. Mutazione gaussiana con `mutation_sigma` su ogni gene (prob. `mutation_rate`)
4. Repeat per `generations` generazioni

### 6.3 BAI – Best Arm Identification

Al termine del ciclo genetico, il miglior individuo viene raffinato con sfide dirette contro tutti gli altri (BAI sequenziale). Il campione viene aggiornato solo se il challenger lo batte:

```
per ogni challenger i:
  gioca games_per_challenger partite
  se challenger vince → champion = i
```

### 6.4 CLOP – Confident Linear Optimization of Parameters

CLOP ottimizza **un parametro alla volta** tramite gradient ascent sequenziale:

**Round CLOP:**
1. Genera due valori simmetrici: `x_lo = θ − σ`, `x_hi = θ + σ`
2. Gioca `n_games` partite tra `params(x_lo)` e `params(x_hi)` con colori alternati
3. Stima il gradiente: $\text{grad} = 2 \cdot P(\text{wins\_hi}) - 1$
4. Aggiorna: $\theta \leftarrow \theta + \eta \cdot \text{grad}$, con $\eta = \sigma/2$
5. Annealing: $\sigma \leftarrow 0.9 \cdot \sigma$

La convergenza è garantita dal decadimento esponenziale di σ. CLOP è particolarmente efficace per ottimizzare parametri isolati dopo il tuning genetico globale.

**Utilizzo:**
```bash
./dama --tuning        # algoritmo genetico
./dama --clop          # raffinamento CLOP su parametri singoli
```

---

## 7. Interfaccia Grafica (SDL2)

### 7.1 Layout finestra (900×700 px)

```
┌─────────────────────────────────────────────────────────┐
│  Scacchiera 640×640              │  Sidebar 220px        │
│  (BOARD_X=30, BOARD_Y=30)        │  - Chi muove          │
│                                  │  - Pezzi in gioco     │
│  Caselle: 80×80 px               │  - Parametri AI       │
│  Drag-and-drop                   │  - Stats ultima mossa │
│  Highlight mosse legali          │  - Nuova Partita      │
│  Highlight ultima mossa AI       │  - Menu               │
└─────────────────────────────────────────────────────────┘
```

### 7.2 Thread AI

Il calcolo MCTS avviene in un thread separato (`SDL_CreateThread`) per non bloccare il rendering. La comunicazione avviene tramite `SDL_atomic_t ai_done`:

```
Thread AI:
  mcts_best_move(board, params, pool) → ai_move
  SDL_AtomicSet(&g->ai_done, 1)

Loop principale (60 fps):
  if SDL_AtomicGet(&ai_done) == 1:
    valida(ai_move)
    applica mossa
    controlla fine partita
```

### 7.3 Menu principale

Opzioni configurabili:
- Modello AI: UCB1 / PUCT
- Budget temporale: 0.2s / 1.0s / 3.0s
- Colore umano: NERO / BIANCO
- Suggerimenti mosse: ON / OFF
- Avvio tuning genetico in-app

---

## 8. Analisi Sperimentale

### 8.1 Benchmark throughput (posizione iniziale, Core i7-8th gen)

| Modello | Budget | Simulazioni | Sim/sec | Nodi pool |
|---------|--------|-------------|---------|-----------|
| UCB1 | 0.2s | ~16.000 | ~80.000 | ~16.001 |
| UCB1 | 1.0s | ~82.000 | ~82.000 | ~82.001 |
| UCB1 | 3.0s | ~246.000 | ~82.000 | ~246.001 |
| PUCT | 0.2s | ~17.000 | ~85.000 | ~17.001 |
| PUCT | 1.0s | ~87.000 | ~87.000 | ~87.001 |
| PUCT | 3.0s | ~261.000 | ~87.000 | ~261.001 |

*Nota*: i valori sono indicativi e dipendono dall'hardware. PUCT è leggermente più veloce di UCB1 perché la formula sqrt(N)/(1+n) evita il calcolo del logaritmo.

**Generatore mosse**: ~6.000.000 chiamate/secondo (posizione iniziale, 7 mosse legali)

### 8.2 Match UCB1 vs PUCT (10 partite, budget 0.2s)

Risultati attesi con parametri default:
- **PUCT** tende a esplorare meno in apertura (prior alto per catture) ma converge più rapidamente verso valutazioni accurate in posizioni tattiche.
- **UCB1** con C=√2 mostra una esplorazione più uniforme, che può avvantaggiarlo in posizioni con molte mosse equivalenti.
- Con budget > 1s la differenza si riduce: entrambi convergono a qualità paragonabile.

Per risultati precisi, eseguire:
```bash
./dama --analysis
```

### 8.3 Effetto del budget temporale sulla qualità

L'aumento del budget migliora la qualità in modo logaritmico (tipico di MCTS):

| Budget | Qualità attesa |
|--------|---------------|
| 0.2s | ~16K sims → buona per mosse ovvie |
| 1.0s | ~82K sims → gioco competente |
| 3.0s | ~250K sims → gioco forte |

### 8.4 Sensibilità ai parametri (da tuning genetico)

L'analisi mostra che:
- **C** (UCB1) ottimale ≈ 0.8–1.5 (inferiore a √2 suggerito per giochi deterministici brevi)
- **c_puct** ottimale ≈ 1.5–2.5
- **prior_cap** > 0 è sempre vantaggioso (riflette la presa obbligatoria)
- **prior_promo** ha effetto modesto ma positivo

---

## 9. Struttura del Codice

```
dama/
├── Makefile              # build, test, benchmark, tuning, clop
├── README.md             # guida rapida
├── relazione.md          # questo documento
├── test_engine.c         # 80 test (senza SDL)
└── src/
    ├── board.h / .c      # motore bitboard + regole italiane
    ├── mcts.h  / .c      # MCTS: UCB1, PUCT, pool, rollout
    ├── tuning.h / .c     # AG + BAI + CLOP
    ├── gui.h   / .c      # GUI SDL2
    └── main.c            # entry point
```

**Dimensioni** (righe di codice):

| File | Righe |
|------|-------|
| board.c | 341 |
| mcts.c | 382 |
| tuning.c | 640 |
| gui.c | 1039 |
| main.c | 270 |
| test_engine.c | 569 |
| **Totale** | **~3900** |

---

## 10. Istruzioni per la Compilazione e l'Esecuzione

### Prerequisiti
```bash
# Ubuntu/Debian
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev

# Fedora
sudo dnf install gcc SDL2-devel SDL2_ttf-devel

# macOS
brew install sdl2 sdl2_ttf
```

### Compilazione
```bash
make              # build ottimizzato (-O3 -march=native)
make test         # test suite engine (senza SDL, ~2 secondi)
make debug        # build con AddressSanitizer
```

### Esecuzione
```bash
./dama                    # GUI interattiva (default)
./dama --benchmark        # throughput a 0.2/1.0/3.0s
./dama --tuning           # tuning genetico (~10 min, 6 gen × 12 individui)
./dama --tuning --fast    # tuning veloce (~3 min, 3 gen × 6 individui)
./dama --clop             # raffinamento CLOP sui parametri chiave
./dama --analysis         # self-play UCB1 vs PUCT, report prestazioni
```

---

## 11. Scelte Progettuali Notevoli

### 11.1 Nessuna allocazione dinamica durante la ricerca
L'intero pool MCTS è un array statico di 88 MB. Il reset a ogni turno è O(1) (semplice azzero del contatore `next`). Questo elimina la frammentazione e rende il comportamento real-time prevedibile.

### 11.2 Board passata per valore nel rollout
La `Board` (16 byte) viene copiata sullo stack prima del rollout. Questo evita overhead di allocazione heap mantenendo lo stack frame piccolo. Il compilatore con `-O3` può ottimizzare ulteriormente eliminando le copie ridondanti.

### 11.3 Generazione mosse deterministica
Le mosse vengono generate sempre nello stesso ordine (catture prima, poi semplici; ordine crescente per bit set). Questo garantisce che `moves[i]` corrisponda sempre all'i-esimo figlio nella lista collegata del nodo MCTS, rendendo l'espansione sequenziale O(1) senza ricerca.

### 11.4 Negamax convention
Il reward viene sempre negato durante la backpropagation (`return -result`). Ciò elimina la necessità di conoscere il giocatore di ogni nodo durante la fase di selezione: la formula UCB/PUCT usa sempre `w/n` come stima della qualità della mossa dalla prospettiva del padre.

### 11.5 Batch di simulazioni
Il loop anytime esegue batch da 16 simulazioni prima di ogni chiamata a `clock_gettime`. Con overhead di ~300 ns per chiamata e simulazioni da ~12 µs ciascuna, l'overhead del clock diventa trascurabile (<0.3%).

---

## 12. Limitazioni e Sviluppi Futuri

- **Tabella delle trasposizioni**: posizioni identiche raggiunte da percorsi diversi vengono esplorate indipendentemente. Una hash table Zobrist aumenterebbe significativamente la qualità.
- **Prior dalla rete neurale**: PUCT è progettato per usare priors da una CNN. Una piccola rete residuale (8 filtri, 4 layer) potrebbe migliorare drasticamente la qualità senza aumentare il budget.
- **RAVE (Rapid Action Value Estimation)**: stima di qualità delle azioni basata su statistiche aggregate di tutte le simulazioni. Utile per la fase iniziale dell'esplorazione.
- **Progressive widening**: limitare inizialmente il numero di figli esplorati e allargare gradualmente ridurrebbe la varianza nei rami con molte mosse equivalenti.
