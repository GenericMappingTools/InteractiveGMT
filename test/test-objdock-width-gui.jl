# The Scene Objects dock opens at 3 cm (objDockOpenWidthPx) in EVERY window, however many were
# opened and closed before it. The "already sized" mark used to be a static set of Scene pointers
# that was never cleared, so a window allocated at a closed one's address opened ~5.7 cm wide.

@testitem "Scene Objects dock: every new window opens at the same 3 cm width" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	function dockw(h)
		buf = zeros(UInt8, 160)
		r = ccall(GmtvtkTest._test_fn(:gmtvtk_dock_probe_test), Cint, (Ptr{Cvoid}, Cstring, Cint, Ptr{UInt8}, Cint),
		          h, "sceneObjectsDock", 0, buf, 160)
		r == 1 || return -1
		return parse(Int, split(unsafe_string(pointer(buf)), ',')[3])
	end
	widths = Int[]
	for k in 1:6
		f = iview(IG.GMT.peaks())
		try
			for _ in 1:20; IG._pump_once(); sleep(0.02); end
			push!(widths, dockw(f.h))
		finally
			ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f.h)
			for _ in 1:5; IG._pump_once(); end
		end
	end
	@test all(>(0), widths)
	@test all(==(widths[1]), widths)
	all(==(widths[1]), widths) || @info "Scene Objects dock widths per window" widths
end
