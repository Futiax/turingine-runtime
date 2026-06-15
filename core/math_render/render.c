// render.c est auto-suffisant : on redéfinit les types publics ici
// pour éviter les problèmes d'ordre d'include sur certains compilateurs.
#include <stdint.h>

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
  MATH_INTEGRAL,
  MATH_DIFF,
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

typedef void (*SetPixelFn)(int x, int y, int on, void *ud);
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/// CONSTANTES UTF-8 POUR LES CARACTÈRES SPÉCIAUX

static const char *barre     = "─";  // U+2500
static const char *int_haut  = "⌠";  // U+2320
static const char *int_bas   = "⌡";  // U+2321
static const char *bar_mid   = "│";  // U+2502
static const char *par_l_top = "⎛";  // U+239B
static const char *par_l_mid = "⎜";  // U+239C
static const char *par_l_bot = "⎝";  // U+239D
static const char *par_r_top = "⎞";  // U+239E
static const char *par_r_mid = "⎟";  // U+239F
static const char *par_r_bot = "⎠";  // U+23A0

/// LEXER (interne)
typedef enum {
  TOK_NUM,    // nombre : "42", "3.14"
  TOK_IDENT,  // identifiant : "x", "sin", "integrale"
  TOK_PLUS,   // +
  TOK_MINUS,  // -
  TOK_STAR,   // "*"
  TOK_SLASH,  // /
  TOK_CARET,  // ^
  TOK_LPAREN, // (
  TOK_RPAREN, // )
  TOK_SEMI,   // ; (accepté comme alias de ,)
  TOK_COMMA,  // ,
  TOK_END,    // fin de la chaîne
  TOK_ERROR,  // caractère invalide
} TokenType;

typedef struct {
  TokenType type;
  char text[64]; // texte brut du token (pour NUM et IDENT)
} Token;

typedef struct {
  const char *src; // la chaîne source
  int pos;         // position courante
  Token cur;       // token courant (déjà consommé → avancer)
} Lexer;

/// FONCTIONS

void cell_set(Cell *c, const char *utf8_char) {
  strncpy(c->bytes, utf8_char, sizeof(c->bytes) - 1);
  c->bytes[sizeof(c->bytes) - 1] =
      '\0'; // Assurez-vous que la chaîne est terminée par un '\0'
}

void cell_get(Cell *c, char *buffer) {
  strncpy(buffer, c->bytes, 4);
  buffer[3] = '\0'; // Assurez-vous que la chaîne est terminée par un '\0'
}

MathBox *mathbox_create(int width, int height, int baseline) {
  MathBox *m = malloc(sizeof(MathBox));
  m->cells = malloc(sizeof(Cell) * width * height);
  m->width = width;
  m->height = height;
  m->baseline = baseline;
  return m;
}

MathBox *mathbox_init(int width, int height, int baseline) {
  MathBox *m = malloc(sizeof(MathBox));
  m->cells = malloc(sizeof(Cell) * width * height);
  for (int i = 0; i < width * height; i++) {
    cell_set(&m->cells[i], " "); // Initialiser chaque cellule avec un espace
  }
  m->width = width;
  m->height = height;
  m->baseline = baseline;
  return m;
}

void mathbox_free(MathBox *m) {
  free(m->cells);
  free(m);
}

void mathbox_set(MathBox *m, int x, int y, const char *utf8_char) {
  if (x < 0 || x >= m->width || y < 0 || y >= m->height) {
    errno = EINVAL;
    return;
  }
  cell_set(&m->cells[y * m->width + x], utf8_char);
}

void mathbox_print(const MathBox *m) {
  for (int y = 0; y < m->height; y++) {
    for (int x = 0; x < m->width; x++) {
      printf("%s", m->cells[y * m->width + x].bytes);
    }
    printf("\n");
  }
}

void mathbox_blit(MathBox *dest, MathBox *src, int dest_x, int dest_y) {
  for (int y = 0; y < src->height; y++) {
    for (int x = 0; x < src->width; x++) {
      int target_x = dest_x + x;
      int target_y = dest_y + y;
      if (target_x >= 0 && target_x < dest->width && target_y >= 0 &&
          target_y < dest->height) {
        cell_set(&dest->cells[target_y * dest->width + target_x],
                 src->cells[y * src->width + x].bytes);
      }
    }
  }
}

void blit_centered(MathBox *dest, MathBox *src, int dest_y) {
  int dest_x = (dest->width - src->width) / 2;
  mathbox_blit(dest, src, dest_x, dest_y);
}

