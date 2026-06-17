# CI-safe tests: exercise the pure-Julia helpers (colour parsing, CPT nodes, vertical-scale
# resolution, dataset/drape packing, poly2fv). These never touch the Qt+VTK DLL, so they run
# anywhere `using InteractiveGMT` succeeds. The viewer windows themselves (view_grid/points/fv)
# need a live display + the built gmtvtk.dll, so they are exercised from examples/, not here.

@testitem "exports present" tags=[:unit, :fast] begin
    for s in (:view_grid, :view_points, :view_fv, :f3dview, :add!, :add_curtain!,
              :show_table, :selection, :isalive, :poly2fv, :save_png, :wait_windows,
              :QtFigure, :QtPoints, :QtFV)
        @test isdefined(InteractiveGMT, s)
    end
end

@testitem "colour parsing" tags=[:unit, :fast] begin
    oc = InteractiveGMT._ovl_color
    @test oc(:red, :lines)        == (1.0, 0.0, 0.0)
    @test oc(nothing, :lines)     == (0.0, 0.0, 0.0)   # default line colour
    @test oc(nothing, :points)    == (1.0, 0.0, 0.0)   # default point colour
    @test oc(128, :points)        == (128/255, 128/255, 128/255)
    @test oc((255, 0, 0), :lines) == (1.0, 0.0, 0.0)
    @test_throws ErrorException oc(:chartreuse, :lines)

    pc = InteractiveGMT._parse_gmt_color
    @test pc("-G#ff0000") == (0xff, 0x00, 0x00)
    @test pc("0/128/255") == (0x00, 0x80, 0xff)
    @test pc("64")        == (0x40, 0x40, 0x40)
    @test pc("red")       == (0xff, 0x00, 0x00)
end

@testitem "vertical-scale resolution" tags=[:unit, :fast] begin
    rz = InteractiveGMT._resolve_zscale
    @test rz(2.5, 10, 10, 5, 0.2, false, :auto) == 2.5            # explicit number passes through
    @test rz(:auto, 10, 10, 5, 0.2, true, 20) ≈ 20 / InteractiveGMT._DEG2M  # geog + vexag
    @test rz(:auto, 10, 0, 0, 0.2, false, :auto) == 1.0          # dz<=0 -> no scale
end

@testitem "CPT control nodes" tags=[:unit, :fast] begin
    cz, crgb, n = InteractiveGMT._cpt_nodes_range(0.0, 100.0, :turbo)
    @test n > 1
    @test length(cz) == n
    @test length(crgb) == 3n
    @test issorted(cz)
    # invalid range / no cmap -> the empty (fall-back-to-ramp) sentinel
    @test InteractiveGMT._cpt_nodes_range(1.0, 1.0, :turbo) == (Float64[], Float64[], 0)
    @test InteractiveGMT._cpt_nodes_range(0.0, 1.0, nothing) == (Float64[], Float64[], 0)
end

@testitem "grid sampling + dataset packing" tags=[:unit, :fast] begin
    GMT = InteractiveGMT.GMT
    G = GMT.mat2grid(Float32[ix + iy for iy in 0:3, ix in 0:4]; x=[0.0, 4.0], y=[0.0, 3.0])
    @test InteractiveGMT._sample_grid(G, 0.0, 0.0) ≈ 0.0
    @test InteractiveGMT._sample_grid(G, 4.0, 3.0) ≈ 4.0 + 3.0
    # a 2-column line drapes onto the surface (z sampled); a 3-column line keeps its own z
    xyz, segoff, nseg, npts = InteractiveGMT._pack_dataset([0.0 0.0; 4.0 3.0], G)
    @test npts == 2 && nseg == 1 && segoff == Int32[0, 2]
    @test xyz[3] ≈ 0.0 && xyz[6] ≈ 7.0          # sampled z at the two corners
end

@testitem "poly2fv builds a GMTfv" tags=[:unit, :fast] begin
    GMT = InteractiveGMT.GMT
    sq(x0, y0, z) = [x0 y0 z; x0+1 y0 z; x0+1 y0+1 z; x0 y0+1 z; x0 y0 z]
    D = [GMT.mat2ds(sq(0, 0, 0.0)), GMT.mat2ds(sq(2, 0, 1.0))]
    fv = poly2fv(D)
    @test fv isa GMT.GMTfv
    @test size(fv.verts, 1) == 8                # 4 corners × 2 squares (closing vertex dropped)
    @test sum(length, fv.color) == 2            # one colour per face
    @test fv.zscale > 0
end

@testitem "table reduction" tags=[:unit, :fast] begin
    tm = InteractiveGMT._table_matrix
    M, cn, nm = tm([1.0 2.0; 3.0 4.0]);  @test size(M) == (2, 2) && nm == "matrix"
    v, _, vnm = tm([1.0, 2.0, 3.0]);     @test size(v) == (3, 1) && vnm == "vector"
end
