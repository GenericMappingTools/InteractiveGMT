# PngQuant — lossy PNG compression (RGBA -> 8-bit palette)

`InteractiveGMT.PngQuant` is a Julia port of **pngquant 3 / libimagequant 4.5** by Kornel Lesiński
(<https://github.com/kornelski/pngquant>, library at <https://github.com/ImageOptim/libimagequant>).
It converts truecolour RGBA images into 8-bit palette images with alpha, and writes them as
indexed PNGs (PLTE + tRNS), typically 60-80% smaller.

libimagequant © 2009-2018 Kornel Lesiński, derived from code by Jef Poskanzer and Greg Roelofs,
GPL v3 or later. This port is distributed under the same terms.

## Quick start

```julia
PQ = InteractiveGMT.PngQuant

# like `pngquant --quality 65-80 photo.png` -> photo-fs8.png
PQ.pngquant("photo.png"; quality="65-80")

# in memory: A is (height, width, 3|4) UInt8, A[1,1,:] = top-left
idx, pal, q = PQ.pngquant(A; ncolors=64, nofs=true)   # idx: 0-based indices, pal: Vector{RGBA}

# the library API, step by step
liq = PQ.LiqAttr(); PQ.set_speed!(liq, 3); PQ.set_quality!(liq, 70, 95)
img = PQ.LiqImage(liq, A)
res = PQ.quantize(liq, img)
PQ.set_dithering_level!(res, 1.0)
pal, idx = PQ.remapped(res, img)
PQ.write_png8("out.png", idx, img.width, img.height, pal)
```

File options follow the CLI: `ncolors`, `quality` (`"N"`, `"-N"`, `"N-"`, `"N-M"` or a tuple),
`speed` (1-11), `floyd`, `nofs`, `posterize`, `output`, `ext`, `force`, `skip_if_larger`,
`last_index_transparent`, `verbose`. A quality floor that can't be met throws
`LiqError(:QualityTooLow)` and writes nothing (CLI exit 99); `skip_if_larger` throws
`LiqError(:TooLargeFile)` (exit 98). Histograms shared by several images: `LiqHistogram`,
`add_image!`, `add_colors!`, `add_fixed_color!`, `quantize(hist, attr)`.

## Source layout

| Julia | Rust / C original |
|---|---|
| `pal.jl` | `pal.rs` — colour types, premultiplied internal-gamma pixel, palettes |
| `attr.jl` | `attr.rs`, `error.rs`, quality<->MSE of `quant.rs` |
| `image.jl` | `image.rs`, `rows.rs`, `blur.rs` — contrast/noise maps, dither map |
| `hist.jl` | `hist.rs` — colour histogram, clusters |
| `nearest.jl` | `nearest.rs` — vantage-point tree |
| `mediancut.jl` | `mediancut.rs` |
| `kmeans.jl` | `kmeans.rs` |
| `quant.jl` | `quant.rs` — feedback loop, refinement, palette sort, remap entry |
| `remap.jl` | `remap.rs` — remapping, Floyd-Steinberg |
| `png.jl` | `rwpng.c` — PNG read (all types/depths, Adam7) and indexed write; zlib by ccall |
| `frontend.jl` | `pngquant.c` — per-file flow and options |

## Differences from the original

Single-threaded (the Rust splits K-Means and dithering across threads; its dithering then runs in
chunks). No low-memory row streaming, row callbacks, background image (GIF "keep") or progress
callback. PNG side: gAMA is honoured and an sRGB chunk carried over, as in a pngquant built without
lcms2 — iCCP/cHRM are not applied; metadata chunks are not copied (as `--strip`); no stdin/stdout,
no `--map`.

## Verification

Checked against the Rust libimagequant 4.5.0 itself (single-threaded build, same RGBA as raw
bytes): identical quality and MSE on alpha images (`test/test-pngquant-unit.jl` pins them), and
on a 1200x640 photo-like PNG Q=61 both, MSE 12.51 vs 12.53 — the remaining difference is hash-map
iteration order, which Rust does not fix either.
