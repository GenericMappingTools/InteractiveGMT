# Smoke test for view_fv: a hand-built coloured unit cube GMTfv.
using InteractiveGMT
using GMT

V = [0.0 0 0; 1 0 0; 1 1 0; 0 1 0; 0 0 1; 1 0 1; 1 1 1; 0 1 1]          # 8 verts
F = [1 4 3 2; 5 6 7 8; 1 2 6 5; 2 3 7 6; 3 4 8 7; 4 1 5 8]              # 6 quad faces (1-based)
cols = ["-G#ff0000", "-G#00ff00", "-G#0000ff", "-G#ffff00", "-G#ff00ff", "-G#00ffff"]
bb = [0.0, 1, 0, 1, 0, 1]
fv = GMT.GMTfv(verts=V, faces=[F], color=[cols], bbox=bb, isflat=[false], zscale=1.0)

fig = view_fv(fv; color=:explicit, edges=true, title="view_fv smoke test — coloured cube")
println("view_fv returned: ", fig, "  alive=", isalive(fig))

# Script mode: keep the window alive (in the REPL the Qt loop is pumped automatically).
abspath(PROGRAM_FILE) == @__FILE__ && wait_windows()
