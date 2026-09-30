# PngQuant.jl — Julia port of pngquant 3 / libimagequant 4 by Kornel Lesiński
# (https://github.com/kornelski/pngquant, lib/ = https://github.com/ImageOptim/libimagequant):
# lossy conversion of truecolour RGBA images to 8-bit palette images with alpha.
#
# libimagequant © 2009-2018 Kornel Lesiński, derived from code by Jef Poskanzer and Greg Roelofs,
# licensed under GPL v3 or later (lib/COPYRIGHT). This port is distributed under the same terms.
#
# Ported file by file from the Rust sources, same function names and the same order of operations,
# so a reader can follow it side by side: pal.rs -> pal.jl, attr.rs + error.rs -> attr.jl,
# image.rs + rows.rs + blur.rs -> image.jl, hist.rs -> hist.jl, nearest.rs -> nearest.jl,
# mediancut.rs -> mediancut.jl, kmeans.rs -> kmeans.jl, quant.rs -> quant.jl, remap.rs -> remap.jl.
# pngquant.c (the CLI) -> frontend.jl, rwpng.c (libpng I/O) -> png.jl.
#
# Differences from the original, all in the plumbing: single-threaded (the Rust's rayon splits
# K-Means and dithering across threads — dithering then runs in chunks, with small seams); the
# float copy of the pixels is always made (no low-memory row streaming); no row callbacks, no
# background image (GIF "keep" mode), no progress callback.
#
# Pipeline: histogram of the colours (weighted by a noise/edge "importance" map) -> median cut
# repeated in a feedback loop with K-Means, reweighting badly-served colours -> K-Means
# refinement -> remap each pixel to its nearest palette colour (vantage-point tree), with
# Floyd-Steinberg dithering restricted to flat areas by a dither map.
module PngQuant

export pngquant, read_png, write_png8, RGBA, LiqAttr, LiqImage, LiqHistogram, LiqResult, LiqError,
       set_quality!, set_speed!, set_max_colors!, set_min_posterization!, set_last_index_transparent!,
       set_importance_map!, add_fixed_color!, add_image!, add_colors!, quantize, remapped, palette,
       set_dithering_level!, set_output_gamma!, quantization_quality, quantization_error,
       remapping_quality, remapping_error

include("pal.jl")
include("attr.jl")
include("image.jl")
include("hist.jl")
include("nearest.jl")
include("mediancut.jl")
include("kmeans.jl")
include("quant.jl")
include("remap.jl")
include("png.jl")
include("frontend.jl")

end
