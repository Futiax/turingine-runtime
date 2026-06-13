# Tuer brutalement n'importe quelle ancienne instance pour faire place nette
killall -9 Xvfb firefox-esr display homescreen xterm 2>/dev/null 
rm -f /tmp/.X99-lock

# IMPORTANT: Si Plymouth (le logo de boot linux) tourne, il bloque l'accès physique à la carte DRM !
if command -v plymouth > /dev/null; then
    plymouth quit --retain-splash 2>/dev/null || killall -9 plymouthd 2>/dev/null
fi

sleep 0.1

export SHELL=/bin/bash

# Le homescreen gère tout : DRM direct au repos, lance Xvfb+display quand nécessaire
./.out/bin/homescreen
