# gmtannot.jl — the X,Y window's annotations (MATLAB's figure ANNOTATIONS: text arrow, arrow, double
# arrow, line, text box, rectangle, ellipse) as GMT.jl code.
#
# File > Plot with GMT.jl writes each annotation as ONE call of `annotation!`, with its coordinates,
# pen, fill and text spelled out, and puts the function's definition at the top of the script (after
# `using GMT`). The definition is plain GMT.jl, so the script reproduces the annotations with nothing
# but GMT.jl: it runs from the editor's Run, from a saved .jl, and from the Standalone zip alike, and
# a user edits an annotation by editing its call. This text is THE definition — the script carries it
# verbatim (`_xy_with_annot_helpers`, xyscript.jl) and nothing else draws an annotation for GMT.

const _GMT_ANNOT_SRC = raw"""
# annotation!(kind; x, y, ...) — MATLAB's figure annotations, drawn on the current GMT.jl figure in the
# axes' DATA coordinates. kind: :textarrow, :arrow, :doublearrow, :line, :textbox, :rectangle, :ellipse.
#   x = (x0, x1), y = (y0, y1)  a line's start and end (an arrow's head at the end, a text arrow's text
#                               at the start), or two opposite corners of a box / of an ellipse's box
#   pen     = "width,colour[,style]"   head = the arrow head's length, e.g. "8p"
#   fill    = the colour inside a box or an ellipse, or nothing
#   box     = a text box draws its box (false: only its text)
#   text    = a text arrow's / text box's text, font = (size, name, colour)
#   justify = where the text hangs from its point ("MR", "TL", ...), offset = its shift ("dx/dy")
#   logx / logy = the axes are logarithmic (an ellipse is traced in their log space)
function annotation!(kind::Symbol; x, y, pen = "1p,black", head = "8p", fill = nothing, text = "", box = true,
                     font = (11, "Helvetica", "black"), justify = "", offset = "", logx = false, logy = false)
	x0, x1 = Float64.(x)
	y0, y1 = Float64.(y)
	p = split(pen, ',')
	pc = length(p) >= 2 ? String(p[2]) : "black"              # the head is filled with the pen's colour
	if kind in (:arrow, :doublearrow, :textarrow)
		sym = "v" * head * (kind == :doublearrow ? "+b" : "") * "+e+a39.3+h0+s"
		GMT.plot!(GMT.mat2ds([x0 y0 x1 y1]), symbol = sym, pen = pen, fill = pc, noclip = true)
	elseif kind == :line
		GMT.plot!(GMT.mat2ds([x0 y0; x1 y1]), pen = pen, noclip = true)
	elseif kind in (:rectangle, :textbox, :ellipse)
		if kind == :ellipse
			fw = (v, l) -> l ? log10(v) : v
			bw = (v, l) -> l ? exp10(v) : v
			cx = (fw(x0, logx) + fw(x1, logx)) / 2;  rx = abs(fw(x1, logx) - fw(x0, logx)) / 2
			cy = (fw(y0, logy) + fw(y1, logy)) / 2;  ry = abs(fw(y1, logy) - fw(y0, logy)) / 2
			t  = range(0, 2pi; length = 97)
			xy = hcat(bw.(cx .+ rx .* cos.(t), logx), bw.(cy .+ ry .* sin.(t), logy))
		else
			xy = [x0 y0; x1 y0; x1 y1; x0 y1; x0 y0]
		end
		if kind != :textbox || box                          # a text box without its box: the text alone
			fill === nothing ? GMT.plot!(GMT.mat2ds(xy), pen = pen, noclip = true) :
			                   GMT.plot!(GMT.mat2ds(xy), pen = pen, fill = fill, noclip = true)
		end
	else
		error("annotation!: unknown kind :$kind")
	end
	(kind in (:textarrow, :textbox) && !isempty(text)) || return nothing
	tx, ty = kind == :textbox ? (min(x0, x1), max(y0, y1)) : (x0, y0)
	jst = !isempty(justify) ? justify : kind == :textbox ? "TL" : (x1 >= x0 ? "MR" : "ML")
	off = !isempty(offset)  ? offset  : kind == :textbox ? "3p/-3p" : (x1 >= x0 ? "-2p/0p" : "2p/0p")
	GMT.text!(GMT.mat2ds([tx ty]; text = [text]), font = font, justify = jst, offset = off, noclip = true)
	return nothing
end
"""
