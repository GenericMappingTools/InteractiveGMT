# Smoke test for poly2fv + the f3dview front-door.
using InteractiveGMT
using GMT

# Two closed 3-D polygons (squares at different heights) as a Vector{GMTdataset}.
sq(x0, y0, z) = [x0 y0 z; x0+1 y0 z; x0+1 y0+1 z; x0 y0+1 z; x0 y0 z]   # closed ring
D = [mat2ds(sq(0, 0, 0.0)), mat2ds(sq(2, 0, 1.0)), mat2ds(sq(1, 2, 2.0))]

fv = poly2fv(D)
println("poly2fv -> verts=", size(fv.verts, 1), " groups=", length(fv.faces),
		" colours=", sum(length, fv.color), " zscale=", round(fv.zscale, digits=3))

fig  = f3dview(D)                         # dataset polygons -> poly2fv -> view_fv
println("f3dview(polys) -> ", typeof(fig), " alive=", isalive(fig))
fig2 = f3dview("peaks")                   # demo grid
println("f3dview(\"peaks\") -> ", typeof(fig2), " alive=", isalive(fig2))
fig3 = f3dview(torus(R=6))                # bare GMTfv (colourless) -> demo z-ramp
println("f3dview(torus()) -> ", typeof(fig3), " alive=", isalive(fig3))

abspath(PROGRAM_FILE) == @__FILE__ && wait_windows()
