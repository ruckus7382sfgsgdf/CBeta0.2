# RenoDX

The optional RenoDX HDR support (`gpu/vk/vk_renodx.cpp`, `gpu/vk/vk_renodx.h`, `scripts/renodx_shaders.py` and the
RenoDX rows of the Insert menu) is worked from RenoDX, by Carlos Lopez (clshortfuse), and from its mod for The Legend
of Zelda: Ocarina of Time (`src/games/zelda-oot-oz`):

- https://github.com/clshortfuse/renodx

None of RenoDX's source or shaders is kept in this repository. A build with `--renodx` downloads them at one pinned
commit and compiles them on the builder's own PC (`scripts/renodx_shaders.py`). The parts written here follow the
mod's addon (`addon.cpp`: its settings, its eye adaptation's state and the constants its shaders read), so RenoDX's
notice is kept with them, as its license asks.

The mod's source files carry this notice:

```
Copyright (C) 2026 Carlos Lopez
SPDX-License-Identifier: MIT
```

RenoDX's license, as its repository has it (`LICENSE`):

```
MIT License

Copyright (c) 2025 Carlos Lopez Jr.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
