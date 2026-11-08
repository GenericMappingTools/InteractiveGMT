# NSWING — iGMT's own solver (deps/src/nswing/nswing.c inside gmtvtk.dll), run the way every NSWING
# door runs it (_nswing_run_c, src/nswing.jl).
#
# WHY IT IS iGMT'S OWN: GMT's nswing started a nested grid by interpolating its parent with land nodes
# masked out and the bicubic weights renormalised over what was left. Where a child node sits on a
# land parent node the leftover weights nearly cancel (~1e-10), and dividing by them wrote ~1e9 into
# the child's initial state. Which nodes blew up depended on the last bits of the grid coordinates,
# so the same run from files and from in-memory grids blew up in DIFFERENT places (2026-11-08). The
# copy restores Mirone's unmasked interpolation; it goes away once GMT_jll ships the fix.
#
# The case below is the smallest one that sets the trap: single-node rocks in the parent (land with
# wet neighbours on all four sides, which is what makes the leftover weights cancel), inside a nested
# grid that does not have them, at non-binary geographic increments (1/60 and 1/300 degree, so child
# nodes land a rounding error off the parent nodes). GMT's own nswing starts this run with 7.5e11 in
# the child (measured 2026-11-08, installed GMT 6.8, every placement tried). It also pins that the
# solver reads a grid identically whatever its memory layout: the window's TRB grids and BCB ones give
# the same bits. Tier: unit (the solver needs the DLL, no window).
@testitem "nswing: a nested start over rocks the child lacks stays bounded, in either grid layout" tags=[:nswing] begin
	IG = InteractiveGMT; GMT = IG.GMT

	dP = 1 / 60;  dC = dP / 5
	xp = [-10.0 + i * dP for i in 0:80];  yp = [36.0 + j * dP for j in 0:60]
	zp = fill(-50.0, length(yp), length(xp))
	for j in 18:3:42, i in 33:3:57          # single-node rocks inside the nest's area
		zp[j, i] = 5.0
	end
	src = [exp(-((x - xp[46])^2 + (y - yp[31])^2) / (2 * (10dP)^2)) for y in yp, x in xp]
	# The child, placed by the nesting rule (corner = parent node + half a parent cell + half a child
	# cell): all water, no rocks.
	xc = [xp[31] + dP / 2 + dC / 2 + k * dC for k in 0:149]
	yc = [yp[16] + dP / 2 + dC / 2 + k * dC for k in 0:149]
	zc = fill(-50.0, length(yc), length(xc))

	mk(z, x, y) = GMT.mat2grid(Float32.(z), x = x, y = y)
	gBCB = GMT.GMTgrid[mk(zp, xp, yp), mk(src, xp, yp), mk(zc, xc, yc)]
	# The same grids in the window's layout, TRB (row-major, north row first), with the header untouched:
	# only the buffer order differs. Not through a file — a write/read round trip moves the grid edges
	# by ~1e-13, which is the file-versus-memory difference, not a layout one.
	function trb(G)
		H = deepcopy(G)
		ny, nx = size(G.z)
		H.z = reshape(vec(permutedims(reverse(G.z, dims = 1))), ny, nx)
		H.layout = "TRB"
		return H
	end
	gTRB = GMT.GMTgrid[trb(G) for G in gBCB]
	@test all(G -> IG._grid_layout_code(G) == 0, gBCB)
	@test all(G -> IG._grid_layout_code(G) == 3, gTRB)
	@test IG._zmat(gTRB[1]) == gBCB[1].z                # same grid, other order
	tmp = mktempdir()

	run(dir, grids) = IG._nswing_run_c(["-Gtsu,1", "-N10", "-t2", "-fg"], grids, nothing; dir = dir)
	dA = mkpath(joinpath(tmp, "A"));  dB = mkpath(joinpath(tmp, "B"))
	@test run(dA, gBCB) == ""
	@test run(dB, gTRB) == ""
	@test isfile(joinpath(dA, "tsu.nc")) && isfile(joinpath(dB, "tsu.nc"))   # relative -G, resolved in `dir`

	slice(d, k) = GMT.gmt("read -Tg " * joinpath(d, "tsu.nc") * "?z[$k]").z
	for k in (0, 1, 5, 10)
		a = slice(dA, k);  b = slice(dB, k)
		@test isequal(a, b)                             # same bits from either layout
		@test maximum(abs, filter(isfinite, a)) < 10    # a 1 m hump over 50 m of water
	end
end
