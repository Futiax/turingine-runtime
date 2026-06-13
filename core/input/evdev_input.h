#pragma once

#include <libevdev/libevdev.h>

struct input_devices {
    struct libevdev *mouse;
    int mouse_fd;
    struct libevdev *kb;
    int kb_fd;
};

/* Détecte et ouvre le meilleur clavier et la meilleure souris.
 * Retourne 0 en cas de succès (au moins un device trouvé), -1 sinon.
 * Remplit la structure fournie. */
int input_devices_detect(struct input_devices *devs);

/* Libère les périphériques et ferme les descripteurs de fichiers. */
void input_devices_release(struct input_devices *devs);

/* Lit le prochain événement clavier (non-bloquant).
 * Retourne 1 si une touche (EV_KEY) a été lue, 0 si aucun événement, -1 si erreur.
 * Remplit *out_code (ex: KEY_A) et *out_value (1=press, 0=release, 2=repeat). */
int input_devices_read_kb(struct input_devices *devs, int *out_code, int *out_value);

/* Convertit un code touche evdev en caractère ASCII en tenant compte de shift.
 * Retourne le caractère correspondant, ou 0 si non imprimable. */
char evdev_code_to_ascii(int code, int shift);
