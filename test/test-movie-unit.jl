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
    for s in (:MovieFrame, :movie, :orbit!, :replace_grid!)
        @test isdefined(InteractiveGMT, s)
    end
end