// Entoure inner de parenthèses extensibles verticalement.
// h=1 → ( )   h=2 → ⎛⎝ ⎞⎠   h≥3 → ⎛⎜⎝ ⎞⎟⎠
MathBox *mathbox_paren(MathBox *inner) {
  int h = inner->height;
  MathBox *result = mathbox_init(inner->width + 2, h, inner->baseline);
  mathbox_blit(result, inner, 1, 0);
  if (h == 1) {
    mathbox_set(result, 0, 0, "(");
    mathbox_set(result, inner->width + 1, 0, ")");
  } else if (h == 2) {
    mathbox_set(result, 0, 0, par_l_top);
    mathbox_set(result, 0, 1, par_l_bot);
    mathbox_set(result, inner->width + 1, 0, par_r_top);
    mathbox_set(result, inner->width + 1, 1, par_r_bot);
  } else {
    mathbox_set(result, 0, 0, par_l_top);
    for (int y = 1; y < h - 1; y++)
      mathbox_set(result, 0, y, par_l_mid);
    mathbox_set(result, 0, h - 1, par_l_bot);
    mathbox_set(result, inner->width + 1, 0, par_r_top);
    for (int y = 1; y < h - 1; y++)
      mathbox_set(result, inner->width + 1, y, par_r_mid);
    mathbox_set(result, inner->width + 1, h - 1, par_r_bot);
  }
  return result;
}

// Crée une boîte 1-ligne à partir d'une chaîne ASCII (pour les erreurs de parsing)
static MathBox *mathbox_atom(const char *text) {
  int len = strlen(text);
  MathBox *m = mathbox_init(len, 1, 0);
  for (int i = 0; i < len; i++) {
    char buf[2] = {text[i], '\0'};
    mathbox_set(m, i, 0, buf);
  }
  return m;
}

MathBox *mathbox_hcat(MathBox *left, MathBox *right, int sep) {
  int new_baseline =
      (left->baseline > right->baseline) ? left->baseline : right->baseline;
  int below_left = left->height - left->baseline;
  int below_right = right->height - right->baseline;
  int new_height =
      new_baseline + (below_left > below_right ? below_left : below_right);
  int new_width = left->width + sep + right->width;
  MathBox *result = mathbox_init(new_width, new_height, new_baseline);
  mathbox_blit(result, left, 0, new_baseline - left->baseline);
  mathbox_blit(result, right, left->width + sep,
               new_baseline - right->baseline);
  return result;
}

MathBox *mathbox_n_hcat(MathBox **boxes, int n, int sep) {
  if (n <= 0) {
    return NULL;
  }
  MathBox *result = boxes[0];
  for (int i = 1; i < n; i++) {
    MathBox *temp = mathbox_hcat(result, boxes[i], sep);
    mathbox_free(result);
    result = temp;
  }
  return result;
}

MathExpr *math_expr_atom(const char *name) {
  MathExpr *expr = malloc(sizeof(MathExpr));
  expr->type = MATH_ATOM;
  expr->name = strdup(name);
  expr->children = NULL;
  expr->n_children = 0;
  return expr;
}

MathExpr *math_expr_node(MathNodeType type, MathExpr **children,
                         int n_children) {
  MathExpr *expr = malloc(sizeof(MathExpr));
  expr->type = type;
  expr->name = NULL;
  expr->children = malloc(sizeof(MathExpr *) * n_children);
  expr->children =
      memcpy(expr->children, children, sizeof(MathExpr *) * n_children);
  expr->n_children = n_children;
  return expr;
}

void math_expr_free(MathExpr *expr) {
  if (expr->name) {
    free(expr->name);
  }
  for (int i = 0; i < expr->n_children; i++) {
    math_expr_free(expr->children[i]);
  }
  free(expr->children);
  free(expr);
}

void math_expr_dump(MathExpr *expr, int indent) {
  for (int i = 0; i < indent; i++) {
    printf("  ");
  }
  switch (expr->type) {
  case MATH_ATOM:
    printf("ATOM: %s\n", expr->name);
    break;
  case MATH_NEG:
    printf("NEG\n");
    break;
  case MATH_ADD:
    printf("ADD\n");
    break;
  case MATH_MUL:
    printf("MUL\n");
    break;
  case MATH_FRAC:
    printf("FRAC\n");
    break;
  case MATH_POW:
    printf("POW\n");
    break;
  case MATH_INTEGRAL:
    printf("INTEGRAL\n");
    break;
  case MATH_DIFF:
    printf("DIFF\n");
    break;
  case MATH_SQRT:
    printf("SQRT\n");
    break;
  case MATH_FUNC:
    printf("FUNC: %s\n", expr->name);
    break;
  }
  for (int i = 0; i < expr->n_children; i++) {
    math_expr_dump(expr->children[i], indent + 1);
  }
}

