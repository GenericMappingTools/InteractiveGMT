# movie.jl — native InteractiveGMT movie scheduler.
#
# GMT.jl's GMT.movie wrapper records GMT plotting calls into shell/batch scripts and delegates to
# the C `gmt movie` module. A live InteractiveGMT Scene* is different: VTK/Qt state lives in-process
# and must be mutated + captured in that same process. We therefore EXTEND GMT.movie by dispatch for
# InteractiveGMT figure handles, keep one scene alive, render deterministic PNG frames from that
# exact Scene*, then invoke ffmpeg directly for the final container.

"""One normalized frame presented to an InteractiveGMT movie callback."""
struct MovieFrame
	index::Int                  # Julia 1-based position in the sequence
	frame::Int                  # GMT-style 0-based frame number
	nframes::Int
	tag::String                 # zero-padded frame number used in the PNG filename
	value::Any                  # original scalar/row supplied through `frames`
	cols::Vector{Any}           # row columns (MOVIE_COL* analogue)
	text::String                # original/textual row (MOVIE_TEXT analogue)
	words::Vector{String}       # whitespace-split text (MOVIE_WORD* analogue)
	progress::Float64           # 0..1 inclusive (1 for a one-frame movie)
end

const _MovieFigure = Union{QtFigure,QtPoints,QtFV,QtImage}

_movie_atom(s::AbstractString) = something(tryparse(Float64, String(s)), String(s))

function _movie_record(v)
	if v isa NamedTuple
		cols = Any[values(v)...]
		text = join(string.(cols), " ")
		return (value=v, cols=cols, text=text, words=String.(split(text)))
	elseif v isa Tuple || (v isa AbstractVector && !(v isa AbstractString))
		cols = Any[v...]
		text = join(string.(cols), " ")
		return (value=v, cols=cols, text=text, words=String.(split(text)))
	elseif v isa Number || v isa AbstractString || v isa Symbol || v isa Char
		text = string(v)
		return (value=v, cols=Any[v], text=text, words=String.(split(text)))
	else
		# Large frame payloads (notably GMTgrid) stay as the value/column object. Converting a grid
		# to text just to populate MOVIE_TEXT-like metadata is expensive and semantically useless.
		return (value=v, cols=Any[v], text="", words=String[])
	end
end

function _movie_table_records(path::AbstractString)
	isfile(path) || throw(ArgumentError("movie: frame table does not exist: $path"))
	out = NamedTuple[]
	for raw in eachline(path)
		line = strip(raw)
		(isempty(line) || startswith(line, '#')) && continue
		words = String.(split(line))
		cols = Any[_movie_atom(w) for w in words]
		value = length(cols) == 1 ? cols[1] : Tuple(cols)
		push!(out, (value=value, cols=cols, text=line, words=words))
	end
	isempty(out) && throw(ArgumentError("movie: frame table '$path' contains no records"))
	return out
end

"""Normalize GMT-like frame sources without touching Qt/VTK (kept pure for CI tests)."""
function _movie_frames(frames)
	recs = if frames isa Integer
		frames > 0 || throw(ArgumentError("movie: integer `frames` must be > 0"))
		[_movie_record(k) for k in 0:Int(frames)-1]
	elseif frames isa AbstractString
		_movie_table_records(frames)
	elseif frames isa AbstractMatrix
		[_movie_record(Tuple(frames[k, :])) for k in axes(frames, 1)]
	elseif hasproperty(frames, :data) && getproperty(frames, :data) isa AbstractMatrix
		A = getproperty(frames, :data)
		[_movie_record(Tuple(A[k, :])) for k in axes(A, 1)]
	else
		vals = collect(frames)
		isempty(vals) && throw(ArgumentError("movie: `frames` is empty"))
		[_movie_record(v) for v in vals]
	end
	n = length(recs)
	width = max(1, ndigits(max(n - 1, 0)))
	return [MovieFrame(k, k - 1, n, lpad(string(k - 1), width, '0'),
	                   recs[k].value, recs[k].cols, recs[k].text, recs[k].words,
	                   n == 1 ? 1.0 : (k - 1) / (n - 1)) for k in 1:n]
end

function _movie_format(x)::Symbol
	f = Symbol(lowercase(String(x)))
	f in (:mp4, :webm, :gif, :png) ||
		throw(ArgumentError("movie: format must be :mp4, :webm, :gif, or :png (got $x)"))
	return f
