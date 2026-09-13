# Unit tests for the IGRF 3-D field-line tracer (src/magfield.jl). No window: this is the maths
# the "Magnetic field lines (3-D)" window draws, checked against what a magnetic field must do.

@testitem "magfield: B from magref, in geocentric Cartesian" tags=[:unit] begin
	IG = InteractiveGMT
	R = IG._MAG_R
	# On the surface at 45N/0E, the IGRF field of 2025 points down and north: Z (down) dominates.
	P = [R * cosd(45.0) 0.0 R * sind(45.0)]
	B, F, r = IG._mag_B_cart(P, 2025.0)
	@test isapprox(r[1], R; rtol = 1e-12)
	@test isapprox(sqrt(sum(abs2, B[1,:])), F[1]; rtol = 1e-6)   # |B| really is the model's F
	@test F[1] > 40000 && F[1] < 55000                            # nT, mid-latitude surface field
	# The field falls off like a dipole: ~1/r^3 with distance.
	P2 = 2.0 .* P
	_, F2, _ = IG._mag_B_cart(P2, 2025.0)
	@test isapprox(F[1] / F2[1], 8.0; rtol = 0.15)
	# ...which is only askable at all because magref is NOT limited to the surface.
	P8 = 8.0 .* P
	_, F8, _ = IG._mag_B_cart(P8, 2025.0)
	@test F8[1] > 0 && F8[1] < F2[1]
end

@testitem "magfield: seeds sit on the sphere, in both hemispheres" tags=[:unit] begin
	IG = InteractiveGMT
	S = IG._mag_seeds(6, 3, 50.0, 70.0)
	@test size(S) == (36, 3)                                       # 6 longitudes x 3 rings x 2 hemispheres
	rr = [sqrt(sum(abs2, S[i,:])) for i in axes(S,1)]
	@test all(isapprox.(rr, IG._MAG_R * 1.0005; rtol = 1e-9))
	lat = [asind(S[i,3] / rr[i]) for i in axes(S,1)]
	@test count(>(0), lat) == count(<(0), lat)
	@test isapprox(maximum(lat), 70.0; atol = 1e-9)
	@test isapprox(minimum(lat), -70.0; atol = 1e-9)
end

@testitem "magfield: traced lines are field lines" tags=[:unit] begin
	IG = InteractiveGMT
	S = IG._mag_seeds(4, 2, 55.0, 70.0)
	L = IG._mag_trace(S, 2025.0; rmax = 6.0, maxsteps = 1200)
	@test length(L) == size(S, 1)
	@test all(size(M, 2) == 4 for M in L)
	@test all(size(M, 1) > 10 for M in L)
	for M in L
		rr = [sqrt(sum(abs2, M[i,1:3])) for i in axes(M,1)]
		@test isapprox(rr[1], 1.0005; rtol = 1e-6)                 # starts on the surface (Earth radii)
		@test maximum(rr) > 1.05                                   # and leaves it
		@test rr[end] <= 1.0001 || rr[end] >= 6.0 * 0.99 || size(M,1) >= 1200
		# EVERY step follows B: the segment direction is parallel to the field at its start.
		for i in (2, div(size(M,1), 2))
			p = M[i, 1:3] .* IG._MAG_R
			B, _, _ = IG._mag_B_cart(reshape(p, 1, 3), 2025.0)
			seg = M[i+1, 1:3] .- M[i, 1:3]
			u = seg ./ sqrt(sum(abs2, seg))
			b = vec(B) ./ sqrt(sum(abs2, B))
			@test isapprox(abs(sum(u .* b)), 1.0; atol = 0.02)     # parallel or antiparallel
		end
		@test all(M[:,4] .> 0)                                     # |B| carried alongside, for the colours
	end
	# A closed line comes back down in the OTHER hemisphere.
	closed = [M for M in L if sqrt(sum(abs2, M[end,1:3])) <= 1.0001]
	@test !isempty(closed)
	for M in closed
		@test sign(M[1,3]) != sign(M[end,3])
	end
end

@testitem "magfield: the date really picks a different model" tags=[:unit] begin
	IG = InteractiveGMT
	P = [IG._MAG_R 0.0 0.0]
	_, F1900, _ = IG._mag_B_cart(P, 1900.0)
	_, F2025, _ = IG._mag_B_cart(P, 2025.0)
	@test !isapprox(F1900[1], F2025[1]; rtol = 1e-3)               # secular variation, 125 years of it
	# Outside the model's window the tool refuses instead of silently answering with the last date.
	counts = zeros(Cint, 16);  err = zeros(UInt8, 512)
	bad = IG._on_magfield_lines(Base.unsafe_convert(Cstring, "1899/2/1/60/60/3.0/50"),
	                            Ptr{Cdouble}(C_NULL), Cint(0), pointer(counts), Cint(16),
	                            pointer(err), Cint(512))
	@test bad == 0
	@test occursin("1900-2030", unsafe_string(pointer(err)))
end

