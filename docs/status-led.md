# LED de statut

Chaque carte dotée d'une WS2812 adressable affiche son état via le composant
`components/status_led` : un cœur pur-C état→couleur (testé à l'hôte,
`test/test_status_led.c`) + un driver WS2812 sur l'encodeur RMT (pas de
dépendance externe), une tâche de rendu priorité 1 démarrée après le splash,
luminosité ~15 %. Au boot, la tâche joue un **auto-test rouge → vert → bleu**
(~0,4 s chacun) : on vérifie d'un coup d'œil le câblage, la broche et l'ordre
des couleurs d'une nouvelle carte (et on distingue la WS2812 d'une éventuelle
LED d'alimentation).

Broches onboard : **node** C3-Zero **GP10**, **master** C6-Zero **GP8**
(variante C5-as-master : **GP27**, libre car cette carte ne fait pas tourner le
récepteur).

| État | Couleur | Motif |
| --- | --- | --- |
| boot / self-test | R→G→B puis jaune | clignote |
| sain / opérationnel | vert | respiration |
| dégradé (node : pas de master · master : un gate tombé) | bleu | respiration |
| appairage (flotte) | magenta | double-pulse |
| erreur récupérable (node : lien C5 perdu) | rouge | clignote rapide |
| erreur fatale (init) | rouge | fixe |
| passage | blanc | flash bref (prioritaire, même son coupé) |

Par rôle :

- **node** (C3-Zero, GP10) : vert quand le master est vivant (message flotte
  reçu < 5 s), bleu en mode dégradé (AP local), magenta en appairage, rouge si
  le lien UART vers son C5 est muet (> 3 s), flash blanc à chaque passage.
- **master** (C6-Zero, GP8) : vert en marche, magenta tant qu'aucun gate n'est
  appairé, bleu si un gate appairé passe hors-ligne, flash blanc à chaque
  passage (gate local ou distant).

Cas sans LED couleur :

- **detector** (C5) : **pas de LED de statut**. La WS2812 onboard du C5-Zero est
  sur **GPIO27**, une broche du loopback I/Q du récepteur (`RF_IQ_GPIO_NUMS`) ;
  la chaîne de réception est prioritaire, donc la LED est désactivée
  (`CONFIG_STATUS_LED_ENABLE=n`). *(Le C5 n'a pas de GPIO29 : le « GP29 » du
  pinout Waveshare est erroné.)*
- **ESP32-C3 SuperMini** : pas de WS2812 adressable (une LED bleue mono sur
  GPIO8 + une LED d'alim rouge). Le schéma couleur ne s'applique pas — prendre un
  C3-Zero pour un node avec LED de statut.

Réglages : `CONFIG_STATUS_LED_*` (ENABLE, GPIO, BRIGHTNESS ~40/255, TICK, STACK) ;
la broche est posée par carte dans le `sdkconfig.defaults` du firmware.
