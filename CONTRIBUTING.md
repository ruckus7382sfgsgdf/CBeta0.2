# Contributing to Triforce

Triforce is a native PC port of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake. The game's code is
translated to C and built on your own PC from your own copy of the game; the system and the graphics are rebuilt
natively, with the graphics replayed on Vulkan. No emulator is involved. Triforce builds on the open-source Hookshot
project (GPL-3.0), and its code keeps that license.

This page covers what to send, how to build and test it, and the rules that keep the project out of trouble. The
[development guide](docs/development.md) has the full pipeline and its commands; the [design notes](docs/design.md)
have the architecture.

## The rule with no exceptions

**No game files, keys, or anything made from them leave your PC.** The repository does not contain the game and
never will. That includes:

- the `.nup`, `romfs`, `exefs`, `.nso`, `.nca`, `.nsp`, `.xci` or `.nsz`/`.xcz` files, and any title keys;
- `work/` and everything in it: the extracted dump, `image.bin`, the translated C, test builds, runs and screenshots;
- `Triforce.exe`, `triforce_vk.dll`, `triforce_shaders.bin`, `triforce_programs.bin` (and `.idx`),
  `triforce_pipelines.bin`, `mkw_vk_pipelines.bin` and every other file a build or a run makes;
- saves (`user/`), `triforce_options.ini`, `triforce_env.txt` and `language.txt`.

Do not open an issue or a pull request asking where to get the game, how to decrypt it, or for someone else's files.
A screenshot, a video and `triforce_log.txt` are fine and often necessary; the log holds no game data.

`.gitignore` covers most of this, but check `git status` before you commit. If you are not sure whether a file is
game data, leave it out.

## Reporting a problem

