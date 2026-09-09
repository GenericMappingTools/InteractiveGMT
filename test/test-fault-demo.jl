@testitem "fault demo: reconstructed dip and fault-parallel slip" tags=[:unit, :fast] begin
	IG = InteractiveGMT
	fw, hw = IG._fault_demo_blocks()
	@test size(fw.vertices) == (348, 3)
	@test size(hw.vertices) == (324, 3)
	@test isapprox(atand(fw.normal[2], fw.normal[3]), 64.0; atol=0.001)
	@test fw.scale == hw.scale == 50.0
	shell, shellfaces = IG._fault_demo_shell()
	volume(v) = sum(sum(v[i,:].*IG.GMT.cross(v[i+1,:],v[i+2,:]))/6 for i in 1:3:size(v,1))
	shellvolume = volume(reduce(vcat,[transpose(shell[j,:]) for i in axes(shellfaces,1) for j in shellfaces[i,:]]))
	# Every dip reconstructs closed solids with the requested face normals and conserved volume.
	for dip in 0.:90.
		meshes = IG._fault_demo_meshes(dip)
		normal = [0.,sind(dip),cosd(dip)]
		@test isapprox(sum(volume,meshes),shellvolume; atol=1e-10)
		for (b,v) in enumerate(meshes)
			@test volume(v) > 0
			distance = v*normal
			@test b == 1 ? maximum(distance) < 1e-10 : minimum(distance) > -1e-10
			# Every edge belongs to two triangles: no cracks, T-junctions or unclosed new faces.
			ids = Dict{NTuple{3,Float64},Int}()
			edges = Dict{Tuple{Int,Int},Int}()
			cap = false
			for t in 1:3:size(v,1)
				face = [get!(ids,Tuple(v[t+k,:]),length(ids)+1) for k in 0:2]
				for (i,j) in ((1,2),(2,3),(3,1))
					e = minmax(face[i],face[j]); edges[e] = get(edges,e,0)+1
				end
				if all(abs.(distance[t:t+2]) .< 1e-10)
					n = IG.GMT.cross(v[t+1,:]-v[t,:],v[t+2,:]-v[t,:])
					if sum(abs2,n) > 1e-20
						@test isapprox(abs(sum(n.*normal))/sqrt(sum(abs2,n)),1.; atol=1e-9)
						cap = true
					end
				end
			end
			@test cap
			@test all(==(2),values(edges))
		end
		allvertices = vcat(meshes...)
		@test minimum(allvertices[:,3]) == minimum(shell[:,3])
		@test maximum(allvertices[:,3]) == maximum(shell[:,3])
	end
	@test IG._fault_demo_meshes(0.) != IG._fault_demo_meshes(90.)
	identity3 = [1.0 0 0; 0 1 0; 0 0 1]
	for az in (0., 37., 90., 180., 270., 360.), dip in (0., 1., 45., 64., 90.),
		rake in (-180., -90., -37., 0., 90., 180.)
		basis = IG._fault_demo_basis(az, dip, rake)
		zero = IG._fault_demo_matrices(az, dip, rake, 0.)
		for slip in (-100., 100.)
			mats = IG._fault_demo_matrices(az, dip, rake, slip)
			# Only the hanging wall translates. It cannot move in the normal direction.
			@test mats[1] == zero[1]
			delta = mats[2][1:3,4] - zero[2][1:3,4]
			@test isapprox(delta, 0.003slip*basis.slip; atol=1e-12)
			@test abs(sum(delta .* basis.normal)) < 1e-12
			centres = [mats[k]*[0.,0.,0.,1.] for k in 1:2]
			@test isapprox(sum((centres[2][1:3]-centres[1][1:3]).*basis.normal),
				IG._FAULT_DEMO_GAP; atol=1e-12)
			for (k, mesh) in enumerate(IG._fault_demo_meshes(dip))
				r = mats[k][1:3,1:3]
				@test isapprox(transpose(r)*r, identity3; atol=1e-12)
				@test r[3,:] == [0.,0.,1.] # dip must never rotate the block's top/bottom
				# The whole mesh stays on its own side, including the raised surface markings.
				v = hcat(mesh, ones(size(mesh,1))) * transpose(mats[k])
				signed = v[:,1:3]*basis.normal
				if k == 1
					@test maximum(signed) <= -IG._FAULT_DEMO_GAP/2 + 1e-5
				else
					@test minimum(signed) >= IG._FAULT_DEMO_GAP/2 - 1e-5
				end
			end
		end
	end
	@test IG._fault_demo_basis(0., 45., 90.).slip[3] > 0 # reverse
	@test IG._fault_demo_basis(0., 45., -90.).slip[3] < 0 # normal
	@test IG._fault_demo_basis(0., 45., 0.).slip == [0., 1., 0.]
	@test IG._fault_demo_basis(0., 45., 180.).slip == [0., -1., 0.]
	@test IG._fault_demo_matrices(0., 64., -180., 100.) == IG._fault_demo_matrices(360., 64., 180., 100.)
	@test_throws ArgumentError IG._fault_demo_matrices(0., 91., 0., 0.)
	@test_throws ArgumentError IG._fault_demo_matrices(0., 45., 181., 0.)
	@test_throws ArgumentError IG._fault_demo_meshes(-1.)
	@test_throws ArgumentError IG._fault_demo_meshes(91.)
	# _focal_demo_sectors prints its serialization to stdout (the C++ side reads it through the
	# g_juliaEval redirect), so it has to be captured the same way, not with sprint's io argument.
	function sectors(s, d, r)
		old = stdout;  rd, wr = redirect_stdout()
		reader = @async read(rd, String)          # drain while it writes: never fill the pipe and block
		try  IG._focal_demo_sectors(s, d, r)  finally  redirect_stdout(old);  close(wr)  end
		return fetch(reader)
	end
	positive = sectors(25.,45.,90.)
	negative = sectors(25.,45.,-90.)
	@test occursin("#N1:",positive)
	@test occursin("#N1:",negative)
	@test positive != negative
