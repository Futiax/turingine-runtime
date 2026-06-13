#include "font8x16.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <giac/giac.h>
#include <giac/vecteur.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <memory>
#include <vector>

extern "C" {
#include "../../core/display/drm_display.h"
#include "evdev_input.h"
}

using namespace giac;

const uint32_t BG_COLOR = 0xFF1A1A2E;
const uint32_t AXIS_COLOR = 0xFF888888;
const uint32_t GRID_COLOR = 0xFFFFFFFF;

const uint32_t PALETTE[] = {
    0xFF4FC3F7, // bleu clair
    0xFFFF7043, // orange
    0xFF66BB6A, // vert
    0xFFAB47BC, // violet
    0xFFFFCA28, // jaune
    0xFFEF5350, // rouge
    0xFF26C6DA, // cyan
    0xFFEC407A, // rose
};

const int NBR_COLORS = sizeof(PALETTE) / sizeof(PALETTE[0]);

struct GiacFunction {
  gen expression;
  gen x_var;
  context *ctx;
  std::string name;

  mutable std::vector<double> _cached_disc;
  mutable bool _disc_valid = false;

  GiacFunction(const std::string &expr_str, context *c) : ctx(c), name(expr_str) {
    expression = giac::eval(gen(expr_str, ctx), ctx);
    x_var = gen("x", ctx);
  }

  GiacFunction(const gen &expr, const std::string &n, context *c) : expression(expr), ctx(c), name(n) { x_var = gen("x", ctx); }

  double eval(double x_val) {
    try {
      gen gen_val(x_val);
      gen result = subst(expression, x_var, gen_val, false, ctx);
      result = evalf(result, 1, ctx);
      if (result.type == _DOUBLE_) {
        return result.DOUBLE_val();
      }
      return NAN;
    } catch (...) {
      return NAN;
    }
  }

  void invalidate_discontinuity_cache() { _disc_valid = false; }

  std::vector<double> find_discontinuities() {
    if (_disc_valid)
      return _cached_disc;

    // 1. Réécrire tan → sin/cos pour que _denom expose tous les dénominateurs
    gen converted = expression;
    try {
      converted = _tan2sincos(expression, ctx);
    } catch (...) {
    }

    // 2. Résoudre denom = 0 symboliquement pour trouver les pôles
    gen denominator = _denom(converted, ctx);
    gen roots;
    try {
      roots = _solve(makesequence(denominator, x_var), ctx);
    } catch (...) {
      _disc_valid = true;
      return _cached_disc;
    }

    // 3. Convertir les racines symboliques en valeurs numériques et mettre en cache
    std::vector<double> discontinuities;
    if (roots.type == _VECT) {
      for (size_t i = 0; i < roots._VECTptr->size(); i++) {
        gen num_sol = evalf((*roots._VECTptr)[i], 1, ctx);
        if (num_sol.type == _DOUBLE_)
          discontinuities.push_back(num_sol.DOUBLE_val());
      }
    } else {
      gen num_sol = evalf(roots, 1, ctx);
      if (num_sol.type == _DOUBLE_)
        discontinuities.push_back(num_sol.DOUBLE_val());
    }

    _cached_disc = discontinuities;
    _disc_valid = true;
    return _cached_disc;
  }
};

struct Image {
  int width, height;
  uint32_t *pixels; // ARGB : 0xAARRGGBB
  Image(int w, int h) : width(w), height(h) {
    // Alloue SANS initialiser
    pixels = static_cast<uint32_t *>(std::malloc(w * h * sizeof(uint32_t)));
    if (!pixels) {
      width = 0;
      height = 0;
      throw std::bad_alloc();
    }
  }
  ~Image() { std::free(pixels); }
  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;
  Image(Image &&other) noexcept : width(other.width), height(other.height), pixels(other.pixels) { other.pixels = nullptr; }
  void set_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < width && y >= 0 && y < height) {
      pixels[y * width + x] = color;
    }
  }

  void clear(uint32_t color = 0xFF000000) {
    std::fill(pixels, pixels + width * height, color);
  }

  void save_ppm(const char *filename) const {
    FILE *f = fopen(filename, "wb");
    if (!f)
      return;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    uint8_t *buf = static_cast<uint8_t *>(std::malloc(width * height * 3));
    for (int i = 0; i < width * height; i++) {
      buf[i * 3 + 0] = (pixels[i] >> 16) & 0xFF;
      buf[i * 3 + 1] = (pixels[i] >> 8) & 0xFF;
      buf[i * 3 + 2] = pixels[i] & 0xFF;
    }
    fwrite(buf, 1, width * height * 3, f);
    std::free(buf);
    fclose(f);
  }

  void blend_pixel(int x, int y, uint32_t color, uint8_t alpha) {
    if (x < 0 || x >= width || y < 0 || y >= height)
      return;

    uint32_t bg = pixels[y * width + x];
    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;
    uint32_t bg_r = (bg >> 16) & 0xFF;
    uint32_t bg_g = (bg >> 8) & 0xFF;
    uint32_t bg_b = bg & 0xFF;
    uint32_t inv_alpha = 255 - alpha;
    uint32_t r = (fg_r * alpha + bg_r * inv_alpha + 128) >> 8;
    uint32_t g = (fg_g * alpha + bg_g * inv_alpha + 128) >> 8;
    uint32_t b = (fg_b * alpha + bg_b * inv_alpha + 128) >> 8;
    pixels[y * width + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
  }

  void fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h; row++)
      for (int col = x; col < x + w; col++)
        set_pixel(col, row, color);
  }

  void draw_char(int x, int y, unsigned char c, uint32_t color) {
    if (c > 127)
      c = '?';
    const uint8_t *glyph = &font8x16[(int)c * 16];
    for (int row = 0; row < 16; row++) {
      uint8_t bits = glyph[row];
      for (int col = 0; col < 8; col++) {
        if (bits & (0x80 >> col)) {
          for (int sy = 0; sy < 2; sy++) {
            for (int sx = 0; sx < 2; sx++) {
              set_pixel(x + col * 2 + sx, y + row * 2 + sy, color);
            }
          }
        }
      }
    }
  }

  void draw_string(int x, int y, const std::string &str, uint32_t color) {
    int char_w = 8 * 2;
    for (size_t i = 0; i < str.length(); i++) {
      draw_char(x + (int)i * char_w, y, str[i], color);
    }
  }

  void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) {
      set_pixel(x0, y0, color);
      return;
    }
    double x_inc = (double)dx / steps;
    double y_inc = (double)dy / steps;

    double x = x0;
    double y = y0;
    for (int i = 0; i <= steps; i++) {
      set_pixel((int)x, (int)y, color);
      x += x_inc;
      y += y_inc;
    }
  }

  void fill_circle(int cx, int cy, int radius, uint32_t color) {
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
      for (int dx = -radius; dx <= radius; dx++) {
        if (dx * dx + dy * dy <= r2) {
          set_pixel(cx + dx, cy + dy, color);
        }
      }
    }
  }

  void draw_line_thick(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    if (thickness <= 1) {
      draw_line_wu(x0, y0, x1, y1, color);
      return;
    }

    double half_t = thickness / 2.0;
    double ddx = x1 - x0;
    double ddy = y1 - y0;
    double len2 = ddx * ddx + ddy * ddy;

    if (len2 == 0) {
      fill_circle(x0, y0, (int)half_t, color);
      return;
    }

    // Bounding box du segment élargi de half_t de chaque côté
    int min_x = std::min(x0, x1) - (int)std::ceil(half_t);
    int max_x = std::max(x0, x1) + (int)std::ceil(half_t);
    int min_y = std::min(y0, y1) - (int)std::ceil(half_t);
    int max_y = std::max(y0, y1) + (int)std::ceil(half_t);
    double inner = half_t - 0.5;
    double outer = half_t + 0.5;
    double inner2 = inner * inner;
    double outer2 = outer * outer;

    // Pour chaque pixel : projeter sur le segment, mesurer la distance perpendiculaire → alpha
    for (int py = min_y; py <= max_y; py++) {
      for (int px = min_x; px <= max_x; px++) {
        double t = ((px - x0) * ddx + (py - y0) * ddy) / len2;
        t = std::max(0.0, std::min(1.0, t));

        double cx = x0 + t * ddx;
        double cy = y0 + t * ddy;
        double dist2 = (px - cx) * (px - cx) + (py - cy) * (py - cy);

        if (dist2 <= inner2) {
          set_pixel(px, py, color);
        } else if (dist2 <= outer2) {
          double dist = std::sqrt(dist2);
          uint8_t alpha = (uint8_t)((outer - dist) * 255);
          blend_pixel(px, py, color, alpha);
        }
      }
    }
  }

  void draw_rect(int x, int y, int w, int h, uint32_t color) {
    draw_line(x, y, x + w - 1, y, color);
    draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    draw_line(x, y, x, y + h - 1, color);
    draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
  }

  void draw_line_wu(int x0, int y0, int x1, int y1, uint32_t color) {
    auto ipart = [](double x) -> int { return (int)std::floor(x); };
    auto fpart = [](double x) -> double { return x - std::floor(x); };
    auto rfpart = [](double x) -> double { return 1.0 - (x - std::floor(x)); };

    // Normalisation : si la pente est > 1, on travaille dans l'espace transposé (steep)
    // puis on s'assure que x0 < x1 pour parcourir de gauche à droite
    bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }

    int dx = x1 - x0;
    int dy = y1 - y0;
    double gradient = (dx == 0) ? 1.0 : (double)dy / dx;

    // Tracé antialiasé : 2 pixels par colonne, alpha proportionnel à la fraction sub-pixel
    double y_inter = (double)y0;
    for (int x = x0; x <= x1; x++) {
      int y_base = ipart(y_inter);
      if (steep) {
        if (y_base >= 0 && y_base < width && x >= 0 && x < height)
          blend_pixel(y_base, x, color, (uint8_t)(rfpart(y_inter) * 255));
        if (y_base + 1 >= 0 && y_base + 1 < width && x >= 0 && x < height)
          blend_pixel(y_base + 1, x, color, (uint8_t)(fpart(y_inter) * 255));
      } else {
        if (x >= 0 && x < width && y_base >= 0 && y_base < height)
          blend_pixel(x, y_base, color, (uint8_t)(rfpart(y_inter) * 255));
        if (x >= 0 && x < width && y_base + 1 >= 0 && y_base + 1 < height)
          blend_pixel(x, y_base + 1, color, (uint8_t)(fpart(y_inter) * 255));
      }
      y_inter += gradient;
    }
  }
};

