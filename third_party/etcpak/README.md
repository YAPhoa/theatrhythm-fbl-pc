Texture block encoders and decoder for the DLC maker's core (`tools/dlcmaker/core/image.cpp`), which
writes the game's BC7 and DXT1 (BC1) textures with the same bytes earlier (Python) releases wrote through the
`etcpak` package, and reads them back:

- `bc7enc.cpp`, `bc7enc.h`: Richard Geldreich, Jr.'s BC7 encoder (MIT or public domain, at the end of
  `bc7enc.cpp`).
- `ProcessDxtc.cpp`, `ProcessDxtc.hpp`, `Dither.cpp`, `Dither.hpp`, `Math.hpp`, `ForceInline.hpp`:
  Bartosz Taudul's etcpak, its BC1 encoder (BSD 3-Clause, `LICENSE`).
- `bcdec.h`: Sergii Kudlai's BC decoder (MIT or the Unlicense, at the end of the file).

All taken unchanged (line endings made LF) from the `etcpak` 0.9.15 source distribution on PyPI
(https://pypi.org/project/etcpak/0.9.15/, K0lb3's Python bindings, MIT), `src/etcpak/`.
