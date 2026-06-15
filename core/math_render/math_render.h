#pragma once
#include <stdint.h>

// ── Types publics ─────────────────────────────────────────────────────────────

typedef struct { char bytes[4]; } Cell;

typedef struct {
  Cell *cells;
  int   width;
  int   height;
  int   baseline;
} MathBox;

typedef enum {
  MATH_ATOM,
  MATH_NEG,
  MATH_ADD,
  MATH_MUL,
  MATH_FRAC,
  MATH_POW,
  MATH_INTEGRAL,  // integrate(expr, var, a, b) ou integrate(expr, var)
  MATH_DIFF,      // diff(expr, var)  →  d(expr)/d(var)
  MATH_SQRT,
  MATH_FUNC,
} MathNodeType;

typedef struct MathExpr MathExpr;
struct MathExpr {
  MathNodeType  type;
  char         *name;
  MathExpr    **children;
  int           n_children;
};

// ── API publique ──────────────────────────────────────────────────────────────

// Parse une expression texte → AST. Retourne NULL si erreur.
MathExpr *math_parse(const char *input);

// Rend un AST en grille 2D. L'appelant libère avec mathbox_free.
MathBox  *math_render(const MathExpr *expr);

void math_expr_free(MathExpr *expr);
void mathbox_free(MathBox *box);

// Affiche le MathBox dans le terminal (UTF-8).
void mathbox_print(const MathBox *box);

// Rendu pixels via callback. scale=1 → 8×16 px/cellule, scale=2 → 16×32.
// set_pixel(x, y, on, userdata) est appelé pour chaque pixel.
typedef void (*SetPixelFn)(int x, int y, int on, void *ud);
void mathbox_render_pixels(const MathBox *box, int origin_x, int origin_y,
                           SetPixelFn set_pixel, void *userdata, int scale);

static inline int mathbox_pixel_width(const MathBox *b, int scale)  { return b->width  * 8  * scale; }
static inline int mathbox_pixel_height(const MathBox *b, int scale) { return b->height * 16 * scale; }
