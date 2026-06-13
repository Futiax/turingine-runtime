/*
 * Anciennes implémentations de tracé de fonctions — non compilées dans le build principal.
 *
 * Compiler manuellement pour tester :
 *   g++ -std=c++17 -O2 -o graph_legacy graph_legacy.cpp -lgiac
 */

#include "font8x16.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <giac/giac.h>
#include <giac/vecteur.h>

using namespace giac;

const uint32_t BG_COLOR   = 0xFF1A1A2E;
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

  double eval(double x_val) {
    try {
      gen gen_val(x_val);
      gen result = subst(expression, x_var, gen_val, false, ctx);
      result = evalf(result, 1, ctx);
      if (result.type == _DOUBLE_)
        return result.DOUBLE_val();
      return NAN;
    } catch (...) {
      return NAN;
    }
  }

  std::vector<double> find_discontinuities() {
    if (_disc_valid)
      return _cached_disc;

    gen converted = expression;
    try { converted = _tan2sincos(expression, ctx); } catch (...) {}
    gen denominator = _denom(converted, ctx);
    gen roots;
    try {
      roots = _solve(makesequence(denominator, x_var), ctx);
    } catch (...) {
      _disc_valid = true;
      return _cached_disc;
    }

    if (roots.type == _VECT) {
      for (size_t i = 0; i < roots._VECTptr->size(); i++) {
        gen num_sol = evalf((*roots._VECTptr)[i], 1, ctx);
        if (num_sol.type == _DOUBLE_)
          _cached_disc.push_back(num_sol.DOUBLE_val());
      }
    } else {
      gen num_sol = evalf(roots, 1, ctx);
      if (num_sol.type == _DOUBLE_)
        _cached_disc.push_back(num_sol.DOUBLE_val());
    }
    _disc_valid = true;
    return _cached_disc;
  }
};

struct Image {
  int width, height;
  uint32_t *pixels; // ARGB : 0xAARRGGBB
  Image(int w, int h) : width(w), height(h) {
    // Alloue SANS initialiser (pas de memset à 0)
    pixels = static_cast<uint32_t *>(std::malloc(w * h * sizeof(uint32_t)));
    if (!pixels) { width = 0; height = 0; throw std::bad_alloc(); }
  }
  ~Image() { std::free(pixels); }
  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;

  void set_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < width && y >= 0 && y < height)
      pixels[y * width + x] = color;
  }
  void clear(uint32_t color = 0xFF000000) {
    std::fill(pixels, pixels + width * height, color);
  }
  void draw_line_wu(int x0, int y0, int x1, int y1, uint32_t color);

  void save_ppm(const char *filename) const {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
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
};

struct Viewport {
  double x_min, x_max;
  double y_min, y_max;
  int pixel_width, pixel_height;

  Viewport(double xmin, double xmax, double ymin, double ymax, int pw, int ph)
      : x_min(xmin), x_max(xmax), y_min(ymin), y_max(ymax), pixel_width(pw), pixel_height(ph) {}

  int math_to_pixel_y(double y) const {
    double r = (y_max - y) / (y_max - y_min) * (pixel_height - 1);
    if (r >= 32767.0) return 32767;
    if (!(r > -32768.0)) return -32768;
    return (int)std::round(r);
  }
  double pixel_to_math_x(int px) const { return x_min + (double)px / (pixel_width - 1) * (x_max - x_min); }
  double pixel_to_math_y(int py) const { return y_max - (double)py / (pixel_height - 1) * (y_max - y_min); }
};

/* ─────────────────────────────────────────────────────────────
 * Tracé pixel par pixel — un point par colonne, sans interpolation.
 * ───────────────────────────────────────────────────────────── */
void plot_function_points(Image &img, const Viewport &vp, GiacFunction &f, uint32_t color) {
  for (int px = 0; px < img.width; px++) {
    double x = vp.pixel_to_math_x(px);
    double y = f.eval(x);
    if (!std::isfinite(y))
      continue;
    int py = vp.math_to_pixel_y(y);
    img.set_pixel(px, py, color);
  }
}

/* ─────────────────────────────────────────────────────────────
 * Tracé par segments Wu — relie chaque colonne à la suivante,
 * coupe aux discontinuités infinies mais pas aux sauts discrets.
 * ───────────────────────────────────────────────────────────── */
void plot_function_line(Image &img, const Viewport &vp, GiacFunction &f, uint32_t color) {
  bool first = true;
  int prev_px = 0, prev_py = 0;

  for (int px = 0; px < img.width; px++) {
    double x = vp.pixel_to_math_x(px);
    double y = f.eval(x);
    if (!std::isfinite(y)) { first = true; continue; }
    int py = vp.math_to_pixel_y(y);
    if (!first)
      img.draw_line_wu(prev_px, prev_py, px, py, color);
    else
      first = false;
    prev_px = px;
    prev_py = py;
  }
}

/* ─────────────────────────────────────────────────────────────
 * Tracé par segments Wu avec détection symbolique des discontinuités
 * et heuristique de saut vertical (> 1/3 hauteur).
 * ───────────────────────────────────────────────────────────── */
void plot_function_smart(Image &img, const Viewport &vp, GiacFunction &f, uint32_t color) {
  bool first = true;
  int prev_px = 0, prev_py = 0;

  std::vector<double> discontinuities = f.find_discontinuities();

  for (int px = 0; px < img.width; px++) {
    double x = vp.pixel_to_math_x(px);
    double y = f.eval(x);
    if (!std::isfinite(y)) { first = true; continue; }

    if (!first) {
      double prev_x = vp.pixel_to_math_x(prev_px);
      for (double d : discontinuities) {
        if (d >= prev_x && d <= x) { first = true; break; }
      }
    }

    int py = vp.math_to_pixel_y(y);
    if (!first && std::abs(py - prev_py) > img.height / 3)
      first = true;

    if (!first)
      img.draw_line_wu(prev_px, prev_py, px, py, color);
    else
      first = false;
    prev_px = px;
    prev_py = py;
  }
}

int main() {
  context ctx;
  Image img(800, 600);
  Viewport vp(-10.0, 10.0, -10.0, 10.0, 800, 600);

  img.clear(BG_COLOR);
  GiacFunction f_points("x^2 - 4", &ctx);
  plot_function_points(img, vp, f_points, PALETTE[0]);
  img.save_ppm("test_plot_function_points.ppm");

  img.clear(BG_COLOR);
  GiacFunction f_line("x^-1 - 4", &ctx);
  plot_function_line(img, vp, f_line, PALETTE[0]);
  img.save_ppm("test_plot_function_line.ppm");

  img.clear(BG_COLOR);
  GiacFunction f_smart("7*sin(1/x)", &ctx);
  GiacFunction f_smart_2("1/x", &ctx);
  plot_function_smart(img, vp, f_smart, PALETTE[0]);
  plot_function_smart(img, vp, f_smart_2, PALETTE[1]);
  img.save_ppm("test_plot_function_smart.ppm");

  return 0;
}