Open an [issue](https://github.com/ruckus7382sfgsgdf/CBeta0.2/issues) and include:

- the Triforce version (the release name, or the first line of `triforce_log.txt`);
- your graphics card, its driver version, and your Windows version;
- where you were in the game and what you did;
- the logs:
  - **a game problem:** `triforce_log.txt` from your game folder (the folder with `Triforce.exe`). If you started the
    game again after the problem, send `triforce_log.1.txt` too: each start keeps the four runs before it as
    `triforce_log.1.txt` (the last) to `triforce_log.4.txt`.
  - **a Setup or build problem:** `setup.log` from `%LOCALAPPDATA%\Programs\Triforce`, and for a failed build the text
    from **Show details** on the build page.

Before you report:

- Try the [newest release](https://github.com/ruckus7382sfgsgdf/CBeta0.2/releases) first.
- Search the open issues; if yours is there, add your details to it.
- For graphics or performance problems, test without ReShade, overlays, shader-cache tools and frame-generation
  injectors: they change what Triforce does.
- If graphics look wrong after an update, clearing the shader caches often helps: close the game and delete
  `triforce_programs.bin`, `triforce_programs.idx`, `triforce_pipelines.bin`, `triforce_pipeline_states.bin`,
  `triforce_computes.bin`, `mkw_vk_pipelines.bin` and `triforce_prepared.bin` from the game folder (never
  `triforce_shaders.bin`). The next start prepares them again.

One topic per issue or pull request, in English, please.

## Ways to help

| | What |
|---|---|
| **Test on other hardware** | NVIDIA cards are the most tested. Reports from AMD and Intel cards and other drivers are useful even when everything works. |
| **Play further** | Reaching new areas, cutscenes, menus and items finds problems nobody has seen yet. |
| **Report what is broken** | As above, with the version, your hardware, where you were and the log. |
| **Send a fix** | Fork, fix, test, open a pull request. Small, focused changes are easiest to review. |
| **Improve the docs** | Corrections and clearer steps are welcome. |

## Development setup

You need your own supported copy of the game: program `0400486024DEC000`, update v131072, build `e0f3ccfa`. Other
versions do not work, and the build reads your files where they are without changing them. A build needs about 60 GB
free and takes around 40 minutes; a run uses about 10 GB of RAM. Build on an SSD if you can.

The easy way to get every tool a build needs is **Triforce Setup** from the
[releases](https://github.com/ruckus7382sfgsgdf/CBeta0.2/releases): it installs the tools (Python with its packages,
llvm-mingw, the Vulkan headers, FFmpeg, SDL2) into `%LOCALAPPDATA%\Programs\Triforce\tools`.

To build from a clone instead:

1. Install Python 3 from [python.org](https://www.python.org/downloads/) (not the Microsoft Store build) and the
   pipeline's packages: `python -m pip install -r requirements.txt`.
2. Get [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build) and set `HOOKSHOT_MINGW` to its
   folder.
3. From the installed tools folder, point `RX_CPM` at `tools\cpm` and `WGPU_NATIVE` at `tools\wgpu-native`, and copy
   `tools\third_party` into `third_party\` and `tools\work` into `work\`.
4. Run everything with `python -I` from the repository root.

(Some environment variables still carry the older `HOOKSHOT_` name; they work as documented.)

### Triforce Setup

The launcher lives in `setup/`: a .NET Framework 4.8 WPF app, built with `powershell -File setup\build.ps1` (needs the
C# compiler from Visual Studio Build Tools). `setup\pack_tools.ps1` packs the tools zip a release carries. Never run a
development build's install, update or uninstall without `TRIFORCE_TEST_ROOT` set to a scratch folder: it would act
on your real install.

## Building

| Command | What it does |
|---|---|
| `python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Triforce" --language en-US` | Build a complete game folder from a `.nup`. |
| `python -I nx2/make_game.py --romfs "C:\path\romfs" --exefs "C:\path\exefs" --out "C:\Games\Triforce"` | The same, from extracted folders. |
| `python -I nx2/make_game.py --check --nup "C:\path\your-game.nup"` | Check the game and its version without building. |

The translated C and every build are local files; they are never committed.

## Testing

The game-free checks need no game files:

```powershell
python -I -B tests/run_all.py
```

It runs every `tests/test_*.py` and `nx2/test_*.py`, the C++ unit tests under `gpu/vk/tests`, and a byte-compile of
every Python file, and ends with a table of results. Name a check to run only the ones whose path contains it. A new
`test_*.py` file is picked up by itself.

When you add a fix, add a game-free test where the behavior can be tested without the game. For a renderer or runtime
fix, say what you saw before and after, on which GPU and driver, and where in the game.

## Code conventions

- Match the file you are editing: its indentation, comment style and naming. Do not reformat unrelated code.
- Do not modify `third_party/`. Its parts are unmodified upstream copies, each with its notice in its own
  `PROVENANCE.md`.
- New switches use the `RX_` prefix in the renderer and `HOS_` in the runtime, are off unless asked for, and cost
  nothing while off.
- Rendering fixes must reproduce what the game itself does, not tuned factors that only look right on one screen.
- Python runs with `-I`; tests stay on the standard library. Keep line endings LF.

## Pull requests

1. Fork the repository and branch from `main`.
2. One topic per pull request: a fix does not carry a refactor.
3. Title: a short imperative sentence. Body: why it is needed, what changed, how it was tested (GPU, driver, game
   area, fps before and after), and the issue it fixes.
4. Screenshots for anything that shows on screen, before and after.
5. No build output, dumps or generated files. Check `git status`.

## Licensing

Triforce is free software under the [GNU General Public License, version 3 or later](LICENSE), like the Hookshot code
it builds on. By submitting a change you agree to license it under those terms. Do not paste code from a project with
an incompatible license, and never paste decompiled game code or anything generated from the game files.

The third-party parts (Eden, suyu, Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus
fonts, zbic and the a64-kit tools) keep their own notices. Preserve them.

Thanks for testing, reporting and sending fixes. Every report from a different GPU and every area played further
moves the port forward.