end

function _movie_ffmpeg_path(ffmpeg)
	if ffmpeg !== nothing
		p = String(ffmpeg)
		(isfile(p) || Sys.which(p) !== nothing) || error("movie: ffmpeg executable not found: $p")
		return isfile(p) ? abspath(p) : String(Sys.which(p))
	end
	p = Sys.which("ffmpeg")
	p === nothing && error("movie: ffmpeg is required for mp4/webm/gif output but was not found on PATH. Use format=:png to keep the frame sequence only, or pass ffmpeg=\"...\".")
	return String(p)
end

# Kept separate/pure so CI can lock down the encoder contract without requiring ffmpeg.
function _movie_ffmpeg_args(exe::AbstractString, pattern::AbstractString, output::AbstractString,
                            format::Symbol, fps::Real; verbose::Bool=false)
	fps > 0 || throw(ArgumentError("movie: frame_rate must be > 0"))
	args = String[String(exe), "-y", "-loglevel", verbose ? "info" : "error",
	              "-framerate", string(fps), "-start_number", "0", "-i", String(pattern)]
	if format === :mp4
		append!(args, ["-c:v", "libx264", "-preset", "medium", "-crf", "18",
		               "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-pix_fmt", "yuv420p",
		               "-movflags", "+faststart"])
	elseif format === :webm
		append!(args, ["-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "0",
		               "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-pix_fmt", "yuv420p"])
	elseif format === :gif
		# One ffmpeg invocation, but still palette-based rather than the low-quality direct GIF encoder.
		append!(args, ["-filter_complex",
		               "split[s0][s1];[s0]palettegen=max_colors=256[p];[s1][p]paletteuse=dither=sierra2_4a"])
	else
		throw(ArgumentError("movie: ffmpeg is not used for format=$format"))
	end
	push!(args, String(output))
	return args
end

function _movie_paths(name::AbstractString, work_dir, format::Symbol)
	requested = abspath(String(name))
	parent = dirname(requested)
	stem = splitext(basename(requested))[1]
	isempty(stem) && throw(ArgumentError("movie: `name` must contain a filename stem"))
	frame_dir = work_dir === nothing ? joinpath(parent, stem) : joinpath(abspath(String(work_dir)), stem)
	output = format === :png ? frame_dir : joinpath(parent, stem * "." * String(format))
	return stem, frame_dir, output
end

"""
	orbit!(fig, azimuth, elevation=0; zoom=1) -> fig

Apply a relative VTK camera orbit to one InteractiveGMT window and render it. This is the simplest
movie callback primitive; `azimuth` and `elevation` are degrees and `zoom > 0` is multiplicative.
"""
function orbit!(fig::_MovieFigure, azimuth::Real, elevation::Real=0; zoom::Real=1)
	isalive(fig) || error("orbit!: the viewer window is closed")
	zoom > 0 || throw(ArgumentError("orbit!: `zoom` must be > 0"))
	ccall(_fn(:gmtvtk_orbit), Cvoid, (Ptr{Cvoid}, Cdouble, Cdouble, Cdouble),
	      _fig_handle(fig), Float64(azimuth), Float64(elevation), Float64(zoom))
	return fig
end

function _same_grid_geometry(A::GMTgrid, B::GMTgrid)
	# (nx,ny) from the COORDINATE VECTORS, never `size(z)` -- a row-major "TRB" grid keeps GMT.jl's
	# (ny,nx) Julia dims while its memory runs x-fastest (see the grid memory-layout law).
	_grid_dims(A) == _grid_dims(B) || return false
	A.registration == B.registration || return false
	_isgeographic(A) == _isgeographic(B) || return false
	for k in 1:4
		isapprox(Float64(A.range[k]), Float64(B.range[k]); rtol=0, atol=1e-10 * max(1.0, abs(Float64(A.range[k])))) || return false
	end
	for k in 1:min(2, length(A.inc), length(B.inc))
		isapprox(Float64(A.inc[k]), Float64(B.inc[k]); rtol=1e-10, atol=0) || return false
	end
	return true
end