MathBox *math_render(const MathExpr *expr) {
  switch (expr->type) {

  case MATH_ATOM: {
    int len = strlen(expr->name);
    MathBox *box = mathbox_init(len, 1, 0);
    for (int i = 0; i < len; i++) {
      char buf[2] = {expr->name[i], '\0'};
      mathbox_set(box, i, 0, buf);
    }
    return box;
  }

  case MATH_FRAC: {
    if (expr->n_children < 2) return mathbox_atom("?/?");
    MathBox *num = math_render(expr->children[0]);
    MathBox *den = math_render(expr->children[1]);
    int width = (num->width > den->width) ? num->width : den->width;
    int height = num->height + 1 + den->height; // +1 pour la ligne de fraction
    int baseline = num->height; // La ligne de fraction est la ligne de base
    MathBox *result = mathbox_init(width, height, baseline);
    // Blit du numérateur centré
    blit_centered(result, num, 0);
    // Ligne de fraction
    for (int x = 0; x < width; x++) {
      mathbox_set(result, x, baseline, barre);
    }
    // Blit du dénominateur centré
    blit_centered(result, den, baseline + 1);
    mathbox_free(num);
    mathbox_free(den);
    return result;
  }

  case MATH_POW: {
    if (expr->n_children < 2) return mathbox_atom("?^?");
    MathBox *base = math_render(expr->children[0]);
    // Parenthèses auto si la base est multiligne ou un ADD/MUL/NEG
    MathNodeType btype = expr->children[0]->type;
    if (base->height > 1 || btype == MATH_ADD || btype == MATH_MUL ||
        btype == MATH_NEG) {
      MathBox *paren = mathbox_paren(base);
      mathbox_free(base);
      base = paren;
    }
    MathBox *exp = math_render(expr->children[1]);
    int width = base->width + exp->width;
    int height = base->height + exp->height;
    int baseline = exp->height + base->baseline;
    MathBox *result = mathbox_init(width, height, baseline);
    mathbox_blit(result, base, 0, exp->height);
    mathbox_blit(result, exp, base->width, 0);
    mathbox_free(base);
    mathbox_free(exp);
    return result;
  }

  case MATH_INTEGRAL: {
    // children selon le nombre d'args :
    //   1 enfant  : [expr]           → intégrale indéfinie ∫ expr dx
    //   3 enfants : [a, b, expr]     → intégrale définie  ∫_a^b expr dx
    int has_bounds = (expr->n_children == 3);
    MathBox *body  = math_render(expr->children[has_bounds ? 2 : 0]);
    MathBox *bdown = has_bounds ? math_render(expr->children[0]) : NULL;
    MathBox *bup   = has_bounds ? math_render(expr->children[1]) : NULL;

    int borne_w = has_bounds
                  ? (bup->width > bdown->width ? bup->width : bdown->width)
                  : 0;
    int left_w  = 1 + borne_w + (borne_w ? 1 : 0); // signe + bornes + espace
    int height  = has_bounds ? body->height + 2 : body->height;
    int baseline = has_bounds ? 1 + body->baseline : body->baseline;
    int dx_w = 3; // " dx"
    MathBox *result =
        mathbox_init(left_w + body->width + dx_w, height, baseline);

    // Signe intégrale
    int body_row = has_bounds ? 1 : 0;
    if (has_bounds) {
      mathbox_set(result, 0, 0,          int_haut);
      for (int y = 1; y < height - 1; y++)
        mathbox_set(result, 0, y,        bar_mid);
      mathbox_set(result, 0, height - 1, int_bas);
      mathbox_blit(result, bup,   1 + borne_w - bup->width,   0);
      mathbox_blit(result, bdown, 1 + borne_w - bdown->width, height - 1);
      mathbox_free(bup);
      mathbox_free(bdown);
    } else {
      // Intégrale indéfinie : juste ⌠/⌡ sur la hauteur du corps
      mathbox_set(result, 0, 0, int_haut);
      for (int y = 1; y < height - 1; y++)
        mathbox_set(result, 0, y, bar_mid);
      if (height > 1) mathbox_set(result, 0, height - 1, int_bas);
    }
    // Corps et " d<var>"
    const char *var = (expr->name && expr->name[0]) ? expr->name : "x";
    int dx_actual = 2 + strlen(var); // " d" + var (1 char supposé ASCII)
    mathbox_blit(result, body, left_w, body_row);
    mathbox_set(result, left_w + body->width,     baseline, " ");
    mathbox_set(result, left_w + body->width + 1, baseline, "d");
    // var peut être un seul caractère
    char vbuf[2] = {var[0], '\0'};
    mathbox_set(result, left_w + body->width + 2, baseline, vbuf);
    (void)dx_actual;

    mathbox_free(body);
    return result;
  }

  case MATH_DIFF: {
    // diff(expr, var)  →  d(expr)
    //                     ──────
    //                     d(var)
    if (expr->n_children < 1) return mathbox_atom("d?/d?");
    MathBox *top_d   = mathbox_atom("d");
    MathBox *body    = math_render(expr->children[0]);
    MathBox *bot_d   = mathbox_atom("d");
    MathBox *bot_var = (expr->n_children >= 2)
                       ? math_render(expr->children[1])
                       : mathbox_atom("x");

    // Numérateur : d·expr (hcat sans espace)
    MathBox *num_parts[2] = {top_d, body};
    MathBox *num = mathbox_n_hcat(num_parts, 2, 0);
    // Dénominateur : d·var
    MathBox *den_parts[2] = {bot_d, bot_var};
    MathBox *den = mathbox_n_hcat(den_parts, 2, 0);

    // Fraction num/den (même logique que MATH_FRAC)
    int width = num->width > den->width ? num->width : den->width;
    MathBox *result = mathbox_init(width, num->height + 1 + den->height,
                                   num->height); // baseline = ligne de barre
    mathbox_blit(result, num, (width - num->width) / 2, 0);
    for (int x = 0; x < width; x++)
      mathbox_set(result, x, num->height, "\xe2\x94\x80"); // ─
    mathbox_blit(result, den, (width - den->width) / 2, num->height + 1);
    mathbox_free(num);
    mathbox_free(den);
    return result;
  }

  case MATH_NEG: {
    MathBox *child = math_render(expr->children[0]);
    MathBox *result =
        mathbox_init(child->width + 1, child->height, child->baseline);
    mathbox_set(result, 0, child->baseline, "-");
    mathbox_blit(result, child, 1, 0);
    mathbox_free(child);
    return result;
  }

  case MATH_MUL: {
    int n = expr->n_children;
    int total = n * 2 - 1;
    MathBox **boxes = malloc(sizeof(MathBox *) * total);
    for (int i = 0; i < n; i++) {
      boxes[i * 2] = math_render(expr->children[i]);
      if (i < n - 1) {
        // · entre deux atomes numériques (sinon espace implicite)
        MathExpr *l = expr->children[i];
        MathExpr *r = expr->children[i + 1];
        int both_num = (l->type == MATH_ATOM && l->name && l->name[0] >= '0' && l->name[0] <= '9') &&
                       (r->type == MATH_ATOM && r->name && r->name[0] >= '0' && r->name[0] <= '9');
        MathBox *sep = mathbox_init(1, 1, 0);
        mathbox_set(sep, 0, 0, both_num ? "\xC2\xB7" : " "); // U+00B7 ·
        boxes[i * 2 + 1] = sep;
      }
    }
    MathBox *result = mathbox_n_hcat(boxes, total, 0);
    free(boxes);
    return result;
  }

  case MATH_ADD: {
    // Construit [child0, "+", child1, "+", child2, ...]
    // puis mathbox_n_hcat avec sep=1 (un espace entre chaque boîte)
    int n = expr->n_children;
    int total = n * 2 - 1;
    MathBox **boxes = malloc(sizeof(MathBox *) * total);
    for (int i = 0; i < n; i++) {
      boxes[i * 2] = math_render(expr->children[i]);
      if (i < n - 1) {
        MathBox *sep = mathbox_init(1, 1, 0);
        mathbox_set(sep, 0, 0, "+");
        boxes[i * 2 + 1] = sep;
      }
    }
    MathBox *result = mathbox_n_hcat(boxes, total, 1);
    free(boxes);
    return result;
  }

  case MATH_SQRT: {
    if (expr->n_children < 1) return mathbox_atom("√?");
    MathBox *child = math_render(expr->children[0]);
    // Layout :
    //  _____   ← row 0 : espace + barre sur toute la largeur du contenu
    // √x + 1  ← rows 1.. : √ à la baseline du contenu, contenu à droite
    int width = 1 + child->width;
    int height = 1 + child->height;
    int baseline = 1 + child->baseline;
    MathBox *result = mathbox_init(width, height, baseline);
    // Lower Barre au-dessus : col 0 = espace, cols 1..width-1 = _
    for (int x = 1; x < width; x++)
      mathbox_set(result, x, 0, barre);
    // √ à la baseline (ligne centrale du contenu)
    mathbox_set(result, 0, baseline, "√");
    // Contenu décalé d'une ligne vers le bas et d'une colonne vers la droite
    mathbox_blit(result, child, 1, 1);
    mathbox_free(child);
    return result;
  }

  case MATH_FUNC: {
    // Boîte du nom directement depuis la chaîne, sans créer un MathExpr
    int nlen = strlen(expr->name);
    MathBox *name_box = mathbox_init(nlen, 1, 0);
    for (int i = 0; i < nlen; i++) {
      char buf[2] = {expr->name[i], '\0'};
      mathbox_set(name_box, i, 0, buf);
    }
    MathBox *child_box = math_render(expr->children[0]);
    MathBox *paren_box = mathbox_paren(child_box);
    mathbox_free(child_box);
    MathBox *result = mathbox_hcat(name_box, paren_box, 0);
    mathbox_free(name_box);
    mathbox_free(paren_box);
    return result;
  }
  default:
    // Nœud non encore implémenté : affiche le type entre crochets
    return mathbox_init(1, 1, 0);
  }
}

