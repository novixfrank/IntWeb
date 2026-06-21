# Dama Italiana – MCTS AI

Progetto d'esame – Magistrale Informatica  
Implementazione in C del gioco della Dama Italiana con intelligenza artificiale basata su **Monte Carlo Tree Search**.

---

## Struttura del progetto

```
dama/
├── Makefile
├── test_engine.c          # test suite standalone (senza SDL)
└── src/
    ├── board.h / board.c  # motore bitboard + regole italiane
    ├── mcts.h  / mcts.c   # MCTS: UCB1 + PUCT
    ├── tuning.h/ tuning.c # tuning genetico + round-robin + BAI
    ├── gui.h   / gui.c    # GUI SDL2: drag-and-drop, thread AI
    └── main.c             # entry point
```

---

## Compilazione

### Requisiti
- **GCC** (o Clang) con supporto C11
- **SDL2** e **SDL2_ttf** per la GUI
- **libm** (standard)

```bash
# Debian/Ubuntu
sudo apt install libsdl2-dev libsdl2-ttf-dev build-essential

# Fedora/RHEL
sudo dnf install SDL2-devel SDL2_ttf-devel gcc

# macOS (Homebrew)
brew install sdl2 sdl2_ttf
```

### Build

```bash
make               # compila tutto
make test          # compila ed esegue la test suite (senza SDL)
make run           # compila ed avvia la GUI
make run-log       # compila ed avvia la GUI e stampa su terminale il ragionamento dell'AI
make clean         # rimuove binari
```

---

## Utilizzo

```bash
./dama                   # GUI SDL2 (default)
./dama --benchmark       # benchmark sims/sec a 0.2s / 1.0s / 3.0s
./dama --analysis        # analisi sperimentale UCB1 vs PUCT
./dama --tuning          # tuning genetico (alcuni minuti)
./dama --tuning --fast   # tuning veloce (meno generazioni)
```

---

## Regole implementate (Dama Italiana)

| Regola | Implementazione |
|--------|----------------|
| Presa obbligatoria | `filter_captures()` scarta le mosse semplici se esistono catture |
| Massimo pezzi | priorità 1 in `filter_captures()` |
| Priorità dama | priorità 2: se esiste almeno una cattura con dama, scarta le pedine |
| Massimo dame catturate | priorità 3 tra le mosse di dame |
| Pedine non catturano dame | `man_cap_rec()` usa solo `opp_men`, ignora `opp_kings` |
| Dame volanti | `king_cap_rec()` + mosse semplici con scorrimento in `sq_ray` |
| Promozione interrompe sequenza | `man_cap_rec()` non ricorre dopo `promotes=true` |
| Pareggio 40 mosse | `nocap >= 80` in `board_result()` |

---

## Architettura MCTS

```
mcts_best_move()
  └─ loop anytime (batch 16 sim fino a time_limit)
       └─ mcts_rec()   [negamax ricorsivo]
            ├─ Selezione  → select_child() [UCB1 o PUCT]
            ├─ Espansione → pool_alloc() + rollout()
            └─ Backprop  → w += result; return -result
```

### UCB1
```
score = Q + C * sqrt(2 * ln(N) / n)
```

### PUCT (AlphaGo-style)
```
score = Q + c_puct * P(a) * sqrt(N) / (1 + n)
P(a) = prior euristico (catture + promozioni)
```

### Pool di memoria
- **2.2M nodi statici** (~88 MB), reset ad ogni mossa AI
- Nessuna allocazione dinamica durante la ricerca
- Fallback a rollout se pool pieno

---

## Tuning iperparametri

Il modulo `tuning.c` implementa:

1. **Algoritmo Genetico**: popolazione di individui con parametri `(C, c_puct, prior_cap, prior_promo)`, crossover uniforme, mutazione gaussiana, élite preservation
2. **Round-Robin Tournament**: ogni individuo gioca contro tutti gli altri, fitness = W − L + 0.5·D
3. **BAI** (Best Arm Identification): sfide dirette tra campione e challenger per raffinamento finale
4. **Salvataggio** in `dama_params.txt` (caricato automaticamente all'avvio della GUI)

```bash
./dama --tuning --fast   # ~5 min: 3 generazioni, 6 individui
./dama --tuning          # completo: 6 generazioni, 12 individui
```

---

## Note tecniche

- **Bitboard 32-bit**: le 32 caselle scure sono enumerate da 0 (riga 0, col 1) a 31 (riga 7, col 6). Le righe pari usano colonne dispari, le righe dispari colonne pari.
- **Look-up tables**: `sq_adj[32][4]` (adiacente) e `sq_ray[32][4][7]` (raggi per dame) pre-calcolate all'avvio.
- **Thread AI**: `SDL_CreateThread` per non bloccare il rendering; comunicazione tramite `SDL_atomic_t`.
- **Rollout**: la `Board` (16 byte) viene copiata per valore sullo stack, nessun clone heap.
