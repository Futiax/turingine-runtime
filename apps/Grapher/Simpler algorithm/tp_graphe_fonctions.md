# TP — Affichage du graphe de fonctions en C++ avec Giac

> **Prérequis** : Connaître le C (pointeurs, `malloc`, structures, boucles).
> **Objectif** : Construire de zéro un moteur de tracé de courbes mathématiques antialiasé, piloté par le moteur de calcul formel **Giac**, prêt à brancher sur ta lib DRM.
> **Sortie visuelle** : On génère des images au format PPM (lisible par n'importe quel viewer) pour voir nos résultats immédiatement.

---

## Partie 0 — Le C++ quand on vient du C

### 0.1 — Compilation

En C tu fais `gcc main.c -o main -lm`. En C++ :
```bash
g++ -std=c++17 -O2 -Wall -Wextra main.cpp -o main
```
L'extension est `.cpp` (ou `.cc`). Le compilateur est `g++`. Le flag `-std=c++17` active les fonctionnalités modernes.

### 0.2 — `#include` : les headers changent de nom

```cpp
// En C :             En C++ :
#include <stdio.h>    // → #include <cstdio>
#include <math.h>     // → #include <cmath>
#include <string.h>   // → #include <cstring>
#include <stdint.h>   // → #include <cstdint>
```
Les fonctions sont les mêmes (`printf`, `sin`, `memcpy`...) mais placées dans le namespace `std::`. En pratique ça marche sans le `std::` pour les fonctions C.

### 0.3 — `std::vector` remplace `malloc` + `realloc`

En C, pour un tableau dynamique tu fais :
```c
int *tab = (int *)malloc(n * sizeof(int));
tab[0] = 42;
// ... plus tard :
free(tab);
```

En C++, tu utilises `std::vector` :
```cpp
#include <vector>

std::vector<int> tab(n);  // alloue n entiers, initialisés à 0
tab[0] = 42;
// Pas de free ! La mémoire est libérée automatiquement
// quand la variable sort du scope (accolade fermante)
```

Opérations utiles :
```cpp
std::vector<int> v;          // vecteur vide
v.push_back(10);             // ajoute 10 à la fin → v = {10}
v.push_back(20);             // v = {10, 20}
v.size();                    // → 2
v[0];                        // → 10
v.clear();                   // vide le vecteur
v.reserve(1000);             // pré-alloue de la place (évite les réallocations)

// Parcours :
for (int i = 0; i < (int)v.size(); i++) {
    printf("%d\n", v[i]);
}
// Ou en "range-based for" (syntaxe C++) :
for (int val : v) {
    printf("%d\n", val);
}
```

### 0.4 — `std::string` remplace `char[]`

```cpp
#include <string>

std::string s = "hello";
s += " world";               // concaténation → "hello world"
s.size();                     // → 11
s.c_str();                    // → const char* (pour printf, fopen, etc.)
printf("%s\n", s.c_str());
```

### 0.5 — Références `&` (alternative aux pointeurs)

En C, pour modifier une variable dans une fonction, tu passes un pointeur :
```c
void doubler(int *x) { *x = *x * 2; }
int a = 5;
doubler(&a);  // a vaut 10
```

En C++, tu peux utiliser une **référence** — c'est un alias vers la variable :
```cpp
void doubler(int& x) { x = x * 2; }  // pas de * ni de ->
int a = 5;
doubler(a);  // a vaut 10, pas besoin de &a à l'appel
```

> [!TIP]
> Une référence ne peut pas être `nullptr`. C'est plus sûr qu'un pointeur.
> Pour passer un gros objet en lecture seule sans le copier : `const std::vector<int>& v`.

### 0.6 — Structures avec méthodes (classes légères)

En C tu as des `struct` + des fonctions séparées :
```c
struct Point { double x, y; };
double distance(struct Point a, struct Point b) {
    return sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y));
}
```

En C++, tu peux coller les fonctions **dans** la structure :
```cpp
struct Point {
    double x, y;

    double distance_to(const Point& other) const {
        double dx = x - other.x;
        double dy = y - other.y;
        return std::sqrt(dx*dx + dy*dy);
    }
};

Point a{1.0, 2.0};   // initialisation directe (pas besoin de "struct Point")
Point b{4.0, 6.0};
double d = a.distance_to(b);  // → 5.0
```

> [!NOTE]
> Le mot-clé `const` après la parenthèse signifie : "cette méthode ne modifie pas l'objet".

### 0.7 — Constructeurs

Au lieu d'écrire une fonction `init()` comme en C, on utilise un **constructeur** :
```cpp
struct ScreenBuffer {
    int width, height;
    std::vector<uint32_t> pixels;

    // Constructeur : appelé automatiquement à la création
    ScreenBuffer(int w, int h)
        : width(w), height(h), pixels(w * h, 0xFF000000)
    {
        // Le corps peut rester vide si tout est dans la liste d'init
    }
};

// Utilisation :
ScreenBuffer ecran(480, 320);  // construit directement, pas de malloc
// ecran.pixels est déjà alloué et rempli de noir
```

### 0.8 — `auto` pour simplifier les déclarations

```cpp
auto x = 3.14;        // double
auto s = std::string("hello");
auto v = std::vector<int>{1, 2, 3};
```

### 0.9 — `std::function` et lambdas (fonctions mathématiques)

En C tu passes des pointeurs de fonctions :
```c
typedef double (*func_t)(double);
double evaluer(func_t f, double x) { return f(x); }
```

En C++, tu peux utiliser `std::function` et des **lambdas** (fonctions anonymes) :
```cpp
#include <functional>

double evaluer(std::function<double(double)> f, double x) {
    return f(x);
}

// Appel avec une lambda :
double y = evaluer([](double x) { return std::sin(x); }, 3.14);

// Ou stocker la lambda dans une variable :
auto ma_fonction = [](double x) { return x * x - 2.0; };
double y2 = evaluer(ma_fonction, 1.5);
```

> [!TIP]
> Une lambda `[](double x) { return ...; }` est comme une fonction inline sans nom.
> Les `[]` sont la *capture list* : si tu veux utiliser des variables extérieures dedans, tu les listes là.

---

### 0.10 — Giac : le moteur de calcul formel

**Giac** est le moteur de calcul formel derrière Xcas (la calculatrice formelle de Bernard Parisse, Université Grenoble Alpes). On va l'utiliser pour parser des expressions mathématiques entrées sous forme de texte (ex: `"sin(x^2)"`) et les évaluer numériquement.

#### Installation

```bash
sudo apt install libgiac-dev
```

Cela installe les headers et la bibliothèque. Pour compiler un programme qui utilise Giac :
```bash
g++ -std=c++17 -O2 graph.cpp -o graph -lgiac -lgmp
```

> [!NOTE]
> `-lgiac` linke la bibliothèque Giac. `-lgmp` linke GMP (arithmétique multi-précision), dont Giac dépend.
> Si tu obtiens des erreurs de linkage supplémentaires, ajoute `-lreadline -lncurses`.

#### Les bases de l'API C++

Tout tourne autour de **deux types** :

| Type | Rôle |
|------|------|
| `giac::gen` | Le type universel : peut contenir un nombre, une expression symbolique, une liste, etc. |
| `giac::context` | L'environnement d'évaluation (variables, modes, etc.) |

```cpp
#include <giac/giac.h>
using namespace giac;

context ctx;  // on en crée un au début du programme
```

#### Parser une expression

Pour transformer une chaîne de caractères en expression symbolique :
```cpp
gen expr("x^2 + sin(x)", &ctx);  // parse la chaîne → objet symbolique
```

#### Évaluer numériquement

Pour évaluer une expression en un point, on **substitue** la variable puis on évalue :
```cpp
gen x_var("x", &ctx);           // la variable x
gen valeur(2.5);                // le point en lequel on évalue

// Substitution : remplacer x par 2.5 dans l'expression
gen substitue = subst(expr, x_var, valeur, false, &ctx);

// Évaluation numérique flottante
gen resultat = evalf(substitue, 1, &ctx);

// Conversion en double C++
double y = resultat.DOUBLE_val();
```

> [!IMPORTANT]
> `evalf` renvoie un `gen` de type flottant. Pour le convertir en `double` natif C++, utilise `.DOUBLE_val()`.
> Vérifie le type avec `resultat.type == _DOUBLE_` avant de convertir (sinon ça peut crasher).

#### Fonctions symboliques utiles

Giac expose les fonctions Xcas en C++ avec un préfixe `_` :

| Xcas | C++ | Exemple |
|------|-----|---------|
| `diff(f, x)` | `_diff(makesequence(f, x), &ctx)` | Dérivée de f par rapport à x |
| `solve(f, x)` | `_solve(makesequence(f, x), &ctx)` | Résoudre f(x) = 0 |
| `limit(f, x, a)` | `_limit(makesequence(f, x, a), &ctx)` | Limite de f quand x → a |
| `denom(f)` | `_denom(f, &ctx)` | Dénominateur de f |
| `simplify(f)` | `_simplify(f, &ctx)` | Simplification |

`makesequence(a, b, c)` emballe plusieurs arguments dans un `gen` de type séquence — c'est comme ça qu'on passe plusieurs paramètres aux fonctions Giac.

#### Exercice 0.10 — Premier contact avec Giac

Crée un fichier `test_giac.cpp` :

1. Déclare un `context`
2. Parse l'expression `"x^2 + 3*x - 10"` en un `gen`
3. Évalue-la en `x = 2.0` et affiche le résultat (attendu : `0.0`)
4. Utilise `_diff` pour calculer sa dérivée par rapport à x et affiche-la (attendu : `2*x+3`)
5. Utilise `_solve` pour trouver les racines et affiche-les (attendu : `[-5, 2]`)

```bash
g++ -std=c++17 -O2 test_giac.cpp -o test_giac -lgiac -lgmp && ./test_giac
```

---

## Partie 1 — Le framebuffer logiciel et la sortie PPM

### Exercice 1.1 — Créer un buffer de pixels

Crée un fichier `graph.cpp`. L'objectif est de créer une image noire de 800×600 pixels et de la sauvegarder au format PPM.

Le format PPM est ultra-simple :
```
P6
800 600
255
<800×600×3 octets de pixels RGB>
```

**Code de départ** — la struct `Image` avec le constructeur et la sauvegarde PPM (le reste est à toi) :

```cpp
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Image {
    int width, height;
    std::vector<uint32_t> pixels;  // ARGB : 0xAARRGGBB

    Image(int w, int h)
        : width(w), height(h), pixels(w * h, 0xFF000000) {}

    // Sauvegarde au format PPM
    void save_ppm(const char* filename) const {
        FILE* f = fopen(filename, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (int i = 0; i < width * height; i++) {
            uint8_t r = (pixels[i] >> 16) & 0xFF;
            uint8_t g = (pixels[i] >>  8) & 0xFF;
            uint8_t b =  pixels[i]        & 0xFF;
            fwrite(&r, 1, 1, f);
            fwrite(&g, 1, 1, f);
            fwrite(&b, 1, 1, f);
        }
        fclose(f);
    }
};
```

**À faire** — implémente les méthodes suivantes dans `Image` :

1. **`void set_pixel(int x, int y, uint32_t color)`** — Écrit un pixel à la position `(x, y)`. L'index dans le vecteur est `y * width + x`. N'oublie pas de **vérifier les bornes** (`0 <= x < width` et `0 <= y < height`) pour ne pas écrire hors du buffer.

2. **`void clear(uint32_t color = 0xFF000000)`** — Remplit toute l'image avec une couleur. Indice : regarde `std::fill` de `<algorithm>`.

3. Dans `main`, crée une image `800×600`, dessine un pixel rouge (`0xFFFF0000`) au centre `(400, 300)`, sauvegarde avec `save_ppm("test.ppm")`, et ouvre l'image pour vérifier.

**Compilation et test** :
```bash
g++ -std=c++17 -O2 graph.cpp -o graph && ./graph
# Ouvre test.ppm avec un viewer d'images
```

---

### Exercice 1.2 — Rectangles pleins et contours

Ajoute deux méthodes à ta struct `Image` :

1. **`void fill_rect(int x, int y, int w, int h, uint32_t color)`** — Dessine un rectangle **plein** : parcours toutes les lignes de `y` à `y+h-1`, et pour chaque ligne tous les pixels de `x` à `x+w-1`, en appelant `set_pixel`.

2. **`void draw_rect(int x, int y, int w, int h, uint32_t color)`** — Dessine un rectangle **contour** (juste les 4 bords). Tu as besoin de tracer :
   - Ligne du haut : y fixe à `y`, x varie de `x` à `x+w-1`
   - Ligne du bas : y fixe à `y+h-1`, x varie
   - Ligne gauche : x fixe à `x`, y varie de `y` à `y+h-1`
   - Ligne droite : x fixe à `x+w-1`, y varie

**À faire** : Implémente les deux fonctions, puis dans `main` dessine un rectangle bleu plein et un rectangle rouge en contour. Sauvegarde et vérifie visuellement.

---

### Exercice 1.3 — Mélange alpha (blending)

Pour l'antialiasing, on a besoin de dessiner des pixels semi-transparents. Implémente cette méthode :

**`void blend_pixel(int x, int y, uint32_t color, uint8_t alpha)`**

Le principe : on mélange la couleur de premier plan (`color`) avec le pixel existant dans le buffer (`bg`) selon un facteur `alpha` :
- `alpha = 0` → totalement transparent (on ne voit que le fond)
- `alpha = 255` → totalement opaque (on ne voit que `color`)

**Formule pour chaque canal (R, G, B)** :
```
result = (fg * alpha + bg * (255 - alpha)) / 255
```

Étapes :
1. Vérifie les bornes (comme `set_pixel`)
2. Lis le pixel existant : `uint32_t bg = pixels[y * width + x]`
3. Décompose `color` et `bg` en composantes R, G, B (avec des décalages et masques `>> 16`, `& 0xFF`, etc.)
4. Applique la formule de blending sur chaque canal
5. Recombine : `0xFF000000 | (r << 16) | (g << 8) | b`

**À faire** : Implémente `blend_pixel`, puis teste en dessinant un carré rouge semi-transparent (`alpha = 128`) par-dessus un carré bleu.

---

## Partie 2 — Tracé de lignes

### Exercice 2.1 — Ligne naïve (DDA)

L'algorithme DDA (*Digital Differential Analyzer*) est le plus simple pour tracer une ligne entre deux points `(x0, y0)` et `(x1, y1)`.

**Principe** :
1. Calcule `dx = x1 - x0` et `dy = y1 - y0`
2. Le nombre de pas est `steps = max(|dx|, |dy|)` — on avance pixel par pixel sur l'axe dominant
3. L'incrément par pas est `x_inc = dx / steps` et `y_inc = dy / steps`
4. Partant de `(x0, y0)`, on avance de `(x_inc, y_inc)` à chaque pas et on allume le pixel le plus proche

**Signature** :
```cpp
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
```

> [!WARNING]
> **Cas dégénéré** : si `(x0, y0) == (x1, y1)`, alors `steps = 0` et la division `dx/steps` donne un NaN. Gère ce cas à part (dessine un seul pixel).

> [!TIP]
> Utilise `std::round()` pour arrondir les coordonnées flottantes au pixel le plus proche, plutôt qu'une troncature `(int)x` qui introduit un biais.

**À faire** :
1. Implémente `draw_line` dans `Image`
2. Dessine quelques lignes de différents angles dans `main`
3. Observe : les lignes diagonales ont un aspect "escalier" (aliasing). C'est normal !

> [!NOTE]
> Il faut `#include <cmath>` pour `std::round`, `std::abs(double)`, etc.
> Et `#include <algorithm>` pour `std::max`.

---

### Exercice 2.2 — Ligne antialiasée de Wu

L'algorithme de Wu résout le problème de l'aliasing. Au lieu de n'allumer qu'un seul pixel par colonne, on allume les **deux pixels adjacents** et on répartit l'intensité selon la distance fractionnaire.

**Principe visuel** :
```
   Courbe idéale passe ici
          ↓
   ┌───┬───┬───┐
   │   │ ░ │   │  ← pixel du dessus : intensité = partie fractionnaire
   ├───┼───┼───┤
   │   │ █ │   │  ← pixel du dessous : intensité = 1 - partie fractionnaire
   ├───┼───┼───┤
   │   │   │   │
   └───┴───┴───┘
```

**Signature** :
```cpp
void draw_line_wu(int x0, int y0, int x1, int y1, uint32_t color);
```

**Pseudo-code** :

```
Fonctions helper :
  ipart(x)  = floor(x)          → partie entière
  fpart(x)  = x - floor(x)      → partie fractionnaire  (ex: 3.7 → 0.7)
  rfpart(x) = 1 - fpart(x)      → complément            (ex: 3.7 → 0.3)

1. Déterminer si la pente est "forte" : steep = |y1-y0| > |x1-x0|
   Si steep : échanger les rôles de x et y (on transpose)
   → Le but est de toujours parcourir sur l'axe où la variation est la plus grande

2. S'assurer de parcourir de gauche à droite :
   Si x0 > x1 : échanger (x0,y0) et (x1,y1)

3. Calculer le gradient :
   gradient = dy / dx  (attention si dx == 0, mettre gradient = 1.0)

4. Initialiser y_inter = y0  (la position y "exacte" en flottant)

5. Pour chaque x de x0 à x1 :
   a. Calculer les deux pixels à allumer :
      - pixel_y     = ipart(y_inter)
      - pixel_y + 1 = le pixel juste en dessous
   b. Intensités :
      - pixel du dessus : rfpart(y_inter) * 255
      - pixel du dessous : fpart(y_inter) * 255
   c. Si steep : les coordonnées sont transposées,
      donc blend_pixel(pixel_y, x, ...) au lieu de blend_pixel(x, pixel_y, ...)
   d. y_inter += gradient
```

**À faire** :
1. Implémente `draw_line_wu` dans `Image` en suivant le pseudo-code
2. Dessine la **même ligne** avec `draw_line` (en rouge) et `draw_line_wu` (en blanc) côte à côte
3. Compare visuellement : la version Wu est beaucoup plus lisse !

---

## Partie 3 — Le système de coordonnées mathématiques

Jusqu'ici on travaille en **coordonnées pixel** (x de gauche à droite, y du haut vers le bas). Pour tracer des fonctions mathématiques, on a besoin d'un **viewport** qui mappe les coordonnées mathématiques vers les pixels.

### Exercice 3.1 — Le Viewport

Crée une struct `Viewport` avec les membres suivants :
- `x_min, x_max, y_min, y_max` (doubles) : la fenêtre mathématique visible
- `pixel_width, pixel_height` (ints) : les dimensions de l'image en pixels

Implémente les 4 fonctions de conversion :

**Math → Pixel** :
- `int math_to_pixel_x(double x)` : mappe `x_min → 0` et `x_max → pixel_width - 1`
  - Formule : `(x - x_min) / (x_max - x_min) * (pixel_width - 1)`
- `int math_to_pixel_y(double y)` : mappe `y_max → 0` (haut) et `y_min → pixel_height - 1` (bas)
  - Formule : `(y_max - y) / (y_max - y_min) * (pixel_height - 1)`

**Pixel → Math** (l'inverse) :
- `double pixel_to_math_x(int px)` : mappe `0 → x_min` et `pixel_width - 1 → x_max`
- `double pixel_to_math_y(int py)` : mappe `0 → y_max` et `pixel_height - 1 → y_min`

> [!IMPORTANT]
> L'inversion de l'axe Y est **le piège classique** de tout rendu 2D.
> En maths : y monte vers le haut. En pixels : y descend vers le bas.
> `math_to_pixel_y` doit inverser.

---

### Exercice 3.2 — Dessiner les axes

Écris une fonction libre (pas une méthode de `Image`) :

```cpp
void draw_axes(Image& img, const Viewport& vp, uint32_t color);
```

Elle doit :
1. **Axe X** (la droite y = 0) : si `y = 0` est visible dans le viewport (`y_min <= 0 <= y_max`), dessine une ligne horizontale sur toute la largeur à la hauteur `math_to_pixel_y(0.0)`
2. **Axe Y** (la droite x = 0) : si `x = 0` est visible, dessine une ligne verticale sur toute la hauteur à la colonne `math_to_pixel_x(0.0)`

**À faire** :
1. Implémente la fonction
2. Crée un viewport de `-10` à `+10` en x et y, dessine les axes en gris foncé (`0xFF444444`)
3. Sauvegarde et vérifie que les axes se croisent bien au centre

---

### Exercice 3.3 — Grille et graduations

Écris une fonction :

```cpp
void draw_grid(Image& img, const Viewport& vp, double step, uint32_t color);
```

Elle dessine des lignes verticales et horizontales espacées de `step` unités mathématiques. Par exemple avec `step = 1.0`, tu as une ligne à x = -10, -9, ..., 9, 10 et pareil pour y.

**Indices** :
- Pour les lignes verticales, itère `x` de `ceil(x_min / step) * step` jusqu'à `x_max` par pas de `step`
- Convertis chaque `x` en pixel avec `math_to_pixel_x`, puis dessine la colonne entière
- Utilise `blend_pixel` avec un alpha faible (ex: 40) pour des lignes très subtiles
- Fais pareil pour les lignes horizontales

**À faire** :
1. Implémente la fonction
2. Dessine la grille avec `step = 1.0` en gris très transparent
3. Dessine les axes par-dessus (ils doivent être plus visibles que la grille)

---

## Partie 4 — Tracé de fonctions avec Giac

On quitte les fonctions lambdas C++ pour utiliser **Giac** comme moteur d'évaluation. L'utilisateur pourra entrer des expressions comme `"sin(x)"`, `"x^2 - 3"`, `"tan(x)"` sous forme de texte.

### Exercice 4.1 — Le wrapper GiacFunction

Crée une struct qui encapsule une expression Giac et permet de l'évaluer en un point :

```cpp
#include <giac/giac.h>
using namespace giac;

struct GiacFunction {
    gen expression;  // l'expression symbolique (ex: sin(x^2))
    gen x_var;       // la variable x
    context* ctx;    // le contexte Giac
    std::string name; // le nom affiché (ex: "sin(x)")
};
```

**À faire** — implémente :

1. **Un constructeur** `GiacFunction(const std::string& expr_str, context* c)` qui :
   - Stocke le contexte
   - Parse `expr_str` en un `gen` avec `gen(expr_str, c)`
   - Crée la variable x avec `gen("x", c)`
   - Stocke le nom (= `expr_str`)

2. **`double eval(double x_val)`** qui :
   - Crée un `gen` à partir de `x_val`
   - Substitue x par cette valeur dans l'expression : `subst(expression, x_var, gen_val, false, ctx)`
   - Évalue numériquement : `evalf(result, 1, ctx)`
   - Vérifie que le résultat est bien un `_DOUBLE_` (via `result.type == _DOUBLE_`)
   - Retourne `result.DOUBLE_val()`, ou `NAN` si le type n'est pas bon

> [!WARNING]
> Giac peut lever des exceptions (ex: `log(-1)` en mode réel). Encadre l'évaluation dans un `try/catch` et retourne `NAN` en cas d'erreur.

**Test** : dans `main`, crée un `GiacFunction("x^2 - 4", &ctx)` et évalue-le en `x = 3.0` (attendu : `5.0`), `x = 2.0` (attendu : `0.0`), `x = 0.0` (attendu : `-4.0`).

**Compilation** :
```bash
g++ -std=c++17 -O2 graph.cpp -o graph -lgiac -lgmp && ./graph
```

---

### Exercice 4.2 — Tracé point par point

Écris une fonction :

```cpp
void plot_function_points(Image& img, const Viewport& vp,
                          GiacFunction& f, uint32_t color);
```

Pour chaque colonne de pixels (`px` de 0 à `width - 1`) :
1. Convertis `px` en coordonnée mathématique x via `pixel_to_math_x`
2. Évalue `f.eval(x)` pour obtenir y
3. Vérifie que y est fini (`std::isfinite(y)`)
4. Convertis y en coordonnée pixel via `math_to_pixel_y`
5. Appelle `set_pixel`

**À faire** :
1. Implémente la fonction
2. Teste avec `"sin(x)"`, viewport `[-2*pi, 2*pi] × [-2, 2]`
3. Observe : c'est une suite de points isolés ! Il manque les segments entre eux

---

### Exercice 4.3 — Segments entre les points successifs

Écris une fonction :

```cpp
void plot_function_lines(Image& img, const Viewport& vp,
                         GiacFunction& f, uint32_t color);
```

Même principe que 4.2, mais au lieu de poser un point isolé, tu **relies chaque point au point précédent** avec `draw_line_wu`.

**Indices** :
- Garde en mémoire `prev_px`, `prev_py` et un booléen `first`
- Si `y` n'est pas fini, reset `first = true` (discontinuité : on recommence sans relier)
- Sinon, si ce n'est pas le premier point, trace un segment Wu de `(prev_px, prev_py)` à `(px, py)`

**À faire** :
1. Implémente la fonction
2. Teste avec `"sin(x)"` — magnifique !
3. Teste avec `"tan(x)"` sur `[-2*pi, 2*pi]` — **problème** : il y a des lignes verticales parasites aux asymptotes !

---

## Partie 5 — Gestion des discontinuités

### Exercice 5.1 — Détection par saut vertical (heuristique)

Le problème de `tan(x)` : entre deux points voisins, la fonction passe de $+\infty$ à $-\infty$. Le segment reliant ces deux points traverse tout l'écran verticalement.

**Solution simple** : si le saut vertical entre deux points consécutifs dépasse un seuil (par exemple `height / 3` pixels), on ne trace pas le segment.

Écris une fonction :

```cpp
void plot_function_safe(Image& img, const Viewport& vp,
                        GiacFunction& f, uint32_t color);
```

C'est `plot_function_lines` avec un test supplémentaire avant de tracer chaque segment.

**À faire** :
1. Implémente la détection par seuil
2. Teste avec `"tan(x)"` — les lignes parasites doivent disparaître
3. Teste avec `"1/x"` sur `[-5, 5]`

---

### Exercice 5.2 — Détection symbolique avec Giac

L'heuristique par seuil est brutale et peut rater des cas subtils. Giac peut faire **beaucoup mieux** en analysant l'expression symboliquement.

**Principe** : extraire le **dénominateur** de l'expression, puis résoudre `dénominateur = 0` pour trouver les points de discontinuité **avant** de tracer.

Écris une fonction :

```cpp
std::vector<double> find_discontinuities(GiacFunction& f);
```

Étapes :
1. Utilise `_denom(f.expression, f.ctx)` pour extraire le dénominateur de l'expression
2. Utilise `_solve(makesequence(denominateur, f.x_var), f.ctx)` pour trouver les zéros du dénominateur
3. Le résultat est un `gen` de type vecteur — parcours-le et convertis chaque élément en `double` avec `evalf` + `.DOUBLE_val()`
4. Retourne le vecteur de points de discontinuité

Puis modifie ta fonction de tracé pour utiliser cette information :

```cpp
void plot_function_smart(Image& img, const Viewport& vp,
                         GiacFunction& f, uint32_t color);
```

Avant de tracer un segment entre deux points `x_prev` et `x_curr`, vérifie qu'**aucune discontinuité** ne se trouve dans l'intervalle `[x_prev, x_curr]`. Si oui, ne trace pas ce segment.

**À faire** :
1. Implémente `find_discontinuities`
2. Teste avec `"tan(x)"` — les discontinuités sont à `±π/2, ±3π/2, ...`
3. Teste avec `"1/(x^2-1)"` — discontinuités à `x = ±1`
4. Teste avec `"1/x"` — discontinuité à `x = 0`
5. Compare avec l'heuristique de 5.1 : la détection symbolique est parfaite !

> [!TIP]
> Giac retourne parfois les solutions sous forme exacte (ex: `pi/2` au lieu de `1.5707...`). Utilise `evalf` pour les convertir en flottant avant la comparaison.

> [!NOTE]
> Cette approche ne gère pas les fonctions non-rationnelles qui ont des discontinuités (ex: `floor(x)`). Pour celles-là, le seuil heuristique de 5.1 reste un filet de sécurité utile — combine les deux !

---

## Partie 6 — Échantillonnage adaptatif

### Le problème

L'approche "un point par pixel" fonctionne bien pour les courbes douces, mais elle peut **rater** des oscillations rapides. Par exemple, `"sin(50*x)"` a une fréquence bien plus élevée que la résolution de l'écran sur un viewport large — on perd complètement la forme de la courbe.

### Exercice 6.1 — Subdivision adaptative

L'idée : au lieu d'échantillonner uniformément, on subdivise récursivement les intervalles où la courbe n'est pas assez "droite".

**Critère de subdivision** : si le point milieu $f\left(\frac{a+b}{2}\right)$ est trop loin du milieu du segment $\frac{f(a)+f(b)}{2}$ (en pixels), on subdivise.

Écris une fonction récursive :

```cpp
struct SamplePoint {
    double x, y;
};

void adaptive_sample(GiacFunction& f,
                     double a, double ya,
                     double b, double yb,
                     std::vector<SamplePoint>& out,
                     const Viewport& vp,
                     int depth,
                     int max_depth = 12);
```

**Pseudo-code** :

```
1. Calculer mid_x = (a + b) / 2
2. Évaluer mid_y = f(mid_x)
3. Calculer le point attendu par interpolation linéaire : expected_y = (ya + yb) / 2
4. Convertir mid_y et expected_y en pixels, calculer l'erreur en pixels
5. Décider de subdiviser si :
   - depth < max_depth
   - ET (erreur > 1 pixel OU mid_y/ya/yb pas fini)
6. Si on subdivise :
   - adaptive_sample(f, a, ya, mid_x, mid_y, ...)
   - adaptive_sample(f, mid_x, mid_y, b, yb, ...)
7. Sinon :
   - out.push_back({mid_x, mid_y})
```

Puis une fonction de tracé qui utilise l'échantillonnage adaptatif :

```cpp
void plot_function_adaptive(Image& img, const Viewport& vp,
                            GiacFunction& f, uint32_t color);
```

Cette fonction doit :
1. Découper le viewport en N segments grossiers (ex: 64)
2. Pour chaque segment, appeler `adaptive_sample` pour raffiner
3. Tracer les segments Wu entre les points résultants (en appliquant la détection de discontinuité de la partie 5)

**À faire** :
1. Implémente `adaptive_sample` et `plot_function_adaptive`
2. Teste avec `"sin(x)"` — le résultat doit être identique à avant
3. Teste avec `"sin(50*x)"` sur `[-pi, pi]` — l'échantillonnage adaptatif brille !
4. Teste avec `"x*sin(1/x)"` sur `[-1, 1]` — la courbe oscille furieusement près de x=0

---

## Partie 7 — Mise en couleur et multi-fonctions

### Exercice 7.1 — Palette de couleurs

Définis une palette de couleurs pour les courbes :
```cpp
const uint32_t PALETTE[] = {
    0xFF4FC3F7,  // bleu clair
    0xFFFF7043,  // orange
    0xFF66BB6A,  // vert
    0xFFAB47BC,  // violet
    0xFFFFCA28,  // jaune
    0xFFEF5350,  // rouge
    0xFF26C6DA,  // cyan
    0xFFEC407A,  // rose
};
const int PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);
```

### Exercice 7.2 — Tracé multi-fonctions depuis des chaînes

Écris une fonction :

```cpp
void plot_multiple(Image& img, const Viewport& vp,
                   const std::vector<std::string>& expressions,
                   context& ctx);
```

Elle doit :
1. Effacer l'image avec un fond sombre (ex: `0xFF1A1A2E`)
2. Dessiner la grille et les axes
3. Pour chaque chaîne du vecteur, créer un `GiacFunction`, et le tracer avec `plot_function_adaptive` en utilisant une couleur de la palette

**À faire** :
1. Trace simultanément `"sin(x)"`, `"cos(x)"`, et `"sin(x)+cos(x)"` sur `[-2*pi, 2*pi]`
2. Chaque courbe doit avoir sa propre couleur

---

## Partie 8 — Épaisseur de trait

Les lignes d'un pixel d'épaisseur sont fines et difficiles à voir. On veut pouvoir dessiner des traits plus épais.

### Exercice 8.1 — Ligne épaisse par décalage

**Principe** : pour une ligne d'épaisseur `t`, on trace `t` lignes Wu **parallèles**, décalées dans la direction **perpendiculaire** au segment.

**Signature** :
```cpp
void draw_line_thick(Image& img, int x0, int y0, int x1, int y1,
                     uint32_t color, int thickness);
```

**Indices** :
1. Calcule le vecteur directeur du segment : `(dx, dy)` et sa longueur `len`
2. Le vecteur **perpendiculaire normalisé** est `(-dy/len, dx/len)`
3. Pour chaque offset de `-thickness/2` à `+thickness/2`, décale les extrémités du segment dans la direction perpendiculaire et trace une ligne Wu

> [!TIP]
> Si `thickness <= 1`, trace simplement une ligne Wu normale (pas besoin de calculer la perpendiculaire).

**À faire** :
1. Implémente la fonction
2. Modifie `plot_function_adaptive` pour accepter un paramètre `thickness`
3. Teste avec une épaisseur de 3 pixels — les courbes ressortent beaucoup mieux

---

## Partie 9 — Le programme final

### Exercice 9.1 — Assemblage complet

Assemble tout ce que tu as codé dans un `main()` qui trace plusieurs fonctions intéressantes :

Suggestions : `"sin(x)"`, `"cos(x)"`, `"tan(x)"`, `"sin(x^2)"`, `"1/x"`

Sur un viewport `[-2*pi, 2*pi] × [-3, 3]`, avec fond sombre, grille, axes, et détection de discontinuités.

---

### Exercice 9.2 — Dérivée symbolique (bonus Giac)

Utilise `_diff` de Giac pour tracer automatiquement une fonction **et sa dérivée** :

1. Parse l'expression `"sin(x^2)"` en un `gen`
2. Calcule sa dérivée : `gen derivee = _diff(makesequence(expr, x_var), &ctx)`
3. Crée deux `GiacFunction` : l'originale et la dérivée
4. Trace les deux sur le même graphe avec des couleurs différentes

---

### Exercice 9.3 — Saisie interactive

L'utilisateur doit pouvoir **taper ses propres fonctions** au clavier !

Écris une boucle interactive dans `main` :
1. Affiche un prompt : `"f(x) = "`
2. Lis une ligne depuis `stdin` (avec `std::getline` et `std::string`)
3. Si l'utilisateur tape `"quit"` ou une ligne vide, quitte
4. Parse l'expression avec `GiacFunction`
5. Trace le graphe et sauvegarde l'image
6. Recommence

**Exemple de session** :
```
f(x) = sin(x)
→ Image sauvegardée : graphe.ppm

f(x) = tan(x)
→ Image sauvegardée : graphe.ppm
→ Discontinuités détectées : x = -4.712, -1.571, 1.571, 4.712

f(x) = x^3 - 3*x + 1
→ Image sauvegardée : graphe.ppm

f(x) = quit
```

> [!TIP]
> Tu peux accumuler les fonctions dans un vecteur pour toutes les afficher en même temps, et ajouter une commande `"clear"` pour repartir de zéro.

---

### Exercice 9.4 — Branchement sur ta lib DRM (bonus pour la calculatrice)

Quand tu es prêt à porter ça sur la Turingine, le changement est minimal.
Remplace `save_ppm` par l'envoi vers ta lib :

```cpp
extern "C" {
    #include "drm_display.h"
}

int main() {
    struct drm_display drm;
    uint32_t scr_w, scr_h, stride;

    if (drm_display_init(&drm, &scr_w, &scr_h, &stride) != 0) {
        fprintf(stderr, "Erreur DRM\n");
        return 1;
    }

    Image img(scr_w, scr_h);
    Viewport vp(-2 * M_PI, 2 * M_PI, -3.0, 3.0, scr_w, scr_h);

    // ... même code de tracé ...

    // Envoi sur l'écran
    drm_display_blit_argb32(&drm, img.pixels.data(), scr_w);
    drm_display_present(&drm);

    // Attente d'une touche ou d'un signal
    getchar();

    drm_display_shutdown(&drm);
    return 0;
}
```

> [!TIP]
> `img.pixels.data()` retourne un `const uint32_t*` — c'est exactement ce que `drm_display_blit_argb32` attend.
> Ton `std::vector` est directement compatible avec ta lib C, sans copie supplémentaire.

---

## Partie 10 — Extension : Zoom, Déplacement et Interface de Saisie (UI)

Cette partie bonus te guide pour transformer ton grapheur statique en une véritable application interactive. Tu vas utiliser `evdev_input` pour :
1. **Zoomer** et **te déplacer** dans le plan mathématique en temps réel avec les touches.
2. **Saisir des fonctions au clavier** directement sur la calculatrice avec un champ d'édition de texte à l'écran, sans jamais devoir recompiler le code.

---

### Exercice 10.1 — Rendre le Viewport Dynamique

Pour zoomer ou déplacer la caméra, il suffit de modifier les limites mathématiques (`x_min`, `x_max`, `y_min`, `y_max`) du `Viewport` et de redessiner l'image.

Ajoute ces méthodes à ta structure `Viewport` (ou implémente-les sous forme de fonctions libres dans ton code) :

```cpp
// 1. Déplacement (Pan)
void pan(double dx, double dy) {
    x_min += dx;
    x_max += dx;
    y_min += dy;
    y_max += dy;
}

// 2. Zoom uniforme autour du centre de l'écran
void zoom(double factor) {
    double x_center = (x_min + x_max) / 2.0;
    double y_center = (y_min + y_max) / 2.0;
    double x_half_range = (x_max - x_min) * factor / 2.0;
    double y_half_range = (y_max - y_min) * factor / 2.0;

    x_min = x_center - x_half_range;
    x_max = x_center + x_half_range;
    y_min = y_center - y_half_range;
    y_max = y_center + y_half_range;
}

// 3. Étirement (Stretch) horizontal ou vertical indépendant
void stretch_x(double factor) {
    double x_center = (x_min + x_max) / 2.0;
    double x_half_range = (x_max - x_min) * factor / 2.0;
    x_min = x_center - x_half_range;
    x_max = x_center + x_half_range;
}

void stretch_y(double factor) {
    double y_center = (y_min + y_max) / 2.0;
    double y_half_range = (y_max - y_min) * factor / 2.0;
    y_min = y_center - y_half_range;
    y_max = y_center + y_half_range;
}
```

**À faire** :
* Ajoute ces fonctions et valide leur comportement en effectuant un déplacement initial ou un zoom dans ton `main()` avant le tracé.

---

### Exercice 10.2 — Piloter la vue au clavier (evdev)

Tu vas maintenant écouter les touches du clavier de la calculatrice via `evdev_input` dans ta boucle principale.

1. **Flèches directionnelles** (`KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`) : déplacer la vue (pan).
2. **Touches `+` / `-`** (ou `KEY_KPPLUS`, `KEY_KPMINUS`) : zoomer (zoom in / zoom out).
3. **Touches `[` / `]`** (ou `KEY_LEFTBRACE`, `KEY_RIGHTBRACE`) : étirer / compresser l'axe horizontal X.

A chaque touche détectée :
1. Tu appliques la transformation au `Viewport`.
2. Tu demandes un rafraîchissement en recalculant tout le tracé (grille, axes, courbes).
3. Tu appelles `drm_display_blit_argb32` et `drm_display_present` pour rafraîchir l'écran.

> [!TIP]
> Calcule le pas de déplacement proportionnellement à la taille courante de la vue pour que le déplacement reste fluide et adapté, quelle que soit l'échelle :
> `double step_x = (vp.x_max - vp.x_min) * 0.1;`

---

### Exercice 10.3 — Saisie de fonction en temps réel (UI)

Pour éviter de recompiler, on va ajouter une zone de texte à l'écran (un bandeau noir en bas de la fenêtre) et écouter les caractères tapés.

Bonne nouvelle : nous avons directement complété notre bibliothèque `evdev_input` avec une fonction `evdev_code_to_ascii(int code, int shift)` qui gère la conversion des codes touches du noyau Linux (lettres, chiffres, opérateurs et le modificateur `Shift`) en caractères ASCII.

Il suffit donc d'utiliser la logique de saisie suivante :
1. Déclare une chaîne `std::string input_buffer = "";` pour stocker la saisie courante.
2. Déclare un booléen `bool is_typing = false;` (la touche `KEY_TAB` bascule entre le mode "Navigation" et le mode "Saisie").
3. En mode **Saisie** :
   - Si la touche lue est `KEY_BACKSPACE`, supprime le dernier caractère : `if(!input_buffer.empty()) input_buffer.pop_back();`
   - Si la touche lue est `KEY_ENTER`, ajoute la fonction saisie à Giac :
     ```cpp
     if (!input_buffer.empty()) {
         functions.push_back({GiacFunction(input_buffer, &ctx), 2, true, PALETTE[functions.size() % 8], true, 0, false, 0});
         input_buffer.clear();
         is_typing = false; // Retour à la navigation
     }
     ```
   - Sinon, convertis la touche en ASCII avec `evdev_code_to_ascii(code, shift_pressed)` et ajoute le caractère non nul obtenu à `input_buffer`.

4. **Affichage à l'écran** :
   - Dessine un bandeau en bas de ton écran (ex: rectangle noir opaque de hauteur 40px couvrant toute la largeur).
   - Utilise ta fonction de dessin de texte pour écrire le buffer à l'écran (ex: `"Saisie f(x) : " + input_buffer`).

---

### Exercice 10.4 — Boucle interactive finale

Voici l'architecture simplifiée et recommandée pour ta boucle interactive dans `graph.cpp` :

```cpp
#include <unistd.h>
extern "C" {
#include "drm_display.h"
#include "evdev_input.h"
}

// ... Tes fonctions de tracé (plot_multiple, etc.) ...

int main() {
    struct drm_display drm;
    uint32_t scr_w, scr_h, stride;
    if (drm_display_init(&drm, &scr_w, &scr_h, &stride) != 0) return 1;

    Image img(scr_w, scr_h);
    Viewport vp(-10.0, 10.0, -10.0, 10.0, scr_w, scr_h);
    context ctx;

    std::vector<FancyFunction> functions;
    functions.push_back({GiacFunction("sin(x)", &ctx), 2, true, PALETTE[0], true, 0, false, 0});

    struct input_devices in_devs;
    input_devices_detect(&in_devs);

    std::string input_buffer = "";
    bool is_typing = false;
    bool shift_pressed = false;
    bool needs_redraw = true;

    while (true) {
        if (needs_redraw) {
            // 1. Dessine le graphique
            plot_multiple(img, vp, functions, ctx);

            // 2. Dessine le bandeau d'UI en bas (ex: hauteur 40 pixels)
            int ui_y = scr_h - 40;
            // draw_rect / fill_rect sur ton image...
            
            if (is_typing) {
                std::string ui_str = "Entree f(x) : " + input_buffer + "_";
                // draw_string(img, 10, ui_y + 12, ui_str, 0xFFFFFFFF);
            } else {
                std::string ui_str = "[TAB] Entrer fonction | [Esc] Quitter | Fleches: Naviguer | +/-: Zoom";
                // draw_string(img, 10, ui_y + 12, ui_str, 0xFF888888);
            }

            // 3. Envoie au framebuffer
            drm_display_blit_argb32(&drm, img.pixels.data(), scr_w);
            drm_display_present(&drm);
            needs_redraw = false;
        }

        int code, value;
        while (input_devices_read_kb(&in_devs, &code, &value) > 0) {
            // Détection shift (pour majuscules et opérateurs)
            if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
                shift_pressed = (value > 0);
            }

            if (value == 1) { // Touche enfoncée
                if (code == KEY_ESC) {
                    goto cleanup;
                }
                
                if (code == KEY_TAB) {
                    is_typing = !is_typing;
                    needs_redraw = true;
                }

                if (is_typing) {
                    if (code == KEY_BACKSPACE) {
                        if (!input_buffer.empty()) input_buffer.pop_back();
                        needs_redraw = true;
                    } else if (code == KEY_ENTER || code == KEY_KPENTER) {
                        if (!input_buffer.empty()) {
                            functions.push_back({GiacFunction(input_buffer, &ctx), 2, true, PALETTE[functions.size() % 8], true, 0, false, 0});
                            input_buffer.clear();
                            is_typing = false;
                        }
                        needs_redraw = true;
                    } else {
                        char c = evdev_code_to_ascii(code, shift_pressed);
                        if (c != 0) {
                            input_buffer += c;
                            needs_redraw = true;
                        }
                    }
                } else {
                    // Mode Navigation
                    double step_x = (vp.x_max - vp.x_min) * 0.1;
                    double step_y = (vp.y_max - vp.y_min) * 0.1;

                    if (code == KEY_LEFT) {
                        vp.pan(-step_x, 0);
                        needs_redraw = true;
                    } else if (code == KEY_RIGHT) {
                        vp.pan(step_x, 0);
                        needs_redraw = true;
                    } else if (code == KEY_UP) {
                        vp.pan(0, step_y);
                        needs_redraw = true;
                    } else if (code == KEY_DOWN) {
                        vp.pan(0, -step_y);
                        needs_redraw = true;
                    } else if (code == KEY_MINUS || code == KEY_KPMINUS) {
                        vp.zoom(1.1); // zoom arrière
                        needs_redraw = true;
                    } else if (code == KEY_EQUAL) {
                        vp.zoom(0.9); // zoom avant
                        needs_redraw = true;
                    }
                }
            }
        }
        usleep(10000);
    }

cleanup:
    input_devices_release(&in_devs);
    drm_display_shutdown(&drm);
    return 0;
}
```

---

## Récapitulatif des concepts appris

| Partie | Ce que tu as appris |
|--------|-------------------|
| 0 | C++ pour les développeurs C : vector, string, références, classes, lambdas, **Giac** |
| 1 | Framebuffer logiciel, format PPM, blending alpha |
| 2 | Tracé de lignes : DDA (naïf) puis Wu (antialiasé) |
| 3 | Viewport : mapping coordonnées mathématiques ↔ pixels |
| 4 | **Giac** : parsing d'expressions, éval numérique, wrapper GiacFunction |
| 5 | Détection des discontinuités : heuristique + **analyse symbolique Giac** |
| 6 | Échantillonnage adaptatif (subdivision récursive) |
| 7 | Multi-fonctions avec palette de couleurs |
| 8 | Épaisseur de trait |
| 9 | Assemblage, **dérivée symbolique**, **saisie interactive** + intégration DRM |
| 10 | **Zoom, déplacement interactif, gestion clavier evdev et saisie libre (UI)** |

---

## Pour aller plus loin

Quelques pistes pour améliorer ton moteur de tracé :

- **Légende des courbes** : Dessine le nom de chaque fonction avec sa couleur (il te faut un moteur de texte, comme ton `font8x16.h`)
- **Graduations numériques** : Affiche les valeurs `1`, `2`, `π`, etc. sur les axes
- **Intégrales** : Utilise `_integrate` de Giac pour calculer et colorier l'aire sous une courbe
- **Tangente en un point** : Utilise `_diff` pour calculer la pente en un point et tracer la droite tangente
- **Fonctions implicites** : Tracer $f(x,y) = 0$ en évaluant chaque pixel (marching squares)
