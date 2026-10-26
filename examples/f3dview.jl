# Smoke test for poly2fv + the viz front-door.
using InteractiveGMT

# Two closed 3-D polygons (squares at different heights) as a Vector{GMTdataset}.
sq(x0, y0, z) = [x0 y0 z; x0+1 y0 z; x0+1 y0+1 z; x0 y0+1 z; x0 y0 z]   # closed ring
D = [mat2ds(sq(0, 0, 0.0)), mat2ds(sq(2, 0, 1.0)), mat2ds(sq(1, 2, 2.0))]

fv = poly2fv(D)
println("poly2fv -> verts=", size(fv.verts, 1), " groups=", length(fv.faces),
		" colours=", sum(length, fv.color), " zscale=", round(fv.zscale, digits=3))

fig  = viz(D)                             # dataset polygons -> poly2fv -> mesh window
println("viz(polys) -> ", typeof(fig), " alive=", isalive(fig))
fig2 = viz("peaks")                       # demo grid
println("viz(\"peaks\") -> ", typeof(fig2), " alive=", isalive(fig2))
fig3 = viz(torus(R=6))                    # bare GMTfv (colourless) -> demo z-ramp
println("viz(torus()) -> ", typeof(fig3), " alive=", isalive(fig3))

abspath(PROGRAM_FILE) == @__FILE__ && wait_windows()
