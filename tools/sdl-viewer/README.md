# wup_view — client SDL factice pour WUP Stream

Client GamePad de test pour ordinateur : affiche le flux vidéo du thin
server dans une fenêtre SDL2 et transmet les entrées d'une manette SDL
au format du rapport 128 octets du GamePad Wii U.

## Build

```sh
cd tools/sdl-viewer && make
```

Deps : `libsdl2-dev`, `libavcodec-dev`, `libavutil-dev`.

## Usage

```sh
./wup_view --server <ip-du-thinserver> [--port 50004] [--mode M] [--screenshot out.bmp]
```

- `--port` : `listen_port` du daemon (50004 pour les profils pi).
- `--mode` : requête `T` de mode d'entrée — `0` gamepad, `1` desktop,
  `255` toggle, omis = ne rien demander.
- `--screenshot` : sauvegarde la première frame décodée en BMP.

Contrôles : manette SDL (mapping standard : croix/d-pad/bumpers/gâches
vers les boutons WUP, stick gauche = souris, droit = scroll, clics de
sticks = L3/R3, LB/RB = L/R, RT/LT = ZR/ZL, START/SELECT/GUIDE =
+/-/HOME). Souris : clic maintenu = écran tactile. Clavier sans manette :
flèches = d-pad, WASD = stick gauche, Entrée = A, Retour = B, Espace = X,
Tab = Y. `Échap` quitte.

## Notes techniques

Les paquets `V` contiennent du CABAC slice brut sans en-tête NAL ni
start codes (format pour le décodeur hardware du GamePad). Le client
reconstruit un flux Annex-B standard — SPS/PPS constants du profil
encodeur + slice header réécrit comme `tools/drcverify.py` upstream —
puis décode via libavcodec. L'audio (`A`) n'est pas encore décodé.
