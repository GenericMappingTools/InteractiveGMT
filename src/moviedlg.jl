# moviedlg.jl — Tools > Make movie: the host side of MovieDialog (70_window.cpp,
# deps/ui/movie_dialog.ui).
#
# This file translates the dialog's "key=value" block into ONE `movie(...)` call and nothing else. It
# renders no frame, writes no file and builds no annotation of its own: a movie made from the menu and
# one made from the console go through the same scheduler, the same mutation primitives and the same
# `-L`/`-P` parser. If a behaviour is missing here, it is missing in `movie` — there is nowhere else
# for the two to disagree.
#
# Three frame sources, so the tool works on any window rather than one kind of data:
#   orbit   — `orbit!` by a fixed step each frame. Any window at all.
#   layers  — `set_layer!` over a cube's layer axis. Either kind of 3-D cube.
#   grids   — `replace_grid!` over a list of same-geometry grids. Any grid window.

# The window a scene pointer belongs to, as the figure handle every movie entry point takes.
function _moviedlg_fig(scene::Ptr{Cvoid})
	fig = get(_FIGREG, scene, nothing)
	fig === nothing && error("Make movie: this window is not in the figure registry")
	return fig
end

# "op=nlayers": how many layers the window's cube has, 0 for none. Answered through the SAME callback
# the run goes through, so the dialog never holds a copy of a count that lives on this side.
function _moviedlg_nlayers(scene::Ptr{Cvoid})::Int
	fig = get(_FIGREG, scene, nothing)
	fig === nothing && return 0
	try
		return nlayers(fig)
	catch
		return 0                      # no cube in this window — the dialog disables that source
	end
end

# The stem to use when the user left the box empty: named after what the window is showing, with no
# directory part, so `movie` places it exactly where GMT's own -N does — the working directory.
function _moviedlg_default_name(fig)::String
	base = try
		nm = _host_grid_name(_fig_handle(fig))
		isempty(nm) ? "movie" : first(splitext(basename(nm)))
	catch
		"movie"
	end
	return isempty(strip(base)) ? "movie" : base
end

# `name=` from the dialog is a FULL PATH with the format's own extension on it, because that is what a
# save dialog hands back. `movie`'s `name` is a stem (it appends the extension itself), so strip a
# trailing extension that just repeats the format instead of producing "film.mp4.mp4".
function _moviedlg_stem(name::String, format::String)::String
	base, ext = splitext(name)
	return lowercase(lstrip(ext, '.')) == lowercase(format) ? base : name
end

function _on_movie(scene::Ptr{Cvoid}, cparams::Cstring)::Cint
	try
		raw = unsafe_string(cparams)
		d = _nswing_parse(raw)
		# The one QUERY this callback answers. Kept here, not in a second export, so the dialog has a
		# single door to this side.
		_get(d, "op") == "nlayers" && return Cint(_moviedlg_nlayers(scene))

		println("Make movie <- ", replace(raw, "\n" => " | "))
		fig = _moviedlg_fig(scene)
		isalive(fig) || error("Make movie: the viewer window is closed")

		format = lowercase(_get(d, "format"))
		isempty(format) && (format = "mp4")
		name = String(strip(_get(d, "name")))
		isempty(name) && (name = _moviedlg_default_name(fig))
		name = _moviedlg_stem(name, format)
		rate = (v = tryparse(Float64, _get(d, "rate"));  v === nothing ? 24.0 : v)
		clean = _get(d, "clean") == "1"
		# format=png IS the product, so deleting the frames would delete the movie. `movie` refuses
		# that combination outright; decide it here rather than handing it an argument pair it will
		# reject in front of the user.
		format == "png" && (clean = false)

		# The annotation specs travel as GMT -L / -P strings — the same option language the scripted
		# API takes, so the dialog is a subset of it and never a second vocabulary.
		label    = (s = _get(d, "label");    isempty(s) ? nothing : s)
		progress = (s = _get(d, "progress"); isempty(s) ? nothing : s)

		src = _get(d, "source")
		common = (name = name, format = Symbol(format), frame_rate = rate, clean = clean,
		          label = label, progress = progress)

		# What the frames ARE, decided before anything is rendered, so the progress bar below knows how
		# many there will be and every source reaches the same one `movie(...)` call.
		frames, mutate = if src == "layers"
			from = (v = tryparse(Int, _get(d, "from")); v === nothing ? 1 : v)
			to   = (v = tryparse(Int, _get(d, "to"));   v === nothing ? nlayers(fig) : v)
			step = (v = tryparse(Int, _get(d, "step")); v === nothing ? 1 : max(1, v))
			(from <= to) || error("Make movie: the layer range is empty ($from to $to)")
			# `frames` counts LAYER NUMBERS here, which is exactly what From/To/Step mean, and
			# `_movie_layer_number` reads the layer off each frame's value.
			(from:step:to, (fg, f) -> set_layer!(fg, _movie_layer_number(f)))
		elseif src == "grids"
			paths = _moviedlg_paths(_get(d, "grids"))
			isempty(paths) && error("Make movie: the grid list is empty")
			grids = [_gmtread_trb(p) for p in paths]
			(eachindex(grids), (fg, f) -> replace_grid!(fg, grids[_movie_layer_number(f)]))
		else
			n  = (v = tryparse(Int, _get(d, "frames")); v === nothing ? 120 : v)
			az = (v = tryparse(Float64, _get(d, "az")); v === nothing ? 3.0 : v)
			el = (v = tryparse(Float64, _get(d, "el")); v === nothing ? 0.0 : v)
			n > 1 || error("Make movie: a camera orbit needs at least 2 frames")
			(n, (fg, _) -> orbit!(fg, az, el))
		end

		# The progress bar is THE shared one every long operation raises (gmtvtk_progress_*), not a
		# second bar built into the movie dialog. It counts FRAMES; the encode that follows is one more
		# step, announced by its own label rather than a second bar.
		nf = length(_movie_frames(frames))
		_progress_show_async(nf + 1, "Make movie — rendering…")
		out = try
			movie(fig; frames = frames, common...) do fg, f
				mutate(fg, f)
				# The encode runs INSIDE `movie`, after the last frame, so the only place to announce it
				# from out here is the last frame's own callback.
				_progress_status(f.index, f.index == nf ? "Make movie — encoding…" :
				                                         "Make movie — frame $(f.index)/$nf")
			end
		finally
			_progress_close()      # closed however this ends, so an error cannot leave a bar on screen
		end
		println("Make movie -> ", out)
		return Cint(1)
	catch e
		@error "Make movie failed" exception=(e, catch_backtrace())
		return Cint(0)
	end
end

# The grid list arrives as ONE `grids=` value, tab-separated, in the order the frames play.
#
# It cannot be one `grid=` line per file: `_nswing_parse` — THE parser for every dialog's block — keeps
# a single value per key, so repeats would collapse to the last one. Teaching it to accumulate would
# change what every other dialog's block means, to serve one caller. A tab cannot occur in a Windows
# path, so the dialog joins on one and this splits on it.
function _moviedlg_paths(s::String)::Vector{String}
	isempty(s) && return String[]
	return String[String(strip(p)) for p in split(s, '\t') if !isempty(strip(p))]
end

function _register_movie_dialog()
	ccall(_fn(:gmtvtk_set_movie_callback), Cvoid, (Ptr{Cvoid},),
	      @cfunction((s, p) -> Base.invokelatest(_on_movie, s, p), Cint, (Ptr{Cvoid}, Cstring)))
	return nothing
end
