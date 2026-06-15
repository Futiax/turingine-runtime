# TP — Rendu 2D d'expressions mathématiques en C

> **Prérequis** : Connaître le C (pointeurs, `malloc`, structures, récursion).
> **Objectif** : Construire un moteur de rendu 2D d'expressions mathématiques — comme sur une NumWorks — qui transforme `"integrale(0;1;2/(x+4))"` en :
> ```
>  1
>  ⌠     2
>  ⌡   ─────  dx
>  0   x + 4
> ```
> **Sortie** : Des chaînes affichables dans n'importe quel terminal UTF-8, réutilisables dans le terminal Turingine, le Grapher, ou une future app calculatrice.

---

## Vue d'ensemble de l'architecture

Le moteur est composé de trois couches indépendantes :

```
Texte brut          AST                  MathBox (grille 2D)
──────────────────────────────────────────────────────────
"2/(x+4)"   →   Frac(2, Add(x, 4))  →   2
                                        ─────
                                        x + 4
```

1. **`math_expr`** — définit l'arbre syntaxique (les nœuds : atome, fraction, puissance, intégrale…)
2. **`math_parse`** — transforme une chaîne de texte en arbre
3. **`math_render`** — transforme un arbre en grille 2D de caractères

On implémente dans cet ordre : d'abord les structures, ensuite le renderer (on peut tester visuellement), et enfin le parser.

---

## Partie 0 — Les caractères UTF-8 pour les maths

### 0.1 — Pourquoi UTF-8

Le terminal Turingine affiche du texte UTF-8. Ça nous donne accès à des caractères qu'on ne peut pas taper au clavier mais qui existent dans la norme Unicode et s'affichent dans le font8x16 :

| Symbole | Code UTF-8 | Usage |
|---------|-----------|-------|
| `─`     | U+2500    | Barre de fraction (ligne horizontale) |
| `⌠`     | U+2320    | Haut du signe intégrale |
| `⌡`     | U+2321    | Bas du signe intégrale |
| `√`     | U+221A    | Racine carrée |
| `·`     | U+00B7    | Multiplication |

En C, ces caractères s'écrivent comme des séquences d'octets littérales dans les chaînes :
```c
const char *barre    = "─";   // 3 octets : 0xE2 0x94 0x80
const char *int_haut = "⌠";   // 3 octets : 0xE2 0x8C 0xA0
const char *int_bas  = "⌡";   // 3 octets : 0xE2 0x8C 0xA1
```

> [!IMPORTANT]
> Notre `MathBox` travaille en **colonnes de caractères**, pas en octets. Un caractère UTF-8 occupe 1 ou 3 octets mais toujours **1 colonne** à l'écran (pour ces symboles). On va gérer ça avec des chaînes de cellules.

### 0.2 — La cellule de caractère

Pour simplifier, on représente chaque case de la grille par une cellule pouvant contenir jusqu'à 4 octets (suffisant pour tout caractère UTF-8 à 3 octets + le `\0`) :

```c
typedef struct {
    char bytes[4];  // jusqu'à 3 octets UTF-8 + '\0'
} Cell;
```

**Exercice 0.2** — Écris ces deux fonctions utilitaires :

```c
// Remplit une Cell avec une chaîne UTF-8 courte (max 3 octets + \0)
void cell_set(Cell *c, const char *utf8);

// Copie le contenu d'une Cell dans un char* de destination (pour printf)
// dst doit avoir au moins 4 octets
void cell_get(const Cell *c, char *dst);
```

Teste avec :
```c
Cell c;
cell_set(&c, "─");
char buf[4];
cell_get(&c, buf);
printf("%s\n", buf);  // doit afficher ─
```

---

## Partie 1 — Le MathBox : la grille 2D

Un `MathBox` est le résultat concret du rendu d'une expression. C'est une grille de cellules avec :
- une **largeur** et une **hauteur** (en colonnes/lignes de caractères)
- une **ligne de base** (`baseline`) : le numéro de ligne sur laquelle se trouve le "centre" de l'expression