// ── Déclarations forward
// ──────────────────────────────────────────────────────
static MathExpr *parse_expression(Lexer *lexer);

// ── Lexer
// ─────────────────────────────────────────────────────────────────────

static void lexer_read(Lexer *lexer) {
  const char *s = lexer->src + lexer->pos;
  while (*s == ' ' || *s == '\t' || *s == '\n') {
    s++;
    lexer->pos++;
  }

  if (*s == '\0') {
    lexer->cur.type = TOK_END;
    lexer->cur.text[0] = '\0';
    return;
  }
  if (*s >= '0' && *s <= '9') {
    int len = 0;
    while (s[len] >= '0' && s[len] <= '9')
      len++;
    if (s[len] == '.') {
      len++;
      while (s[len] >= '0' && s[len] <= '9')
        len++;
    }
    strncpy(lexer->cur.text, s, len);
    lexer->cur.text[len] = '\0';
    lexer->cur.type = TOK_NUM;
    lexer->pos += len;
    return;
  }
  if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_') {
    int len = 0;
    while ((s[len] >= 'a' && s[len] <= 'z') ||
           (s[len] >= 'A' && s[len] <= 'Z') || s[len] == '_' ||
           (s[len] >= '0' && s[len] <= '9'))
      len++;
    strncpy(lexer->cur.text, s, len);
    lexer->cur.text[len] = '\0';
    lexer->cur.type = TOK_IDENT;
    lexer->pos += len;
    return;
  }
  lexer->cur.text[0] = *s;
  lexer->cur.text[1] = '\0';
  lexer->pos++;
  switch (*s) {
  case '+':
    lexer->cur.type = TOK_PLUS;
    break;
  case '-':
    lexer->cur.type = TOK_MINUS;
    break;
  case '*':
    lexer->cur.type = TOK_STAR;
    break;
  case '/':
    lexer->cur.type = TOK_SLASH;
    break;
  case '^':
    lexer->cur.type = TOK_CARET;
    break;
  case '(':
    lexer->cur.type = TOK_LPAREN;
    break;
  case ')':
    lexer->cur.type = TOK_RPAREN;
    break;
  case ';':
    lexer->cur.type = TOK_SEMI;
    break;
  case ',':
    lexer->cur.type = TOK_COMMA;
    break;
  default:
    lexer->cur.type = TOK_ERROR;
    break;
  }
}