"""
	replace_grid!(fig::QtFigure, G::GMTgrid; name="", zrange=nothing) -> fig

Replace the base grid in a live InteractiveGMT scene without rebuilding the window, preserving its
camera, vertical exaggeration and overlays. Movie use intentionally requires the new grid to have the
same x/y geometry as the current base grid; changing the mesh geometry while animating would invalidate
a fixed camera and is outside the first movie backend. Pass `zrange=(zmin,zmax)` to keep a stable
colour scale across frames instead of autoscaling each replacement grid independently.
"""
function replace_grid!(fig::QtFigure, G::GMTgrid; name::AbstractString="", zrange=nothing)
	isalive(fig) || error("replace_grid!: the viewer window is closed")
	curfig = get(_FIGREG, fig.h, fig)
	base = curfig isa QtFigure ? curfig.G : fig.G
	_same_grid_geometry(base, G) || throw(ArgumentError("replace_grid!: movie frames must have the same size, registration, range and increment as the current base grid"))
	objname = isempty(name) ? _host_grid_name(fig.h) : String(name)
	_apply_host_grid!(fig.h, G, objname; zrange=zrange)
	return fig
end

function _movie_resolve_alias(primary, alias, default, longname, shortname)
	alias === nothing && return primary
	!isequal(primary, default) && !isequal(primary, alias) &&
		throw(ArgumentError("movie: specify only `$longname` or `$shortname`, not conflicting values for both"))
	return alias
end

function _movie_owned_frame_file(filename::AbstractString, stem::AbstractString)
	base, ext = splitext(String(filename))
	lowercase(ext) == ".png" || return false
	prefix = String(stem) * "_"
	startswith(base, prefix) || return false
	# Drop the exact prefix using character-aware iteration (works for Unicode movie names).
	i = nextind(base, firstindex(base), length(prefix))
	tag = i > lastindex(base) ? "" : String(SubString(base, i))
	return !isempty(tag) && all(isdigit, tag)
end

function _movie_render_size(fig::_MovieFigure)
	w = Ref{Cint}(0); h = Ref{Cint}(0)
	ok = ccall(_fn(:gmtvtk_render_size_h), Cint,
	           (Ptr{Cvoid}, Ref{Cint}, Ref{Cint}), _fig_handle(fig), w, h)
	ok != 0 || error("movie: could not query the viewer render size")
	return (Int(w[]), Int(h[]))
end