struct Viewport {
  double x_min, x_max;
  double y_min, y_max;
  int pixel_width, pixel_height;

  Viewport(double xmin, double xmax, double ymin, double ymax, int pw, int ph) : x_min(xmin), x_max(xmax), y_min(ymin), y_max(ymax), pixel_width(pw), pixel_height(ph) {}

  // Clamp à ±32767 : empêche l'overflow int32 dans clip_segment pour les fonctions non bornées
  int math_to_pixel_x(double x) const {
    double r = (x - x_min) / (x_max - x_min) * (pixel_width - 1);
    if (r >= 32767.0) return 32767;
    if (!(r > -32768.0)) return -32768; // catches NaN and -inf
    return (int)std::round(r);
  }
  int math_to_pixel_y(double y) const {
    double r = (y_max - y) / (y_max - y_min) * (pixel_height - 1);
    if (r >= 32767.0) return 32767;
    if (!(r > -32768.0)) return -32768;
    return (int)std::round(r);
  }

  double pixel_to_math_x(int px) const { return x_min + (double)px / (pixel_width - 1) * (x_max - x_min); }
  double pixel_to_math_y(int py) const { return y_max - (double)py / (pixel_height - 1) * (y_max - y_min); }

  void pan(double dx, double dy) {
    x_min += dx;
    x_max += dx;
    y_min += dy;
    y_max += dy;
  }

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
};

struct SamplePoint {
  double x, y;
};

struct FancyFunction {
  GiacFunction func;
  int thickness = 1;
  bool draw_f = true;
  uint32_t color_f;
  bool draw_f_prime = false;
  uint32_t color_f_prime;
  bool draw_f_double_prime = false;
  uint32_t color_f_double_prime;

  bool derivatives_computed = false;
  gen f_prime_expr;
  gen f_double_prime_expr;
  std::unique_ptr<GiacFunction> f_prime_func;
  std::unique_ptr<GiacFunction> f_pp_func;

  void ensure_derivatives() {
    if (derivatives_computed)
      return;
    if (draw_f_prime || draw_f_double_prime) {
      f_prime_expr = _diff(makesequence(func.expression, func.x_var), func.ctx);
      f_prime_func = std::make_unique<GiacFunction>(f_prime_expr, func.name + "'", func.ctx);
    }
    if (draw_f_double_prime) {
      f_double_prime_expr = _diff(makesequence(f_prime_expr, func.x_var), func.ctx);
      f_pp_func = std::make_unique<GiacFunction>(f_double_prime_expr, func.name + "''", func.ctx);
    }
    derivatives_computed = true;
  }
};

struct FrameStats {
  double clear_ms;
  double grid_ms;
  double axes_ms;
  double plot_ms;
  double total_ms;
};
void draw_axes(Image &img, const Viewport &vp, uint32_t color) {
  if (vp.y_min <= 0.0 && 0.0 <= vp.y_max) {
    int py = vp.math_to_pixel_y(0.0);
    img.draw_line(vp.math_to_pixel_x(vp.x_min), py, vp.math_to_pixel_x(vp.x_max), py, color);
  }
  if (vp.x_min <= 0.0 && 0.0 <= vp.x_max) {
    int px = vp.math_to_pixel_x(0.0);
    img.draw_line(px, vp.math_to_pixel_y(vp.y_min), px, vp.math_to_pixel_y(vp.y_max), color);
  }
}

