#define _DEFAULT_SOURCE

#include "term_shell.h"
#include "terminal.h"
#include "term_render.h"
#include "evdev_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pty.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

/* ══════════════════════════════════════════════════════════════
 * Variables globales de l'utilisateur et du prompt
 * ══════════════════════════════════════════════════════════════ */

char user_name[64] = "root";
char user_home[128] = "/root";
char user_cwd[256] = "/root";
char prompt_str[256] = "root@turingine:~# ";

/* ══════════════════════════════════════════════════════════════
 * Détection du premier utilisateur non-root (UID >= 1000)
 * ══════════════════════════════════════════════════════════════ */

void detect_default_user(void) {
  /* 1. Lecture ligne par ligne de /etc/passwd */
  FILE *f = fopen("/etc/passwd", "r");
  if (!f) {
    printf("detect_default_user: impossible d'ouvrir /etc/passwd\n");
    return;
  }

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    /* 2. Découpage manuel par ':' (strtok fusionne les champs vides) */
    char *fields[7] = {0};
    char *cur = line;
    for (int i = 0; i < 7 && cur; i += 1) {
      fields[i] = cur;
      char *sep = strchr(cur, ':');
      if (sep) {
        *sep = '\0';
        cur = sep + 1;
      } else {
        /* Dernier champ : retirer le \n */
        char *nl = strchr(cur, '\n');
        if (nl) *nl = '\0';
        cur = NULL;
      }
    }

    /* fields: 0=name 1=pass 2=uid 3=gid 4=gecos 5=home 6=shell */
    if (!fields[0] || !fields[2] || !fields[5])
      continue;

    /* 3. Premier utilisateur humain (UID 1000-65533) → mettre à jour les variables globales */
    int uid = atoi(fields[2]);
    if (uid >= 1000 && uid < 65534) {
      snprintf(user_name, sizeof(user_name), "%s", fields[0]);
      snprintf(user_home, sizeof(user_home), "%s", fields[5]);
      snprintf(user_cwd, sizeof(user_cwd), "%s", fields[5]);
      snprintf(prompt_str, sizeof(prompt_str), "%s@turingine:~$ ", user_name);
      printf("Utilisateur détecté : %s (home: %s)\n", user_name, user_home);
      break;
    }
  }
  fclose(f);

  /* 4. Placer le processus dans le répertoire home pour que les chemins relatifs fonctionnent */
  if (chdir(user_home) != 0)
    printf("Attention: impossible de chdir vers %s\n", user_home);
}

/* Reconstruit le prompt après un changement de répertoire */
void prompt_refresh(void) {
  int home_len = strlen(user_home);
  const char *display_path = user_cwd;
  static char short_path[256];

  if (strcmp(user_cwd, user_home) == 0) {
    display_path = "~";
  } else if (strncmp(user_cwd, user_home, home_len) == 0 &&
             user_cwd[home_len] == '/') {
    snprintf(short_path, sizeof(short_path), "~%s", user_cwd + home_len);
    display_path = short_path;
  }

  const char *sym = (strcmp(user_name, "root") == 0) ? "#" : "$";
  snprintf(prompt_str, sizeof(prompt_str), "%s@turingine:%s%s ",
           user_name, display_path, sym);
}

/* ══════════════════════════════════════════════════════════════
 * Commandes internes
 * ══════════════════════════════════════════════════════════════ */

/* Gestion de la commande cd (changement de répertoire) */
void handle_cd(const char *arg) {
  /* cd sans argument ou cd ~ → retour au home */
  if (arg[0] == '\0' || strcmp(arg, "~") == 0) {
    snprintf(user_cwd, sizeof(user_cwd), "%s", user_home);
    prompt_refresh();
    return;
  }

  /* Résoudre le chemin via le shell */
  char full_cmd[512];
  if (strcmp(user_name, "root") == 0) {
    snprintf(full_cmd, sizeof(full_cmd),
             "cd %s && cd %s && pwd 2>&1", user_cwd, arg);
  } else {
    snprintf(full_cmd, sizeof(full_cmd),
             "su -l %s -c 'cd %s && cd %s && pwd' 2>&1",
             user_name, user_cwd, arg);
  }

  FILE *f = popen(full_cmd, "r");
  if (!f) {
    term_print("cd: erreur interne\n");
    return;
  }

  char result[256] = {0};
  if (fgets(result, sizeof(result), f)) {
    int len = strlen(result);
    if (len > 0 && result[len - 1] == '\n')
      result[len - 1] = '\0';

    if (result[0] == '/') {
      snprintf(user_cwd, sizeof(user_cwd), "%s", result);
      prompt_refresh();
    } else {
      /* Le shell a renvoyé une erreur */
      term_print(result);
      term_print("\n");
    }
  }
  pclose(f);
}

/* ══════════════════════════════════════════════════════════════
 * Exécution via PTY
 * ══════════════════════════════════════════════════════════════ */

/* fd du master PTY (-1 si aucune commande en cours) */
int pty_fd = -1;
pid_t pty_child = -1;

