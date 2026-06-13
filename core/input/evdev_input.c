#include "evdev_input.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_primary_interface(struct libevdev *dev) {
  const char *phys = libevdev_get_phys(dev);
  if (!phys)
    return 1;
  const char *p = strstr(phys, "/input");
  if (!p)
    return 1;
  int iface_num = atoi(p + 6);
  return (iface_num == 0) ? 1 : 0;
}

static int score_keyboard(struct libevdev *dev) {
  if (!libevdev_has_event_type(dev, EV_REP))
    return 0;
  int score = 0;

  /* Touches alphabet (26 lettres) */
  const int alpha[] = {KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_A, KEY_S, KEY_D,
                       KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M};
  for (int i = 0; i < 26; i += 1)
    if (libevdev_has_event_code(dev, EV_KEY, alpha[i]))
      score += 1;

  /* Touches spéciales de saisie */
  if (libevdev_has_event_code(dev, EV_KEY, KEY_SPACE))
    score += 2;
  if (libevdev_has_event_code(dev, EV_KEY, KEY_ENTER))
    score += 2;
  if (libevdev_has_event_code(dev, EV_KEY, KEY_BACKSPACE))
    score += 2;
  if (libevdev_has_event_code(dev, EV_KEY, KEY_LEFTSHIFT))
    score += 2;
  if (libevdev_has_event_code(dev, EV_KEY, KEY_LEFTCTRL))
    score += 1;

  /* LEDs clavier — fort indicateur d'un vrai clavier */
  if (libevdev_has_event_code(dev, EV_LED, LED_CAPSL))
    score += 10;
  if (libevdev_has_event_code(dev, EV_LED, LED_NUML))
    score += 5;
  if (libevdev_has_event_code(dev, EV_LED, LED_SCROLLL))
    score += 5;

  /* Pénalités : interface secondaire ou device de pointage déguisé */
  if (!is_primary_interface(dev))
    score -= 40;
  if (libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) && libevdev_has_event_type(dev, EV_REL))
    score -= 20;
  return score;
}

static int score_mouse(struct libevdev *dev) {
  if (!libevdev_has_event_type(dev, EV_REL))
    return 0;
  if (!libevdev_has_event_code(dev, EV_REL, REL_X))
    return 0;
  if (!libevdev_has_event_code(dev, EV_REL, REL_Y))
    return 0;
  if (!libevdev_has_event_code(dev, EV_KEY, BTN_LEFT))
    return 0;
  if (libevdev_has_event_type(dev, EV_REP))
    return 0;
  if (libevdev_has_event_type(dev, EV_LED))
    return 0;
  int score = 10;
  if (libevdev_has_event_code(dev, EV_KEY, BTN_RIGHT))
    score += 3;
  if (libevdev_has_event_code(dev, EV_KEY, BTN_MIDDLE))
    score += 2;
  if (libevdev_has_event_code(dev, EV_REL, REL_WHEEL))
    score += 3;
  if (libevdev_has_event_code(dev, EV_REL, REL_HWHEEL))
    score += 1;
  if (libevdev_has_event_code(dev, EV_REL, REL_WHEEL_HI_RES))
    score += 1;
  if (is_primary_interface(dev))
    score += 5;
  if (libevdev_has_event_code(dev, EV_KEY, KEY_SPACE))
    score -= 15;
  return score;
}

