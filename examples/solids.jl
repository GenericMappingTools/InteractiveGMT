# Smoke test for the SOLIDS catalogue, dispatched through viz(name).
using InteractiveGMT

const SOLIDS = InteractiveGMT.SOLIDS          # the named-generator catalogue (unexported)

println("SOLIDS: ", sort(collect(keys(SOLIDS))))
for nm in ("cube", "sphere", "torus", "icosahedron")
	fv = SOLIDS[nm]()                 # default generator
	println(nm, ": verts=", size(fv.verts, 1), " groups=", length(fv.faces))
end

fig  = viz("torus"; R=8, nx=120, edges=true)
println("viz(\"torus\") -> ", typeof(fig), "  alive=", isalive(fig))
fig2 = viz("cube"; r=3)
println("viz(\"cube\"; r=3) alive=", isalive(fig2))

abspath(PROGRAM_FILE) == @__FILE__ && wait_windows()
