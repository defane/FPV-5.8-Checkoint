# Pas de vidéo, audio optionnel

## Pas de sortie vidéo

Le projet ne produit que du son : `firmware/detector/sdkconfig.defaults` met
`CONFIG_C5VRX_VIDEO_OUTPUT=n` (menu *C5VRX receiver*). Le transport (PARLIO
TX + BitScrambler) tourne exactement comme avant, mais ses bits ne sortent
sur aucune broche (`s_dac_gpio` à -1 dans `components/c5vrx_rx/transport.c`).
La capture I/Q, le C/N, le gain et `FIELD_DIAG` ne changent pas. On perd
l'image, le menu à l'écran et le test DAC (`Z`). Les six broches du DAC (8, 9,
11, 12, 23 et 24) ainsi que 7 et 10 deviennent libres pour l'ampli. Après un
changement de `firmware/detector/sdkconfig.defaults`, supprimez `firmware/detector/sdkconfig`
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
broches utilisables sont 8, 9, 11, 12, 23 et 24, ainsi que 7 et 10 (6 et 8
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
