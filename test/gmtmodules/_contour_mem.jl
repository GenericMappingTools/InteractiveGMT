# Peak commit per contour vertex: tracing (_contour_gdal) and the whole Apply (_on_contours).
# julia _contour_mem.jl <n> <nlevels> trace|full
using InteractiveGMT, GMT
const IG = InteractiveGMT
mutable struct PMC
	cb::UInt32; pf::UInt32; pws::UInt; ws::UInt; a::UInt; b::UInt; c::UInt; d::UInt; pfu::UInt; ppfu::UInt
end
function commit()
	m = PMC(UInt32(sizeof(PMC)), 0, 0, 0, 0, 0, 0, 0, 0, 0)
	ccall((:K32GetProcessMemoryInfo, "kernel32"), Cint, (Ptr{Cvoid}, Ref{PMC}, UInt32),
	      ccall((:GetCurrentProcess, "kernel32"), Ptr{Cvoid}, ()), m, m.cb)
	return Int(m.pfu), Int(m.ppfu)
end
function grid(n)
	x = collect(range(0, 20, length = n))
	z = Float32[sin(xi) * cos(yj) + 0.3f0 * sin(3.7xi + 1.3yj) + 0.05f0 * sin(17xi) * cos(19yj) for yj in x, xi in x]
	return mat2grid(z, x = x, y = x)
end
n, nlev, mode = parse(Int, ARGS[1]), parse(Int, ARGS[2]), ARGS[3]
lev = collect(range(-1.3, 1.3, length = nlev))
p = "0;0;" * join(lev, ',')
# JIT on a small grid first
g0 = grid(200)
if mode != "full"
	IG._contour_gdal(g0, lev, 0)
else
	f0 = IG.viz(g0); IG._on_contours(f0.h, p); ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), f0.h)
end
G = grid(n)
h = mode == "full" ? IG.viz(G).h : C_NULL
GC.gc(); GC.gc()
cur0, pk0 = commit()
if mode == "trace"
	sets = IG._contour_gdal(G, lev, 0)
	npts = sum(s[5] for s in sets)
elseif mode == "gdal"
	# _contour_gdal's first half only: GDAL's own layer of traced lines, nothing copied out
	gdl = GMT.Gdal
	dsr = GMT.gmt2gd(G)
	band = ccall((:GDALGetRasterBand, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid}, Cint), dsr.ptr, Cint(1))
	ds = gdl.create(gdl.getdriver("MEM"))
	lyr = gdl.createlayer(name = "contour", dataset = ds, geom = gdl.wkbLineString)
	gdl.addfielddefn!(lyr, "ID", gdl.OFTInteger); gdl.addfielddefn!(lyr, "ELEV", gdl.OFTReal)
	copts = C_NULL
	for o in ["ID_FIELD=0", "ELEV_FIELD=1", "FIXED_LEVELS=" * join(lev, ',')]
		global copts = ccall((:CSLAddString, GMT.libgdal), Ptr{Ptr{UInt8}}, (Ptr{Ptr{UInt8}}, Cstring), copts, o)
	end
	GC.@preserve dsr ds ccall((:GDALContourGenerateEx, GMT.libgdal), Cint,
	      (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Ptr{UInt8}}, Ptr{Cvoid}, Ptr{Cvoid}), band, lyr.ptr, copts, C_NULL, C_NULL)
	npts = 0
	ccall((:OGR_L_ResetReading, GMT.libgdal), Cvoid, (Ptr{Cvoid},), lyr.ptr)
	cur1, pk1 = commit()
	while true
		f = ccall((:OGR_L_GetNextFeature, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), lyr.ptr)
		f == C_NULL && break
		g = ccall((:OGR_F_GetGeometryRef, GMT.libgdal), Ptr{Cvoid}, (Ptr{Cvoid},), f)
		global npts += Int(ccall((:OGR_G_GetPointCount, GMT.libgdal), Cint, (Ptr{Cvoid},), g))
		ccall((:OGR_F_Destroy, GMT.libgdal), Cvoid, (Ptr{Cvoid},), f)
	end
	println("gdal layer: vertices=$npts  bytes/vertex=$(round((pk1 - cur0) / npts, digits = 1))")
	exit()
else
	IG._on_contours(h, p)
	npts = sum(s[5] for s in IG._contour_gdal(G, lev, 0))   # count only, after the peak was taken
end
cur1, pk1 = commit()
println("n=$n levels=$nlev mode=$mode vertices=$npts  peak-before=$(pk0 - cur0 >> 20) MiB over current  ",
        "peak growth over current=$((pk1 - cur0) >> 20) MiB  bytes/vertex=$(round((pk1 - cur0) / npts, digits = 1))")