La baseline est **fondamentale** pour aligner les expressions côte à côte. Par exemple, dans `a + b/c` :
```
         b       ← ligne 0
a  +   ─────     ← ligne 1  (baseline de chaque élément)
         c       ← ligne 2
```
La fraction `b/c` a sa baseline à la ligne 1 (la barre). Le `a` et le `+` ont leur baseline à la ligne 0. Pour les aligner, on les **décale vers le bas** de `baseline_fraction - baseline_atome = 1` ligne.

### Exercice 1.1 — Déclarer MathBox

Dans `math_render.h`, déclare la structure :

```c
#pragma once
#include <stddef.h>

typedef struct {
    char bytes[4];
} Cell;

typedef struct {
    Cell  *cells;     // grille aplatie : cells[y * width + x]
    int    width;     // nombre de colonnes
    int    height;    // nombre de lignes
    int    baseline;  // index de la ligne "centrale" (0 = ligne du haut)
} MathBox;
```

**À faire** — implémente dans `math_render.c` :

1. **`MathBox *mathbox_create(int width, int height, int baseline)`**
   - Alloue la struct et le tableau `cells` (`width * height` cellules)
   - Remplit chaque cellule avec un espace `" "`
   - Retourne le pointeur (ou `NULL` si `malloc` échoue)

2. **`void mathbox_free(MathBox *box)`**
   - Libère `box->cells` puis `box`

3. **`void mathbox_set(MathBox *box, int x, int y, const char *utf8)`**
   - Écrit un caractère UTF-8 à la position `(x, y)`, en vérifiant les bornes

4. **`void mathbox_print(const MathBox *box)`**
   - Affiche la grille ligne par ligne dans le terminal (avec `printf` ou `fputs`)
   - À la fin de chaque ligne, imprime `\n`

**Test** :
```c
MathBox *b = mathbox_create(5, 3, 1);
mathbox_set(b, 0, 1, "─");
mathbox_set(b, 1, 1, "─");
mathbox_set(b, 2, 1, "─");
mathbox_set(b, 3, 1, "─");
mathbox_set(b, 4, 1, "─");
mathbox_print(b);  // doit afficher 3 lignes, dont la centrale avec "─────"
mathbox_free(b);
```

---

### Exercice 1.2 — Copier une boîte dans une autre

On va souvent placer une sous-boîte à une position donnée dans une boîte parente. Implémente :

```c
void mathbox_blit(MathBox *dst, const MathBox *src, int x_off, int y_off);
```

Pour chaque cellule `(cx, cy)` de `src`, copie-la vers `(x_off + cx, y_off + cy)` dans `dst`. Vérifie que la destination est dans les bornes avant de copier.

**Indices** :
- Deux boucles imbriquées : `for y in 0..src->height`, `for x in 0..src->width`
- Utilise `cell_set` et `cell_get` ou une copie directe de `Cell`

---

### Exercice 1.3 — Concaténer des boîtes horizontalement

Cette opération est très fréquente (pour afficher `a + b`) :

```c
// Crée une nouvelle boîte contenant left puis right, alignées sur leur baseline
// Un espace de séparation est ajouté entre les deux si sep == 1
MathBox *mathbox_hcat(const MathBox *left, const MathBox *right, int sep);
```

**Principe** :
1. La baseline de la boîte résultante est `max(left->baseline, right->baseline)`
2. La hauteur résultante est `max(left->height - left->baseline, right->height - right->baseline) + baseline_resultat`
3. La largeur résultante est `left->width + right->width + sep`
4. Place `left` avec un décalage vertical de `baseline_resultat - left->baseline`
5. Place `right` idem, à la colonne `left->width + sep`

> [!TIP]
> Si `sep == 1`, laisse une colonne d'espaces entre les deux boîtes. Si `sep == 0`, elles sont collées.

**Test** — deux atomes "a" et "b" concaténés avec séparateur doivent donner `"a b"` sur une seule ligne.

---

## Partie 2 — L'AST : l'arbre syntaxique

### Exercice 2.1 — Les types de nœuds

Dans `math_expr.h`, définis l'énumération des types :

```c
#pragma once
#include <stddef.h>

typedef enum {
    MATH_ATOM,      // feuille : un nombre ou une variable ("x", "42", "pi")
    MATH_NEG,       // négation unaire : -a
    MATH_ADD,       // addition : a + b
    MATH_MUL,       // multiplication implicite : a * b
    MATH_FRAC,      // fraction : numérateur / dénominateur
    MATH_POW,       // puissance : base ^ exposant
    MATH_INTEGRAL,  // intégrale : ∫(borne_basse; borne_haute; expression)
    MATH_SQRT,      // racine carrée : √(expression)
    MATH_FUNC,      // appel de fonction : sin(x), cos(x), etc.
} MathNodeType;
```

