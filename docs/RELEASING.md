# Publishing a release (GitHub Releases)

EdoPro+ ships as a Windows installer on **GitHub Releases** (free, global, no
server of your own). The app's built-in update check reads that repo's *latest
release* tag and shows players a "new version available" notice — so cutting a
release is all it takes to notify everyone.

## One-time setup

1. Create a GitHub repo (e.g. `Ryunix/EdoProPlus`) and push the code.
2. Decide the repo string `OWNER/NAME` — you'll bake it into builds so the app
   knows where to check for updates.

## Each release

### 1. Bump the version

Edit `CMakeLists.txt`:

```cmake
project(EdoProPlus VERSION 1.1.0 LANGUAGES CXX C)
```

The version flows into the window title, the main-menu card, and the
update-check comparison automatically.

### 2. Build with the release options baked in

```powershell
cmake -S . -B build/windows ^
  -DEDOPRO_UPDATE_REPO=OWNER/NAME ^
  -DEDOPRO_DEFAULT_RELAY=your.relay.host   # optional; blank = localhost
cmake --build build/windows --config Release
```

(Or just run `build.bat` if you've already configured those once — CMake
remembers cache variables between builds.)

### 3. Make the installer

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_installer.ps1 -Version 1.1.0
```

Produces `dist\EdoProPlus-v1.1.0-Setup.exe` (and you can also run
`tools\package_release.ps1` for the portable `.zip`).

### 4. Publish on GitHub

**The release TAG must be `vX.Y.Z` matching the build version** — the in-app
updater compares against it.

With the GitHub CLI (`winget install GitHub.cli`):

```powershell
git tag v1.1.0 && git push origin v1.1.0
gh release create v1.1.0 ^
  dist\EdoProPlus-v1.1.0-Setup.exe ^
  dist\EdoProPlus-v1.1.0-win64.zip ^
  --title "EdoPro+ v1.1.0" --notes-file CHANGELOG_v1.1.0.md
```

Or via the website: **Releases → Draft a new release →** tag `v1.1.0`, attach
the `Setup.exe` (+ optional `.zip`), write notes, **Publish**.

### 5. Done

Players on older versions see **"Update available: v1.1.0 → Download"** on the
main menu the next time they launch; the button opens the release page.

## How the update check works

- On launch the app GETs `https://api.github.com/repos/OWNER/NAME/releases/latest`
  and compares `tag_name` to its built-in version.
- It only ever **notifies + links** — it never downloads or installs anything.
- Disabled if `EDOPRO_UPDATE_REPO` was blank at build time, or via
  **Settings → "Check for updates on launch."**
- GitHub's unauthenticated API allows ~60 checks/hour per IP — fine for a
  once-per-launch check.

## Tips

- Keep a short `CHANGELOG.md` and paste the relevant section into the release
  notes.
- Mark pre-releases as "pre-release" on GitHub — `releases/latest` skips those,
  so testers on a pre-release build won't nag everyone else.
- The relay's protocol version must match across a release; if you change the
  wire format, bump it on both the app and `relay_server.py`.
