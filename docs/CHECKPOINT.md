# Checkpoint : « le drone vient de passer »

Ajout à C5VRX-5 : un détecteur de passage sur le **canal accordé** et un son
de pièce 8-bit joué à une hauteur propre au canal (R1 à R8, gamme
pentatonique majeure, de −7 à +9 demi-tons, R4 = hauteur d'origine).

| Partie | Fichier |
| --- | --- |
| Détecteur, C pur | `components/pass_detector/` (test : `test/test_pass_detector.c`) |
| Lecteur audio I2S | `components/audio_player/` (test : `test/test_audio_resample.c`) |
| Intégration (détection + son + lien) | `components/checkpoint/checkpoint.c`, `checkpoint_link.c` |
| Console | `components/c5vrx_rx/console.c` (moteur C5VRX amont) |
| Protocole UART C5↔C3 | `components/link_proto/` (test : `test/test_link_proto.c`) |
| Pont config/télémétrie C3 | `firmware/node/` (Wi-Fi SoftAP + web) |
| Son | `tools/coin_sound.py` → `components/audio_player/sounds/coin.pcm` (`assets/README.md`) |
| Banc A/B/A | `tools/checkpoint_bench.py` |

## Pages

- **[Détecteur : fonctionnement et paramètres](detector.md)** — signal C/N,
  machine à états, paramètres console `K` (sauvés en NVS).
- **[Pas de vidéo, audio optionnel](no-video-audio.md)** — sortie son seule,
  broches libérées par la vidéo off, activation de l'audio après recâblage.
- **[Flotte de chronométrage (maître C6)](fleet.md)** — C6 + nœuds C3,
  protocoles, appairage, rôles web, mode dégradé, C5-as-master.
- **[LED de statut](status-led.md)** — WS2812 par carte, schéma couleur ↔ état.
- **[Procédure de banc](bench.md)** — build/flash, source du signal, essais
  détecteur et audio, qualité de réception A/B/A.

## Plateforme à deux ESP

Le C5 (`firmware/detector`) fait toute la réception C5VRX, la détection et le son.
Le C3 (`firmware/node`) expose un point d'accès Wi-Fi et une page web qui règlent
le détecteur et affichent le C/N, les stats et les passages, via un lien UART
(`components/link_proto`, GPIO 6/8 côté C5). Le C3 est **autoritaire** sur la
config : il la garde en NVS, l'adopte du C5 au premier démarrage s'il n'en a
pas, et la repousse à chaque `HELLO` du C5 (donc après un reboot du C5). Le C5
émet un `EVENT_PASS` par passage et des `STATS` périodiques.

Schéma de câblage : [`wiring.svg`](wiring.svg). Source Graphviz régénérable :
[`wiring.dot`](wiring.dot) (`dot -Tpng docs/wiring.dot -o docs/wiring.png`).
Brochage consolidé : [README](../README.md#pinout).