void lexer_init(Lexer *lexer, const char *src) {
  lexer->src = src;
  lexer->pos = 0;
  lexer->cur.type = TOK_END;
  lexer->cur.text[0] = '\0';
  lexer_read(lexer); // primer le premier token
}

Token lexer_peek(Lexer *lexer) { return lexer->cur; }

Token lexer_next(Lexer *lexer) {
  Token prev = lexer->cur;
  lexer_read(lexer);
  return prev; // retourne le token qu'on vient de consommer
}

// ── Parser
// ────────────────────────────────────────────────────────────────────

// Lit les arguments séparés par ',' ou ';' jusqu'au ')' fermant.
static int parse_args(Lexer *lexer, MathExpr **args, int max) {
  int n = 0;
  while (n < max && lexer_peek(lexer).type != TOK_RPAREN &&
         lexer_peek(lexer).type != TOK_END) {
    args[n++] = parse_expression(lexer);
    TokenType t = lexer_peek(lexer).type;
    if (t == TOK_COMMA || t == TOK_SEMI)
      lexer_next(lexer);
    else
      break;
  }
  return n;
}

// primaire : nombre | ident | ident'(' args ')' | '(' expr ')' | '-' primaire
static MathExpr *parse_primary(Lexer *lexer) {
  Token tok = lexer_peek(lexer);

  if (tok.type == TOK_MINUS) {
    lexer_next(lexer);
    MathExpr *child = parse_primary(lexer);
    MathExpr *ch[1] = {child};
    return math_expr_node(MATH_NEG, ch, 1);
  }

  if (tok.type == TOK_NUM) {
    lexer_next(lexer);
    return math_expr_atom(tok.text);
  }

  if (tok.type == TOK_IDENT) {
    lexer_next(lexer);
    if (lexer_peek(lexer).type != TOK_LPAREN)
      return math_expr_atom(tok.text);

    // Appel de fonction : ident '(' args ')'
    lexer_next(lexer); // consomme '('
    MathExpr *args[8];
    int n;

    // integrate(expr, var [, a, b])  — syntaxe Giac
    // children selon n : [expr,var] ou [expr,var,a,b]
    if (strcmp(tok.text, "integrate") == 0 ||
        strcmp(tok.text, "integrale") == 0) {
      n = parse_args(lexer, args, 4);
      lexer_next(lexer); // consomme ')'
      if (n < 1) return math_expr_atom("\xe2\x88\xab"); // ∫ vide
      if (n == 4) {
        // integrate(expr, var, a, b) → [a, b, expr]
        MathExpr *ordered[3] = {args[2], args[3], args[0]};
        char *varname = (args[1]->name) ? strdup(args[1]->name) : strdup("x");
        math_expr_free(args[1]);
        if (n > 4) for (int i = 4; i < n; i++) math_expr_free(args[i]);
        MathExpr *node = math_expr_node(MATH_INTEGRAL, ordered, 3);
        node->name = varname;
        return node;
      }
      // integrate(expr) ou integrate(expr, var) → intégrale indéfinie
      char *varname2 = (n >= 2 && args[1]->name) ? strdup(args[1]->name) : strdup("x");
      if (n >= 2) math_expr_free(args[1]);
      for (int i = 2; i < n; i++) math_expr_free(args[i]);
      MathExpr *inode = math_expr_node(MATH_INTEGRAL, args, 1);
      inode->name = varname2;
      return inode;
    }

    // diff(expr, var)  — syntaxe Giac
    if (strcmp(tok.text, "diff") == 0 ||
        strcmp(tok.text, "derive") == 0) {
      n = parse_args(lexer, args, 2);
      lexer_next(lexer);
      if (n < 1) return math_expr_atom("d?/dx");
      return math_expr_node(MATH_DIFF, args, n);
    }

    if (strcmp(tok.text, "sqrt") == 0 || strcmp(tok.text, "racine") == 0) {
      n = parse_args(lexer, args, 1);
      lexer_next(lexer);
      if (n < 1) return math_expr_atom("\xe2\x88\x9a"); // √
      return math_expr_node(MATH_SQRT, args, 1);
    }
    // Fonction générique : sin, cos, etc.
    n = parse_args(lexer, args, 8);
    lexer_next(lexer); // consomme ')'
    if (n < 1) return math_expr_atom(tok.text); // sin() vide → "sin"
    MathExpr *node = math_expr_node(MATH_FUNC, args, n);
    node->name = strdup(tok.text);
    return node;
  }

  if (tok.type == TOK_LPAREN) {
    lexer_next(lexer); // consomme '('
    MathExpr *expr = parse_expression(lexer);
    if (lexer_peek(lexer).type == TOK_RPAREN)
      lexer_next(lexer); // consomme ')'
    else
      fprintf(stderr, "Erreur : ')' attendu\n");
    return expr;
  }

  fprintf(stderr, "Erreur : token inattendu '%s'\n", tok.text);
  return math_expr_atom("?");
}

