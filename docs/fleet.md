# Flotte de chronométrage (maître C6)

Pour une course à plusieurs portes, un **maître ESP32-C6** (`firmware/master`)
remplace le rôle de cerveau du C3 et porte le haut-parleur. Chaque **porte** est
un C5 ; le C5 du maître est câblé en UART au C6 (porte 0), les autres sont
reliés à un **C3 nœud** qui relaie en **ESP-NOW** vers le C6.

Schéma global des éléments et de leurs interactions :
[`fleet.svg`](fleet.svg). Câblage pin-level par type de set :
[`wiring-master.svg`](wiring-master.svg) (C6 + C5),
[`wiring-node.svg`](wiring-node.svg) (C3 + C5),
[`wiring-c5c5.svg`](wiring-c5c5.svg) (C5+C5 temporaire),
[`wiring.svg`](wiring.svg) (C5 autonome). Brochage consolidé : voir le
[README](../README.md#pinout).

**Montage temporaire C5+C5** (en attendant le C6) : un second C5 fait tourner le
firmware du maître compilé pour esp32c5 (`firmware/master`, cible esp32c5). L'ampli
passe sur les broches C5 9/8/7/10 (les GPIO 16‑22 sont la flash du C5), l'UART
reste 4/5. Build : `make flash FW=master-wave-c5` (ou `idf.py -DIDF_TARGET=esp32c5
-DSDKCONFIG=sdkconfig.c5 -DSDKCONFIG_DEFAULTS=sdkconfig.c5.defaults -B build-c5
build` depuis `firmware/master`, → `build-c5/master-c5.bin`). On remplace ensuite
ce C5 par un vrai C6 sans rien changer côté porte.

- **Protocoles** : `components/link_proto` (UART C5↔pont, dont `SET_CHANNEL` qui
  déclenche un retune `rf_set_channel` sur le C5) ; `components/fleet_proto`
  (ESP-NOW C6↔C3 : appairage, balises temps, `CONFIG`, `GATE_PASS`,
  `NODE_STATUS`) ; `components/espnow_link` (transport) ;
  `components/race_timing` (tours/secteurs par canal).
- **C6** (`c6_hub`) : table de nœuds en NVS, balises temps périodiques, moteur
  `race_timing`, **son centralisé**, et un **web d'admin** (AP) pour appairer,
  ordonner les portes, assigner les canaux et voir les chronos.
- **Appairage** : web C6 → *pairing open* ; un C3 non appairé diffuse un
  `PAIR_REQUEST` ; le C6 lui assigne porte+canal (`PAIR_ACCEPT` + `CONFIG`),
  persistés de part et d'autre.
- **Mode dégradé C3** : si le C6 est injoignable `CONFIG_C3_FLEET_TIMEOUT_MS` au
  boot, le C3 sert son AP+web local (sélection de bande → `SET_CHANNEL`,
  affichage des passages).
- **Rôles web** : deux niveaux. Un **spectateur** voit les chronos/état en
  lecture seule (sans login) ; les actions **admin** (appairage, ordre des
  portes, canal, reset ; sélection de bande sur un C3 dégradé) exigent un login
  (`CONFIG_*_ADMIN_USER`/`_PASSWORD`, en-tête HTTP Basic vérifié côté ESP). Un
  mot de passe vide désactive l'auth admin.
- **Contrainte** : l'AP du C6 et des C3 **et** ESP-NOW partagent le **même canal
  WiFi** (`CONFIG_*_AP_CHANNEL` / `CONFIG_C6_AP_CHANNEL`).
- **Limite** : la base de temps (balises ESP-NOW) vise ~10 ms ; pas un
  photo-finish sub-ms.