double compute_grid_step(double range) {
  double raw_step = range / 8.0;
  double magnitude = std::pow(10, std::floor(std::log10(raw_step)));
  double normalized = raw_step / magnitude;
  double nice;
  if (normalized < 1.5)      nice = 1.0;
  else if (normalized < 3.5) nice = 2.0;
  else if (normalized < 7.5) nice = 5.0;
  else                       nice = 10.0;
  return nice * magnitude;
}

void draw_grid(Image &img, const Viewport &vp, double step, uint32_t color, uint8_t alpha = 40) {
  if (step <= 0.0) {
    return;
  }
  for (double x = std::ceil(vp.x_min / step) * step; x <= vp.x_max; x += step) {
    int px = vp.math_to_pixel_x(x);
    for (int py = 0; py < img.height; py++)
      img.blend_pixel(px, py, color, alpha);
  }
  for (double y = std::ceil(vp.y_min / step) * step; y <= vp.y_max; y += step) {
    int py = vp.math_to_pixel_y(y);
    for (int px = 0; px < img.width; px++)
      img.blend_pixel(px, py, color, alpha);
  }
}


/* ─────────────────────────────────────────────────────────────
 * Raffine récursivement un segment [a,b] jusqu'à max_depth si la
 * courbe s'écarte de la droite de plus d'un pixel.
 * ───────────────────────────────────────────────────────────── */
void adaptive_sample(GiacFunction &f, double a, double ya, double b, double yb, std::vector<SamplePoint> &out, const Viewport &vp, int depth, int max_depth = 12) {
  if (depth > max_depth)
    return;
  if (!std::isfinite(ya) && !std::isfinite(yb))
    return;

  double mid_x = (a + b) / 2.0;
  double mid_y = f.eval(mid_x);

  // Subdiviser si un bord est indéfini, ou si le milieu s'écarte de plus d'un pixel
  // de la droite linéaire attendue entre (a, ya) et (b, yb).
  bool must_subdivide = false;
  if (depth < max_depth) {
    if (!std::isfinite(mid_y) || !std::isfinite(ya) || !std::isfinite(yb)) {
      must_subdivide = true;
    } else {
      double expected_y = (ya + yb) / 2.0;
      int py_yc = vp.math_to_pixel_y(mid_y);
      int py_expected = vp.math_to_pixel_y(expected_y);
      if (std::abs(py_yc - py_expected) > 1)
        must_subdivide = true;
    }
  }

  if (must_subdivide) {
    adaptive_sample(f, a, ya, mid_x, mid_y, out, vp, depth + 1, max_depth);
    out.push_back({mid_x, mid_y});
    adaptive_sample(f, mid_x, mid_y, b, yb, out, vp, depth + 1, max_depth);
  }
}

/* ─────────────────────────────────────────────────────────────
 * Cohen-Sutherland avec int64 et limite d'itérations.
 * Retourne false si le segment est entièrement hors zone.
 * La limite de 32 iter évite le cycle infini sur les segments qui
 * passent hors du coin du clip rect sans jamais y entrer.
 * ───────────────────────────────────────────────────────────── */
static bool clip_segment(int &x0, int &y0, int &x1, int &y1, int xmin, int ymin, int xmax, int ymax) {
  auto code = [&](int64_t x, int64_t y) -> int {
    return ((x < xmin) ? 1 : 0) | ((x > xmax) ? 2 : 0) | ((y < ymin) ? 4 : 0) | ((y > ymax) ? 8 : 0);
  };
  int64_t X0 = x0, Y0 = y0, X1 = x1, Y1 = y1;
  int c0 = code(X0, Y0), c1 = code(X1, Y1);
  for (int iter = 0; iter < 32; ++iter) {
    if (!(c0 | c1)) { x0 = (int)X0; y0 = (int)Y0; x1 = (int)X1; y1 = (int)Y1; return true; }
    if (c0 & c1)    return false;
    int c = c0 ? c0 : c1;
    int64_t X = X0, Y = Y0;
    int64_t DX = X1 - X0, DY = Y1 - Y0;
    if      (c & 8) { X = X0 + (DY ? DX * (ymax - Y0) / DY : 0); Y = ymax; }
    else if (c & 4) { X = X0 + (DY ? DX * (ymin - Y0) / DY : 0); Y = ymin; }
    else if (c & 2) { Y = Y0 + (DX ? DY * (xmax - X0) / DX : 0); X = xmax; }
    else            { Y = Y0 + (DX ? DY * (xmin - X0) / DX : 0); X = xmin; }
    if (c == c0) { X0 = X; Y0 = Y; c0 = code(X0, Y0); }
    else         { X1 = X; Y1 = Y; c1 = code(X1, Y1); }
  }
  return false;
}

void plot_function_adaptive(Image &img, const Viewport &vp, GiacFunction &f, uint32_t color, int thickness) {
  // 1. Grille uniforme de départ
  int n_segments = std::min(img.width / 4, 42);
  std::vector<SamplePoint> initial;
  initial.reserve(n_segments + 1);
  for (int i = 0; i <= n_segments; i++) {
    double x = vp.pixel_to_math_x(i * img.width / (double)n_segments);
    initial.push_back({x, f.eval(x)});
  }

  // 2. Raffinage adaptatif entre chaque paire de points initiaux
  std::vector<SamplePoint> all_samples;
  all_samples.reserve(n_segments * 16);
  for (int i = 0; i < n_segments; i++) {
    all_samples.push_back(initial[i]);
    adaptive_sample(f, initial[i].x, initial[i].y, initial[i + 1].x, initial[i + 1].y, all_samples, vp, 0);
  }
  all_samples.push_back(initial.back());

  std::vector<double> discontinuities = f.find_discontinuities();

  // 3. Tracé des segments : skip aux discontinuités symboliques et aux sauts verticaux
  bool first = true;
  int prev_px = 0, prev_py = 0;
  double prev_x = 0;
  for (const auto &p : all_samples) {
    if (!std::isfinite(p.y)) {
      first = true;
      continue;
    }

    int px = vp.math_to_pixel_x(p.x);
    int py = vp.math_to_pixel_y(p.y);

    if (!first) {
      bool skip = false;
      for (double d : discontinuities) {
        if (d >= prev_x && d <= p.x) { skip = true; break; }
      }
      if (!skip && std::abs(py - prev_py) > img.height / 3)
        skip = true;
      if (!skip) {
        int cx0 = prev_px, cy0 = prev_py, cx1 = px, cy1 = py;
        int margin = thickness / 2 + 1;
        if (clip_segment(cx0, cy0, cx1, cy1, -margin, -margin, img.width + margin, img.height + margin))
          img.draw_line_thick(cx0, cy0, cx1, cy1, color, thickness);
      }
    } else {
      first = false;
    }

    prev_px = px;
    prev_py = py;
    prev_x = p.x;
  }
}

