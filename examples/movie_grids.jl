using GMT
using InteractiveGMT

# All frames must share the same x/y grid geometry in the first backend.
x = collect(range(-5, 5; length=151))
frames = Any[]
for phase in range(0, 2pi; length=120)
    Z = Float32[sin(hypot(xx, yy) - phase) for yy in x, xx in x]
    push!(frames, GMT.mat2grid(Z; x=x, y=x))
end

fig = view_grid(first(frames))
out = movie(fig; frames=frames, name="wave", frame_rate=30, format=:mp4) do fig, f
    replace_grid!(fig, f.value; zrange=(-1.0, 1.0))
end
println(out)
