# Détecteur : fonctionnement et paramètres

Détection de passage sur le canal accordé (`components/pass_detector`, C pur).

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

## Paramètres (console `K`, sauvegardés en NVS « checkpoint »)

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
