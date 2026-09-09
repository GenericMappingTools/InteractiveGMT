# Fault-plane teaching model. Julia reconstructs the STL blocks at the requested dip,
# then supplies meshes and azimuth/slip transforms to the GUI. No extra dependencies.

struct _FaultDemoBlock
	vertices::Matrix{Float64}         # three consecutive vertices per STL triangle
	normal::Vector{Float64}          # mating face normal, footwall -> hanging wall
	centre::Vector{Float64}          # centre of that face at the common mid-height
	scale::Float64                   # along-strike block length
end

const _FAULT_DEMO_BLOCKS = Ref{Union{Nothing,NTuple{2,_FaultDemoBlock}}}(nothing)
const _FAULT_DEMO_GAP = 0.025       # fraction of the along-strike block length

# The two shipped assets are binary STL. Read with Base, including little-endian conversion;
# no MeshIO or other package is needed.
function _fault_demo_read(path::String)
	vertices = open(path, "r") do io
		read(io, 80)
		n = Int(ltoh(read(io, UInt32)))
		filesize(path) == 84 + 50n || error("Invalid binary STL: $path")
		v = Matrix{Float64}(undef, 3n, 3)
		for t in 1:n
			read(io, 12)             # stored normal; compute from vertices instead
			for j in 1:3, k in 1:3
				v[3(t-1)+j, k] = reinterpret(Float32, ltoh(read(io, UInt32)))
			end
			read(io, UInt16)         # attribute byte count
		end
		v
	end
	all(isfinite, vertices) || error("Non-finite STL vertices: $path")
	largest, plane = 0.0, 0.0
	normal = zeros(3)
	for i in 1:3:size(vertices, 1)
		p, q, r = vertices[i,:], vertices[i+1,:], vertices[i+2,:]
		n = GMT.cross(q-p, r-p)
		area = sqrt(sum(abs2, n))
		area > 0 || continue
		n ./= area
		# Mating faces run along X; exclude horizontal/vertical outer faces and tiny markings.
		(area > largest && abs(n[1]) < 1e-4 && abs(n[2]) > 0.1 && abs(n[3]) > 0.1) || continue
		n[2] < 0 && (n .*= -1)
		largest, normal, plane = area, n, sum(n .* p)
	end
	largest > 0 || error("No inclined mating face in $path")
	xmin, xmax = extrema(vertices[:,1])
	zmin, zmax = extrema(vertices[:,3])
	zmid = (zmin+zmax)/2
	centre = [(xmin+xmax)/2, (plane-normal[3]*zmid)/normal[2], zmid]
	return _FaultDemoBlock(vertices, normal, centre, xmax-xmin)
end

function _fault_demo_blocks()
	if _FAULT_DEMO_BLOCKS[] === nothing
		dir = joinpath(_PKGROOT, "data", "fault_plane_demo")
		fw = _fault_demo_read(joinpath(dir, "footwall.stl"))
		hw = _fault_demo_read(joinpath(dir, "hanging_wall.stl"))
		sum(fw.normal .* hw.normal) > 0.999999 || error("STL mating faces are not parallel")
		isapprox(fw.scale, hw.scale; rtol=1e-6) || error("STL along-strike lengths differ")
		# STL facets differ slightly from a perfect plane (sub-millimetre rounding). Use the
		# support planes of ALL vertices and one shared normal to guarantee the requested gap.
		for (i, block) in enumerate((fw,hw))
			distance = block.vertices * fw.normal
			plane = i == 1 ? maximum(distance) : minimum(distance)
			block.centre[2] = (plane-fw.normal[3]*block.centre[3])/fw.normal[2]
		end
		_FAULT_DEMO_BLOCKS[] = (fw, hw)
	end
	return _FAULT_DEMO_BLOCKS[]::NTuple{2,_FaultDemoBlock}
end

# Rejoin the original blocks into their closed exterior, removing the old mating faces.
# Weld only STL rounding-sized seams (0.005 in the source units), preserving the surface markings.
const _FAULT_DEMO_SHELL = Ref{Union{Nothing,Tuple{Matrix{Float64},Matrix{Int}}}}(nothing)
const _FAULT_DEMO_MESH_DIP = Ref(NaN)
const _FAULT_DEMO_MESHES = Ref((zeros(0,3), zeros(0,3)))

