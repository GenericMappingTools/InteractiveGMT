# CI-safe unit tests for the Potrace port (src/potrace/). The reference numbers were produced by the
# original potrace 1.16 executable (`potrace -b geojson -u 1000`, same bitmap fed as a PBM), whose
# geojson backend samples each Bézier at 8 steps exactly like `potrace_gmtds`' default — so rings
# must agree point for point to the executable's 3 printed decimals.

@testitem "potrace: matches potrace 1.16 on a shape with a hole" tags=[:unit, :fast] begin
	PT = InteractiveGMT.Potrace
	T = falses(10, 12)                  # displayed layout: T[1,1] = top-left
	T[2:9, 2:8] .= true
	T[4:6, 4:6] .= false
	T[3:5, 10:11] .= true
	res = PT.potrace(T)
	P = PT.paths(res)
	@test [p.sign for p in P] == ['+', '-', '+']
	@test [p.area for p in P] == [56, 9, 6]
	@test all(p -> p.fcurve.tag == [1, 1, 1, 1], P)
	D = PT.potrace_gmtds(res)
	@test [size(d.data) for d in D] == [(33, 2), (33, 2), (33, 2)]
	@test [d.header for d in D] == ["", " -Ph", ""]
	# first three points of each ring, as potrace.exe prints them
	ref = ([1.0 5.0; 1.01 3.707; 1.066 2.728], [6.0 5.5; 5.969 5.199; 5.882 4.918], [9.0 6.5; 9.02 6.199; 9.079 5.918])
	for k = 1:3
		@test maximum(abs.(D[k].data[1:3, :] .- ref[k])) < 6e-4
		@test D[k].data[1, :] == D[k].data[end, :]      # closed
	end
end

@testitem "potrace: alphamax=0 gives the polygon of a square" tags=[:unit, :fast] begin
	PT = InteractiveGMT.Potrace
	S = falses(20, 20)
	S[6:15, 6:15] .= true
	res = PT.potrace(S; alphamax=0.0)
	P = PT.paths(res)
	@test length(P) == 1
	cv = P[1].fcurve
	@test all(==(PT.CORNER), cv.tag)
	# corners of the 10x10 block, y up: x 5..15, y 5..15
	vx = sort([(round(cv.c[i, 2].x; digits=6), round(cv.c[i, 2].y; digits=6)) for i = 1:cv.n])
	@test vx == [(5.0, 5.0), (5.0, 15.0), (15.0, 5.0), (15.0, 15.0)]
end

@testitem "potrace: turdsize, threshold and region" tags=[:unit, :fast] begin
	PT = InteractiveGMT.Potrace
	A = ones(Float64, 30, 40)            # white page
	A[5:20, 5:30] .= 0.0                 # dark block
	A[25, 35] = 0.0                      # 1-pixel speckle
	@test length(PT.paths(PT.potrace(A))) == 1                  # speckle eaten (turdsize=2)
	@test length(PT.paths(PT.potrace(A; turdsize=0))) == 2
	@test length(PT.paths(PT.potrace(A; invert=true))) == 2     # page traced, block is its hole
	D = PT.potrace_gmtds(PT.potrace(A; alphamax=0.0); region=(100.0, 140.0, -30.0, 0.0))
	# block columns 5:30 -> pixel edges 4..30 -> x 104..130; rows 5:20 (from top) -> y up 10..26 -> -20..-4
	@test extrema(D[1].data[:, 1]) == (104.0, 130.0)
	@test extrema(D[1].data[:, 2]) == (-20.0, -4.0)
	@test D[1].geom == 3
end