void plot_multiple(Image &img, const Viewport &vp, std::vector<FancyFunction> &expressions, context &ctx) {
  (void)ctx;
  img.clear(BG_COLOR);
  double step = compute_grid_step(vp.x_max - vp.x_min);
  draw_grid(img, vp, step / 5.0, GRID_COLOR, 15); // sous-graduations légères
  draw_grid(img, vp, step, GRID_COLOR, 40);       // graduations principales
  draw_axes(img, vp, AXIS_COLOR);

  for (auto &expr : expressions) {
    expr.ensure_derivatives();
    if (expr.draw_f)
      plot_function_adaptive(img, vp, expr.func, expr.color_f, expr.thickness);
    if (expr.draw_f_prime && expr.f_prime_func) {
      if (!expr.f_prime_func->_disc_valid && expr.func._disc_valid) {
        expr.f_prime_func->_cached_disc = expr.func._cached_disc;
        expr.f_prime_func->_disc_valid = true;
      }
      plot_function_adaptive(img, vp, *expr.f_prime_func, expr.color_f_prime, expr.thickness);
    }
    if (expr.draw_f_double_prime && expr.f_pp_func) {
      if (!expr.f_pp_func->_disc_valid && expr.func._disc_valid) {
        expr.f_pp_func->_cached_disc = expr.func._cached_disc;
        expr.f_pp_func->_disc_valid = true;
      }
      plot_function_adaptive(img, vp, *expr.f_pp_func, expr.color_f_double_prime, expr.thickness);
    }
  }
}

FrameStats plot_multiple_timed(Image &img, const Viewport &vp, std::vector<FancyFunction> &expressions, context &ctx) {
  (void)ctx;
  using clk = std::chrono::high_resolution_clock;
  FrameStats stats = {};

  auto t0 = clk::now();
  img.clear(BG_COLOR);
  auto t1 = clk::now();
  stats.clear_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  double step = compute_grid_step(vp.x_max - vp.x_min);
  draw_grid(img, vp, step / 5.0, GRID_COLOR, 15);
  draw_grid(img, vp, step, GRID_COLOR, 40);
  auto t2 = clk::now();
  stats.grid_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

  draw_axes(img, vp, AXIS_COLOR);
  auto t3 = clk::now();
  stats.axes_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

  for (auto &expr : expressions) {
    expr.ensure_derivatives();
    if (expr.draw_f)
      plot_function_adaptive(img, vp, expr.func, expr.color_f, expr.thickness);
    if (expr.draw_f_prime && expr.f_prime_func) {
      if (!expr.f_prime_func->_disc_valid && expr.func._disc_valid) {
        expr.f_prime_func->_cached_disc = expr.func._cached_disc;
        expr.f_prime_func->_disc_valid = true;
      }
      plot_function_adaptive(img, vp, *expr.f_prime_func, expr.color_f_prime, expr.thickness);
    }
    if (expr.draw_f_double_prime && expr.f_pp_func) {
      if (!expr.f_pp_func->_disc_valid && expr.func._disc_valid) {
        expr.f_pp_func->_cached_disc = expr.func._cached_disc;
        expr.f_pp_func->_disc_valid = true;
      }
      plot_function_adaptive(img, vp, *expr.f_pp_func, expr.color_f_double_prime, expr.thickness);
    }
  }
  auto t4 = clk::now();
  stats.plot_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
  stats.total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();

  return stats;
}

double get_process_cpu_time() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec * 1e-6 + (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec * 1e-6;
  }
  return 0.0;
}

long get_current_rss_kb() {
  FILE *file = fopen("/proc/self/statm", "r");
  if (!file)
    return 0;
  long pages = 0;
  // Le deuxième champ de /proc/self/statm est la taille résidente (RSS) en pages
  if (fscanf(file, "%*d %ld", &pages) != 1) {
    fclose(file);
    return 0;
  }
  fclose(file);
  static long page_size = -1;
  if (page_size == -1) {
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
      page_size = 4096;
  }
  return pages * (page_size / 1024);
}