function _fault_demo_vertex!(vertices::Vector{Vector{Float64}}, p::Vector{Float64}, tolerance::Float64)
	i = findfirst(q -> sum(abs2, q-p) <= tolerance^2, vertices)
	if i === nothing
		push!(vertices, p)
		return length(vertices)
	end
	return i
end

function _fault_demo_shell()
	_FAULT_DEMO_SHELL[] !== nothing && return _FAULT_DEMO_SHELL[]
	vertices, faces = Vector{Float64}[], Vector{Int}[]
	for block in _fault_demo_blocks()
		v = (block.vertices .- transpose(block.centre))/block.scale
		for t in 1:3:size(v,1)
			p = [v[t+k,:] for k in 0:2]
			d = [sum(q .* block.normal) for q in p]
			all(abs.(d) .< 1e-4) && continue # discard the original internal fault face
			ids = Int[]
			for (q, distance) in zip(p,d)
				abs(distance) < 1e-4 && (q -= distance*block.normal)
				push!(ids, _fault_demo_vertex!(vertices, q, 1e-4))
			end
			push!(faces, ids)
		end
	end
	# The assembled box is shown 1:1 in plan — along-strike length == across-strike width. The STL is
	# 50 long by ~119 wide once welded, so the length is stretched to the width and the whole model
	# renormalised back to unit length, which is the same thing as dividing Y and Z by the width:
	# X (already 1 after the /block.scale above) stays the unit every constant below is a fraction of,
	# and scaling Y and Z TOGETHER leaves the cross section — hence the look of the dipping face —
	# exactly as the STL author drew it. Y is scaled about 0, which is the mating plane, so the join
	# holds; Z about 0, which is the blocks' mid-height.
	width = maximum(p[2] for p in vertices) - minimum(p[2] for p in vertices)
	for p in vertices
		p[2] /= width
		p[3] /= width
	end
	_FAULT_DEMO_SHELL[] = (reduce(vcat, transpose.(vertices)), reduce(vcat, transpose.(faces)))
	return _FAULT_DEMO_SHELL[]
end

# Ear clipping keeps every boundary vertex, including collinear subdivisions. A centre fan
# would bridge the small concave surface markings and could leave gaps in the reconstructed cap.
function _fault_demo_cap!(faces::Vector{Vector{Int}}, ring::Vector{Int},
	vertices::Vector{Vector{Float64}}, dip::Float64, hanging::Bool)
	xy = [[p[1], cosd(dip)*p[2]-sind(dip)*p[3]] for p in vertices]
	cross2(a,b,c) = (b[1]-a[1])*(c[2]-a[2]) - (b[2]-a[2])*(c[1]-a[1])
	area = sum(xy[ring[i]][1]*xy[ring[mod1(i+1,length(ring))]][2] -
		xy[ring[i]][2]*xy[ring[mod1(i+1,length(ring))]][1] for i in eachindex(ring))
	area < 0 && reverse!(ring)
	while length(ring) > 3
		ear = false
		for i in eachindex(ring)
			a,b,c = ring[mod1(i-1,length(ring))], ring[i], ring[mod1(i+1,length(ring))]
			cross2(xy[a],xy[b],xy[c]) > 1e-14 || continue
			any(j -> j != a && j != b && j != c &&
				cross2(xy[a],xy[b],xy[j]) >= -1e-14 &&
				cross2(xy[b],xy[c],xy[j]) >= -1e-14 &&
				cross2(xy[c],xy[a],xy[j]) >= -1e-14, ring) && continue
			push!(faces, hanging ? [c,b,a] : [a,b,c])
			deleteat!(ring,i)
			ear = true
			break
		end
		ear || error("Cannot triangulate the reconstructed fault face")
	end
	push!(faces, hanging ? reverse(ring) : copy(ring))
	return
end

