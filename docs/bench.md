# Procédure de banc

Matériel : le récepteur, un VTX sur un canal Raceband à puissance fixe, la
vidéo sur un écran. Propulsion retirée. Console : `python tools/flash.py
PORT --monitor-only --log run.log`.

## 1. Build, flash, démarrage

```sh
( cd firmware/detector && idf.py build )            # → build/detector-c5.bin (0 warning attendu)
python tools/flash.py PORT --check
```

- Le démarrage se termine par `C5VRX5 … ready`. Il n'y a pas de ligne
  `CHECKPOINT disabled`.
- Avec `T` : une ligne `CHECKPOINT source=cn …`. Notez `heap_internal`,
  `heap_min` et `heap_largest`, et comparez avec le firmware amont (même
  carte). `heap_min` ne doit pas baisser : la tâche démarre après le boot.
- Le démarrage affiche `CHECKPOINT video_output=off`. Il n'y a pas
  d'image : c'est normal.

## 2. Source du signal

- Envoyez `K` puis `l` (trace). Avec le VTX allumé et proche,
  `CP_TRACE … cn_db=` affiche des valeurs stables. Avec le VTX éteint,
  vous lisez `cn_db=unknown` ou une valeur faible.
- Dans `T`, `samples_valid` doit dominer `unknown` quand une vidéo est
  reçue. **Si `unknown` domine avec une bonne image, signalez-le-moi** :
  il faudra une autre source, par exemple le dBm de `receiver_level()`
  mis à jour à 1 Hz.
- Coupez la trace (`K` `l`) avant les mesures de qualité.

## 3. Détecteur (sans audio)

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

## 4. Audio (après recâblage et build audio)

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

## 5. Qualité de réception, audio actif ou non

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
