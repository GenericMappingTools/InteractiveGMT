# Image > K-means classification — the compute half of Mirone's src_figs/classificationfig.m
# ("image classification, supervised and unsupervised, using k-means").
#
# The clustering is that file's own `dcKMeans` (Copyright (C) David Corney 2000), ported as it
# stands: initial centres are either the colours the user clicked (supervised) or k random pixels
# (unsupervised); each iteration labels every pixel with its nearest centre and moves each centre to
# the mean of its members; it stops when the centres stop moving (sum|ΔC| < 1e-8) or after 50
# iterations. An empty cluster keeps its centre — `if (~isempty(idx))` in the .m.
#
# NOT MATLAB's `kmeans` and not any other clustering method: classificationfig.m has the two
# `kmeans_j` calls commented out and ships dcKMeans, so dcKMeans is what a port has to reproduce.
#
# Pixel access reuses floodfill.jl's `_ff_pixels` / `_ff_image` / `_ff_world_to_pixel` — one image
# is one normalised (ny, nx, nb) array here as it is there, and a world seed becomes a pixel through
# the same converter, never a second one.

# The classification a Compute left behind: `Isolate selected class` reads it, exactly as
# push_getClass_CB reads the class figure's CData and the original image's. Keyed by scene, so each
# window keeps its own.
struct _KMResult
	src::GMTimage          # the image that was classified (push_getClass_CB's img_orig)
	idx::Matrix{Int}       # per-pixel class, 1-based, normalised (ny, nx) orientation
	k::Int
end
const _KM_STATE = Dict{Ptr{Cvoid}, _KMResult}()

# ChooseInitialCentres: k distinct rows picked at random. k is a handful, so a rejection loop is
# cheaper than a randperm over the millions of rows `Data` has.
function _km_choose_centres(Data::Matrix{Float32}, k::Int)::Matrix{Float32}
	R = size(Data, 1)
	R >= k || error("K-means: the image has fewer pixels ($R) than classes ($k)")
	pick = Int[]
	while length(pick) < k
		r = rand(1:R)
		r in pick || push!(pick, r)
	end
	return Data[pick, :]
end

# dcKMeans(Data, k, InitCentres, MaxIters). Returns (classes, centres); classes is 1-based.
#
# The .m minimises `sum(C.^2,2)' - 2*Data*C'`, i.e. the squared distance with the constant `Data.^2`
# term dropped ("Do we need DataSq? It's constant, and we're minimising things"). Here the full
# squared distance is accumulated per pixel instead — same argmin, and it avoids materialising the
# R-by-k distance matrix the .m builds with `repmat` for every iteration.
function _km_dckmeans(Data::Matrix{Float32}, k::Int, init::Union{Nothing,Matrix{Float32}},
                      maxiters::Int = 50)
	R, nb = size(Data)
	C = (init === nothing) ? _km_choose_centres(Data, k) : copy(init)
	size(C, 1) == k || error("K-means: got $(size(C,1)) initial centres for $k classes")
	size(C, 2) == nb || error("K-means: initial centres have $(size(C,2)) bands, the image has $nb")
	Cold = copy(C)
	cls  = ones(Int, R)
	S, n = zeros(Float32, k, nb), zeros(Int, k)
	for _ = 1:maxiters
		@inbounds for i = 1:R
			best, bi = Inf32, 1
			for j = 1:k
				d = 0f0
				for b = 1:nb
					t = Data[i, b] - C[j, b]
					d += t * t
				end
				if (d < best)  best = d;  bi = j  end
			end
			cls[i] = bi
		end
		fill!(S, 0f0);  fill!(n, 0)
		@inbounds for i = 1:R
			j = cls[i];  n[j] += 1
			for b = 1:nb  S[j, b] += Data[i, b]  end
		end
		@inbounds for j = 1:k
			n[j] == 0 && continue                 # `if (~isempty(idx))`: an empty class keeps its centre
			for b = 1:nb  C[j, b] = S[j, b] / n[j]  end
		end
		# `Change < 1e-8` in the .m, which is a DOUBLE threshold: at Float32 the centres (0..1) resolve
		# to ~1e-7, so 1e-8 could never be reached and the loop would always run all 50 iterations. A
		# converged pass recomputes each centre as the same mean, i.e. Change is exactly 0, so 1f-6
		# stops on the same iteration the .m does.
		sum(abs, Cold .- C) < 1f-6 && break
		Cold .= C
	end
	return cls, C
end