function _fault_demo_cut(dip::Float64, hanging::Bool)
	shell, triangles = _fault_demo_shell()
	normal = [0., sind(dip), cosd(dip)]
	vertices, faces = Vector{Float64}[], Vector{Int}[]
	for t in axes(triangles,1)
		polygon = [shell[j,:] for j in triangles[t,:]]
		distance = [sum(p .* normal) for p in polygon]
		for i in 1:3
			if abs(distance[i]) < 1e-12
				polygon[i] -= distance[i]*normal
				distance[i] = 0.
			end
		end
		hanging && (distance = -distance)
		clipped = Vector{Float64}[]
		for i in 1:3
			j = mod1(i+1,3)
			distance[i] <= 0 && push!(clipped, polygon[i])
			if (distance[i] < 0 && distance[j] > 0) || (distance[i] > 0 && distance[j] < 0)
				f = distance[i]/(distance[i]-distance[j])
				push!(clipped, polygon[i] + f*(polygon[j]-polygon[i]))
			end
		end
		length(clipped) >= 3 || continue
		ids = [_fault_demo_vertex!(vertices,p,1e-10) for p in clipped]
		for i in 2:length(ids)-1
			length(unique((ids[1],ids[i],ids[i+1]))) == 3 && push!(faces,[ids[1],ids[i],ids[i+1]])
		end
	end
	# The only open edges are the new cut. Join them into boundary loops, then cap each loop.
	edges = Dict{Tuple{Int,Int},Int}()
	for f in faces, (a,b) in ((f[1],f[2]),(f[2],f[3]),(f[3],f[1]))
		e = minmax(a,b)
		edges[e] = get(edges,e,0)+1
	end
	adjacent = Dict{Int,Vector{Int}}()
	for ((a,b),count) in edges
		count == 1 || continue
		push!(get!(adjacent,a,Int[]),b)
		push!(get!(adjacent,b,Int[]),a)
	end
	all(length(n) == 2 for n in values(adjacent)) || error("Open boundary in reconstructed fault block")
	remaining = Set(keys(adjacent))
	while !isempty(remaining)
		first = minimum(remaining)
		ring, previous, current = Int[], 0, first
		while true
			push!(ring,current)
			delete!(remaining,current)
			next = adjacent[current][1] == previous ? adjacent[current][2] : adjacent[current][1]
			previous, current = current, next
			current == first && break
		end
		_fault_demo_cap!(faces,ring,vertices,dip,hanging)
	end
	return reduce(vcat, [transpose(vertices[i]) for f in faces for i in f])
end

function _fault_demo_meshes(dip::Float64)
	0 <= dip <= 90 || throw(ArgumentError("Fault dip must be between 0 and 90 degrees"))
	if dip != _FAULT_DEMO_MESH_DIP[]
		meshes = (_fault_demo_cut(dip,false), _fault_demo_cut(dip,true))
		_FAULT_DEMO_MESHES[] = meshes
		_FAULT_DEMO_MESH_DIP[] = dip
	end
	return _FAULT_DEMO_MESHES[]
end

# ENU, right-hand-rule strike; positive rake is toward up-dip (reverse at +90).
# https://pubs.usgs.gov/of/2011/1060/of2011-1060.pdf
# The fault normal and the slip vector are NOT derived here. `_focal_sdr_to_nu` (focal.jl) is THE
# (strike,dip,rake) -> (n̂,û) function of this program — the one the beachballs are built from — so
# the demo asks it and relabels the answer: it works in Aki & Richards' (North, East, Down) frame,
# this window is (East, North, Up). Same quantity, one function.
_fault_demo_enu(v::NTuple{3,Float64}) = [v[2], v[1], -v[3]]

function _fault_demo_basis(azimuth::Float64, dip::Float64, rake::Float64)
	n, u = _focal_sdr_to_nu(azimuth, dip, rake)
	strike = [sind(azimuth), cosd(azimuth), 0.0]
	return (; strike, normal = _fault_demo_enu(n), slip = _fault_demo_enu(u))
end

# The bounds mirror the four sliders in deps/ui/fault_plane_demo.ui, which are the single source of
# the demo's ranges. This is the contract check on what the GUI is allowed to send, not a second
# definition of the range: widen a slider there and this is what reports the mismatch.
function _fault_demo_matrices(azimuth::Float64, dip::Float64, rake::Float64, slip::Float64)
	(0 <= azimuth <= 360 && 0 <= dip <= 90 && -180 <= rake <= 180 && -100 <= slip <= 100) ||
		throw(ArgumentError("Fault demo: angles or slip outside slider ranges"))
	b = _fault_demo_basis(azimuth, dip, rake)
	# Dip is already built into the mesh. Azimuth rotates about Up only, so the top/bottom
	# surfaces stay horizontal. Source -X is strike, +Y is the horizontal dip direction.
	rotation = [-sind(azimuth) cosd(azimuth) 0.; -cosd(azimuth) -sind(azimuth) 0.; 0. 0. 1.]
	mats = ntuple(3) do i
		m = zeros(4,4)
		m[4,4] = 1.0
		if i <= 2
			m[1:3,1:3] = rotation
			offset = (i == 1 ? -0.5 : 0.5)*_FAULT_DEMO_GAP*b.normal
			i == 2 && (offset += (0.003slip)*b.slip)
			m[1:3,4] = offset
		else
			# Direction arrow outside the end of the blocks, in the middle of the gap.
			m[1:3,1:3] = 0.5*hcat(b.slip, GMT.cross(b.normal, b.slip), b.normal)
			m[1:3,4] = 0.85*b.strike - 0.25*b.slip
		end
		m
	end
	return mats