void run_bench_interactive(Image &img, Viewport vp, std::vector<FancyFunction> &functions, context &ctx, struct drm_display &drm, uint32_t scr_w, uint32_t scr_h,
                           struct input_devices &in_devs, int duration_sec = 30) {
  using clk = std::chrono::high_resolution_clock;

  const int WARMUP_FRAMES = 5;

  Viewport vp_orig = vp;

  fprintf(stderr, "=== WARMUP (%d frames) ===\n", WARMUP_FRAMES);
  for (int i = 0; i < WARMUP_FRAMES; i++) {
    auto t_warmup = clk::now();
    plot_multiple_timed(img, vp, functions, ctx);
    double phase = i * 0.1;
    vp.x_min = vp_orig.x_min + std::sin(phase) * 2.0;
    vp.x_max = vp_orig.x_max + std::sin(phase) * 2.0;
    auto t_done = clk::now();
    double ms = std::chrono::duration<double, std::milli>(t_done - t_warmup).count();
    fprintf(stderr, "Warmup %d: %.1f ms\n", i + 1, ms);
  }
  vp = vp_orig;
  fprintf(stderr, "=== FIN WARMUP ===\n\n");

  fprintf(stderr, "=== BENCHMARK INTERACTIF (%ds) ===\n", duration_sec);
  fprintf(stderr, "Resolution: %ux%u | Fonctions: %zu\n", scr_w, scr_h, functions.size());
  fprintf(stderr, "%-8s  %-8s  %-8s  %-8s  %-8s  %-8s  %-8s  %-8s  %s\n", "Frame", "Total", "Clear", "Grid", "Axes", "Plot", "CPU%", "RAM(MB)", "FPS");
  fprintf(stderr, "---------------------------------------------------------------------------------------\n");

  struct FrameRecord {
    int frame;
    double total;
    double clear;
    double grid;
    double axes;
    double plot;
    double blit;
    double fps;
    double cpu_usage;
    long rss_kb;
  };
  std::vector<FrameRecord> records;
  records.reserve(duration_sec * 60 + 100);

  auto bench_start = clk::now();
  int frame_count = 0;
  double total_ms_sum = 0, clear_ms_sum = 0, grid_ms_sum = 0;
  double axes_ms_sum = 0, plot_ms_sum = 0, blit_ms_sum = 0;
  double min_frame_ms = 1e9, max_frame_ms = 0;
  double cpu_pct_sum = 0;
  long max_rss_kb = 0;
  double rss_kb_sum = 0;
  bool cancelled = false;

  while (true) {
    auto now = clk::now();
    double elapsed = std::chrono::duration<double>(now - bench_start).count();
    if (elapsed >= duration_sec)
      break;

    int code, value;
    while (input_devices_read_kb(&in_devs, &code, &value) > 0) {
      if (value == 1 && code == KEY_ESC) {
        cancelled = true;
        break;
      }
    }
    if (cancelled)
      break;

    // Varier le viewport pour simuler une interaction réelle (pan léger)
    double phase = elapsed * 0.3;
    vp.x_min = vp_orig.x_min + std::sin(phase) * 2.0;
    vp.x_max = vp_orig.x_max + std::sin(phase) * 2.0;

    // Rendu frame
    double cpu_start = get_process_cpu_time();
    auto t_frame = clk::now();
    FrameStats stats = plot_multiple_timed(img, vp, functions, ctx);

    // Barre de progression en bas de l'écran
    int ui_y = scr_h - 40;
    img.fill_rect(0, ui_y, scr_w, 40, 0xFF111111);
    char bench_str[128];
    snprintf(bench_str, sizeof(bench_str), "BENCH: frame %d | %.1f ms/frame | %.1f FPS | %.0fs restantes  [ESC] Annuler", frame_count, stats.total_ms,
             frame_count > 0 ? frame_count / elapsed : 0.0, duration_sec - elapsed);
    img.draw_string(10, ui_y + 4, bench_str, 0xFF4FC3F7);

    // Blit + mesures de temps
    auto t_blit = clk::now();
    drm_display_blit_argb32(&drm, img.pixels, scr_w);
    drm_display_present(&drm);
    auto t_end = clk::now();
    double cpu_end = get_process_cpu_time();
    double blit_ms = std::chrono::duration<double, std::milli>(t_end - t_blit).count();
    double frame_ms = std::chrono::duration<double, std::milli>(t_end - t_frame).count();

    double elapsed_sec = frame_ms * 1e-3;
    double cpu_used_sec = cpu_end - cpu_start;
    double cpu_pct = (elapsed_sec > 0.0) ? (cpu_used_sec / elapsed_sec) * 100.0 : 0.0;
    if (cpu_pct > 100.0)
      cpu_pct = 100.0; // single-threaded clamping

    long current_rss = get_current_rss_kb();

    // Comptabilisation + log stderr
    fprintf(stderr, "%-8d  %6.2f  %6.2f  %6.2f  %6.2f  %6.2f  %6.1f%%   %7.2f  %6.1f\n", frame_count, stats.total_ms + blit_ms, stats.clear_ms, stats.grid_ms,
            stats.axes_ms, stats.plot_ms, cpu_pct, current_rss / 1024.0, 1000.0 / frame_ms);

    total_ms_sum += frame_ms;
    clear_ms_sum += stats.clear_ms;
    grid_ms_sum += stats.grid_ms;
    axes_ms_sum += stats.axes_ms;
    plot_ms_sum += stats.plot_ms;
    blit_ms_sum += blit_ms;
    if (frame_ms < min_frame_ms)
      min_frame_ms = frame_ms;
    if (frame_ms > max_frame_ms)
      max_frame_ms = frame_ms;

    cpu_pct_sum += cpu_pct;
    if (current_rss > max_rss_kb)
      max_rss_kb = current_rss;
    rss_kb_sum += current_rss;

    records.push_back(
        {frame_count, stats.total_ms + blit_ms, stats.clear_ms, stats.grid_ms, stats.axes_ms, stats.plot_ms, blit_ms, 1000.0 / frame_ms, cpu_pct, current_rss});
    frame_count++;
  }

  vp = vp_orig;

  if (frame_count == 0)
    return;

  // Agr\u00e9gats \u2014 on exclut les WARMUP_FRAMES premi\u00e8res frames (cache froid)
  const int skip_frames = (frame_count > WARMUP_FRAMES) ? WARMUP_FRAMES : 0;
  int valid_frames = frame_count - skip_frames;
  if (valid_frames <= 0)
    valid_frames = frame_count;

  double total_ms_sum_valid = 0, clear_ms_sum_valid = 0, grid_ms_sum_valid = 0;
  double axes_ms_sum_valid = 0, plot_ms_sum_valid = 0, blit_ms_sum_valid = 0;
  double min_frame_ms_valid = 1e9, max_frame_ms_valid = 0;
  double cpu_pct_sum_valid = 0;
  long max_rss_kb_valid = 0;
  double rss_kb_sum_valid = 0;

  for (int i = skip_frames; i < frame_count; i++) {
    total_ms_sum_valid += records[i].total;
    clear_ms_sum_valid += records[i].clear;
    grid_ms_sum_valid += records[i].grid;
    axes_ms_sum_valid += records[i].axes;
    plot_ms_sum_valid += records[i].plot;
    blit_ms_sum_valid += records[i].blit;
    if (records[i].total < min_frame_ms_valid) min_frame_ms_valid = records[i].total;
    if (records[i].total > max_frame_ms_valid) max_frame_ms_valid = records[i].total;
    cpu_pct_sum_valid += records[i].cpu_usage;
    if (records[i].rss_kb > max_rss_kb_valid) max_rss_kb_valid = records[i].rss_kb;
    rss_kb_sum_valid += records[i].rss_kb;
  }

  double avg_fps = valid_frames * 1000.0 / total_ms_sum_valid;
  double avg_frame = total_ms_sum_valid / valid_frames;
  double avg_cpu = cpu_pct_sum_valid / valid_frames;
  double avg_rss_mb = (rss_kb_sum_valid / valid_frames) / 1024.0;
  double max_rss_mb = max_rss_kb_valid / 1024.0;

  // R\u00e9sum\u00e9 stderr
  fprintf(stderr, "\n=== RESULTATS (%d frames, %d warmup exclues%s) ===\n", valid_frames, skip_frames, cancelled ? ", annul\u00e9" : "");
  fprintf(stderr, "FPS moyen:     %.1f\n", avg_fps);
  fprintf(stderr, "FPS min:       %.1f\n", 1000.0 / max_frame_ms_valid);
  fprintf(stderr, "FPS max:       %.1f\n", 1000.0 / min_frame_ms_valid);
  fprintf(stderr, "Frame moyen:   %.2f ms\n", avg_frame);
  fprintf(stderr, "  - clear:     %.2f ms (%.0f%%)\n", clear_ms_sum_valid / valid_frames, clear_ms_sum_valid / total_ms_sum_valid * 100);
  fprintf(stderr, "  - grid:      %.2f ms (%.0f%%)\n", grid_ms_sum_valid / valid_frames, grid_ms_sum_valid / total_ms_sum_valid * 100);
  fprintf(stderr, "  - axes:      %.2f ms (%.0f%%)\n", axes_ms_sum_valid / valid_frames, axes_ms_sum_valid / total_ms_sum_valid * 100);
  fprintf(stderr, "  - plot:      %.2f ms (%.0f%%)\n", plot_ms_sum_valid / valid_frames, plot_ms_sum_valid / total_ms_sum_valid * 100);
  fprintf(stderr, "  - blit:      %.2f ms (%.0f%%)\n", blit_ms_sum_valid / valid_frames, blit_ms_sum_valid / total_ms_sum_valid * 100);
  fprintf(stderr, "Frame min:     %.2f ms\n", min_frame_ms_valid);
  fprintf(stderr, "Frame max:     %.2f ms\n", max_frame_ms_valid);
  fprintf(stderr, "CPU moyen:     %.1f%%\n", avg_cpu);
  fprintf(stderr, "RAM max:       %.2f MB (moyenne: %.2f MB)\n", max_rss_mb, avg_rss_mb);

  // Export CSV (toutes les frames, colonne Warmup pour les exclure côté analyse)
  FILE *fcsv = fopen("bench_results.csv", "w");
  if (fcsv) {
    fprintf(fcsv, "# BENCHMARK RESULTS%s\n", cancelled ? " (CANCELLED)" : "");
    fprintf(fcsv, "# Resolution: %ux%u\n", scr_w, scr_h);
    fprintf(fcsv, "# Functions:\n");
    for (const auto &fn : functions) {
      fprintf(fcsv, "#   - %s\n", fn.func.name.c_str());
    }
    fprintf(fcsv, "# Warmup frames excluded from summary: %d\n", skip_frames);
    fprintf(fcsv, "# FPS: avg=%.2f, min=%.2f, max=%.2f\n", avg_fps, 1000.0 / max_frame_ms_valid, 1000.0 / min_frame_ms_valid);
    fprintf(fcsv, "# Frame Time (ms): avg=%.2f, min=%.2f, max=%.2f\n", avg_frame, min_frame_ms_valid, max_frame_ms_valid);
    fprintf(fcsv, "# Phase Average (ms): clear=%.2f, grid=%.2f, axes=%.2f, plot=%.2f, blit=%.2f\n", clear_ms_sum_valid / valid_frames, grid_ms_sum_valid / valid_frames,
            axes_ms_sum_valid / valid_frames, plot_ms_sum_valid / valid_frames, blit_ms_sum_valid / valid_frames);
    fprintf(fcsv, "# CPU: avg=%.1f%%\n", avg_cpu);
    fprintf(fcsv, "# RAM: avg=%.2f MB, max=%.2f MB\n", avg_rss_mb, max_rss_mb);
    fprintf(fcsv, "Frame,Total_ms,Clear_ms,Grid_ms,Axes_ms,Plot_ms,Blit_ms,FPS,CPU_Pct,RAM_Kb,Warmup\n");
    for (int i = 0; i < (int)records.size(); i++) {
      const auto &r = records[i];
      fprintf(fcsv, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%ld,%s\n", r.frame, r.total, r.clear, r.grid, r.axes, r.plot, r.blit, r.fps, r.cpu_usage, r.rss_kb,
              (i < skip_frames) ? "YES" : "NO");
    }
    fclose(fcsv);
  }

  // Overlay résultats à l'écran
  plot_multiple(img, vp, functions, ctx);

  int box_w = 700;
  int box_h = 420;
  int box_x = (scr_w - box_w) / 2;
  int box_y = (scr_h - box_h) / 2;

  for (int row = box_y; row < box_y + box_h; row++)
    for (int col = box_x; col < box_x + box_w; col++)
      img.blend_pixel(col, row, 0xFF0D0D15, 220);
  img.fill_rect(box_x, box_y, box_w, 4, 0xFF4FC3F7);

  int y = box_y + 15;
  int x = box_x + 20;
  char line[128];

  snprintf(line, sizeof(line), "=== RESULTATS BENCHMARK%s ===", cancelled ? " (annule)" : "");
  img.draw_string(x, y, line, 0xFF4FC3F7);
  y += 36;

  snprintf(line, sizeof(line), "Fonctions: %zu  |  Frames: %d (+%d warmup)  |  Resolution: %ux%u", functions.size(), valid_frames, skip_frames, scr_w, scr_h);
  img.draw_string(x, y, line, 0xFFCCCCCC);
  y += 32;

  snprintf(line, sizeof(line), "FPS moyen: %.1f   min: %.1f   max: %.1f", avg_fps, 1000.0 / max_frame_ms_valid, 1000.0 / min_frame_ms_valid);
  img.draw_string(x, y, line, 0xFFFFFFFF);
  y += 32;

  snprintf(line, sizeof(line), "Frame moyen: %.2f ms", avg_frame);
  img.draw_string(x, y, line, 0xFFFFFFFF);
  y += 28;

  snprintf(line, sizeof(line), "CPU moyen: %.1f%%  |  RAM max: %.2f MB  moy: %.2f MB", avg_cpu, max_rss_mb, avg_rss_mb);
  img.draw_string(x, y, line, 0xFFFFFFFF);
  y += 28;

  // Barre de décomposition du temps de frame par phase
  int bar_x = x;
  int bar_y = y + 4;
  int bar_w = box_w - 40;
  int bar_h = 24;
  double pct_clear = clear_ms_sum_valid / total_ms_sum_valid;
  double pct_grid = grid_ms_sum_valid / total_ms_sum_valid;
  double pct_axes = axes_ms_sum_valid / total_ms_sum_valid;
  double pct_plot = plot_ms_sum_valid / total_ms_sum_valid;
  double pct_blit = blit_ms_sum_valid / total_ms_sum_valid;

  int w_clear = (int)(pct_clear * bar_w);
  int w_grid = (int)(pct_grid * bar_w);
  int w_axes = (int)(pct_axes * bar_w);
  int w_plot = (int)(pct_plot * bar_w);
  int w_blit = bar_w - w_clear - w_grid - w_axes - w_plot;

  int bx = bar_x;
  img.fill_rect(bx, bar_y, w_clear, bar_h, 0xFF66BB6A);
  bx += w_clear;
  img.fill_rect(bx, bar_y, w_grid, bar_h, 0xFF4FC3F7);
  bx += w_grid;
  img.fill_rect(bx, bar_y, w_axes, bar_h, 0xFFFFCA28);
  bx += w_axes;
  img.fill_rect(bx, bar_y, w_plot, bar_h, 0xFFEF5350);
  bx += w_plot;
  img.fill_rect(bx, bar_y, w_blit, bar_h, 0xFFAB47BC);
  y += bar_h + 12;

  // Légende couleurs
  img.fill_rect(x, y + 2, 12, 12, 0xFF66BB6A);
  snprintf(line, sizeof(line), "  Clear %.1f%%", pct_clear * 100);
  img.draw_string(x + 16, y - 4, line, 0xFFCCCCCC);

  img.fill_rect(x + 160, y + 2, 12, 12, 0xFF4FC3F7);
  snprintf(line, sizeof(line), "  Grid %.1f%%", pct_grid * 100);
  img.draw_string(x + 176, y - 4, line, 0xFFCCCCCC);

  img.fill_rect(x + 320, y + 2, 12, 12, 0xFFEF5350);
  snprintf(line, sizeof(line), "  Plot %.1f%%", pct_plot * 100);
  img.draw_string(x + 336, y - 4, line, 0xFFCCCCCC);

  img.fill_rect(x + 480, y + 2, 12, 12, 0xFFAB47BC);
  snprintf(line, sizeof(line), "  Blit %.1f%%", pct_blit * 100);
  img.draw_string(x + 496, y - 4, line, 0xFFCCCCCC);
  y += 32;

  // Temps moyens par phase
  snprintf(line, sizeof(line), "  clear: %.2f ms | grid: %.2f ms | axes: %.2f ms", clear_ms_sum_valid / valid_frames, grid_ms_sum_valid / valid_frames,
           axes_ms_sum_valid / valid_frames);
  img.draw_string(x, y, line, 0xFF888888);
  y += 24;

  snprintf(line, sizeof(line), "  plot:  %.2f ms | blit: %.2f ms", plot_ms_sum_valid / valid_frames, blit_ms_sum_valid / valid_frames);
  img.draw_string(x, y, line, 0xFF888888);
  y += 32;

  snprintf(line, sizeof(line), "Donnees exportees dans bench_results.csv");
  img.draw_string(x, y, line, 0xFF81C784);
  y += 28;

  snprintf(line, sizeof(line), "Appuyez sur une touche pour revenir...");
  img.draw_string(x, y, line, 0xFF4FC3F7);

  drm_display_blit_argb32(&drm, img.pixels, scr_w);
  drm_display_present(&drm);

  while (true) {
    int code, value;
    while (input_devices_read_kb(&in_devs, &code, &value) > 0) {
      if (value == 1)
        return;
    }
    usleep(10000);
  }
}