# The colour of a supervised seed: "seed point color is computed by averaging pixels inside a square
# window with this number of points side" (edit_nNeighbors's tooltip). W = fix(nNeighbors/2).
function _km_seed_colors(P::Array{UInt8,3}, seeds::Vector{Tuple{Int,Int}}, nneigh::Int)::Matrix{Float32}
	ny, nx, nb = size(P)
	W = div(nneigh, 2)
	C = zeros(Float32, length(seeds), nb)
	for (i, (r, c)) in enumerate(seeds)
		r1, r2 = max(r - W, 1), min(r + W, ny)
		c1, c2 = max(c - W, 1), min(c + W, nx)
		for b = 1:nb
			s = 0f0
			@inbounds for cc = c1:c2, rr = r1:r2
				s += P[rr, cc, b]
			end
			C[i, b] = s / ((r2 - r1 + 1) * (c2 - c1 + 1) * 255f0)  # img is double(CData)/255 in the .m
		end
	end
	return C
end

# The class colours as a k-by-3 UInt8 palette. "if (size(colors,2) == 1) colors = [colors colors
# colors]": a one-band image classifies on grey levels, and the class colour is that grey.
function _km_palette(C::Matrix{Float32})::Matrix{UInt8}
	RGB = size(C, 2) == 1 ? hcat(C, C, C) : C[:, 1:min(size(C, 2), 3)]
	size(RGB, 2) == 3 || (RGB = hcat(RGB, RGB[:, end], RGB[:, end])[:, 1:3])
	return UInt8[UInt8(clamp(round(Int, RGB[j, b] * 255), 0, 255)) for j = 1:size(RGB, 1), b = 1:3]
end

# The class figure, INDEXED exactly as classificationfig.m makes it: the pixel value is the class
# number (`Idx = uint8(Idx-1)`, so 0-based, which is also what the listbox offers) and the colormap
# is the class centres (`set(h,'ColorMap',colors)`). It is NOT expanded to RGB — an indexed image
# stays indexed here, from this builder through Scene Objects to Save image; only the VTK texture is
# expanded, once, inside `_pixaccess_img` (drape.jl).
function _km_class_image(idx::Matrix{Int}, C::Matrix{Float32}, I::GMTimage)::GMTimage
	ny, nx = size(idx)
	out = Array{UInt8,2}(undef, ny, nx)
	@inbounds for c = 1:nx, r = 1:ny
		out[r, c] = UInt8(idx[r, c] - 1)                 # `Idx = uint8(Idx-1)`
	end
	J = GMT.mat2img(_ff_rowmajor(I) ? permutedims(out, (2, 1)) : out, I)
	J.layout = I.layout
	return _img_set_palette!(J, _km_palette(C))
end

# push_getClass_CB, "Isolate as color": the selected classes keep their ORIGINAL pixels, everything
# else goes to the background colour. (The .m walks the classes one by one, blanking on the first and
# restoring on the rest; the result is the union, which is what this writes directly.)
#
# An INDEXED source stays indexed: the kept pixels keep their index and the background becomes one
# more palette entry (or an existing one that already holds that colour). Only a palette with no room
# left for it — a full 256-entry GDAL palette — falls back to RGB, and then because there is nowhere
# to put the colour, not by choice.
function _km_isolate_color(sel::BitMatrix, bg::Vector{UInt8}, I::GMTimage)::GMTimage
	if _img_is_indexed(I)
		L = _img_palette(I)
		nL, nc = size(L, 1), min(size(L, 2), 3)
		want = UInt8[bg[min(b, length(bg))] for b = 1:3]
		# A stored palette is padded to 256 entries, so "is there room" is not `nL < 256` — it is
		# whether the image uses every index. The highest index in use says where the free slots start.
		used = Int(maximum(I.image)) + 1
		ib = findfirst(i -> all(b -> L[i, b <= nc ? b : nc] == want[b], 1:3), 1:used)
		if ib === nothing && used < nL                   # room for one more colour: take the next slot
			ib = used + 1
			@inbounds for b = 1:3  L[ib, b] = want[b]  end
			(size(L, 2) >= 4) && (L[ib, 4] = 0xff)
		end
		if ib !== nothing
			S  = I.image
			rowmajor = _ff_rowmajor(I)
			ny, nx = size(sel)
			out = Array{UInt8,2}(undef, ny, nx)
			@inbounds for c = 1:nx, r = 1:ny
				out[r, c] = sel[r, c] ? (rowmajor ? S[c, r] : S[r, c]) : UInt8(ib - 1)
			end
			J = GMT.mat2img(rowmajor ? permutedims(out, (2, 1)) : out, I)
			J.layout = I.layout
			return _img_set_palette!(J, L[:, 1:min(size(L, 2), 4)])
		end
	end
	P = _ff_pixels(I)                                    # indexed sources arrive here already RGB
	ny, nx, nb = size(P)
	out = Array{UInt8,3}(undef, ny, nx, nb)
	@inbounds for b = 1:nb, c = 1:nx, r = 1:ny
		out[r, c, b] = sel[r, c] ? P[r, c, b] : bg[min(b, length(bg))]
	end
	return _ff_image(out, I)