"""
	movie(frame!::Function, fig::InteractiveGMT figure; frames, kwargs...) -> String
	movie(fig, frame!::Function; frames, kwargs...) -> String

Create a movie from a **live InteractiveGMT Qt/VTK scene**. This method extends `GMT.movie` by Julia
dispatch; the existing GMT.jl script-based `movie(main; ...)` method is unchanged.

For each normalized frame, `frame!(fig, f::MovieFrame)` mutates the live scene and InteractiveGMT
captures the VTK render surface belonging to that exact figure. PNG frames are encoded directly with
FFmpeg for `format=:mp4`, `:webm`, or `:gif`; `format=:png` leaves the PNG sequence as the product.

`frames` accepts a positive integer, range/vector, matrix (one row per frame), GMTdataset-like object
with a matrix `.data`, or a text table filename. `MovieFrame.frame` is zero-based like GMT movie's
`MOVIE_FRAME`; `cols`, `text`, and `words` are the in-process counterparts of `MOVIE_COL*`,
`MOVIE_TEXT`, and `MOVIE_WORD*`.

Supported GMT-style keyword aliases in this first backend are `T`/`frames`, `N`/`name`,
`D`/`frame_rate`, `F`/`format`, `W`/`work_dir`, `Z`/`clean`, `H`/`scale`, and `Q`/`debug`.
`scale` is currently VTK capture magnification; GMT's supersample-then-downsample `-H` semantics are a
follow-up refinement.

By default the original display state (camera, VE, 2-D/3-D mode, colorbar placement) is restored after
rendering, even on an error. Arbitrary data/overlay mutations performed by the callback are **not**
rolled back.
"""
function movie(frame!::Function, fig::_MovieFigure;
               frames=nothing, T=nothing,
               name::AbstractString="movie", N=nothing,
               frame_rate::Real=24, D=nothing,
               format=:mp4, F=nothing,
               work_dir=nothing, W=nothing,
               clean::Bool=false, Z=nothing,
               scale::Integer=1, H=nothing,
               debug::Bool=false, Q=nothing,
               restore_view::Bool=true,
               ffmpeg=nothing, overwrite::Bool=true, verbose::Bool=false)
	isalive(fig) || error("movie: the viewer window is closed")
	frames = _movie_resolve_alias(frames, T, nothing, "frames", "T")
	frames === nothing && throw(ArgumentError("movie: `frames` (or `T`) is required"))
	name = String(_movie_resolve_alias(name, N, "movie", "name", "N"))
	frame_rate = Float64(_movie_resolve_alias(frame_rate, D, 24, "frame_rate", "D"))
	format = _movie_format(_movie_resolve_alias(format, F, :mp4, "format", "F"))
	work_dir = _movie_resolve_alias(work_dir, W, nothing, "work_dir", "W")
	clean = Bool(_movie_resolve_alias(clean, Z, false, "clean", "Z"))
	scale = Int(_movie_resolve_alias(scale, H, 1, "scale", "H"))
	debug = Bool(_movie_resolve_alias(debug, Q, false, "debug", "Q"))
	frame_rate > 0 || throw(ArgumentError("movie: `frame_rate` must be > 0"))
	scale >= 1 || throw(ArgumentError("movie: `scale` must be >= 1"))
	debug && (clean = false; verbose = true)
	format === :png && clean && throw(ArgumentError("movie: `clean=true` would delete the PNG product; use clean=false with format=:png"))

	seq = _movie_frames(frames)
	stem, frame_dir, output = _movie_paths(name, work_dir, format)
	mkpath(frame_dir)
	width = length(seq[1].tag)
	existing = filter(f -> _movie_owned_frame_file(f, stem), readdir(frame_dir))
	if !isempty(existing)
		overwrite || error("movie: frame directory already contains '$(stem)_<number>.png' files: $frame_dir")
		foreach(f -> rm(joinpath(frame_dir, f); force=true), existing)
	end
	if format !== :png && isfile(output)
		overwrite || error("movie: output already exists: $output")
		rm(output; force=true)
	end

	h = _fig_handle(fig)
	initial_state = ""
	if restore_view
		try
			initial_state = _scene_state_full_raw(h)
		catch
			initial_state = ""
		end
	end
	frame_size = _movie_render_size(fig)
	created = String[]
	try
		for f in seq
			isalive(fig) || error("movie: viewer window was closed while rendering frame $(f.frame)")
			frame!(fig, f)
			# Let queued Qt state changes settle before the explicit render+capture in save_png(fig,...).
			ccall(_fn(:gmtvtk_process_events), Cint, ())
			_movie_render_size(fig) == frame_size ||
				error("movie: the viewer render surface was resized during frame $(f.frame); keep the window size fixed while rendering")
			path = joinpath(frame_dir, stem * "_" * f.tag * ".png")
			save_png(fig, path; scale=scale) || error("movie: VTK failed to capture frame $(f.frame) to '$path'")
			push!(created, path)
			verbose && println("InteractiveGMT movie: frame $(f.frame + 1)/$(f.nframes) -> $path")
		end

		if format !== :png
			exe = _movie_ffmpeg_path(ffmpeg)
			pattern = joinpath(frame_dir, stem * "_%0$(width)d.png")
			args = _movie_ffmpeg_args(exe, pattern, output, format, frame_rate; verbose=verbose)
			verbose && println("InteractiveGMT movie: encoding -> $output")
			run(Cmd(args))
			isfile(output) || error("movie: ffmpeg returned without creating '$output'")
		end
	finally
		if restore_view && !isempty(initial_state) && isalive(fig)
			try
				ccall(_fn(:gmtvtk_apply_scene_state), Cvoid, (Ptr{Cvoid}, Cstring), h, initial_state)
			catch e
				@warn "movie: failed to restore the original InteractiveGMT display state" exception=(e,)
			end
		end
	end

	if clean && format !== :png
		foreach(p -> rm(p; force=true), created)
		try
			isempty(readdir(frame_dir)) && rm(frame_dir)
		catch
		end
	end
	return output
end

# Also support the explicit argument order used in the design discussion; do-block syntax naturally
# targets the callback-first method above.
movie(fig::_MovieFigure, frame!::Function; kwargs...) = movie(frame!, fig; kwargs...)