// facteur : primaire ('^' primaire)?
static MathExpr *parse_factor(Lexer *lexer) {
  MathExpr *base = parse_primary(lexer);
  if (lexer_peek(lexer).type == TOK_CARET) {
    lexer_next(lexer); // consomme '^'
    MathExpr *exp = parse_primary(lexer);
    MathExpr *ch[2] = {base, exp};
    return math_expr_node(MATH_POW, ch, 2);
  }
  return base;
}

// terme : facteur (('*' | '/' | <implicite>) facteur)*
// Multiplication implicite : "2x", "2(x+1)" — deux facteurs sans opérateur
static MathExpr *parse_term(Lexer *lexer) {
  MathExpr *left = parse_factor(lexer);
  for (;;) {
    TokenType t = lexer_peek(lexer).type;
    if (t == TOK_STAR || t == TOK_SLASH) {
      Token op = lexer_next(lexer);
      MathExpr *right = parse_factor(lexer);
      MathExpr *ch[2] = {left, right};
      left = math_expr_node(op.type == TOK_STAR ? MATH_MUL : MATH_FRAC, ch, 2);
    } else if (t == TOK_IDENT || t == TOK_LPAREN) {
      // Pas d'opérateur → multiplication implicite
      MathExpr *right = parse_factor(lexer);
      MathExpr *ch[2] = {left, right};
      left = math_expr_node(MATH_MUL, ch, 2);
    } else {
      break;
    }
  }
  return left;
}