@testitem "potrace: SVG and EPS writers" tags=[:unit, :fast] begin
	PT = InteractiveGMT.Potrace
	T = falses(10, 12)
	T[2:9, 2:8] .= true
	T[4:6, 4:6] .= false
	res = PT.potrace(T)
	io = IOBuffer(); PT.potrace_svg(io, res); sv = String(take!(io))
	@test occursin("<svg", sv) && occursin("</svg>", sv)
	@test count("<path", sv) == 1                            # outer + its hole in ONE path element
	@test count(" Z", sv) == 2
	io = IOBuffer(); PT.potrace_eps(io, res; color="#ff0000"); ep = String(take!(io))
	@test startswith(ep, "%!PS-Adobe-3.0 EPSF-3.0")
	@test occursin("%%BoundingBox: 0 0 12 10", ep)
	@test occursin("1.0 0.0 0.0 setrgbcolor", ep)
	@test count("closepath", ep) == 2 && count("\nfill", ep) == 1
end

@testitem "potrace: layered SVG and EPS" tags=[:unit, :fast] begin
	PT = InteractiveGMT.Potrace
	T = falses(10, 12);  T[2:9, 2:8] .= true
	U = falses(10, 12);  U[4:6, 4:6] .= true
	L = ["#ff0000" => PT.potrace(T), "#0000ff" => PT.potrace(U)]
	io = IOBuffer(); PT.potrace_svg(io, L; background="#ffffff"); sv = String(take!(io))
	@test count("<g ", sv) == 2 && occursin("fill=\"#ffffff\"", sv)
	@test findfirst("#ff0000", sv)[1] < findfirst("#0000ff", sv)[1]     # painted in the order given
	io = IOBuffer(); PT.potrace_eps(io, L; background="#ffffff"); ep = String(take!(io))
	@test occursin("rectfill", ep) && count("setrgbcolor", ep) == 3
end

@testitem "Vector Wizard: settings, tones and stacked layers" tags=[:unit, :fast] begin
	M = InteractiveGMT
	p = M._vw_params("mode=bw,thr=100,inv=0,n=4,turd=0,alpha=1,opti=1,tol=0.2,turn=minority")
	@test p.mode === :bw && p.thr == 100 && p.pp.turdsize == 0
	@test_throws ErrorException M._vw_params("mode=nope")
	# a 3-tone grey picture: white page, grey square, black square inside it
	A = fill(0xff, 40, 50, 1)
	A[5:35, 5:45, 1] .= 0x80
	A[15:25, 15:30, 1] .= 0x00
	# black & white: the dark pixels are rank 2, the layer's colour is their mean
	rank, rgb = M._vw_tones(A, p)
	@test count(==(2), rank) == 11 * 16 && rgb[2, :] == [0.0, 0.0, 0.0]
	# three grey levels: lightest is rank 1 (the background), black the top rank
	st = M._VWState(C_NULL, "t", A, (0.0, 50.0, 0.0, 40.0), "", "", 0)
	pg = M._vw_params("mode=grey,n=3,turd=0")
	rank, rgb = M._vw_tones(A, pg)
	@test rank[1, 1] == 1 && rank[20, 20] == 3 && rank[10, 10] == 2
	bg, layers = M._vw_trace(st, pg)
	@test bg == "#ffffff" && length(layers) == 2
	@test layers[2].first == "#000000"
	# layer 2 = "grey or darker": ONE ring, the grey square (the black one lies inside it, same layer)
	@test length(InteractiveGMT.Potrace.paths(layers[1].second)) == 1
	D = InteractiveGMT.Potrace.potrace_gmtds(layers[1].second; region=st.region)
	@test extrema(D[1].data[:, 1]) == (4.0, 45.0)                    # pixel edges in world units
	# a GMT file carries -G per area and the background first
	f = tempname() * ".txt"
	@test_throws ErrorException M._vw_save(st, tempname() * ".gmt", bg, layers)   # .gmt is the OGR/GMT format
	try
		M._vw_save(st, f, bg, layers)
		txt = read(f, String)
		@test occursin("-G255/255/255", txt) && occursin("-G0/0/0", txt)
	finally
		rm(f; force=true)
	end
end