int input_devices_detect(struct input_devices *devs) {
  if (!devs)
    return -1;
  memset(devs, 0, sizeof(*devs));
  devs->mouse_fd = -1;
  devs->kb_fd = -1;

  char dev_path[64];
  int best_mouse_score = 0;
  int best_kb_score = 0;

  /* Scan de /dev/input/event0-63 : scorer chaque device, garder le meilleur clavier et la meilleure souris */
  printf("Recherche automatique de Clavier et Souris dans /dev/input...\n");
  for (int i = 0; i < 64; i += 1) {
    snprintf(dev_path, sizeof(dev_path), "/dev/input/event%d", i);
    int temp_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (temp_fd < 0)
      continue;

    struct libevdev *temp_dev;
    if (libevdev_new_from_fd(temp_fd, &temp_dev) != 0) {
      close(temp_fd);
      continue;
    }

    int ms = score_mouse(temp_dev);
    int ks = score_keyboard(temp_dev);

    if (ms > 0 || ks > 0) {
      printf("  [%s] %-40s | souris: %2d | clavier: %2d\n", dev_path, libevdev_get_name(temp_dev), ms, ks);
    }

    if (ms > best_mouse_score) {
      if (devs->mouse) { libevdev_free(devs->mouse); close(devs->mouse_fd); }
      devs->mouse = temp_dev;
      devs->mouse_fd = temp_fd;
      best_mouse_score = ms;
    }
    if (ks > best_kb_score) {
      if (devs->kb) { libevdev_free(devs->kb); close(devs->kb_fd); }
      devs->kb = temp_dev;
      devs->kb_fd = temp_fd;
      best_kb_score = ks;
    }
    if (temp_dev != devs->mouse && temp_dev != devs->kb) {
      libevdev_free(temp_dev);
      close(temp_fd);
    }
  }

  /* Grab exclusif des devices retenus pour éviter les fuites d'événements vers d'autres processus */
  if (devs->mouse) {
    printf("Souris sélectionnée : %s\n", libevdev_get_name(devs->mouse));
    libevdev_grab(devs->mouse, LIBEVDEV_GRAB);
  } else {
    fprintf(stderr, "AUCUNE souris détectée !\n");
  }

  if (devs->kb) {
    printf("Clavier sélectionné : %s\n", libevdev_get_name(devs->kb));
    libevdev_grab(devs->kb, LIBEVDEV_GRAB);
  } else {
    fprintf(stderr, "AUCUN clavier détecté !\n");
  }

  return (devs->mouse || devs->kb) ? 0 : -1;
}

void input_devices_release(struct input_devices *devs) {
  if (!devs)
    return;
  if (devs->mouse) {
    libevdev_grab(devs->mouse, LIBEVDEV_UNGRAB);
    libevdev_free(devs->mouse);
    close(devs->mouse_fd);
    devs->mouse = NULL;
    devs->mouse_fd = -1;
  }
  if (devs->kb) {
    libevdev_grab(devs->kb, LIBEVDEV_UNGRAB);
    libevdev_free(devs->kb);
    close(devs->kb_fd);
    devs->kb = NULL;
    devs->kb_fd = -1;
  }
}