### Exercice 2.2 — La structure MathExpr

Toujours dans `math_expr.h` :

```c
typedef struct MathExpr MathExpr;

struct MathExpr {
    MathNodeType type;

    // Pour MATH_ATOM et MATH_FUNC : le nom/valeur en texte
    char *name;         // ex: "x", "42", "sin"

    // Enfants génériques (pour ADD, MUL, POW, FRAC, NEG, FUNC, INTEGRAL)
    MathExpr **children;
    int        n_children;
};
```

**À faire** dans `math_expr.c` :

1. **`MathExpr *math_expr_atom(const char *name)`**
   - Alloue un nœud `MATH_ATOM`, duplique `name` avec `strdup`
   - `children = NULL`, `n_children = 0`

2. **`MathExpr *math_expr_node(MathNodeType type, MathExpr **children, int n)`**
   - Alloue un nœud du type donné
   - Copie le tableau de pointeurs enfants (`malloc` + `memcpy`)
   - `name = NULL`

3. **`void math_expr_free(MathExpr *expr)`**
   - Libère récursivement tous les enfants, puis `name`, puis la struct elle-même

> [!WARNING]
> `math_expr_free` est récursive. Si tu oublies un cas, tu as une fuite mémoire. Vérifie que `free(NULL)` ne crash pas (c'est garanti par la norme C).

**Test** — construis l'AST de `2/(x+4)` à la main :

```c
MathExpr *num   = math_expr_atom("2");
MathExpr *x     = math_expr_atom("x");
MathExpr *four  = math_expr_atom("4");
MathExpr *add_children[] = { x, four };
MathExpr *den   = math_expr_node(MATH_ADD, add_children, 2);
MathExpr *frac_children[] = { num, den };
MathExpr *frac  = math_expr_node(MATH_FRAC, frac_children, 2);
// frac représente 2/(x+4)
math_expr_free(frac);
```

---

### Exercice 2.3 — Affichage debug de l'AST

Pour pouvoir vérifier le parser plus tard, implémente une fonction qui affiche l'AST sous forme d'arbre indenté :

```c
void math_expr_dump(const MathExpr *expr, int indent);
```

Avec `indent = 0`, elle affiche :
```
FRAC
  ATOM(2)
  ADD
    ATOM(x)
    ATOM(4)
```

**Indice** : utilise une boucle `for (int i = 0; i < indent*2; i++) putchar(' ')` pour l'indentation, puis appelle récursivement sur chaque enfant avec `indent + 1`.

---

## Partie 3 — Le renderer : AST → MathBox

C'est le cœur du moteur. On écrit une fonction récursive `math_render(expr)` qui retourne un `MathBox*`.

### Exercice 3.1 — Rendre un atome

```c
MathBox *math_render(const MathExpr *expr);
```

Commence par le cas de base : `MATH_ATOM`.

Un atome comme `"x"`, `"42"`, ou `"pi"` est une boîte de hauteur 1, de largeur `strlen(name)` (en caractères, pas en octets), et de baseline 0.

**À faire** :
1. Dans `math_render`, traite le cas `MATH_ATOM`
2. Crée une boîte `1 × len`
3. Remplis la ligne 0 avec les caractères du nom (un par case)

> [!NOTE]
> Pour l'instant, assume que les atomes ne contiennent que des caractères ASCII 1-octet. Le cas des symboles multi-octets sera géré au besoin.

**Test** :
```c
MathExpr *x = math_expr_atom("x");
MathBox *box = math_render(x);
mathbox_print(box);  // affiche : x
```

---

### Exercice 3.2 — Rendre une addition

Pour `MATH_ADD` avec `n` enfants, on concatène les boîtes des enfants avec `" + "` entre chacun.

**À faire** :
1. Render chaque enfant → tableau de `MathBox*`
2. Concatène-les avec `mathbox_hcat` en insérant un atome `"+"` (avec espaces) entre chaque paire
3. Libère les boîtes intermédiaires

**Alternative plus propre** : écris une fonction `mathbox_hcat_sep(boxes[], n, sep_str)` qui concatène `n` boîtes avec un séparateur texte.

**Test** :
```c
// Doit afficher : x + 4
MathExpr *x    = math_expr_atom("x");
MathExpr *four = math_expr_atom("4");
MathExpr *children[] = { x, four };
MathExpr *add  = math_expr_node(MATH_ADD, children, 2);
MathBox  *box  = math_render(add);
mathbox_print(box);
```

---

### Exercice 3.3 — Rendre une fraction

C'est là que le rendu 2D devient intéressant.

```
   numérateur
  ───────────
  dénominateur
```

**Principe** :
1. Render le numérateur → `num_box`
2. Render le dénominateur → `den_box`
3. Largeur de la barre = `max(num_box->width, den_box->width) + 2` (marges de 1 de chaque côté)
4. Hauteur totale = `num_box->height + 1 + den_box->height`
5. Baseline = `num_box->height` (la ligne de la barre est la baseline de la fraction)
6. Centre `num_box` horizontalement dans la ligne du haut, remplis la ligne centrale avec `"─"`, centre `den_box` dans la ligne du bas

**Fonction helper** :

```c
// Centre src horizontalement dans dst à la ligne y
static void blit_centered(MathBox *dst, const MathBox *src, int y);
```

> [!TIP]
> L'offset horizontal pour centrer : `(dst->width - src->width) / 2`.

**Test** :
```c
// Doit afficher :
//    2
//  ─────
//  x + 4
MathExpr *num = math_expr_atom("2");
MathExpr *x   = math_expr_atom("x");
MathExpr *den_ch[] = { x, math_expr_atom("4") };
MathExpr *den = math_expr_node(MATH_ADD, den_ch, 2);
MathExpr *frac_ch[] = { num, den };
MathExpr *frac = math_expr_node(MATH_FRAC, frac_ch, 2);
MathBox  *box  = math_render(frac);
mathbox_print(box);
```

---

### Exercice 3.4 — Rendre une puissance

```
  a
x
```

La base est à sa position normale, l'exposant est placé **en haut à droite**, une ligne au-dessus de la baseline de la base.

**Principe** :
1. Render la base → `base_box`
2. Render l'exposant → `exp_box`
3. Largeur totale = `base_box->width + exp_box->width`
4. Hauteur totale = `base_box->height + exp_box->height - 1` (ils se chevauchent d'une ligne)
5. Baseline = `exp_box->height - 1 + base_box->baseline`
6. Place `exp_box` en haut à droite, `base_box` en bas à gauche

> [!NOTE]
> Si l'exposant est un atome d'un seul caractère (ex: `"2"`), le rendu est suffisamment lisible. Pour un exposant complexe (ex: `x+1`), on pourrait le réduire, mais c'est hors scope de ce TP.

**Test** :
```c
// Doit afficher :
//  2
// x
MathExpr *base = math_expr_atom("x");
MathExpr *exp  = math_expr_atom("2");
MathExpr *pow_ch[] = { base, exp };
MathExpr *pw   = math_expr_node(MATH_POW, pow_ch, 2);
MathBox  *box  = math_render(pw);
mathbox_print(box);
```

---

### Exercice 3.5 — Rendre une intégrale

```
 b
 ⌠
 ⌡  f(x) dx
 a
```

**Principe** :
1. Render `borne_haute` → `up_box`
2. Render `borne_basse` → `down_box`
3. Render `expression` → `expr_box`
4. La hauteur de la colonne intégrale = `max(3, expr_box->height)` (au minimum 3 lignes pour `⌠`, corps, `⌡`)
5. Structure de la colonne intégrale (de haut en bas) :
   - Ligne 0 : `up_box` aligné à droite de la colonne intégrale
   - Lignes 1 à h-2 : `⌠` puis lignes vides, puis `⌡` sur la dernière ligne avant `down_box`
   - Dernière ligne : `down_box` aligné à droite
6. Place `expr_box` à droite de la colonne, suivi de `" dx"`
7. La baseline de la boîte totale est le milieu vertical

> [!TIP]
> Commence par construire la colonne de gauche (bornes + symbole intégrale) comme une boîte, puis `mathbox_hcat` avec l'expression et `" dx"`.

**Test** :
```c
// Doit afficher quelque chose comme :
//  1
//  ⌠     2
//  ⌡   ─────  dx
//  0   x + 4
```

---

### Exercice 3.6 — Rendre une racine carrée

```
  ___
√ x+1
```

**Principe** :
1. Render l'expression sous la racine → `inner_box`
2. Largeur totale = `1 + inner_box->width` (le `√` + le contenu)
3. Ajoute une ligne de `─` au-dessus du contenu (le trait horizontal de la racine)
4. Place `√` sur la ligne de la baseline de `inner_box`

---

### Exercice 3.7 — Rendre une fonction nommée

Pour `sin(x)`, `cos(x)`, etc. : le nom de la fonction suivi de l'argument entre parenthèses.

```c
// Cas MATH_FUNC : name = "sin", children[0] = argument
```

**Principe** :
1. Render l'argument → `arg_box`
2. Si `arg_box->height == 1` : résultat simple = `"sin(x)"` → atome
3. Si l'argument est multiligne (ex: une fraction) : place `"sin("` à gauche, l'argument au milieu, `")"` à droite — les parenthèses doivent s'étirer verticalement sur toute la hauteur de l'argument

Pour les parenthèses étirées, une approximation simple : remplis la colonne gauche/droite avec `│` et mets `(` / `)` sur la ligne de la baseline.

---

## Partie 4 — Le parser : texte → AST

On implémente un **parser à descente récursive** pour la grammaire suivante (simplifiée) :

```
expression = terme (("+" | "-") terme)*
terme      = facteur (("*" | "/") facteur)*  |  facteur facteur  (mul implicite)
facteur    = primaire ("^" primaire)?
primaire   = NOMBRE
           | IDENTIFIANT
           | IDENTIFIANT "(" args ")"   (appel de fonction)
           | "(" expression ")"
           | "-" primaire

args       = expression (";" expression)*
```

Les fonctions spéciales reconnues par leur nom :
- `integrale(borne_basse; borne_haute; expr)` → `MATH_INTEGRAL`
- `sqrt(expr)` ou `racine(expr)` → `MATH_SQRT`
- Tout autre identifiant avec parenthèses → `MATH_FUNC`

### Exercice 4.1 — Le lexer (tokenizer)

Avant de parser, il faut découper la chaîne en **tokens** (unités lexicales).

Définis ces types dans `math_parse.h` :

```c
typedef enum {
    TOK_NUM,    // nombre : "42", "3.14"
    TOK_IDENT,  // identifiant : "x", "sin", "integrale"
    TOK_PLUS,   // +
    TOK_MINUS,  // -
    TOK_STAR,   // *
    TOK_SLASH,  // /
    TOK_CARET,  // ^
    TOK_LPAREN, // (
    TOK_RPAREN, // )
    TOK_SEMI,   // ;
    TOK_END,    // fin de la chaîne
    TOK_ERROR,  // caractère invalide
} TokenType;

typedef struct {
    TokenType type;
    char      text[64];   // texte brut du token (pour NUM et IDENT)
} Token;
```

Et l'état du lexer :

```c
typedef struct {
    const char *src;   // la chaîne source
    int         pos;   // position courante
    Token       cur;   // token courant (déjà consommé → avancer)
} Lexer;
```

**À faire** :

1. **`void lexer_init(Lexer *lex, const char *src)`** — initialise le lexer et lit le premier token
2. **`Token lexer_peek(Lexer *lex)`** — retourne le token courant **sans avancer**
3. **`Token lexer_next(Lexer *lex)`** — retourne le token courant et lit le suivant

Pour lire le prochain token depuis `src + pos` :
- Ignore les espaces
- Si chiffre ou `.` : lit jusqu'à la fin du nombre → `TOK_NUM`
- Si lettre : lit jusqu'à la fin de l'identifiant → `TOK_IDENT`
- Sinon : caractère unique → le token correspondant

> [!TIP]
> `isdigit(c)`, `isalpha(c)`, `isalnum(c)` et `isspace(c)` de `<ctype.h>` sont tes amis.

**Test** :
```c
Lexer lex;
lexer_init(&lex, "2/(x+4)");
// Doit produire : TOK_NUM("2"), TOK_SLASH, TOK_LPAREN, TOK_IDENT("x"),
//                TOK_PLUS, TOK_NUM("4"), TOK_RPAREN, TOK_END
```

---

### Exercice 4.2 — Parser l'expression (descente récursive)

Implémente les fonctions de parsing mutuellement récursives :

```c
static MathExpr *parse_expression(Lexer *lex);
static MathExpr *parse_terme(Lexer *lex);
static MathExpr *parse_facteur(Lexer *lex);
static MathExpr *parse_primaire(Lexer *lex);
```

**`parse_expression`** :
```c
MathExpr *left = parse_terme(lex);
while (lexer_peek(lex).type == TOK_PLUS || lexer_peek(lex).type == TOK_MINUS) {
    Token op = lexer_next(lex);  // consomme + ou -
    MathExpr *right = parse_terme(lex);
    MathExpr *children[] = { left, right };
    left = math_expr_node(op.type == TOK_PLUS ? MATH_ADD : MATH_ADD, children, 2);
    // TODO : gérer la soustraction (MATH_ADD avec un enfant MATH_NEG)
}
return left;
```

**`parse_terme`** :
- Même structure, mais avec `TOK_STAR` et `TOK_SLASH`
- Pour `/`, crée un nœud `MATH_FRAC` au lieu de `MATH_MUL`

**`parse_facteur`** :
- Parse un `primaire`, puis si `^` suit, parse un autre `primaire` → `MATH_POW`

**`parse_primaire`** :
- `TOK_NUM` → `math_expr_atom(token.text)`
- `TOK_IDENT` suivi de `(` → appel de fonction (voir exercice 4.3)
- `TOK_IDENT` seul → `math_expr_atom(token.text)`
- `(` expression `)` → retourne l'expression intérieure
- `-` primaire → `MATH_NEG`

> [!WARNING]
> N'oublie pas de **consommer** (appeler `lexer_next`) les tokens que tu lis, mais de **ne pas consommer** ceux que tu ne veux pas (utilise `lexer_peek` pour regarder sans avancer).

---

### Exercice 4.3 — Parser les fonctions spéciales

Dans `parse_primaire`, quand on voit `IDENTIFIANT "("` :

1. **`integrale`** : lit 3 arguments séparés par `;` → `MATH_INTEGRAL`
2. **`sqrt` / `racine`** : lit 1 argument → `MATH_SQRT`
3. **Tout autre nom** : lit les arguments séparés par `,` → `MATH_FUNC` avec `name = identifiant`

Pour lire les arguments :

```c
// Lit les arguments d'une fonction jusqu'au ) fermant
// Séparateur : sep (TOK_SEMI ou TOK_COMMA)
// Retourne le nombre d'arguments lus dans args[]
int parse_args(Lexer *lex, MathExpr **args, int max_args, TokenType sep);
```

**Test** :
```c
MathExpr *expr = math_parse("integrale(0;1;2/(x+4))");
math_expr_dump(expr, 0);
// Doit afficher :
// INTEGRAL
//   ATOM(0)
//   ATOM(1)
//   FRAC
//     ATOM(2)
//     ADD
//       ATOM(x)
//       ATOM(4)
MathBox *box = math_render(expr);
mathbox_print(box);
math_expr_free(expr);
mathbox_free(box);
```

---

### Exercice 4.4 — Point d'entrée public

Expose une seule fonction dans `math_parse.h` :

```c
// Parse une expression mathématique et retourne l'AST
// Retourne NULL en cas d'erreur de parsing
MathExpr *math_parse(const char *input);
```

Et dans `math_render.h`, expose :

```c
// Render une expression en une grille 2D
// L'appelant est propriétaire de la MathBox retournée (mathbox_free)
MathBox *math_render(const MathExpr *expr);
```

---

## Partie 5 — Intégration dans le terminal Turingine

### Exercice 5.1 — Utiliser math_render dans term_shell.c

Le terminal reconnaît déjà des commandes. L'idée : quand l'utilisateur entre une expression mathématique (qui commence par un chiffre ou contient un `=`), on l'affiche en mode rendu 2D.

Ajoute dans `term_shell.c` :

```c
#include "../../core/math_render/math_render.h"
#include "../../core/math_render/math_parse.h"

// Appel dans la boucle de traitement des commandes :
MathExpr *expr = math_parse(input_line);
if (expr != NULL) {
    MathBox *box = math_render(expr);
    mathbox_print(box);
    mathbox_free(box);
    math_expr_free(expr);
}
```

> [!NOTE]
> `mathbox_print` utilise `printf` — ça fonctionne dans le terminal sans modification, tant que le terminal supporte UTF-8. Le terminal Turingine l'affiche via sa couche `term_render.c` qui forward vers le framebuffer.

---

### Exercice 5.2 — Mode ligne / mode rendu

Ajoute une commande de bascule dans le terminal :

- **`:render`** ou touche dédiée — bascule entre le mode "affichage ligne" (`2/(x+4)`) et le mode "rendu 2D"
- Garde l'état dans une variable globale `int render_mode = 0` dans `term_shell.c`
- En mode rendu, l'expression est passée à `math_render` et affichée en 2D
- En mode ligne, elle est affichée telle quelle

---

### Exercice 5.3 — Makefile

Ajoute les fichiers du module à la compilation. Dans le `Makefile` du terminal (ou le `Makefile` racine), ajoute :

```makefile
MATH_OBJS = core/math_render/math_expr.o \
            core/math_render/math_parse.o \
            core/math_render/math_render.o
```

Et inclus `$(MATH_OBJS)` dans la cible `terminal`.

**Compilation** :
```bash
make terminal
```

---

## Partie 6 — Extensions (bonus)

### Exercice 6.1 — Multiplication implicite

En mathématiques, `2x` signifie `2 × x`. Dans le parser, si deux facteurs se suivent sans opérateur, c'est une multiplication implicite.

Modifie `parse_terme` pour détecter ce cas :
- Après un `primaire`, si le token suivant est un `IDENT` ou un `(` (et pas un opérateur), crée un nœud `MATH_MUL` implicite

**Exemple** : `"2x"` → `Mul(Atom("2"), Atom("x"))` → affiche `2x` (sans espace ni `×`)

---

### Exercice 6.2 — Parenthèses extensibles

Pour les expressions hautes (fractions dans un argument de fonction), les parenthèses `(` et `)` devraient s'étirer verticalement :

```
  ⎛  2  ⎞
  ⎜─────⎟
  ⎝x + 4⎠
```

Implémente une fonction :
```c
MathBox *mathbox_paren(const MathBox *inner);
```

qui entoure `inner` de parenthèses verticalement extensibles selon sa hauteur (en utilisant les caractères Unicode `⎛`, `⎜`, `⎝`, `⎞`, `⎟`, `⎠`).

---

### Exercice 6.3 — Rendu dans le Grapher

Le Grapher peut aussi bénéficier du rendu 2D pour **afficher les légendes des courbes** avec leur notation mathématique.

Modifie `apps/Grapher/graph.cpp` pour afficher le nom d'une `GiacFunction` en utilisant `math_parse` + `math_render` + un helper qui convertit le `MathBox` en pixels dans l'`Image` du grapheur (via `font8x16.h`).

---

## Récapitulatif des concepts appris

| Partie | Ce que tu as appris |
|--------|-------------------|
| 0 | Encodage UTF-8, cellules de caractères, types de symboles mathématiques |
| 1 | Structure MathBox, grille 2D, baseline, `blit`, `hcat` |
| 2 | AST : nœuds, récursion, gestion mémoire |
| 3 | Renderer récursif : atome, addition, fraction, puissance, intégrale, racine |
| 4 | Parser à descente récursive : lexer, grammaire, fonctions spéciales |
| 5 | Intégration dans le terminal Turingine, mode bascule, Makefile |
| 6 | Mul implicite, parenthèses extensibles, export vers le Grapher |

---

## Pour aller plus loin

- **Nombres mixtes** : `1 + 1/2` → affiche `1½` en une seule boîte
- **Sommes et produits** : `∑(k=0; n; k²)` avec `k=0` sous le sigma et `n` au-dessus
- **Matrices** : grille de boîtes avec crochets extensibles `⎡ ⎤`
- **Vecteurs** : flèche au-dessus d'une variable `→`
- **Dérivées** : notation de Leibniz `df/dx` ou de Lagrange `f'(x)`
- **Limites** : `lim` avec `x → a` en dessous