void handle_navigation(int code, Viewport &vp, bool &needs_redraw) {
  double step_x = (vp.x_max - vp.x_min) * 0.1;
  double step_y = (vp.y_max - vp.y_min) * 0.1;

  switch (code) {
  case KEY_LEFT:
    vp.pan(-step_x, 0);
    needs_redraw = true;
    break;
  case KEY_RIGHT:
    vp.pan(step_x, 0);
    needs_redraw = true;
    break;
  case KEY_UP:
    vp.pan(0, step_y);
    needs_redraw = true;
    break;
  case KEY_DOWN:
    vp.pan(0, -step_y);
    needs_redraw = true;
    break;
  case KEY_MINUS:
  case KEY_KPMINUS:
    vp.zoom(1.1);
    needs_redraw = true;
    break;
  case KEY_EQUAL:
  case KEY_KPPLUS:
    vp.zoom(0.9);
    needs_redraw = true;
    break;
  }
}

struct AppState {
  Image &img;
  Viewport &vp;
  context &ctx;
  struct drm_display &drm;
  struct input_devices &in_devs;
  std::vector<FancyFunction> &functions;
  uint32_t scr_w, scr_h;

  std::string input_buffer;
  size_t input_cursor = 0;
  bool is_typing = false;
  bool is_selecting_checkboxes = false;
  bool is_listing_functions = false;
  int list_cursor = 0;
  bool check_f = true;
  bool check_f_prime = false;
  bool check_f_double_prime = false;
  int checkbox_cursor = 0;
  bool shift_pressed = false;
  bool needs_redraw = true;

