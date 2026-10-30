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
| Pont config/télémétrie C3 | `firmware/c3/` (Wi-Fi SoftAP + web) |
| Son | `tools/coin_sound.py` → `components/audio_player/sounds/coin.pcm` (`assets/README.md`) |
| Banc A/B/A | `tools/checkpoint_bench.py` |

## Plateforme à deux ESP

Le C5 (`firmware/c5`) fait toute la réception C5VRX, la détection et le son.
Le C3 (`firmware/c3`) expose un point d'accès Wi-Fi et une page web qui règlent
le détecteur et affichent le C/N, les stats et les passages, via un lien UART
(`components/link_proto`, GPIO 6/15 côté C5). Le C3 est **autoritaire** sur la
config : il la garde en NVS, l'adopte du C5 au premier démarrage s'il n'en a
pas, et la repousse à chaque `HELLO` du C5 (donc après un reboot du C5). Le C5
émet un `EVENT_PASS` par passage et des `STATS` périodiques.

Schéma de câblage : [`wiring.svg`](wiring.svg). Source Graphviz régénérable :
[`wiring.dot`](wiring.dot) (`dot -Tpng docs/wiring.dot -o docs/wiring.png`).

## Flotte de chronométrage (maître C6)

Pour une course à plusieurs portes, un **maître ESP32-C6** (`firmware/c6`)
remplace le rôle de cerveau du C3 et porte le haut-parleur. Chaque **porte** est
un C5 ; le C5 du maître est câblé en UART au C6 (porte 0), les autres sont
reliés à un **C3 nœud** qui relaie en **ESP-NOW** vers le C6.

