# Automat

[![License](https://img.shields.io/github/license/mafik/automat)](https://github.com/mafik/automat/blob/main/LICENSE) [![Last Commit](https://img.shields.io/github/last-commit/mafik/automat)](https://github.com/mafik/automat/commits/main) [![Release](https://img.shields.io/github/v/release/mafik/automat)](https://github.com/mafik/automat/releases/latest) [![Stars](https://img.shields.io/github/stars/mafik/automat?style=social)](https://github.com/mafik/automat) [![Steam](https://img.shields.io/badge/steam-wishlist_now!-00ee00?style=social&logo=steam&logoColor=181d24)
](https://store.steampowered.com/app/4122050/Automat/)

Automat's objective is to be able to semi-autonomously play a variety of games.
It's the first step towards a more general environment for interacting with computers.

# Status

Automat can:

- record and replay keyboard & mouse macros
- react to hotkeys
- capture windows as bitmaps and perform basic OCR
- run a subset of x86 assembly to perform simple logic operations
- runs on Windows & Linux (X11)

Stability of Automat is rather poor but sufficient for use as a macro utility.

Automat is continuously developed. Its roadmap can be found [here](https://www.tldraw.com/ro/3d97dFMiuM0MLgqyyP0SG?d=v-1312.-804.2500.1170.page).

# Downloading and Running

Automat can be downloaded from the [releases page](https://github.com/mafik/automat/releases/latest/).

[Release Process.md](docs/design/Release%20Process.md) contains some more notes about Automat's release schedule.

# Talks / Videos

In reverse chronological order:

- [Live-Coding](https://www.youtube.com/@MarekRogalski/streams) ([YouTube](https://www.youtube.com/@MarekRogalski/streams) / [Twitch](https://twitch.tv/maf_pl), 3 &times; a week, ~4 hours / stream, since 2024-02) - development livestreams
- [Automat](https://www.youtube.com/watch?v=hUFDF62e37s) (1 hour, Feeling of Computing, 2026-01) - overview of Automat's design
- [Automat: Objects As Syntax Not Data](https://www.youtube.com/watch?v=7CwxoUwY9aQ) (6 min, [LIVE 2025](https://liveprog.org/), 2025-09) - introduction to Automat, comes along with an [article](https://automat.org/live2025)
- [Make building software more "fun"](https://www.youtube.com/watch?v=In_BjcsDlfY&list=PLCC8lmauZTzeEP7mIsOOI4HKeeyBN2rIy&index=4&t=67s) (15 min, Feeling of Computing, 2025-06) - provides motivation and explains design decisions that shaped Automat's build system
- [Intro to Automat](https://www.youtube.com/watch?v=_7z77QGARLE&list=PLCC8lmauZTzeEP7mIsOOI4HKeeyBN2rIy&index=11) (15 min, Feeling of Computing, 2024-10) - problem statement, short & long-term goals for project
- [Automat Devlog](https://www.youtube.com/playlist?list=PLnt9Nwtpt_DEuwSV7DDCC0_BJ4FHxrGbn) (series of 5..15 minute videos) - deep-dives that discuss various aspects of Automat's design
- [Automat Playing](https://www.youtube.com/playlist?list=PLnt9Nwtpt_DGrDnBA4RIb6VEjoXNCPTIP) (series of ~10 minute videos) - demos of Automat in action

# Contributing

[![Discord](https://dcbadge.limes.pink/api/server/https://discord.gg/MRfuBBvdjV)](https://discord.gg/MRfuBBvdjV)

First, read [ARCHITECTURE.md](ARCHITECTURE.md) to get a general idea of how things are organized.

Second, download Automat's source code (you can do this by pressing the "Code" button above and selecting the "Download ZIP" option), extract it and enter its directory. Right click and on an empty area and select "Open in Terminal".

First-time users on Windows may then run Automat like so:

```bat
./run.bat automat
```

On Linux (and also on Windows - assuming you already have Python installed and associated with `.py` files):

```sh
./run.py automat
```

The build script should take care of everything for you - if not - you can report it as a bug. Be patient while it builds - it may take quite a bit of time to build everything the first time (expect at least an hour).

Once Automat builds, feel free to tweak its sources to your hearts content. Running the script again will rebuild Automat using your changes.

If you'd like to share your tweaks, you can do so through the GitHub's pull request system. To use it you'll have to "fork" this repository & apply your changes to the fork. It's a good opportunity to clean up the code. Once your changes are pushed to GitHub, you can use GitHub's website to create & send a pull request.

Over the course of this process you may encounter some issues. If you do - don't hesitate to ask AI chatbots for help. If you manage to fix some of these issues - then your fixes would be an amazing contribution for others that will follow your steps. Coding is hard - don't be discouraged by small failures - because ultimately it is those failures that will make you better at coding!

# License

Automat is licensed under the MIT license. Some of its components use slightly different but compatible licenses.

For details, take a look at the contents of the LICENSES directory (this directory is distributed as part of Automat).

# Credits

- Cable & rubber textures -- [TextureCan](https://www.texturecan.com)
- Paper texture -- [Krita](https://krita.org/)
- Grenze font -- [Omnibus-Type](https://www.omnibus-type.com/fonts/grenze/)
- Noto Sans font -- [Google Fonts](https://fonts.google.com/noto)
- Silkscreen font -- [Jason Kottke](https://kottke.org/plus/type/silkscreen/)
- Heavy Data font -- [Vic Fieger](https://www.vicfieger.com/) (website seems to be offline)
- Pbio font -- [Gregor Adams](http://pixelass.com)
- MIT License Logo -- [ExcaliburZero](https://www.deviantart.com/excaliburzero/art/MIT-License-Logo-595847140)

## Default Background

- Stork 1 -- [Manfred Heyde](https://pl.wikipedia.org/wiki/Plik:White_Stork_Glider.jpg) -- License: Creative Commons Attribution-Share Alike 4.0
- Stork 2 -- [Ken Billington](<https://commons.wikimedia.org/wiki/File:White_Stork_(Ciconia_ciconia)_(6).jpg>) -- Creative Commons Attribution-Share Alike 3.0 Unported
- Stork 3 -- [yhoebeke](https://flickr.com/photos/60322572@N04/28568974271) -- Creative Commons Attribution-Share Alike 2.0 Generic

## Sounds

- enter key by uEffects -- https://freesound.org/s/180966/ -- License: Creative Commons 0
- MOUSE, CLICK.wav by xtrgamr -- https://freesound.org/s/252713/ -- License: Attribution 4.0
- Lifting Up Intercom Phone.wav by F.M.Audio -- https://freesound.org/s/555140/ -- License: Attribution 4.0
- Clipping on Beard Trimmer Extension.wav by F.M.Audio -- https://freesound.org/s/547578/ -- License: Attribution 4.0
- RecogerObjetoSuelo.wav by Trancox -- https://freesound.org/s/391924/ -- License: Creative Commons 0
- paper sliding and falling gently by pauliperez1999 -- https://freesound.org/s/428798/ -- License: Attribution 3.0
- crumple-06.ogg by drooler -- https://freesound.org/s/508597/ -- License: Creative Commons 0
- Kaka (New Zealand parrot) by Mings -- https://freesound.org/s/160381/ -- License: Attribution 4.0
- cable noise unplug plug back in.wav by lyd4tuna -- https://freesound.org/s/453262/ -- License: Creative Commons 0
- 24_SeRompeCable.wav by Lextao -- https://freesound.org/s/471828/ -- License: Attribution 4.0
- Button Click 05 by Fats Million -- https://freesound.org/s/187785/ -- License: Attribution 4.0
- sherman cable66.wav by schluppipuppie -- https://freesound.org/s/88368/ -- License: Attribution 4.0

## Website

- Liquid Drink .mp3 by SilverIllusionist -- https://freesound.org/s/411172/ -- License: Attribution 4.0
- VHS Tape by coltures -- https://freesound.org/s/391476/ -- License: Creative Commons 0