static const char keymap_qwerty[256] = {
    [KEY_1] = '1',          [KEY_2] = '2',         [KEY_3] = '3',          [KEY_4] = '4',     [KEY_5] = '5',     [KEY_6] = '6',         [KEY_7] = '7',
    [KEY_8] = '8',          [KEY_9] = '9',         [KEY_0] = '0',          [KEY_MINUS] = '-', [KEY_EQUAL] = '=', [KEY_Q] = 'q',         [KEY_W] = 'w',
    [KEY_E] = 'e',          [KEY_R] = 'r',         [KEY_T] = 't',          [KEY_Y] = 'y',     [KEY_U] = 'u',     [KEY_I] = 'i',         [KEY_O] = 'o',
    [KEY_P] = 'p',          [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']', [KEY_A] = 'a',     [KEY_S] = 's',     [KEY_D] = 'd',         [KEY_F] = 'f',
    [KEY_G] = 'g',          [KEY_H] = 'h',         [KEY_J] = 'j',          [KEY_K] = 'k',     [KEY_L] = 'l',     [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'',
    [KEY_BACKSLASH] = '\\', [KEY_Z] = 'z',         [KEY_X] = 'x',          [KEY_C] = 'c',     [KEY_V] = 'v',     [KEY_B] = 'b',         [KEY_N] = 'n',
    [KEY_M] = 'm',          [KEY_COMMA] = ',',     [KEY_DOT] = '.',        [KEY_SLASH] = '/', [KEY_SPACE] = ' ',
    [KEY_KP0] = '0',        [KEY_KP1] = '1',       [KEY_KP2] = '2',        [KEY_KP3] = '3',   [KEY_KP4] = '4',
    [KEY_KP5] = '5',        [KEY_KP6] = '6',       [KEY_KP7] = '7',        [KEY_KP8] = '8',   [KEY_KP9] = '9',
    [KEY_KPASTERISK] = '*', [KEY_KPMINUS] = '-',   [KEY_KPPLUS] = '+',     [KEY_KPDOT] = '.', [KEY_KPSLASH] = '/'};

static const char keymap_qwerty_shift[256] = {
    [KEY_1] = '!',         [KEY_2] = '@',         [KEY_3] = '#',          [KEY_4] = '$',     [KEY_5] = '%',     [KEY_6] = '^',         [KEY_7] = '&',
    [KEY_8] = '*',         [KEY_9] = '(',         [KEY_0] = ')',          [KEY_MINUS] = '_', [KEY_EQUAL] = '+', [KEY_Q] = 'Q',         [KEY_W] = 'W',
    [KEY_E] = 'E',         [KEY_R] = 'R',         [KEY_T] = 'T',          [KEY_Y] = 'Y',     [KEY_U] = 'U',     [KEY_I] = 'I',         [KEY_O] = 'O',
    [KEY_P] = 'P',         [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}', [KEY_A] = 'A',     [KEY_S] = 'S',     [KEY_D] = 'D',         [KEY_F] = 'F',
    [KEY_G] = 'G',         [KEY_H] = 'H',         [KEY_J] = 'J',          [KEY_K] = 'K',     [KEY_L] = 'L',     [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"',
    [KEY_BACKSLASH] = '|', [KEY_Z] = 'Z',         [KEY_X] = 'X',          [KEY_C] = 'C',     [KEY_V] = 'V',     [KEY_B] = 'B',         [KEY_N] = 'N',
    [KEY_M] = 'M',         [KEY_COMMA] = '<',     [KEY_DOT] = '>',        [KEY_SLASH] = '?', [KEY_SPACE] = ' ',
    [KEY_KP0] = '0',        [KEY_KP1] = '1',       [KEY_KP2] = '2',        [KEY_KP3] = '3',   [KEY_KP4] = '4',
    [KEY_KP5] = '5',        [KEY_KP6] = '6',       [KEY_KP7] = '7',        [KEY_KP8] = '8',   [KEY_KP9] = '9',
    [KEY_KPASTERISK] = '*', [KEY_KPMINUS] = '-',   [KEY_KPPLUS] = '+',     [KEY_KPDOT] = '.', [KEY_KPSLASH] = '/'};

static const char keymap_azerty[256] = {
    [KEY_1] = '&', [KEY_3] = '"', [KEY_4] = '\'', [KEY_5] = '(',         [KEY_6] = '-',   [KEY_8] = '_',     [KEY_MINUS] = ')',     [KEY_EQUAL] = '=',
    [KEY_Q] = 'a', [KEY_W] = 'z', [KEY_E] = 'e',  [KEY_R] = 'r',         [KEY_T] = 't',   [KEY_Y] = 'y',     [KEY_U] = 'u',         [KEY_I] = 'i',
    [KEY_O] = 'o', [KEY_P] = 'p', [KEY_A] = 'q',  [KEY_S] = 's',         [KEY_D] = 'd',   [KEY_F] = 'f',     [KEY_G] = 'g',         [KEY_H] = 'h',
    [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',  [KEY_SEMICOLON] = 'm', [KEY_Z] = 'w',   [KEY_X] = 'x',     [KEY_C] = 'c',         [KEY_V] = 'v',
    [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = ',',  [KEY_COMMA] = ';',     [KEY_DOT] = ':', [KEY_SLASH] = '!', [KEY_BACKSLASH] = '*', [KEY_SPACE] = ' ',
    [KEY_LEFTBRACE] = '^',
    [KEY_KP0] = '0',        [KEY_KP1] = '1',       [KEY_KP2] = '2',        [KEY_KP3] = '3',   [KEY_KP4] = '4',
    [KEY_KP5] = '5',        [KEY_KP6] = '6',       [KEY_KP7] = '7',        [KEY_KP8] = '8',   [KEY_KP9] = '9',
    [KEY_KPASTERISK] = '*', [KEY_KPMINUS] = '-',   [KEY_KPPLUS] = '+',     [KEY_KPDOT] = '.', [KEY_KPSLASH] = '/'};

static const char keymap_azerty_shift[256] = {
    [KEY_1] = '1',   [KEY_2] = '2',         [KEY_3] = '3',     [KEY_4] = '4', [KEY_5] = '5', [KEY_6] = '6', [KEY_7] = '7',         [KEY_8] = '8',
    [KEY_9] = '9',   [KEY_0] = '0',         [KEY_EQUAL] = '+', [KEY_Q] = 'A', [KEY_W] = 'Z', [KEY_E] = 'E', [KEY_R] = 'R',         [KEY_T] = 'T',
    [KEY_Y] = 'Y',   [KEY_U] = 'U',         [KEY_I] = 'I',     [KEY_O] = 'O', [KEY_P] = 'P', [KEY_A] = 'Q', [KEY_S] = 'S',         [KEY_D] = 'D',
    [KEY_F] = 'F',   [KEY_G] = 'G',         [KEY_H] = 'H',     [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L', [KEY_SEMICOLON] = 'M', [KEY_APOSTROPHE] = '%',
    [KEY_Z] = 'W',   [KEY_X] = 'X',         [KEY_C] = 'C',     [KEY_V] = 'V', [KEY_B] = 'B', [KEY_N] = 'N', [KEY_M] = '?',         [KEY_COMMA] = '.',
    [KEY_DOT] = '/', [KEY_BACKSLASH] = '|', [KEY_SPACE] = ' ',
    [KEY_LEFTBRACE] = '^',
    [KEY_KP0] = '0',        [KEY_KP1] = '1',       [KEY_KP2] = '2',        [KEY_KP3] = '3',   [KEY_KP4] = '4',
    [KEY_KP5] = '5',        [KEY_KP6] = '6',       [KEY_KP7] = '7',        [KEY_KP8] = '8',   [KEY_KP9] = '9',
    [KEY_KPASTERISK] = '*', [KEY_KPMINUS] = '-',   [KEY_KPPLUS] = '+',     [KEY_KPDOT] = '.', [KEY_KPSLASH] = '/'};

static const char *active_keymap = keymap_qwerty;
static const char *active_keymap_shift = keymap_qwerty_shift;
static int layout_detected = 0;

static void detect_layout(void) {
  if (layout_detected)
    return;
  FILE *f = fopen("/etc/default/keyboard", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      if (strstr(line, "XKBLAYOUT") && strstr(line, "fr")) {
        active_keymap = keymap_azerty;
        active_keymap_shift = keymap_azerty_shift;
        printf("evdev_input: Clavier AZERTY (fr) détecté\n");
        break;
      }
    }
    fclose(f);
  }
  layout_detected = 1;
}

int input_devices_read_kb(struct input_devices *devs, int *out_code, int *out_value) {
  if (!devs || !devs->kb)
    return -1;

  struct input_event ev;
  while (1) {
    int rc = libevdev_next_event(devs->kb, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
      if (ev.type == EV_KEY) {
        if (out_code)  *out_code  = ev.code;
        if (out_value) *out_value = ev.value;
        return 1;
      }
      continue; /* Ignorer les événements non-clavier (EV_SYN, EV_MSC...) */
    }
    if (rc == -EAGAIN)
      return 0; /* Aucun événement disponible */

    /* LIBEVDEV_READ_STATUS_SYNC : le kernel a dropé des événements, vider le backlog */
    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
      while (rc == LIBEVDEV_READ_STATUS_SYNC)
        rc = libevdev_next_event(devs->kb, LIBEVDEV_READ_FLAG_SYNC, &ev);
      continue;
    }
    return -1;
  }
}

char evdev_code_to_ascii(int code, int shift) {
  detect_layout();
  if (code < 0 || code >= 256)
    return 0;
  return shift ? active_keymap_shift[code] : active_keymap[code];
}