end

# One small numeric callback per frame; no console eval, per-frame I/O, or C++ geometry maths.
function _on_fault_demo(azimuth::Float64, dip::Float64, rake::Float64, slip::Float64,
	out::Ptr{Cdouble}, err::Ptr{UInt8}, cap::Cint)::Cint
	try
		mats = _fault_demo_matrices(azimuth, dip, rake, slip)
		for b in 1:3, i in 1:4, j in 1:4
			unsafe_store!(out, mats[b][i,j], (b-1)*16 + (i-1)*4 + j) # VTK row-major
		end
		return Cint(1)
	catch e
		msg = sprint(showerror, e)
		n = _console_write(err, cap, msg)
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end

function _on_fault_demo_mesh(dip::Float64, out::Ptr{Cdouble}, capacity::Cint,
	counts::Ptr{Cint}, err::Ptr{UInt8}, cap::Cint)::Cint
	try
		warm_wait("faultdemo")     # never cut geometry while the warm-up is filling the same caches
		meshes = _fault_demo_meshes(dip)
		for i in 1:2
			unsafe_store!(counts, Cint(size(meshes[i],1) ÷ 3), i)
		end
		out == C_NULL && return Cint(1) # size query; geometry is cached for the following copy
		capacity >= sum(length, meshes) || error("Fault mesh buffer is too small")
		k = 1
		for mesh in meshes, i in axes(mesh,1), j in 1:3
			unsafe_store!(out, mesh[i,j], k)
			k += 1
		end
		return Cint(1)
	catch e
		n = _console_write(err, cap, sprint(showerror,e))
		cap > 0 && unsafe_store!(err, 0x00, Int(n)+1)
		return Cint(0)
	end
end

# Opening the dialog is the FIRST time any of this runs, and its first act is a full mesh cut on the
# GUI thread (STL read, weld, clip, ear-clipping cap) plus the beachball projection. Compile all of
# it at the default dip while the menu action is still building the window — see warmup.jl. Pure
# computation on the demo's own shipped assets; it touches no scene.
function _fault_demo_warm()
	_fault_demo_meshes(25.0)                 # the dipSlider's own default value in the .ui
	yield()
	_fault_demo_matrices(0.0, 25.0, -90.0, 0.0)
	yield()
	_, _, nodal1, nodal2 = _focal_patch_meca(0.0, 25.0, -90.0)
	_focal_sectors(0.0, 25.0, -90.0, nodal1, nodal2)   # what the dialog's beachball preview paints
	yield()
	precompile(_on_fault_demo, (Cdouble, Cdouble, Cdouble, Cdouble, Ptr{Cdouble}, Ptr{UInt8}, Cint))
	precompile(_on_fault_demo_mesh, (Cdouble, Ptr{Cdouble}, Cint, Ptr{Cint}, Ptr{UInt8}, Cint))
	return nothing
end

function _register_fault_demo()
	warm_register("faultdemo", _fault_demo_warm)   # C++ fires this when the dialog opens (70_window.cpp)
	ptr = @cfunction((a,d,r,s,o,e,c) -> Base.invokelatest(_on_fault_demo, a,d,r,s,o,e,c),
		Cint, (Cdouble,Cdouble,Cdouble,Cdouble,Ptr{Cdouble},Ptr{UInt8},Cint))
	meshptr = @cfunction((d,o,n,t,e,c) -> Base.invokelatest(_on_fault_demo_mesh, d,o,n,t,e,c),
		Cint, (Cdouble,Ptr{Cdouble},Cint,Ptr{Cint},Ptr{UInt8},Cint))
	ccall(_fn(:gmtvtk_set_fault_demo_callback), Cvoid, (Ptr{Cvoid},Ptr{Cvoid}), ptr, meshptr)
	return
end