/* Lance cmd dans un PTY. Retourne immédiatement.
 * pty_fd et pty_child sont mis à jour.
 * La boucle principale gère les I/O. */
void run_command(const char *cmd) {
  /* 1. Configurer la taille du terminal virtuel avant le fork */
  struct winsize ws = {
    .ws_row = ROWS,
    .ws_col = COLS,
    .ws_xpixel = 0,
    .ws_ypixel = 0,
  };

  /* 2. Créer un PTY et forker : forkpty() ouvre master+slave et fait le fork atomiquement */
  pid_t pid = forkpty(&pty_fd, NULL, NULL, &ws);
  if (pid < 0) {
    term_print("Erreur: forkpty() a echoue\n");
    pty_fd = -1;
    return;
  }

  if (pid == 0) {
    /* ── Processus enfant ── */

    /* 3a. Environnement : exporter les variables attendues par bash et les apps */
    setenv("HOME",    user_home, 1);
    setenv("USER",    user_name, 1);
    setenv("LOGNAME", user_name, 1);
    setenv("TERM",    "xterm",   1);

    /* 3b. Répertoire courant */
    if (chdir(user_cwd) != 0) {
      /* Silencieux — le shell le signalera lui-même si besoin */
    }

    /* 3c. Remplacer le processus par bash */
    execl("/bin/bash", "bash", "-c", cmd, NULL);
    _exit(127); /* Atteint seulement si execl échoue */
  }

  /* ── Processus parent ── */

  /* 4. Mettre le fd master en non-bloquant pour les lectures dans la boucle principale */
  pty_child = pid;
  int flags = fcntl(pty_fd, F_GETFL, 0);
  fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK);
}

/* ══════════════════════════════════════════════════════════════
 * Keymaps (QWERTY + AZERTY)
 * ══════════════════════════════════════════════════════════════ */

static const char keymap_qwerty[128] = {
    [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
    [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0',
    [KEY_MINUS] = '-', [KEY_EQUAL] = '=',
    [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p', [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']',
    [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'', [KEY_BACKSLASH] = '\\',
    [KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n', [KEY_M] = 'm', [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/',
    [KEY_SPACE] = ' '
};

static const char keymap_qwerty_shift[128] = {
    [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$', [KEY_5] = '%',
    [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*', [KEY_9] = '(', [KEY_0] = ')',
    [KEY_MINUS] = '_', [KEY_EQUAL] = '+',
    [KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T', [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P', [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}',
    [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G', [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L', [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"', [KEY_BACKSLASH] = '|',
    [KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B', [KEY_N] = 'N', [KEY_M] = 'M', [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?',
    [KEY_SPACE] = ' '
};

/* AZERTY français — les caractères non-ASCII (é, è, ç, à, ù) donnent 0 */
static const char keymap_azerty[128] = {
    [KEY_1] = '&', [KEY_3] = '"', [KEY_4] = '\'', [KEY_5] = '(',
    [KEY_6] = '-', [KEY_8] = '_',
    [KEY_MINUS] = ')', [KEY_EQUAL] = '=',
    [KEY_Q] = 'a', [KEY_W] = 'z', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't', [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p',
    [KEY_A] = 'q', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g', [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l', [KEY_SEMICOLON] = 'm',
    [KEY_Z] = 'w', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b', [KEY_N] = 'n',
    [KEY_M] = ',', [KEY_COMMA] = ';', [KEY_DOT] = ':', [KEY_SLASH] = '!', [KEY_BACKSLASH] = '*',
    [KEY_SPACE] = ' '
};

static const char keymap_azerty_shift[128] = {
    [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
    [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0',
    [KEY_EQUAL] = '+',
    [KEY_Q] = 'A', [KEY_W] = 'Z', [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T', [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P',
    [KEY_A] = 'Q', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G', [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L', [KEY_SEMICOLON] = 'M', [KEY_APOSTROPHE] = '%',
    [KEY_Z] = 'W', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B', [KEY_N] = 'N',
    [KEY_M] = '?', [KEY_COMMA] = '.', [KEY_DOT] = '/', [KEY_BACKSLASH] = '|',
    [KEY_SPACE] = ' '
};

/* Pointeurs vers la keymap active (QWERTY par défaut) */
const char *active_keymap       = keymap_qwerty;
const char *active_keymap_shift  = keymap_qwerty_shift;

/* Détecte le layout clavier depuis /etc/default/keyboard (Debian/Armbian) */
void detect_keyboard_layout(void) {
  FILE *f = fopen("/etc/default/keyboard", "r");
  if (!f)
    return;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    /* Chercher XKBLAYOUT="fr" ou variantes */
    if (strstr(line, "XKBLAYOUT") && strstr(line, "fr")) {
      active_keymap = keymap_azerty;
      active_keymap_shift = keymap_azerty_shift;
      printf("Clavier AZERTY (fr) détecté\n");
      break;
    }
  }
  fclose(f);
}