  AppState(Image &i, Viewport &v, context &c, struct drm_display &d, struct input_devices &in, std::vector<FancyFunction> &f, uint32_t w, uint32_t h)
      : img(i), vp(v), ctx(c), drm(d), in_devs(in), functions(f), scr_w(w), scr_h(h) {}
};

static void render_graph(AppState &s) { plot_multiple(s.img, s.vp, s.functions, s.ctx); }

static void render_list_overlay(AppState &s) {
  int box_w = 600, box_h = 400;
  int box_x = ((int)s.scr_w - box_w) / 2;
  int box_y = ((int)s.scr_h - box_h) / 2;

  s.img.fill_rect(box_x, box_y, box_w, box_h, 0xEE0D0D15);
  s.img.fill_rect(box_x, box_y, box_w, 4, 0xFF4FC3F7);
  s.img.draw_string(box_x + 20, box_y + 15, "=== LISTE DES COURBES ===", 0xFF4FC3F7);

  if (s.functions.empty()) {
    s.img.draw_string(box_x + 20, box_y + 60, "Aucune fonction enregistree.", 0xFF888888);
  } else {
    for (size_t i = 0; i < s.functions.size() && i < 12; i++) {
      std::string prefix = ((int)i == s.list_cursor) ? "> " : "  ";
      std::string info = prefix + s.functions[i].func.name;
      if (s.functions[i].draw_f)
        info += " [f]";
      if (s.functions[i].draw_f_prime)
        info += " [f']";
      if (s.functions[i].draw_f_double_prime)
        info += " [f'']";
      s.img.draw_string(box_x + 20, box_y + 60 + (int)i * 24, info, s.functions[i].color_f);
    }
  }

  int ui_y = s.scr_h - 40;
  s.img.fill_rect(0, ui_y, s.scr_w, 40, 0xFF111111);
  s.img.draw_string(10, ui_y + 4, "[HAUT/BAS] Choisir | [SUPPR/BACKSPACE] Effacer | [CAPSLOCK/ESC] Retour", 0xFFFFFFFF);
}

static void render_bottom_bar(AppState &s) {
  int ui_y = s.scr_h - 40;
  s.img.fill_rect(0, ui_y, s.scr_w, 40, 0xFF111111);

  if (s.is_selecting_checkboxes) {
    std::string opt0 = (s.checkbox_cursor == 0 ? "> " : "  ") + std::string(s.check_f ? "[X] " : "[ ] ") + "f(x)";
    std::string opt1 = (s.checkbox_cursor == 1 ? "> " : "  ") + std::string(s.check_f_prime ? "[X] " : "[ ] ") + "f'(x)";
    std::string opt2 = (s.checkbox_cursor == 2 ? "> " : "  ") + std::string(s.check_f_double_prime ? "[X] " : "[ ] ") + "f''(x)";
    std::string str = "Plot " + s.input_buffer + ": " + opt0 + "   " + opt1 + "   " + opt2 + "  (Space: Toggle, Enter: OK)";
    s.img.draw_string(10, ui_y + 4, str, 0xFFFFFFFF);
  } else if (s.is_typing) {
    std::string str = "Entree f(x) : " + s.input_buffer.substr(0, s.input_cursor) + "|" + s.input_buffer.substr(s.input_cursor);
    s.img.draw_string(10, ui_y + 4, str, 0xFFFFFFFF);
  } else {
    s.img.draw_string(10, ui_y + 4, "[TAB] Fonction | [CAPSLOCK] Liste | [F5] Benchmark | [Esc] Quitter", 0xFF888888);
  }
}

static void present_frame(AppState &s) {
  drm_display_blit_argb32(&s.drm, s.img.pixels, s.scr_w);
  drm_display_present(&s.drm);
  s.needs_redraw = false;
}

static void handle_shift(AppState &s, int code, int value) {
  if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT)
    s.shift_pressed = (value > 0);
}

static bool handle_global_keys(AppState &s, int code) {
  if (code == KEY_ESC) {
    if (s.is_listing_functions) {
      s.is_listing_functions = false;
      return true;
    }
    if (s.is_selecting_checkboxes) {
      s.is_selecting_checkboxes = false;
      return true;
    }
    return false; // quit
  }
  return true; // continue
}