Schéma global des éléments et de leurs interactions :
[`fleet.svg`](fleet.svg). Brochage consolidé des trois cartes : voir le
[README](../README.md#pinout).

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

## Fonctionnement

- **Signal :** `receiver_obs_cn_x10()`, le rapport porteuse/bruit (C/N) du
  canal accordé en dB, rapporté à une référence de bruit. Il est lu toutes
  les 50 ms par une tâche de priorité 1, en dessous de toutes les tâches de
  réception (control et gain_obs à 3, gain_fast à 4).
- **Pas de mesure** quand le menu, le scan, une calibration ou une pause
  occupe le récepteur, ni quand le C/N est inconnu (gain en cours de
  réglage, pas de référence de bruit, écrêtage). Au-delà de `stale_ms` sans
  mesure, le canal repart en IDLE. Un changement de canal remet le
  détecteur à zéro.
- **Lecture seule :** aucun GPIO, DMA, changement de canal ni écriture de
  gain. La chaîne MODEM_DIAG, BitScrambler, PARLIO et AGC n'est pas modifiée.
- **Machine à états** par canal : `IDLE → RISING → PEAK_PASSED → COOLDOWN`
  (voir `pass_detector.h`). Un drone qui stationne ne sonne qu'une fois :
  pour se réarmer, il faut que la période réfractaire soit écoulée **et**
  que le niveau soit repassé sous `active_db − hysteresis_db`.
- **Une ligne par passage :**
  `PASS channel=R3 freq=5732 peak_cn_db=31.4 peak_ms=… at_ms=… delay_ms=… semitones=-3 audio=queued|muted|off_build|refused`.

### Paramètres (console `K`, sauvegardés en NVS « checkpoint »)

| Paramètre | Défaut | Touches |
| --- | --- | --- |
| `alpha` (lissage exponentiel, poids d'une nouvelle mesure) | 0,30 | `a`/`A` ±0,05 |
| `active_db` (seuil d'activité, C/N) | 15 dB | `t`/`T` ±1 dB |
| `hysteresis_db` | 3 dB | `h`/`H` ±0,5 dB |
| `drop_db` (chute sous le maximum qui marque le passage) | 3 dB | `d`/`D` ±0,5 dB |
| `min_peak_db` (en dessous, drone trop loin : pas de son) | 25 dB | `m`/`M` ±1 dB |
| `refractory_ms` | 1500 ms | `r`/`R` ±250 ms |
| `stale_ms` | 1000 ms | (fixe) |
| volume | 60 % | `v`/`V` ±10 % |

Autres touches : `K` puis `l` (trace C/N en direct, 20 lignes/s), `0`
(valeurs par défaut), `k` (affichage), `q` (fermer). La fenêtre `K` reste
ouverte 5 s après chaque touche. Au premier niveau : `P` joue le son à la
hauteur du canal, `p` coupe ou rétablit le son, `T` affiche la ligne
`CHECKPOINT`.

**Réglage de `active_db` :** placez-le sous le C/N d'un drone qui passe la
porte, mais au-dessus du C/N du même drone ailleurs sur le parcours. Sinon,
le canal ne se réarme pas entre deux tours.

## Pas de sortie vidéo

Le projet ne produit que du son : `firmware/c5/sdkconfig.defaults` met
`CONFIG_C5VRX_VIDEO_OUTPUT=n` (menu *C5VRX receiver*). Le transport (PARLIO
TX + BitScrambler) tourne exactement comme avant, mais ses bits ne sortent
sur aucune broche (`s_dac_gpio` à -1 dans `components/c5vrx_rx/transport.c`).
La capture I/Q, le C/N, le gain et `FIELD_DIAG` ne changent pas. On perd
l'image, le menu à l'écran et le test DAC (`Z`). Les six broches du DAC (8, 9,
11, 12, 23 et 24) ainsi que 7 et 10 deviennent libres pour l'ampli. Après un
changement de `firmware/c5/sdkconfig.defaults`, supprimez `firmware/c5/sdkconfig`
avant de reconstruire.

## Audio : désactivé par défaut

`CONFIG_AUDIO_PLAYER_ENABLE=n`. Le câblage actuel de l'ampli (BCLK sur
GPIO3, LRC sur GPIO4, DIN sur GPIO5, SD sur GPIO1) tombe sur la boucle I/Q
MODEM_DIAG → PARLIO (`RF_IQ_GPIO_NUMS = {1, 0, 25, 2, 27, 5, 3, 4}` dans
`components/c5vrx_rx/rf.h`). Piloter ces broches détruirait la réception.
**Débranchez l'ampli de ces broches.**

Pour activer l'audio après recâblage : `idf.py menuconfig` → *Checkpoint
audio (MAX98357A I2S)*. Activez l'option et saisissez les broches. Le
firmware refuse de démarrer l'audio sur une broche du récepteur et
l'indique par une ligne `CHECKPOINT audio=refused`. Sans vidéo, les
broches utilisables sont 8, 9, 11, 12, 23 et 24, ainsi que 7 et 10 (6 et 15
servent au lien UART vers le C3).
Câblage retenu (valeurs par défaut du Kconfig) : **BCLK sur GPIO9, LRC sur
GPIO8, DIN sur GPIO7, SD sur GPIO10**. SD est refusé sur GPIO7 (broche de *strapping*).
Les GPIO I/Q (0, 1, 2, 3, 4, 5, 25 et 27) restent interdits : le C/N est
calculé à partir de ces échantillons. Il faut aussi `coin.pcm` (`assets/README.md`).

Le canal I2S, et donc un canal GDMA TX, n'existe que pendant les sons.
`transport.c:quiet_interrupts()` coupe les interruptions des trois canaux
GDMA à chaque redémarrage du transport (boot, sortie du menu, changement de
OUTPUT, de profil ou de démodulateur, récupération). Un canal I2S permanent
resterait bloqué ; un canal recréé à chaque série de sons réarme ses
interruptions. Si un redémarrage tombe pendant un son, le son est coupé
(`AUDIO error=write`, compteur `failed`) et le suivant repart normalement.

## Procédure de banc

Matériel : le récepteur, un VTX sur un canal Raceband à puissance fixe, la
vidéo sur un écran. Propulsion retirée. Console : `python tools/flash.py
PORT --monitor-only --log run.log`.

### 1. Build, flash, démarrage

```sh
( cd firmware/c5 && idf.py build )            # 0 warning attendu
python tools/flash.py PORT --check
```

- Le démarrage se termine par `C5VRX5 … ready`. Il n'y a pas de ligne
  `CHECKPOINT disabled`.
- Avec `T` : une ligne `CHECKPOINT source=cn …`. Notez `heap_internal`,
  `heap_min` et `heap_largest`, et comparez avec le firmware amont (même
  carte). `heap_min` ne doit pas baisser : la tâche démarre après le boot.
- Le démarrage affiche `CHECKPOINT video_output=off`. Il n'y a pas
  d'image : c'est normal.

### 2. Source du signal

- Envoyez `K` puis `l` (trace). Avec le VTX allumé et proche,
  `CP_TRACE … cn_db=` affiche des valeurs stables. Avec le VTX éteint,
  vous lisez `cn_db=unknown` ou une valeur faible.
- Dans `T`, `samples_valid` doit dominer `unknown` quand une vidéo est
  reçue. **Si `unknown` domine avec une bonne image, signalez-le-moi** :
  il faudra une autre source, par exemple le dBm de `receiver_level()`
  mis à jour à 1 Hz.
- Coupez la trace (`K` `l`) avant les mesures de qualité.

### 3. Détecteur (sans audio)

| Essai | Attendu |
| --- | --- |
| Approcher le VTX de l'antenne puis l'éloigner, en environ 2 s | une ligne `PASS`, avec `delay_ms` < 1000 |
| Même passage, 5 fois avec au moins 3 s d'écart | 5 lignes `PASS` |
| VTX tenu immobile près de l'antenne pendant 30 s | une seule `PASS` (ou aucune) |
| VTX éteint, ou loin, pendant 2 min | aucune `PASS` |
| VTX à distance moyenne (pic sous `min_peak_db`) | aucune `PASS` |
| `c` (canal suivant) pendant un passage | aucune `PASS` fantôme ; `semitones` change selon le canal |
| `o` (menu) ou `s` (scan) pendant un passage | aucune `PASS` ; dans `T`, `unknown` augmente |

Ajustez avec `K` : `t`/`T` et `m`/`M` d'après les `peak_cn_db` mesurés,
puis `d`/`D` pour la réactivité.

### 4. Audio (après recâblage et build audio)

- `P` sur chaque canal, de R1 à R8 (`c` pour avancer) : la hauteur monte
  de R1 à R8. `semitones` affiche −7, −5, −3, 0, +2, +5, +7, +9.
- `K` `v`/`V` : le volume change. `p` coupe le son (`audio=muted` dans
  `PASS`), puis `p` le rétablit.
- À l'oscilloscope ou à la LED : SD passe à l'état haut environ 5 ms avant
  le son et revient à l'état bas environ 50 ms après. Pas de clic au
  début ni à la fin.
- `P P P P` en rafale : au plus 3 sons (un en cours et deux en file) ;
  `dropped` augmente dans `T`.
- Pendant un son, `g` (OUTPUT, redémarrage du transport) : la vidéo
  revient et le `P` suivant sonne. Un `AUDIO error=write` isolé et
  `failed` +1 sont attendus.

### 5. Qualité de réception, audio actif ou non

Avec un montage fixe (même VTX, canal, puissance, distance), sans toucher
à rien pendant la mesure :

```sh
python3 tools/checkpoint_bench.py PORT --seconds 120 --every 2 --out bench_audio
```

Le script enregistre le flux `FIELD_DIAG` en trois phases : A1 sans son,
B avec `P` toutes les 2 s, A2 sans son. Il les compare ensuite avec
`tools/field_diag_compare.py`.

- A1 et A2 doivent concorder entre eux : c'est la dérive du montage.
- B ne doit pas être moins bon que A1 et A2 sur `sync_evidence_ok_pct`,
  `coverage_pct_median`, `cn_est_median_db`, `chain_fault_s` et
  `dropouts`. Un écart de B qui sort de l'écart A1/A2 indique un effet de
  l'audio.
- Recommencez en limite de portée (atténuateur ou distance), là où une
  dégradation serait la plus visible.
- Faites une fois la même chose avec la trace `K` `l` active : elle
  charge la console USB et n'est pas faite pour le vol.
