APNG Optimizer
===================

`apngopt` is a command-line optimizer for animated PNG files. This fork keeps
the original APNG optimization model, updates the dependency stack, and adds
new compression, safety, and threading controls intended for modern batch
workflows.

The default path is tuned for practical throughput: strong size reduction with
low wall-clock time. Slower high-compression modes are available when final
file size matters more than CPU time.

Highlights
----------

- Moved to CMake build system for easy cross-platform builds, 
  removed legacy VS2010-2013 projects and Makefile.
- Updated vendored compression stack: zlib, libpng, zopfli, libimagequant, and
  7z Deflate to latest versions as of May 2026.
- More robust imagequant path with configurable quality, speed, palette, dither,
  posterization, and thread controls.
- Safer PNG/APNG I/O handling with overflow-checked buffer sizing and better
  write/error propagation.
- Parallelized local optimization stages, including candidate compression
  evaluation.
- Optional exact candidate selection for `-z1` and `-z2`, allowing the chosen
  backend to decide the best frame candidate rather than relying only on fast
  zlib estimates.
- Fine-grained final zlib controls for `-z0`.
- CMake presets for Windows MSVC and Linux builds, with Linux verified both
  with and without imagequant/Cargo.

Usage
-----

```bash
apngopt [options] input.png [output.png]
```

Core compression modes:

```text
-z0  zlib compression (default)
-z1  7zip Deflate compression
-z2  zopfli compression
-iN  iteration count for 7zip/zopfli modes, default -i15
-dN  disable imagequant path when N > 0
```

Imagequant controls:

```text
--liq-speed=N      imagequant speed, 1..10, default 4
--liq-colors=N     maximum palette colors, 2..256, default 256
--liq-posterize=N  posterization bits, 0..4, default 0
--liq-dither=F     dithering level, 0..1, default 1.0
--liq-quality=A-B  quality range, 0..100, default 50-100
--liq-threads=N    imagequant/Rayon thread count
```

Threading and candidate selection:

```text
--mt=N              local worker count for independent optimizer stages
--exact-candidates  for -z1/-z2, recompress valid frame candidates with the
                    selected backend before choosing the best candidate
```

Final zlib controls (`-z0` only):

```text
--zlib-mode=M       preset: speed, balanced, size
--zlib-level=N      compression level, 1..9
--zlib-mem-level=N  zlib memLevel, 1..9
--zlib-strategy=S   default, filtered, huffman, rle, fixed
```

Behavior notes:

- `--exact-candidates` only applies to `-z1` and `-z2`.
- `--zlib-*` options only affect final `-z0` compression. They do not change
  the fast pre-evaluation streams used for candidate ranking.
- `--zlib-mode` provides defaults; explicit `--zlib-level`,
  `--zlib-mem-level`, and `--zlib-strategy` override those defaults.
- `--mt` can improve candidate evaluation and independent cleanup stages, but
  APNG frame disposal semantics still keep the main frame pipeline sequential.

Recommended Presets
-------------------

Fast default, usually the best starting point:

```bash
apngopt input.png output.png
```

Fast zlib size mode, useful when zopfli/7zip latency is not worth it:

```bash
apngopt -z0 --zlib-mode=size --zlib-level=9 --zlib-mem-level=9 --zlib-strategy=filtered --mt=16 --liq-threads=16 input.png output.png
```

Maximum-size-reduction mode, slower by design:

```bash
apngopt -z2 -i15 --exact-candidates --mt=16 --liq-threads=16 input.png output.png
```

Legacy-style comparison mode, useful for regression checks:

```bash
apngopt -z1 -i15 -d1 input.png output.png
```

Build
-----

Windows MSVC release build:

```bat
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Linux release build without imagequant/Cargo:

```bash
cmake --preset linux-release-no-imagequant
cmake --build --preset linux-release-no-imagequant
```

Linux release build with imagequant:

```bash
cmake --preset linux-release-imagequant
cmake --build --preset linux-release-imagequant
```

Build notes:

- The project uses vendored dependencies under `vendor/`.
- Rust/Cargo is required only when `APNGOPT_ENABLE_IMAGEQUANT=ON`.
- `linux-release-no-imagequant` builds without Cargo and uses the existing
  downconvert fallback path.
- `-z1` uses vendored 7z Deflate sources from `vendor/7z2601`.

Benchmark Snapshot
------------------

These numbers are from a local Windows MSVC Release build on a small corpus of
four APNG files, each roughly 0.55-1.04 MB. They are intended to show relative
behavior, not universal performance guarantees.

Baseline:

```text
old apngopt -z1 -i15
```

| Scenario | Command shape | Mean time | Mean output/original | Compared with old `-z1 -i15` |
|---|---|---:|---:|---|
| Old z1 baseline | `old apngopt -z1 -i15` | 9.798 s | 63.915% | baseline |
| Old zopfli | `old apngopt -z2 -i15` | 52.829 s | 63.595% | 5.4x slower, nearly same size |
| New default | `apngopt` | 0.496 s | 31.012% | 19.7x faster, about 51.5% smaller output |
| New legacy-style path | `apngopt -z1 -i15 -d1` | 11.972 s | 63.995% | 1.2x slower, nearly same size |
| New tuned zlib size path | `apngopt -z0 --zlib-mode=size --zlib-level=9 --zlib-mem-level=9 --zlib-strategy=filtered --mt=16 --liq-threads=16` | 0.433 s | 33.270% | 22.6x faster, about 48.0% smaller output |
| New extreme zopfli path | `apngopt -z2 -i15 --exact-candidates --mt=16 --liq-threads=16` | 21.874 s | 29.097% | 2.2x slower, about 54.5% smaller output |

Interpretation:

- The new default path is the best general-purpose profile in this benchmark:
  much faster than the old z1 baseline and much smaller.
- The tuned zlib profile is the fastest measured profile while still producing
  outputs far smaller than the old baseline.
- The extreme zopfli profile is intentionally slower. It spends more CPU on
  backend compression and exact candidate selection to get the smallest output
  in this benchmark.
- The legacy-style path is useful for checking behavioral drift. It bypasses
  the new imagequant path and stays close to old output sizes, with a modest
  runtime cost from the newer safety/refactor path.

Version History
---------------

- 1.4.1: imagequant path, zlib default path, and `-d` compatibility switch.
- 1.4: updated codebase based on apngdis 2.8 and apngasm 2.9.
- 1.3: updated codebase based on apngdis 2.7 and apngasm 2.9; added 7zip and zopfli options.
- 1.2: updated codebase based on apngdis 2.5 and apngasm 2.7; joined identical frames.
- 1.1: updated codebase based on apngdis 2.4 and apngasm 2.5; improved optimization.
- 1.0: initial release based on apngdis 2.3 and apngasm 2.3.

License
-------

This project is distributed under the zlib license.
