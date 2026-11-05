# Grid Tools > Contours (src/contours.jl): the memory guard that stops a trace which would not fit.

@testitem "contours: the memory guard lets a normal trace through and refuses one that would not fit" tags=[:unit, :contours] begin
	IG = InteractiveGMT
	x = collect(range(0, 10, length = 300))
	G = IG.GMT.mat2grid(Float32[sin(a) * cos(b) for b in x, a in x], x = x, y = x)
	sets = IG._contour_gdal(G, collect(range(-0.9, 0.9, length = 10)), 0)
	@test length(sets) == 10 && all(s -> s[5] > 0, sets)        # guarded trace still traces everything
	@test IG._CONTOUR_MEM.need[] == 0                             # ...and was never stopped
	nul = reinterpret(Cstring, C_NULL)
	# What the callback says to GDAL: go on while the traced vertices, carried through the Apply, fit…
	IG._CONTOUR_MEM.avail0[] = IG._mem_available();  IG._CONTOUR_MEM.next[] = 0.0
	@test IG._contour_progress(0.5, nul, C_NULL) == 1
	# …stop when what has already been traced could not be drawn (16 TiB "used" since the trace began).
	IG._CONTOUR_MEM.avail0[] = IG._mem_available() + 2^44;  IG._CONTOUR_MEM.next[] = 0.0
	@test IG._contour_progress(0.5, nul, C_NULL) == 0
	@test IG._CONTOUR_MEM.need[] > 2^44
	# The draw step's check names what it refused and what is free.
	e = try IG._contour_mem_check(2^50, "draw the contours"); "" catch err; sprint(showerror, err) end
	@test occursin("not enough free memory to draw the contours", e) && occursin("GB are free", e)
	IG._CONTOUR_MEM.need[] = 0
end
