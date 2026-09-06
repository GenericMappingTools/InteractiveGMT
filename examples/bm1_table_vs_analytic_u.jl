#!/usr/bin/env julia
#
# Catalina benchmark 1 — table vs analytic solution, VELOCITY (m/s, positive offshore), at t =
# 160/175/220 s. The free-surface twin of this is `bm1_table_vs_analytic.jl`, which is also where the
# comparison itself lives: this driver only picks the quantity, so the two figures cannot drift apart.
#
#   julia examples/bm1_table_vs_analytic_u.jl [outdir]     -> <outdir>/bm1_table_vs_analytic_u.png

include(joinpath(@__DIR__, "bm1_table_vs_analytic.jl"))

bm1_compare(:u; outdir = isempty(ARGS) ? raw"C:\TMP\claude" : ARGS[1])
