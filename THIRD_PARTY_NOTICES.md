# Third-Party Notices

EdoPro+ is a fan-made, non-commercial project. It builds on the following
open-source and community-maintained components. Each remains under its own
license; this file is an attribution summary, not a substitute for the
upstream license texts. When redistributing, include the upstream licenses
for any component you ship.

## Trademark / IP disclaimer

"Yu-Gi-Oh!" and all associated card names, artwork, and trademarks are the
property of **Konami** and the respective rights holders. EdoPro+ is an
unofficial fan project and is **not affiliated with, endorsed by, or sponsored
by Konami**. No ownership of the Yu-Gi-Oh! IP is claimed.

## Rules engine

- **ocgcore** — the Yu-Gi-Oh! card rules engine (the YGOPro / EDOPro core),
  originally by Fluorohydride and extended by the ProjectIgnis / EDOPro
  contributors. Used to resolve all card interactions.
  - Upstream: <https://github.com/Fluorohydride/ygopro-core> /
    <https://github.com/edo9300/ygopro-core>

## Card database, scripts & images

- **Card database (`cards.cdb`), card scripts, and card images** — maintained
  by the **ProjectIgnis / EDOPro** community.
  - Card scripts: <https://github.com/ProjectIgnis/CardScripts>
  - Card database: <https://github.com/ProjectIgnis/BabelCDB>
  - These represent the Yu-Gi-Oh! card pool and are subject to Konami's IP
    (see disclaimer above).
- **Card images** are NOT bundled with the application. They are fetched at
  runtime from a public card-image CDN (default: images.ygoprodeck.com) and
  cached locally for personal use only. The artwork is the property of Konami;
  no rights to it are claimed or granted.

## Libraries

- **Dear ImGui** — immediate-mode GUI, by Omar Cornut and contributors.
  MIT License. <https://github.com/ocornut/imgui>
- **SDL2** — windowing, input, and audio. zlib License.
  <https://www.libsdl.org/>
- **Lua** — embedded scripting runtime used by ocgcore. MIT License.
  <https://www.lua.org/>
- **SQLite** — card database storage. Public Domain.
  <https://www.sqlite.org/>
- **stb_image** — image loading, by Sean Barrett. Public Domain / MIT.
  <https://github.com/nothings/stb>

## Fonts & audio

- Fonts in `assets/fonts/` and sound effects in `assets/sfx/` are bundled for
  use within the application. Verify the license of any font/audio asset before
  redistributing it outside this project.
