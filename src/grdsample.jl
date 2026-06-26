# grdsample.jl — GMT > Resample (grdsample) tool. Port of Mirone's grdsample dialog,
# with full -I (inc variations), -n (interp + clipping), and -r (registration) options.

# C callback: `scene` = receiving window's Scene* handle; `params` = "input;output;I;R;n;r;T;S"
#   input: input grid file path, or "selected" for THIS window's loaded grid
#   output: output grid file path ("" -> result is added INTO this window as a new layer)
#   I: grid increment (e.g. "1m", "0:01", "0.5")
#   R: region as "W/E/S/N" (any field blank -> keep input region)
#   n: interpolation + options (e.g. "bilinear", "bicubic+c")
#   r: registration "g" (gridline) or "p" (pixel)
#   T: "1" for toggle registration, "0" for no toggle
#   S: Scene Objects label of the loaded element (used for the "<name> resampled" layer name)
function _on_grdsample(scene::Ptr{Cvoid}, cparams::Cstring)::Cvoid
	try
		parts   = split(unsafe_string(cparams), ';')
		getp(i) = length(parts) >= i ? String(parts[i]) : ""
		input   = getp(1)
		output  = getp(2)
		inc     = getp(3)
		region  = getp(4)
		interp  = getp(5)
		reg     = getp(6)
		toggle  = getp(7) == "1"
		srcname = getp(8)

		# Resolve the input. "selected" -> the grid/image this window is showing (via _FIGREG).
		src = if input == "selected"
			fig = get(_FIGREG, scene, nothing)
			fig isa QtFigure ? fig.G : fig isa QtImage ? fig.I :
				error("No grid or image loaded in this window")
		else
			isempty(input) && error("No input grid given")
			gmtread(input)
		end

		base = isempty(srcname) ? "grid" : srcname
		if src isa GMTgrid
			# Grid -> GMT.grdsample. Region only when all four W/E/S/N fields are present.
			kw = Dict{Symbol,Any}()
			!isempty(inc)    && (kw[:inc] = inc)
			!isempty(interp) && (kw[:interp] = interp)
			!isempty(reg)    && (kw[:reg] = (reg == "p" ? 1 : 0))
			toggle && (kw[:toggle] = true)
			rfields = split(region, '/')
			(length(rfields) == 4 && !any(isempty, rfields)) && (kw[:region] = region)
			G_out = GMT.grdsample(src; kw...)
			if isempty(output)
				_add_grid_to_scene(scene, G_out, base * " resampled")
			else
				GMT.gmtwrite(output, G_out); view_grid(G_out)
			end
		elseif src isa GMTimage
			# Image -> GMT.gdalwarp (grdsample is grid-only).
			I_out = _gdalwarp_image(src; inc, region, interp)
			if isempty(output)
				_add_image_to_scene(scene, I_out, base * " resampled")
			else
				GMT.gdalwrite(output, I_out); iview_image_obj(I_out, base * " resampled")
			end
		else
			error("grdsample: unsupported input type $(typeof(src))")
		end
	catch e
		_viewer_log_error(scene, "grdsample FAILED: $(sprint(showerror, e))")
		@warn "grdsample FAILED" exception=(e,)
	end
	return
end

# Resample a GMTimage with GMT.gdalwarp. `inc` -> -tr (x or x/y), `region` (W/E/S/N) -> -te
# (xmin ymin xmax ymax), `interp` -> -r resampling method. Empty fields are omitted.
function _gdalwarp_image(I::GMTimage; inc::AbstractString="", region::AbstractString="",
                         interp::AbstractString="")
	opts = String[]
	if !isempty(inc)
		if occursin('/', inc)
			xi, yi = split(inc, '/')
			append!(opts, ["-tr", String(xi), String(yi)])
		else
			append!(opts, ["-tr", inc, inc])
		end
	end
	rf = split(region, '/')
	if length(rf) == 4 && !any(isempty, rf)
		append!(opts, ["-te", rf[1], rf[3], rf[2], rf[4]])   # W/E/S/N -> xmin ymin xmax ymax
	end
	r = _gdal_resample(interp)
	!isempty(r) && append!(opts, ["-r", r])
	return GMT.gdalwarp(I, opts)
end

# Map the dialog's interpolation code (possibly with a "+c" clip suffix) to a gdalwarp -r method.
function _gdal_resample(interp::AbstractString)
	m = String(first(split(interp, '+')))
	m == "nearest" ? "near" :
	m == "linear"  ? "bilinear" :
	m == "cubic"   ? "cubic" :
	m == "bspline" ? "cubicspline" : ""
end

# Register the callback. Lazy (first window) via _ensure_callbacks.
function _register_grdsample()
	fptr = @cfunction((s, c) -> Base.invokelatest(_on_grdsample, s, c), Cvoid, (Ptr{Cvoid}, Cstring))
	ccall(_fn(:gmtvtk_set_grdsample_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end

# ── "OR Ref grid" metadata callback ───────────────────────────────────────────────────────────
# The grdsample dialog calls this with a grid/image path; we gmtread its header and return
# "W/E/S/N/xinc/yinc/nx/ny" so the C++ side fills the Griding Line Geometry boxes. Returns "" on
# any failure (dialog then leaves the user's values untouched).
#
# The returned Cstring points into a MODULE-GLOBAL byte buffer so the bytes outlive the ccall
# return (Julia owns them; C++ copies immediately and never frees). Single UI thread -> one buffer
# is enough; each call overwrites the previous (no reentrancy).
const _GRIDMETA_BUF = Ref{Vector{UInt8}}(UInt8[0])

function _gridmeta_string(o)::String
	if o isa GMT.GMTgrid || o isa GMT.GMTimage
		r  = o.range
		dx = o.inc[1]
		dy = length(o.inc) > 1 ? o.inc[2] : o.inc[1]
		nx = length(o.x)
		ny = length(o.y)
		return "$(r[1])/$(r[2])/$(r[3])/$(r[4])/$dx/$dy/$nx/$ny"
	end
	return ""
end

function _on_gridmeta(cpath::Cstring)::Cstring
	s = ""
	try
		path = unsafe_string(cpath)
		!isempty(path) && (s = _gridmeta_string(gmtread(path)))
	catch e
		@warn "Ref grid metadata read FAILED" exception=(e,)
	end
	_GRIDMETA_BUF[] = Vector{UInt8}(codeunits(s * "\0"))
	return Cstring(pointer(_GRIDMETA_BUF[]))
end

function _register_gridmeta()
	fptr = @cfunction((c) -> Base.invokelatest(_on_gridmeta, c), Cstring, (Cstring,))
	ccall(_fn(:gmtvtk_set_gridmeta_callback), Cvoid, (Ptr{Cvoid},), fptr)
	return
end