// expression : terme (('+' | '-') terme)*
static MathExpr *parse_expression(Lexer *lexer) {
  MathExpr *left = parse_term(lexer);
  while (lexer_peek(lexer).type == TOK_PLUS ||
         lexer_peek(lexer).type == TOK_MINUS) {
    Token op = lexer_next(lexer);
    MathExpr *right = parse_term(lexer);
    if (op.type == TOK_PLUS) {
      MathExpr *ch[2] = {left, right};
      left = math_expr_node(MATH_ADD, ch, 2);
    } else {
      // a - b → ADD(a, NEG(b))
      MathExpr *neg_ch[1] = {right};
      MathExpr *neg = math_expr_node(MATH_NEG, neg_ch, 1);
      MathExpr *ch[2] = {left, neg};
      left = math_expr_node(MATH_ADD, ch, 2);
    }
  }
  return left;
}

// ── Rendu pixels (6.3) ───────────────────────────────────────────────────────
//
// Chaque cellule du MathBox est rendue en 8×16 pixels via font8x16 (ASCII)
// ou via une table de glyphes custom pour les symboles math UTF-8.
//
// Interface : le caller fournit un callback set_pixel(x, y, on, userdata)
// ce qui rend la fonction compatible avec n'importe quel framebuffer
// (DRM, Image du Grapher, SDL, etc.).

#include "../common/font8x16.h"

// Glyphes custom 8×16 pour les symboles math hors-ASCII.
// Chaque ligne est un masque de bits : MSB = pixel gauche (comme font8x16).
typedef struct {
  const char *utf8;
  uint8_t bmp[16];
} MathGlyph;

static const MathGlyph math_glyphs[] = {
    {"─",
     {// Ligne horizontale milieu
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00}},
    {"│",
     {// Ligne verticale centre
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10}},
    {"⌠",
     {// Intégrale haut : accroche à droite en haut, trait droit vers le bas
      0x06, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
      0x08, 0x08, 0x08, 0x08}},
    {"⌡",
     {// Intégrale bas : trait droit, accroche à gauche en bas
      0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
      0x08, 0x08, 0x10, 0x60}},
    {"⎛",
     {// Parenthèse gauche haut
      0x06, 0x08, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10}},
    {"⎜",
     {// Parenthèse gauche milieu
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x10, 0x10}},
    {"⎝",
     {// Parenthèse gauche bas
      0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
      0x10, 0x10, 0x08, 0x06}},
    {"⎞",
     {// Parenthèse droite haut
      0x60, 0x10, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
      0x08, 0x08, 0x08, 0x08}},
    {"⎟",
     {// Parenthèse droite milieu
      0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
      0x08, 0x08, 0x08, 0x08}},
    {"⎠",
     {// Parenthèse droite bas
      0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
      0x08, 0x08, 0x10, 0x60}},
    {"√",
     {// Racine carrée
      0x01, 0x01, 0x01, 0x03, 0x02, 0x06, 0x04, 0x8C, 0x88, 0xD0, 0xA0, 0xC0,
      0x40, 0x40, 0x00, 0x00}},
};
static const int N_MATH_GLYPHS =
    (int)(sizeof(math_glyphs) / sizeof(math_glyphs[0]));