end

# ---------------------------------------------------------------------------------------------
# The C callback. params = "op;args":
#   "compute;<name>;<supervised>;<nClasses>;<nNeighbors>;x1,y1[;x2,y2…]"
#   "isolate;<name>;<asMask>;<r>,<g>,<b>;<c1>[,<c2>…]"        classes 0-based, as the listbox shows
# ---------------------------------------------------------------------------------------------
function _on_classify(scene::Ptr{Cvoid}, dlg::Ptr{Cvoid}, cparams::Cstring)::Cint
	op = ""
	try
		p  = split(unsafe_string(cparams), ';')
		op = p[1]
		name = length(p) >= 2 ? String(strip(p[2])) : ""
		src  = isempty(name) ? "Image" : name

		if op == "compute"
			I = isempty(name) ? _find_object_named(scene, :image)[2] : _find_object(scene, :image, name)
			(I isa GMTimage) || error("K-means: this window has no image named '$name'")
			supervised = strip(p[3]) == "1"
			nclasses   = parse(Int, strip(p[4]))
			nneigh     = parse(Int, strip(p[5]))
			P = _ff_pixels(I)
			ny, nx, nb = size(P)
			# segcolors = [rc(:) gc(:) bc(:)] / 255 — one row per pixel, one column per band.
			Data = Matrix{Float32}(undef, ny * nx, nb)
			@inbounds for b = 1:nb
				k = 0
				for c = 1:nx, r = 1:ny
					Data[k += 1, b] = P[r, c, b] / 255f0
				end
			end
			init = nothing
			if supervised
				seeds = Tuple{Int,Int}[]
				for f in p[6:end]
					isempty(strip(f)) && continue
					xy = split(f, ',')
					length(xy) == 2 || continue
					push!(seeds, _ff_world_to_pixel(I, parse(Float64, xy[1]), parse(Float64, xy[2])))
				end
				length(seeds) >= 2 || error("K-means: supervised classification needs at least two seed points")
				nclasses = length(seeds)                       # nClusters = length(xp)
				init = _km_seed_colors(P, seeds, nneigh)
			end
			cls, C = _km_dckmeans(Data, nclasses, init, 50)
			# Back to (ny, nx). `Data`'s rows were filled column by column, so the reshape matches.
			idx = reshape(cls, ny, nx)
			_KM_STATE[scene] = _KMResult(I, idx, nclasses)
			# Derived-variable display law: a new named row, checked, the source unchecked, Scene
			# Objects unfolded — the landing every derived image uses (_commit_derived_image!).
			_commit_derived_image!(scene, _km_class_image(idx, C, I),
			                       "$src (k-means, $nclasses classes)")
			ccall(_fn(:gmtvtk_classify_set_classes), Cvoid, (Ptr{Cvoid}, Cint), dlg, Cint(nclasses))
			return Cint(1)

		elseif op == "isolate"
			st = get(_KM_STATE, scene, nothing)
			st === nothing && error("K-means: nothing classified yet in this window — press Compute first")
			asmask = strip(p[3]) == "1"
			rgb = [UInt8(clamp(parse(Int, strip(t)), 0, 255)) for t in split(p[4], ',')]
			classes = Int[]
			for t in split(p[5], ',')
				isempty(strip(t)) && continue
				push!(classes, parse(Int, strip(t)) + 1)       # the listbox labels them from 0
			end
			isempty(classes) && error("K-means: no class selected")
			all(c -> 1 <= c <= st.k, classes) ||
				error("K-means: class out of range (this window has $(st.k) classes)")
			sel = falses(size(st.idx))
			@inbounds for c in classes, j in eachindex(st.idx)
				st.idx[j] == c && (sel[j] = true)
			end
			lbl = join(sort(classes) .- 1, ",")
			if asmask                                          # "Isolate as mask": the union, binary
				_commit_derived_image!(scene, _ff_mask_image(sel, st.src), "$src (class $lbl mask)")
			else                                               # "Isolate as color"
				_commit_derived_image!(scene, _km_isolate_color(sel, rgb, st.src), "$src (class $lbl)")
			end
			return Cint(1)
		end
		error("K-means: unknown op '$op'")
	catch e
		_tool_failed(scene, "K-means classification ($op)", e)
		return Cint(0)
	end
end

function _register_classify()
	fptr = @cfunction((s, d, c) -> Base.invokelatest(_on_classify, s, d, c)::Cint, Cint,
	                  (Ptr{Cvoid}, Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_classify_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
