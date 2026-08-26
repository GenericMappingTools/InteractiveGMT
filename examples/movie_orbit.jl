using GMT
using InteractiveGMT

G = GMT.peaks()
fig = view_grid(G)

# Julia's do-block is passed as the first positional argument, hence this dispatches to
# InteractiveGMT's extension of GMT.movie rather than GMT.jl's script-recording movie method.
out = movie(fig; frames=180, name="peaks_orbit", frame_rate=30, format=:mp4) do fig, f
    orbit!(fig, 2.0, 0.0)
end
println(out)
