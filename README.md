# Automat

[![Build](https://github.com/mafik/automat/actions/workflows/build.yaml/badge.svg?branch=main)](https://github.com/mafik/automat/actions/workflows/build.yaml)

Automat's objective is to be able to semi-autonomously play a variety of games. It's the first step towards a more general environment for interacting with
computers.

# Status

Currently Automat's functionality is limited to keyboard macro recording &
playback. It's fairly unstable but if you're lucky and it runs on your machine,
it can be useful for automating some basic actions.

You can, of course, check it out just to see how the interface works - but
don't assume that it's going to stay like that. It's very much a work in
progress. Roadmap includes some pretty radical new features which will
drastically change how it looks and works.

Current roadmap can be found [here](https://www.tldraw.com/ro/3d97dFMiuM0MLgqyyP0SG?d=v-1312.-804.2500.1170.page).

# Downloading and Running

Automat can be downloaded from the [releases page](https://github.com/mafik/automat/releases/latest/).

[Release Process.md](docs/design/Release%20Process.md) contains some more notes about the Automat's release regime.

# Contributing

First read [ARCHITECTURE.md](ARCHITECTURE.md) to get a general idea of how
things are organized. Once you do that, you may try to clone the Automat
repository and run it (build script should take care of everything for you - if
not - you can report it as a bug).

The same commands should work on Linux and Windows:

```sh
git clone https://github.com/mafik/automat.git
cd automat
./run.py automat
```

# License

Automat is licensed under the MIT license. Some of its components use slightly
different but compatible licenses. This means that you are free to embed &
resell Automat, as long as you follow the license terms.

Here is an overview of the licenses used by Automat's dependencies:

- Skia: BSD 3-Clause
- FastTrigo: BSD 3-Clause
- HIDAPI: BSD 3-Clause
- libXau: MIT/X Consortium License
- libxcb: MIT/X Consortium License
- libxcb-proto: MIT/X Consortium License
- xorg-util-macros: MIT/X Consortium License
- xorgproto: various licenses
- concurrentqueue: BSD 2-Clause
- PipeWire: (not linked statically) MIT + LGPL for some plugins
- RapidJSON: MIT (tests rely on non-MIT licensed files but they're not used for release)
- vk-bootstrap: MIT
- Vulkan-Headers: Apache 2.0 + MIT
- Noto Sans: Open Font License

Do your own due dilligence and check the licenses of the dependencies you use!

# Credits

- Cable texture: [TextureCan](https://www.texturecan.com)

## Sounds

- enter key by uEffects -- https://freesound.org/s/180966/ -- License: Creative Commons 0
- MOUSE, CLICK.wav by xtrgamr -- https://freesound.org/s/252713/ -- License: Attribution 4.0
- Lifting Up Intercom Phone.wav by F.M.Audio -- https://freesound.org/s/555140/ -- License: Attribution 4.0
- Clipping on Beard Trimmer Extension.wav by F.M.Audio -- https://freesound.org/s/547578/ -- License: Attribution 4.0
- RecogerObjetoSuelo.wav by Trancox -- https://freesound.org/s/391924/ -- License: Creative Commons 0
- paper  sliding and falling gently by pauliperez1999 -- https://freesound.org/s/428798/ -- License: Attribution 3.0
- crumple-06.ogg by drooler -- https://freesound.org/s/508597/ -- License: Creative Commons 0
- Kaka (New Zealand parrot) by Mings -- https://freesound.org/s/160381/ -- License: Attribution 4.0
- cable noise unplug plug back in.wav by lyd4tuna -- https://freesound.org/s/453262/ -- License: Creative Commons 0
- 24_SeRompeCable.wav by Lextao -- https://freesound.org/s/471828/ -- License: Attribution 4.0
- Button Click 05 by Fats Million -- https://freesound.org/s/187785/ -- License: Attribution 4.0
- sherman cable66.wav by schluppipuppie -- https://freesound.org/s/88368/ -- License: Attribution 4.0

## Website

- 電磁祭囃子 in NEO TOKYO 🏮 by ELECTRONICOS FANTASTICOS! -- https://www.youtube.com/watch?v=A0VYsiMtrNE -- License: CC BY-NC-SA 4.0
- Liquid Drink .mp3 by SilverIllusionist -- https://freesound.org/s/411172/ -- License: Attribution 4.0
- VHS Tape by coltures -- https://freesound.org/s/391476/ -- License: Creative Commons 0