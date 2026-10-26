# daylength_anim.jl — a movie of how the tilt of Earth's rotation axis sets the length of the day
# through a year. The pictures are the ones T & L > "Day length & axial tilt (GMT figures)" shows, drawn
# by the SAME function (InteractiveGMT.daylength_frame, src/daylength.jl, where the maths is described);
# this file only steps the days and encodes them with ffmpeg.
#
#   include(raw"C:\Users\j\.julia\dev\InteractiveGMT\examples\daylength_anim.jl")
#   daylength_anim(out=raw"C:\Users\j\Documents\daylength.mp4")   # 2026, every 2nd day
#   daylength_anim(step=1, tilt=0.0, out=raw"C:\Users\j\Documents\daylength_notilt.mp4")

using InteractiveGMT, Printf
using InteractiveGMT: daylength_year, daylength_cpt, daylength_frame, EARTH_TILT

"""
    daylength_anim(; out, year=2026, tilt=23.44, step=2, fps=12, view=0)

Render every `step`-th day of `year` with `daylength_frame` and encode the frames into `out` with
ffmpeg. `view`: 0 orthographic, 1 Polar (North), 2 Polar (South). The frames go to a folder beside
`out`, removed when the movie is written. Returns `out`.
"""
function daylength_anim(; out::String, year::Int=2026, tilt::Float64=EARTH_TILT, step::Int=2, fps::Int=12,
                        view::Int=0)
	println("Computing the year's declinations and day lengths...")
	decl, G = daylength_year(; year=year, tilt=tilt)
	cpt = daylength_cpt()
	dir = splitext(out)[1] * "_frames"
	mkpath(dir)
	days = collect(1:step:length(decl))
	for (k, doy) in enumerate(days)
		print("\rframe $k/$(length(days))")
		daylength_frame(doy; year=year, tilt=tilt, decl=decl, G=G, cpt=cpt, view=view,
		                savefig=joinpath(dir, @sprintf("f%04d.png", k)))
	end
	println()
	pat = joinpath(dir, "f%04d.png")
	run(`ffmpeg -y -loglevel error -framerate $fps -i $pat -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2"
	     -c:v libx264 -pix_fmt yuv420p $out`)
	rm(dir; recursive=true)
	println("Wrote $out")
	return out
end
