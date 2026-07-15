# rtp3d.jl — reduce a magnetic field anomaly grid to the pole via 2-D Fourier transform, given the
# inclination/declination of the field and of the magnetization (port of Mirone/M.A.Tivey's
# utils/rtp3d.m, 1992/1994). Can also isolate the X (north), Y (east) or Z (up) component instead
# of doing a full RTP. The 2-D FFT is built from GMT.jl's own `fft1d` (GMT_FFT_1D), applied
# row-wise then column-wise — no FFTW.jl, no in-house FFT (same convention as xyanalysis.jl's 1-D
# case: everything Fourier goes through the GMT library).

"""
    fout, k = rtp3d(f3d, incl_fld, decl_fld, incl_mag, decl_mag; component=0)

Reduce a magnetic field anomaly map `f3d` to the pole, given the inclination/declination (degrees)
of the ambient field (`incl_fld`, `decl_fld`) and of the magnetization (`incl_mag`, `decl_mag`).

`component` selects an alternative output instead of the plain RTP: `1` = X/North, `2` = Y/East,
`3` = Z/Up component. Default `0` is the RTP.

Returns the transformed grid `fout` and the wavenumber array `k`.
"""
function rtp3d(f3d::Matrix{<:Real}, incl_fld::Real, decl_fld::Real, incl_mag::Real, decl_mag::Real;
               component::Integer=0)
	_rtp3d(Float64.(f3d), Float64(incl_fld), Float64(decl_fld), Float64(incl_mag), Float64(decl_mag), Int(component))
end

# Annotate all inputs because Julia recompiles everything if a single input type changes.
function _rtp3d(f3d::Matrix{Float64}, incl_fld::Float64, decl_fld::Float64, incl_mag::Float64, decl_mag::Float64,
                 component::Int)
	D2R = pi / 180
	incl_fld *= D2R;  decl_fld *= D2R
	incl_mag *= D2R;  decl_mag *= D2R

	ny, nx = size(f3d)
	x = collect((-0.5):(1/nx):(0.5 - 1/nx))
	y = collect((-0.5):(1/ny):(0.5 - 1/ny))

	X = Matrix{Float64}(undef, ny, nx)
	Y = Matrix{Float64}(undef, ny, nx)
	@inbounds for c = 1:nx, r = 1:ny
		X[r, c] = x[c]
		Y[r, c] = y[r]
	end
	k = 2pi .* sqrt.(X.^2 .+ Y.^2)			# wavenumber array
	aux = atan.(Y, X)						# auxiliary angle, computed once (as in the .m)

	# ------ geometric and amplitude factors
	Ob = sin(incl_fld) .+ im .* cos(incl_fld) .* sin.(aux .+ decl_fld)
	O = if (abs(incl_fld - incl_mag) > 0.03 || abs(decl_fld - decl_mag) > 0.03 || component != 0)  # 0.03 rad is < 2 deg
		Om = sin(incl_mag) .+ im .* cos(incl_mag) .* sin.(aux .+ decl_mag)
		Ob .* Om
	else
		Ob .* Ob
	end

	if (component != 0)						# X, Y or Z component instead of a plain RTP
		new_inc, new_dec = component == 1 ? (0.0, 0.0) :
		                   component == 2 ? (0.0, 90.0) :
		                   component == 3 ? (90.0, 0.0) : error("rtp3d: component must be 0, 1, 2 or 3")
		new_inc *= D2R;  new_dec *= D2R
		O = O ./ ((sin(new_inc) .+ im .* (cos(new_inc) .* sin.(aux .+ new_dec)) .+ eps()) .* Om)
	end
	O = _fftshift2(O)

	mfin = sum(f3d) / length(f3d)			# mean of the input field
	F = _fft2_gmt(complex.(f3d .- mfin))
	fout = real.(_ifft2_gmt(F ./ O))
	return fout, k
end

# ---- 2-D FFT/IFFT via GMT's own 1-D C FFT (GMT.fft1d -> GMT_FFT_1D), applied separably: each row
# then each column. Mathematically identical to a native 2-D FFT since the DFT is separable.
function _fft2_gmt(A::Matrix{<:Complex})
	ny, nx = size(A)
	B = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for r = 1:ny
		B[r, :] = GMT.fft1d(view(A, r, :))
	end
	C = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for c = 1:nx
		C[:, c] = GMT.fft1d(view(B, :, c))
	end
	return C
end

function _ifft2_gmt(A::Matrix{<:Complex})
	ny, nx = size(A)
	B = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for r = 1:ny
		B[r, :] = GMT.fft1d(view(A, r, :); inverse=true)
	end
	C = Matrix{ComplexF64}(undef, ny, nx)
	@inbounds for c = 1:nx
		C[:, c] = GMT.fft1d(view(B, :, c); inverse=true)
	end
	return C
end

# Same convention as AbstractFFTs' fftshift: circular shift by half (rounded down) each dimension.
_fftshift2(A::Matrix{<:Complex}) = circshift(A, div.(size(A), 2))