end

@testitem "fault demo: menu, controls, playback and close" tags=[:gui] setup=[GmtvtkTest] begin
	IG = InteractiveGMT
	e = iview()
	function drive(control="", value=0, png="")
		out = zeros(51)
		ok = ccall(_test_fn(:gmtvtk_fault_demo_test), Cint,
			(Ptr{Cvoid}, Cstring, Cint, Cstring, Ptr{Cdouble}), e.h, control, value, png, out)
		@test ok == 1
		out
	end
	try
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring),
			e.h, "Seismology/Fault plane demo") == 2
		baseline = drive()
		@test baseline[33:34] == [size(v,1)/3 for v in IG._fault_demo_meshes(25.)]
		@test baseline[35:36] == [0.,0.]
		@test baseline[41:42] == [80.,66.]
		# The dialog opens on the .ui's OWN slider defaults (azimuth 45, dip 25, rake 90) - the .ui is
		# the single source of those, exactly as it is of the slip range. This line used to assert
		# 0/25/-90, which no slider in fault_plane_demo.ui has ever been set to.
		@test baseline[43:45] == [45.,25.,90.]
		# The REAL gizmo (20_gizmo.cpp) is up, and its vertical-exaggeration handle reaches the blocks:
		# Scene::ve travels through applyVE and faultDemoApplyVE into the actors' own matrices.
		@test baseline[46] == 1.0
		@test baseline[48] == 1.0
		stretched = drive("ve:3")
		@test stretched[46] == 3.0
		@test stretched[47] ≈ 3*baseline[47]
		@test drive("ve:1")[47] ≈ baseline[47]
		# 'c' recentres the rotation point on what is under the pointer. It is alive here because the
		# demo NAMES its pick targets (Scene::pickTargets) rather than owning a base surface, and
		# because the key runs the same camRecenterAtCursor the middle-click does.
		focal0 = drive()[49:51]
		focal1 = drive("keyC")[49:51]
		@test focal1 != focal0
		@test all(abs.(focal1) .< 1.0)          # landed on the model, not off in space
		for (name, val) in (("dipSlider",0),("dipSlider",90),("azimuthSlider",360),
			("azimuthSlider",45),("rakeSlider",-180),("rakeSlider",180),("slipSlider",100))
			drive(name,val)
		end
		shown = drive()
		@test shown[33:34] == [size(v,1)/3 for v in IG._fault_demo_meshes(90.)]
		# The BOX keeps its full height at every dip. WHICH block owns the low corner does not: with a
		# square plan a shallow dip runs the fault out through the front and back faces, so the hanging
		# wall is a wedge that stops short of the floor - hence the union, not a per-block equality.
		@test [min(shown[37],shown[39]), max(shown[38],shown[40])] ≈
		      [min(baseline[37],baseline[39]), max(baseline[38],baseline[40])]
		@test shown[43:45] == [45.,90.,180.]
		expected = IG._fault_demo_matrices(45.,90.,180.,100.)
		@test shown[1:32] ≈ vcat(vec(permutedims(expected[1])),vec(permutedims(expected[2])))
		# Negative slip runs the hanging wall backwards along the same line: the ball shows rake + 180.
		@test drive("slipSlider",-100)[43:45] == [45.,90.,0.]
		@test drive("resetSlipButton")[35] == 0
		@test drive()[43:45] == [45.,90.,180.]   # slip back to zero -> the slider's own rake again
		@test drive("playButton")[36] == 1
		sleep(0.2); IG._pump_once()
		playing = drive("playButton")
		@test playing[35] > 0
		@test playing[36] == 0
		drive("resetSlipButton")
		drive("dipSlider",25); drive("azimuthSlider",0); drive("rakeSlider",-90)
		drive("resetViewButton")
		shot = joinpath(tempdir(), "igmt_fault_demo.png")
		drive("",0,shot)
		@test filesize(shot) > 10_000
		rm(shot; force=true)             # test output never outlives the test
		drive("close")
		@test ccall(_test_fn(:gmtvtk_menu_trigger_test), Cint, (Ptr{Cvoid}, Cstring),
			e.h, "Fault plane demo") == 1
		@test drive()[35] == 0
		drive("playButton") # parent close must tear down a running timer and its renderer safely
	finally
		ccall(IG._fn(:gmtvtk_close), Cvoid, (Ptr{Cvoid},), e.h)
		IG._pump_once()
	end
end