// Cherche un glyphe custom pour une cellule UTF-8. Retourne NULL si ASCII.
static const uint8_t *find_glyph(const Cell *cell) {
  // ASCII : géré par font8x16
  if ((unsigned char)cell->bytes[0] < 0x80)
    return NULL;
  for (int i = 0; i < N_MATH_GLYPHS; i++) {
    if (strcmp(cell->bytes, math_glyphs[i].utf8) == 0)
      return math_glyphs[i].bmp;
  }
  return NULL; // symbole inconnu → sera affiché comme espace
}

// Rend le MathBox en pixels. scale=1 → 8×16 px/cellule, scale=2 → 16×32.
// set_pixel(px_x, px_y, on=1/0, userdata) est appelé pour chaque pixel.
void mathbox_render_pixels(const MathBox *box, int origin_x, int origin_y,
                           SetPixelFn set_pixel, void *userdata, int scale) {
  for (int cy = 0; cy < box->height; cy++) {
    for (int cx = 0; cx < box->width; cx++) {
      const Cell *cell = &box->cells[cy * box->width + cx];
      const uint8_t *bmp = find_glyph(cell);
      const uint8_t *rows;

      if (bmp) {
        rows = bmp;
      } else {
        unsigned char c = (unsigned char)cell->bytes[0];
        if (c == 0) c = ' ';
        rows = font8x16 + c * 16;
      }

      int px0 = origin_x + cx * 8 * scale;
      int py0 = origin_y + cy * 16 * scale;
      for (int row = 0; row < 16; row++) {
        uint8_t mask = rows[row];
        for (int bit = 0; bit < 8; bit++) {
          int on = (mask >> (7 - bit)) & 1;
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++)
              set_pixel(px0 + bit * scale + sx, py0 + row * scale + sy, on, userdata);
        }
      }
    }
  }
}

MathExpr *math_parse(const char *input) {
  Lexer lex;
  lexer_init(&lex, input);
  return parse_expression(&lex);
}

#ifdef MATH_RENDER_TEST
// ── Main (standalone test) ────────────────────────────────────────────────────

typedef struct {
  int w, h;
  uint8_t *buf;
} PPM;

static void ppm_set(int x, int y, int on, void *ud) {
  PPM *p = ud;
  if (x < 0 || x >= p->w || y < 0 || y >= p->h)
    return;
  uint8_t v = on ? 0x00 : 0xFF; // noir sur blanc
  int i = (y * p->w + x) * 3;
  p->buf[i] = p->buf[i + 1] = p->buf[i + 2] = v;
}

int main() {
  const char *tests[] = {
      "2/(x+4)",       "integrale(0;1;2/(x+4))",
      "(x+1)^2",
      "x^2 + 2x + 1", // multiplication implicite
      "2(a+b)",       // 2*(a+b) implicite
      "-x^2 + 3x - 1", "sqrt(x+1)",
      "sin(x/2)",
  };
  for (int i = 0; i < (int)(sizeof(tests) / sizeof(tests[0])); i++) {
    printf("\n=== %s ===\n", tests[i]);
    MathExpr *expr = math_parse(tests[i]);
    MathBox *box = math_render(expr);
    mathbox_print(box);
    mathbox_free(box);
    math_expr_free(expr);
  }

  // Test rendu pixels : génère un PPM pour "integrale(0;1;2/(x+4))"
  {
    MathExpr *expr = math_parse("integrale(0;1;2/(x+4))");
    MathBox *box = math_render(expr);
    int pw = box->width * 8;
    int ph = box->height * 16;
    PPM ppm = {pw, ph, malloc(pw * ph * 3)};
    memset(ppm.buf, 0xFF, pw * ph * 3); // fond blanc
    mathbox_render_pixels(box, 0, 0, ppm_set, &ppm, 1);
    FILE *f = fopen("math_render.ppm", "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", pw, ph);
      fwrite(ppm.buf, 1, pw * ph * 3, f);
      fclose(f);
      printf("\nPPM sauvegardé : math_render.ppm (%dx%d)\n", pw, ph);
    }
    free(ppm.buf);
    mathbox_free(box);
    math_expr_free(expr);
  }
}
#endif // MATH_RENDER_TEST
