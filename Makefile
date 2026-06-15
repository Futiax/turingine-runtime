# Makefile pour la plateforme Turingine
CFLAGS += -O2 -Wall -Wextra -Icore/display -Icore/input -Icore/common -I/usr/include/libdrm
LDFLAGS += -ldrm

EVDEV_CFLAGS := $(shell pkg-config --cflags libevdev 2>/dev/null)
EVDEV_LIBS   := $(shell pkg-config --libs libevdev 2>/dev/null || echo "-levdev")

CFLAGS += $(EVDEV_CFLAGS)

OUT_DIR := .out
OBJ_DIR := $(OUT_DIR)/build
BIN_DIR := $(OUT_DIR)/bin

# ── Core Library (Turingine Engine) ──
LIB_SRC := core/display/drm_display.c core/input/evdev_input.c
LIB_OBJ := $(OBJ_DIR)/display/drm_display.o $(OBJ_DIR)/input/evdev_input.o
LIB_ARCHIVE := $(OUT_DIR)/libturingine.a

# ── Applications ──
APPS := homescreen x11_bridge terminal graph
APP_BINS := $(patsubst %,$(BIN_DIR)/%,$(APPS))

all: $(LIB_ARCHIVE) $(APP_BINS)

# Construction de la librairie statique
$(LIB_ARCHIVE): $(LIB_OBJ) | $(OUT_DIR)
	ar rcs $@ $^

$(OUT_DIR):
	mkdir -p $@

# Compilation des objets de la librairie
$(OBJ_DIR)/display/%.o: core/display/%.c | $(OBJ_DIR)/display
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/input/%.o: core/input/%.c | $(OBJ_DIR)/input
	$(CC) $(CFLAGS) -c $< -o $@

# Construction des applications
$(BIN_DIR)/homescreen: apps/homescreen/main.c $(LIB_ARCHIVE) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(EVDEV_LIBS)

$(BIN_DIR)/x11_bridge: apps/x11_bridge/main.c $(LIB_ARCHIVE) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lX11 -lXext -lXtst $(EVDEV_LIBS)

TERM_SRC := apps/terminal/main.c apps/terminal/term_render.c \
           apps/terminal/term_history.c apps/terminal/term_complete.c \
           apps/terminal/term_shell.c

$(BIN_DIR)/terminal: $(TERM_SRC) $(LIB_ARCHIVE) | $(BIN_DIR)
	$(CC) $(CFLAGS) -Iapps/terminal -o $@ $(TERM_SRC) $(LIB_ARCHIVE) $(LDFLAGS) $(EVDEV_LIBS) -lutil

MATH_RENDER_OBJ := $(OBJ_DIR)/math_render/render.o

$(OBJ_DIR)/math_render/render.o: core/math_render/render.c core/math_render/math_render.h | $(OBJ_DIR)/math_render
	$(CC) $(CFLAGS) -Icore/math_render -c $< -o $@

$(BIN_DIR)/graph: apps/Grapher/graph.cpp $(MATH_RENDER_OBJ) $(LIB_ARCHIVE) | $(BIN_DIR)
	$(CXX) $(CFLAGS) -DSIZEOF_VOID_P=8 -Icore/math_render -o $@ $^ $(LDFLAGS) $(EVDEV_LIBS) -lgiac

# Création des dossiers
$(OBJ_DIR)/display $(OBJ_DIR)/input $(OBJ_DIR)/math_render $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(OUT_DIR)

.SUFFIXES:
.PHONY: all clean
