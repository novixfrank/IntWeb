# ══════════════════════════════════════════════════════
#  Dama Italiana – MCTS AI
#  Makefile
# ══════════════════════════════════════════════════════

CC      = gcc
TARGET  = dama

SRCDIR  = src
SRCS    = $(SRCDIR)/main.c \
          $(SRCDIR)/board.c \
          $(SRCDIR)/mcts.c \
          $(SRCDIR)/tuning.c \
          $(SRCDIR)/gui.c

OBJS    = $(SRCS:.c=.o)

# ── Flag di compilazione ──────────────────────────────
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
          -O3 -march=native -ffast-math \
          -D_POSIX_C_SOURCE=200809L \
          -I$(SRCDIR)

# SDL2 + SDL2_ttf via pkg-config
SDL_CFLAGS  := $(shell pkg-config --cflags sdl2 SDL2_ttf 2>/dev/null)
SDL_LIBS    := $(shell pkg-config --libs   sdl2 SDL2_ttf 2>/dev/null)

# Fallback manuale se pkg-config non disponibile
ifeq ($(SDL_CFLAGS),)
  SDL_CFLAGS = -I/usr/include/SDL2 -D_REENTRANT
  SDL_LIBS   = -lSDL2 -lSDL2_ttf
endif

CFLAGS  += $(SDL_CFLAGS)
LDFLAGS  = $(SDL_LIBS) -lm

# ── Build debug ───────────────────────────────────────
CFLAGS_DEBUG = -std=c11 -Wall -Wextra -g -O0 -fsanitize=address \
               -D_POSIX_C_SOURCE=200809L \
               $(SDL_CFLAGS) -I$(SRCDIR)

# ── Regole principali ─────────────────────────────────
.PHONY: all clean debug run benchmark analysis tuning test help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo ""
	@echo "  Build completata: ./$(TARGET)"
	@echo "  Avvio GUI:        ./$(TARGET)"
	@echo "  Benchmark:        ./$(TARGET) --benchmark"
	@echo "  Tuning:           ./$(TARGET) --tuning [--fast]"
	@echo "  Analisi:          ./$(TARGET) --analysis"
	@echo ""

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# ── Debug build ───────────────────────────────────────
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS += -fsanitize=address
debug: clean $(TARGET)
	@echo "Debug build completata."

# ── Esecuzione rapida ─────────────────────────────────
run: all
	./$(TARGET)

benchmark: all
	./$(TARGET) --benchmark

analysis: all
	./$(TARGET) --analysis

tuning: all
	./$(TARGET) --tuning

tuning-fast: all
	./$(TARGET) --tuning --fast

# ── Test suite (senza SDL) ────────────────────────────
test: test_engine
	./test_engine

test_engine: test_engine.c $(SRCDIR)/board.c $(SRCDIR)/mcts.c $(SRCDIR)/board.h $(SRCDIR)/mcts.h
	$(CC) -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L \
	      -I$(SRCDIR) test_engine.c $(SRCDIR)/board.c $(SRCDIR)/mcts.c \
	      -lm -o $@

# ── Pulizia ───────────────────────────────────────────
clean:
	rm -f $(OBJS) $(TARGET) test_engine

# ── Dipendenze header ─────────────────────────────────
$(SRCDIR)/board.o:  $(SRCDIR)/board.h
$(SRCDIR)/mcts.o:   $(SRCDIR)/mcts.h   $(SRCDIR)/board.h
$(SRCDIR)/tuning.o: $(SRCDIR)/tuning.h $(SRCDIR)/mcts.h  $(SRCDIR)/board.h
$(SRCDIR)/gui.o:    $(SRCDIR)/gui.h    $(SRCDIR)/mcts.h  $(SRCDIR)/board.h $(SRCDIR)/tuning.h
$(SRCDIR)/main.o:   $(SRCDIR)/board.h  $(SRCDIR)/mcts.h  $(SRCDIR)/tuning.h $(SRCDIR)/gui.h

# ── Help ─────────────────────────────────────────────
help:
	@echo "Target disponibili:"
	@echo "  test         Compila ed esegue la test suite (senza SDL)"
	@echo "  all          Compila il progetto (default)"
	@echo "  debug        Compila con AddressSanitizer e debug info"
	@echo "  run          Compila ed esegue la GUI"
	@echo "  benchmark    Compila e avvia il benchmark"
	@echo "  analysis     Compila e avvia l'analisi sperimentale"
	@echo "  tuning       Compila e avvia il tuning genetico"
	@echo "  tuning-fast  Tuning veloce (meno generazioni)"
	@echo "  clean        Rimuove file oggetto e binario"