static void handle_list_mode(AppState &s, int code) {
  switch (code) {
  case KEY_CAPSLOCK:
    s.is_listing_functions = false;
    break;
  case KEY_UP:
    if (!s.functions.empty())
      s.list_cursor = (s.list_cursor - 1 + s.functions.size()) % s.functions.size();
    break;
  case KEY_DOWN:
    if (!s.functions.empty())
      s.list_cursor = (s.list_cursor + 1) % s.functions.size();
    break;
  case KEY_DELETE:
  case KEY_BACKSPACE:
    if (!s.functions.empty() && s.list_cursor >= 0 && s.list_cursor < (int)s.functions.size()) {
      s.functions.erase(s.functions.begin() + s.list_cursor);
      if (s.list_cursor >= (int)s.functions.size() && !s.functions.empty())
        s.list_cursor = s.functions.size() - 1;
    }
    break;
  default:
    return;
  }
  s.needs_redraw = true;
}

static void handle_checkbox_mode(AppState &s, int code) {
  switch (code) {
  case KEY_LEFT:
  case KEY_UP:
    s.checkbox_cursor = (s.checkbox_cursor - 1 + 3) % 3;
    break;
  case KEY_RIGHT:
  case KEY_DOWN:
    s.checkbox_cursor = (s.checkbox_cursor + 1) % 3;
    break;
  case KEY_SPACE:
    if (s.checkbox_cursor == 0)
      s.check_f = !s.check_f;
    else if (s.checkbox_cursor == 1)
      s.check_f_prime = !s.check_f_prime;
    else
      s.check_f_double_prime = !s.check_f_double_prime;
    break;
  case KEY_ENTER:
  case KEY_KPENTER:
    if (s.check_f || s.check_f_prime || s.check_f_double_prime) {
      s.functions.push_back({GiacFunction(s.input_buffer, &s.ctx), 2, s.check_f, PALETTE[s.functions.size() % 8], s.check_f_prime, PALETTE[(s.functions.size() + 1) % 8],
                             s.check_f_double_prime, PALETTE[(s.functions.size() + 2) % 8], false, gen(), gen()});
    }
    s.input_buffer.clear();
    s.is_typing = false;
    s.is_selecting_checkboxes = false;
    break;
  default:
    return;
  }
  s.needs_redraw = true;
}

static void handle_typing_mode(AppState &s, int code) {
  switch (code) {
  case KEY_LEFT:
    if (s.input_cursor > 0)
      s.input_cursor--;
    break;
  case KEY_RIGHT:
    if (s.input_cursor < s.input_buffer.length())
      s.input_cursor++;
    break;
  case KEY_BACKSPACE:
    if (s.input_cursor > 0) {
      s.input_buffer.erase(s.input_cursor - 1, 1);
      s.input_cursor--;
    }
    break;
  case KEY_DELETE:
    if (s.input_cursor < s.input_buffer.length())
      s.input_buffer.erase(s.input_cursor, 1);
    break;
  case KEY_ENTER:
  case KEY_KPENTER:
    if (!s.input_buffer.empty()) {
      s.is_selecting_checkboxes = true;
      s.check_f = true;
      s.check_f_prime = false;
      s.check_f_double_prime = false;
      s.checkbox_cursor = 0;
    }
    break;
  default: {
    char c = evdev_code_to_ascii(code, s.shift_pressed);
    if (c != 0) {
      s.input_buffer.insert(s.input_cursor, 1, c);
      s.input_cursor++;
    } else {
      return;
    }
  }
  }
  s.needs_redraw = true;
}

static void handle_normal_mode(AppState &s, int code) {
  if (code == KEY_CAPSLOCK && !s.is_typing && !s.is_selecting_checkboxes) {
    s.is_listing_functions = true;
    s.list_cursor = 0;
  } else if (code == KEY_TAB && !s.is_selecting_checkboxes) {
    s.is_typing = !s.is_typing;
    if (s.is_typing)
      s.input_cursor = s.input_buffer.length();
  } else if (code == KEY_F5 && !s.functions.empty()) {
    run_bench_interactive(s.img, s.vp, s.functions, s.ctx, s.drm, s.scr_w, s.scr_h, s.in_devs);
  } else {
    bool dummy = true;
    handle_navigation(code, s.vp, dummy);
  }
  s.needs_redraw = true;
}

static void process_input(AppState &s) {
  int code, value;
  while (input_devices_read_kb(&s.in_devs, &code, &value) > 0) {
    handle_shift(s, code, value);
    if (value != 1)
      continue;

    if (!handle_global_keys(s, code)) {
      extern volatile int keep_running;
      keep_running = 0;
      return;
    }

    if (s.is_listing_functions) {
      handle_list_mode(s, code);
    } else if (s.is_selecting_checkboxes) {
      handle_checkbox_mode(s, code);
    } else if (s.is_typing) {
      handle_typing_mode(s, code);
    } else {
      handle_normal_mode(s, code);
    }
  }
}
volatile int keep_running = 1;

static void handle_sigint(int sig) {
  (void)sig;
  keep_running = 0;
}

int main(int argc, char *argv[]) {
  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);

  // 1. Parse des arguments CLI
  bool bench_mode = false;
  int bench_duration = 30;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--bench") {
      bench_mode = true;
      if (i + 1 < argc) {
        int d = std::atoi(argv[i + 1]);
        if (d > 0) {
          bench_duration = d;
          i++;
        }
      }
    }
  }

  // 2. Initialisation DRM + structures
  struct drm_display drm;
  uint32_t scr_w, scr_h, stride;
  if (drm_display_init(&drm, &scr_w, &scr_h, &stride) != 0) {
    fprintf(stderr, "Erreur DRM\n");
    return 1;
  }

  Image img(scr_w, scr_h);
  Viewport vp(-2 * M_PI, 2 * M_PI, -3.0, 3.0, scr_w, scr_h);
  context ctx;
  std::vector<FancyFunction> functions;

  struct input_devices in_devs;
  input_devices_detect(&in_devs);

  AppState state(img, vp, ctx, drm, in_devs, functions, scr_w, scr_h);

  // 3. Exécution : mode benchmark ou boucle interactive
  if (bench_mode) {
    functions.push_back({GiacFunction("sin(1/x)", &ctx), 2, true, PALETTE[0], true, PALETTE[1], true, PALETTE[2], false, gen(), gen()});
    run_bench_interactive(img, vp, functions, ctx, drm, scr_w, scr_h, in_devs, bench_duration);
    input_devices_release(&in_devs);
    drm_display_shutdown(&drm);
    return 0;
  }

  while (keep_running) {
    if (state.needs_redraw) {
      render_graph(state);
      if (state.is_listing_functions) {
        render_list_overlay(state);
      } else {
        render_bottom_bar(state);
      }
      present_frame(state);
    }
    process_input(state);
    usleep(10000);
  }

  input_devices_release(&in_devs);
  drm_display_shutdown(&drm);
  return 0;
}