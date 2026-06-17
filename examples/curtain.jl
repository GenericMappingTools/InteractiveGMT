# Vertical "curtain" demo — a Fledermaus-style seismic profile hung as a vertical wall UNDER the
# bathymetry, along a ship track. Ported from GMTF3D's `grid_vcurtain` gallery example.
#
# A real seismic-profile image (deps/assets/seismic_E46.jpg, the WSW–ENE E46/E52 profile in the
# Gulf of Cádiz) follows a straight two-point track over a fixed `zrange` (0 down to -10000 m).
# `clip=true` cuts the wall's top edge to the seafloor so the part of the image above the relief
# is dropped (sub-seafloor section only).
#
# Run:   julia examples/curtain.jl   (NEEDS NETWORK — grdcut fetches @earth_relief_04m)
# or in the REPL:  include("examples/curtain.jl")

using InteractiveGMT
using GMT

const SEISMIC_IMG = normpath(joinpath(@__DIR__, "..", "deps", "assets", "seismic_E46.jpg"))

# Gulf of Cádiz bathymetry (cut from GMT's remote earth_relief).
G = GMT.grdcut("@earth_relief_04m", region=(-12, 0, 35, 45))

# Hang the seismic profile along the two-point WSW–ENE track, clipped to the seafloor.
# Form 1 — the `vcurtain=` kwarg of view_grid (one spec, or a vector of specs):
fig = view_grid(G; cmap=:geo, title="GMT 3-D Viewer  —  vertical curtain",
                vcurtain = (; image  = SEISMIC_IMG,
                              path   = [-11.045 36.077; -6.9846 36.1846],
                              zrange = (-10000.0, 0.0),
                              clip   = true))

# Form 2 — equivalently, add it after the window is up with add_curtain!:
#   fig = view_grid(G; cmap=:geo)
#   add_curtain!(fig, [-11.045 36.077; -6.9846 36.1846];
#                image=SEISMIC_IMG, zrange=(-10000.0, 0.0), clip=true)

# Script mode has no REPL: keep the process alive until the window closes.
abspath(PROGRAM_FILE) == @__FILE__ && wait_windows()
