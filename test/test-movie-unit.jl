# Pure-Julia movie scheduler tests. No Qt/VTK DLL and no ffmpeg process are required.
@testitem "movie: frame normalization" tags=[:unit, :fast, :movie] begin
    IG = InteractiveGMT
    F = IG._movie_frames(3)
    @test length(F) == 3
    @test [f.frame for f in F] == [0, 1, 2]
    @test [f.value for f in F] == [0, 1, 2]
    @test F[1].progress == 0.0
    @test F[end].progress == 1.0

    R = IG._movie_frames(10:10:30)
    @test [f.value for f in R] == [10, 20, 30]
    @test R[2].cols == Any[20]

    M = [1.0 2.0; 3.0 4.0]
    T = IG._movie_frames(M)
    @test T[1].cols == Any[1.0, 2.0]
    @test T[2].value == (3.0, 4.0)
end

@testitem "movie: text table and ffmpeg contract" tags=[:unit, :fast, :movie] begin
    IG = InteractiveGMT
    p = tempname()
    write(p, "# comment\n1 2 alpha\n3 4 beta\n")
    F = IG._movie_frames(p)
    rm(p; force=true)
    @test length(F) == 2
    @test F[1].cols == Any[1.0, 2.0, "alpha"]
    @test F[1].words == ["1", "2", "alpha"]

    a = IG._movie_ffmpeg_args("ffmpeg", "x_%01d.png", "x.mp4", :mp4, 30)
    @test "libx264" in a
    @test "yuv420p" in a
    @test a[end] == "x.mp4"
    @test_throws ArgumentError IG._movie_ffmpeg_args("ffmpeg", "x.png", "x.png", :png, 30)
end

@testitem "movie: public helpers present" tags=[:unit, :fast, :movie] begin
    for s in (:MovieFrame, :movie, :orbit!, :replace_grid!, :set_layer!, :nlayers,
              :add_label!, :add_progress!, :remove_annotation!, :movie_annotations)
        @test isdefined(InteractiveGMT, s)
    end
end

# GMT -L / -P spec parsing. Pure option handling: no window, no DLL.
@testitem "movie: -L label spec parsing" tags=[:unit, :fast, :movie, :movieanno] begin
    IG = InteractiveGMT
    o = IG._anno_parse_label("f+jTL+gwhite+p1p,black+r")
    @test o.progress == false && o.source == 1
    @test o.just == 0 && o.hasfill && o.haspen && o.rounded
    @test o.fillrgb == [1.0, 1.0, 1.0] && o.penwidth == 1.0

    e = IG._anno_parse_label("e+s0.5+t%.2f s+jBL")
    @test e.source == 0 && e.scale == 0.5 && e.format == "%.2f s" && e.just == 6

    # A '+' inside a fixed string is NOT a modifier: only '+' followed by one of THIS option's own
    # modifier letters starts one, so the label keeps its own text.
    s = IG._anno_parse_label("s50+ years+jBC")
    @test s.source == 3 && s.fixed == "50+ years" && s.just == 7

    @test IG._anno_parse_label("c3+t%.1f").col == 3
    @test IG._anno_parse_label("t2").source == 5
    @test_throws ArgumentError IG._anno_parse_label("z")
    @test_throws ArgumentError IG._anno_parse_label("cX")
end

@testitem "movie: -P progress spec parsing" tags=[:unit, :fast, :movie, :movieanno] begin
    IG = InteractiveGMT
    a = IG._anno_parse_progress("a")
    @test a.progress && a.style == 0
    @test a.just == IG._ANNO_JUST["TR"]         # GMT: circles default top-right

    d = IG._anno_parse_progress("d+w500")
    @test d.style == 3 && d.width == 500.0
    @test d.just == IG._ANNO_JUST["BC"]         # …and axes bottom-centre

    # Case matters exactly as in GMT: lower case is the MOVING look, upper case the STATIC one.
    f = IG._anno_parse_progress("f+w300+glightred+P2p,black")
    @test f.style == 5 && f.hasfg && f.hasbg && f.bgwidth == 2.0
    @test f.fgrgb ≈ [1.0, 111/255, 111/255]

    b = IG._anno_parse_progress("b+ac3+f14p,Helvetica,white")
    @test b.annot && b.source == 4 && b.col == 3 && b.fontsize == 14.0
    @test b.fontrgb == [1.0, 1.0, 1.0]

    @test IG._anno_parse_progress("c+ae").source == 0
    @test_throws ArgumentError IG._anno_parse_progress("z")
end

@testitem "movie: annotation text is rendered host-side" tags=[:unit, :fast, :movie, :movieanno] begin
    IG = InteractiveGMT
    F = IG._movie_frames(1:5)
    mk(src; fmt="", scale=NaN, col=0, fixed="") =
        IG._AnnoSpec(1, false, src, fixed, fmt, scale, col, false)

    @test IG._anno_text(mk(1), F[3], 10.0) == "2"                 # frame numbers count from 0
    @test IG._anno_text(mk(2), F[5], 10.0) == "100"               # percent, whole by default
    @test IG._anno_text(mk(2; fmt="%.1f%%"), F[1], 10.0) == "0.0%"
    @test IG._anno_text(mk(3; fixed="hello"), F[1], 10.0) == "hello"
    # Elapsed: +s wins, else 1/frame_rate.
    @test IG._anno_text(mk(0; scale=2.0), F[3], 10.0) == "4"
    @test IG._anno_text(mk(0), F[3], 10.0) == "0.2"
    # A column/word index is 0-based like GMT's, and out of range is an error, not a silent blank.
    T = IG._movie_frames([(1.0, 2.0), (3.0, 4.0)])
    @test IG._anno_text(mk(4; col=1, fmt="%.1f"), T[2], 10.0) == "4.0"
    @test_throws ArgumentError IG._anno_text(mk(4; col=9), T[1], 10.0)
end

@testitem "movie: annotation lengths, justifications and colours" tags=[:unit, :fast, :movie, :movieanno] begin
    IG = InteractiveGMT
    @test IG._anno_len("20")  == 20.0
    @test IG._anno_len("20p") == 20.0
    @test IG._anno_len("1i")  == 72.0
    @test IG._anno_len("2.54c") ≈ 72.0
    @test_throws ArgumentError IG._anno_len("wide")

    @test IG._anno_just(:TL) == 0 && IG._anno_just("br") == 8
    @test_throws ArgumentError IG._anno_just("XX")

    @test IG._anno_rgb(:white) == [1.0, 1.0, 1.0]
    @test IG._anno_rgb("255/0/0") == [1.0, 0.0, 0.0]
    @test IG._anno_rgb((0, 128, 255)) ≈ [0.0, 128/255, 1.0]
    w, rgb = IG._anno_pen("2p,red")
    @test w == 2.0 && rgb == [1.0, 0.0, 0.0]
end

# The cube-sweep method reads the LAYER off each frame's value, so a sub-range animates the layers
# the caller named instead of 1..N. Pure normalization, no window needed.
@testitem "movie: cube sweep reads layer numbers from frame values" tags=[:unit, :fast, :movie] begin
    IG = InteractiveGMT
    F = IG._movie_frames(20:22)
    @test [IG._movie_layer_number(f) for f in F] == [20, 21, 22]
    @test IG._movie_layer_number(IG._movie_frames([7.0])[1]) == 7   # whole Float64 is a layer too
    @test_throws ArgumentError IG._movie_layer_number(IG._movie_frames([2.5])[1])
    @test_throws ArgumentError IG._movie_layer_number(IG._movie_frames(["a b"])[1])
end