@testitem "magfield: the C two-phase protocol hands over what it promised" tags=[:unit] begin
	IG = InteractiveGMT
	params = "2025.0/3/1/60/60/4.0/600"
	counts = zeros(Cint, 64);  err = zeros(UInt8, 512)
	n = IG._on_magfield_lines(Base.unsafe_convert(Cstring, params), Ptr{Cdouble}(C_NULL), Cint(0),
	                          pointer(counts), Cint(64), pointer(err), Cint(512))
	@test n == 6                                                   # 3 longitudes x 1 ring x 2 hemispheres
	npts = sum(Int.(counts[1:n]))
	@test npts > 0
	buf = zeros(Cdouble, npts * 4)
	n2 = IG._on_magfield_lines(Base.unsafe_convert(Cstring, params), pointer(buf), Cint(length(buf)),
	                           pointer(counts), Cint(64), pointer(err), Cint(512))
	@test n2 == n
	@test sqrt(buf[1]^2 + buf[2]^2 + buf[3]^2) ≈ 1.0005 rtol=1e-6
	@test buf[4] > 0
	# A buffer that is too small is an error, not a memory stomp.
	small = zeros(Cdouble, 8)
	@test IG._on_magfield_lines(Base.unsafe_convert(Cstring, params), pointer(small), Cint(8),
	                            pointer(counts), Cint(64), pointer(err), Cint(512)) == 0
	@test occursin("too small", unsafe_string(pointer(err)))
end

@testitem "magfield: the globe's texture is a real RGB(A) raster" tags=[:unit] begin
	IG = InteractiveGMT
	dims = zeros(Cint, 3);  err = zeros(UInt8, 512)
	@test IG._on_magfield_texture(Cint(0), Cdouble(2025.0), Ptr{UInt8}(C_NULL), Cint(0),
	                              pointer(dims), pointer(err), Cint(512)) == 1
	@test dims[1] > 1 && dims[2] > 1 && (dims[3] == 3 || dims[3] == 4)
	buf = zeros(UInt8, prod(Int.(dims)))
	@test IG._on_magfield_texture(Cint(0), Cdouble(2025.0), pointer(buf), Cint(length(buf)),
	                              pointer(dims), pointer(err), Cint(512)) == 1
	@test any(!=(0), buf)
end

@testitem "magfield: the dip poles are where the field stands on end" tags=[:unit] begin
	IG = InteractiveGMT
	(lonN, latN, incN), (lonS, latS, incS) = IG._mag_dip_poles(2025.0)
	# A dip pole is where the inclination reaches +-90 deg; the search must get essentially there.
	@test incN > 89.99 && incS < -89.99
	@test latN > 75.0 && latS < -60.0
	@test -180.0 <= lonN <= 180.0 && -180.0 <= lonS <= 180.0
	# Not antipodal -- the real field is not a centred dipole, which is why both are computed.
	@test abs(abs(lonN - lonS) - 180.0) > 5.0 || abs(latN + latS) > 5.0
	# The answer really is an extremum: the inclination right there beats a point a degree away.
	P = Matrix{Float64}(undef, 2, 3)
	for (k, (lo, la)) in enumerate(((lonN, latN), (lonN + 1.0, latN - 1.0)))
		sla, cla = sincosd(la);  slo, clo = sincosd(lo)
		P[k,1] = IG._MAG_R * cla * clo;  P[k,2] = IG._MAG_R * cla * slo;  P[k,3] = IG._MAG_R * sla
	end
	B, _, _ = IG._mag_B_cart(P, 2025.0)
	dip(i) = abs(asind(clamp(-(B[i,1]*P[i,1] + B[i,2]*P[i,2] + B[i,3]*P[i,3]) /
	                          (IG._MAG_R * sqrt(B[i,1]^2 + B[i,2]^2 + B[i,3]^2)), -1.0, 1.0)))
	@test dip(1) > dip(2)
	# They MOVE with the date -- the north dip pole crossed the Arctic in the 20th century.
	(lon1900, lat1900, _), _ = IG._mag_dip_poles(1900.0)
	@test abs(lon1900 - lonN) > 5.0 || abs(lat1900 - latN) > 1.0
	# Out of the model's window the C entry point refuses, with a message.
	out = zeros(Cdouble, 6);  err = zeros(UInt8, 512)
	@test IG._on_magfield_poles(Cdouble(2099.0), pointer(out), pointer(err), Cint(512)) == 0
	@test occursin("1900-2030", unsafe_string(pointer(err)))
	@test IG._on_magfield_poles(Cdouble(2025.0), pointer(out), pointer(err), Cint(512)) == 1
	@test out[2] > 75.0 && out[5] < -60.0
end

@testitem "magfield: the IGRF intensity skin is a real coloured image" tags=[:unit] begin
	IG = InteractiveGMT
	buf, nlon, nlat, comps, zmn, zmx = IG._mag_intensity_texture(2025.0; inc = 2.0)
	@test nlon == 181 && nlat == 91                # 2-degree grid over the whole globe
	@test comps == 3 || comps == 4
	@test length(buf) == nlon * nlat * comps
	@test zmn > 15000 && zmx < 75000               # nT: the field's own surface range
	@test zmx > zmn
	@test length(unique(buf)) > 16                 # a colour ramp, not one flat colour
	# The same skin through the C entry point, for the same date.
	dims = zeros(Cint, 3);  err = zeros(UInt8, 512)
	@test IG._on_magfield_texture(Cint(1), Cdouble(2025.0), Ptr{UInt8}(C_NULL), Cint(0),
	                              pointer(dims), pointer(err), Cint(512)) == 1
	@test dims[1] > 1 && dims[2] > 1
	tex = zeros(UInt8, prod(Int.(dims)))
	@test IG._on_magfield_texture(Cint(1), Cdouble(2025.0), pointer(tex), Cint(length(tex)),
	                              pointer(dims), pointer(err), Cint(512)) == 1
	@test any(!=(0), tex)
	# ...and it is refused outside the model's window rather than answered with another date.
	@test IG._on_magfield_texture(Cint(1), Cdouble(1850.0), Ptr{UInt8}(C_NULL), Cint(0),
	                              pointer(dims), pointer(err), Cint(512)) == 0
	@test occursin("1900-2030", unsafe_string(pointer(err)))
end
