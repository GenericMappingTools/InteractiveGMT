# Potrace.jl — Julia port of the tracing core of Potrace 1.16 by Peter Selinger
# (C:\programs\compa_libs\potrace-1.16\src): decompose.c, trace.c, curve.c, potracelib.c.
#
# Copyright (C) 2001-2019 Peter Selinger (the original C). Potrace is free software covered by the
# GNU General Public License; this port is distributed under the same terms.
#
# The algorithm is ported function by function, same names, same order of operations, so a reader
# can follow it side by side with the C. Only the containers changed: calloc'd arrays -> Vectors,
# the `goto`/TRY error plumbing -> nothing (Julia throws), the singly-linked list macros of lists.h
# -> the tiny hook helpers in types.jl, which keep the C's exact list surgery in pathlist_to_tree.
#
# NOT ported: the frontend (main.c, getopt, bitmap_io, greymap, mkbitmap, progress bars) and the
# backends. The writers in output.jl (SVG, EPS, GMTdataset) are new and write the same geometry
# the C backends write.
#
# Coordinates: potrace counts y from the BOTTOM — pixel (x,y) has its lower-left corner at the path
# point (x,y), and the traced curves live in [0,w]x[0,h] with y up. `PotraceBitmap(M)` takes a
# matrix the way an image is displayed (M[1,1] = top-left) and maps it into that frame.
module Potrace

using GMT: GMT, GMTdataset

export PotraceParams, PotraceBitmap, PotraceResult, potrace, paths, potrace_svg, potrace_eps, potrace_pdf, potrace_gmtds

include("types.jl")
include("decompose.jl")
include("trace.jl")
include("output.jl")

end
