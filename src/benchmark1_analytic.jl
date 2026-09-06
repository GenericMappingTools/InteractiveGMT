#!/usr/bin/env julia

"""
Carrier--Greenspan analytical solution for Catalina 2004 benchmark 1.

The program writes free-surface profiles at 160, 175 and 220 s, plus the
shoreline history, as CSV files.  It is self-contained and uses no packages.

Run with:

    julia catalina_benchmark1.jl [output_directory]

Coordinates are positive offshore, while velocity is positive shoreward.
Thus a negative shoreline x denotes run-up onto the initially dry beach.
"""
module CatalinaBenchmark1

export catalina1, initial_eta, Parameters

# NO `using Printf`: this file is included by InteractiveGMT, whose Project.toml does not carry it,
# and the only uses were the CSV/report formatting of `run` below — done here with Base instead. The
# maths is untouched.
_g12(v::Real) = string(round(Float64(v); sigdigits = 12))
_f(v::Real, nd::Int, w::Int) = lpad(string(round(Float64(v); digits = nd)), w)

const G = 9.81

Base.@kwdef struct Parameters
    reference_length::Float64 = 5_000.0  # L [m], Catalina benchmark scaling
    beach_slope::Float64 = 0.1           # tan(beta), i.e. 1:10
    H1::Float64 = 0.006
    c1::Float64 = 0.4444
    x1::Float64 = 4.1209
    H2::Float64 = 0.018
    c2::Float64 = 4.0
    x2::Float64 = 1.6384
end

# Cephes approximations. Accuracy is about 1e-7 and is ample relative to the
# finite quadrature used for the two Carrier--Greenspan integrals.
function j0(x::Float64)
    ax = abs(x)
    if ax < 8.0
        y = x*x
        p = 57568490574.0 + y*(-13362590354.0 + y*(651619640.7 + y*(-11214424.18 + y*(77392.33017 + y*(-184.9052456)))))
        q = 57568490411.0 + y*(1029532985.0 + y*(9494680.718 + y*(59272.64853 + y*(267.8532712 + y))))
        return p/q
    end
    z = 8.0/ax; y = z*z; xx = ax - 0.785398164
    p = 1.0 + y*(-0.1098628627e-2 + y*(0.2734510407e-4 + y*(-0.2073370639e-5 + y*0.2093887211e-6)))
    q = -0.1562499995e-1 + y*(0.1430488765e-3 + y*(-0.6911147651e-5 + y*(0.7621095161e-6 - y*0.934945152e-7)))
    return sqrt(0.636619772/ax) * (cos(xx)*p - z*sin(xx)*q)
end

function j1(x::Float64)
    ax = abs(x)
    if ax < 8.0
        y = x*x
        p = x*(72362614232.0 + y*(-7895059235.0 + y*(242396853.1 + y*(-2972611.439 + y*(15704.48260 + y*(-30.16036606))))))
        q = 144725228442.0 + y*(2300535178.0 + y*(18583304.74 + y*(99447.43394 + y*(376.9991397 + y))))
        return p/q
    end
    z = 8.0/ax; y = z*z; xx = ax - 2.356194491
    p = 1.0 + y*(0.183105e-2 + y*(-0.3516396496e-4 + y*(0.2457520174e-5 + y*(-0.240337019e-6))))
    q = 0.04687499995 + y*(-0.2002690873e-3 + y*(0.8449199096e-5 + y*(-0.88228987e-6 + y*0.105787412e-6)))
    ans = sqrt(0.636619772/ax) * (cos(xx)*p - z*sin(xx)*q)
    return x < 0 ? -ans : ans
end

trapweights(n::Int, dx::Float64) = [i == 1 || i == n ? dx/2 : dx for i in 1:n]

function phi_initial(s::Float64, p::Parameters)
    s1sq, s2sq = 16p.x1, 16p.x2
    q1, q2 = s*s-s1sq, s*s-s2sq
    return -(p.H1*p.c1/16)*q1*exp(-p.c1*q1*q1/256) + (p.H2*p.c2/16)*q2*exp(-p.c2*q2*q2/256)
end

struct Spectrum
    omega::Vector{Float64}
    Bw::Vector{Float64}       # B(omega) times omega quadrature weights
end

function spectrum(p::Parameters; xi_max=15.0, nxi=3001, omega_max=100.0, nomega=3001)
    xi = collect(range(0.0, xi_max; length=nxi))
    omega = collect(range(0.0, omega_max; length=nomega))
    wx = trapweights(nxi, xi[2]-xi[1])
    ww = trapweights(nomega, omega[2]-omega[1])
    source = [wx[i] * xi[i]^2 * phi_initial(xi[i], p) for i in eachindex(xi)]
    B = zeros(nomega)
    Threads.@threads for k in eachindex(omega)
        w = omega[k]
        acc = 0.0
        @inbounds @simd for i in eachindex(xi)
            acc += source[i] * j1(w*xi[i])
        end
        B[k] = acc * ww[k]
    end
    return Spectrum(omega, B)
end

const _DEFAULT_SPECTRUM = Ref{Union{Nothing,Spectrum}}(nothing)

function default_spectrum()
    S = _DEFAULT_SPECTRUM[]
    if S === nothing
        S = spectrum(Parameters())
        _DEFAULT_SPECTRUM[] = S
    end
    return S::Spectrum
end

# u, du/dlambda, and phi_lambda at one point in transform space.
function fields(S::Spectrum, sigma::Float64, lambda::Float64)
    u = 0.0; ul = 0.0; phil = 0.0
    shoreline = sigma == 0.0
    @inbounds @simd for k in eachindex(S.omega)
        w = S.omega[k]; bw = S.Bw[k]
        r = shoreline ? 0.5w : j1(w*sigma)/sigma
        sn, cs = sincos(w*lambda)
        u    += bw*r*sn
        ul   += bw*w*r*cs
        phil -= bw*j0(w*sigma)*cs
    end
    return u, ul, phil
end

function physical_point(S::Spectrum, sigma::Float64, t::Float64; tol=2e-11, maxiter=20,
                        lambda0::Float64=2t)
    lambda = lambda0
    for _ in 1:maxiter
        u, ul, _ = fields(S, sigma, lambda)
        delta = (0.5lambda - u - t)/(0.5-ul)
        lambda -= delta
        abs(delta) < tol && break
    end
    u, _, phil = fields(S, sigma, lambda)
    eta = 0.25phil - 0.5u*u
    x = sigma*sigma/16 - eta
    return x, eta, u, lambda
end

"""
    initial_eta(x_m, p=Parameters()) -> Vector{Float64}

The benchmark's INITIAL free surface, in metres, at the offshore-positive coordinates `x_m` (metres).

This is Carrier--Wu--Yeh's two-Gaussian condition in closed form — the very function `phi_initial`
above is the transform of — evaluated directly, so it carries none of the quadrature error of the
Fourier--Bessel round trip (`catalina1(0)` reproduces this to 0.05 m rms on an 8.8 m wave). It is
what a numerical model should be STARTED from: a model given a coarser version of this condition
solves a different problem, and no amount of grid refinement brings it back.
"""
function initial_eta(x_m::Vector{<:Real}, p::Parameters=Parameters())::Vector{Float64}
    zscale = p.reference_length * p.beach_slope
    return [begin
                xn = Float64(xi) / p.reference_length
                zscale * (p.H1*exp(-p.c1*(xn - p.x1)^2) - p.H2*exp(-p.c2*(xn - p.x2)^2))
            end for xi in x_m]
end

function dimensional_scales(p::Parameters)
    zscale = p.reference_length*p.beach_slope
    uscale = sqrt(G*zscale)
    tscale = sqrt(p.reference_length/(G*p.beach_slope))
    return zscale, uscale, tscale
end

function snapshot(S::Spectrum, seconds::Real, p::Parameters; xmax=10.2, npoints=1201)
    zscale, uscale, tscale = dimensional_scales(p)
    sigmas = collect(range(0.0, 4sqrt(xmax); length=npoints))
    out = Matrix{Float64}(undef, npoints, 4)
    # SIGMA IS SWEPT OUTWARD FROM THE SHORELINE, each Newton started at the previous sigma's lambda.
    # The lambda(sigma, t) equation has more than one root near the shore, and solving every point
    # independently from lambda = 2t (which is what a threaded loop must do) lets neighbouring sigmas
    # land on DIFFERENT branches: at t = 175 s — maximum draw-down, the shoreline at x = +240 m —
    # that produced a curve folding back to x = -150 m with eta = +14 m, i.e. water 400 m landward of
    # a beach that is dry. Continuation keeps the whole profile on one branch; the sweep is serial for
    # exactly that reason, and it is the reason it cannot be threaded.
    tnd = seconds/tscale
    lam = 2tnd
    for (i, sigma) in pairs(sigmas)
        x, eta, u, lam = physical_point(S, sigma, tnd; lambda0=lam)
        out[i,:] .= (x*p.reference_length, eta*zscale, u*uscale, sigma)
    end
    return out[sortperm(view(out,:,1)), :]
end

"""
    catalina1(t; xmax=50_000.0, npoints=1201, parameters=Parameters())

Evaluate the nonlinear Carrier--Greenspan solution of Catalina benchmark 1 at
the arbitrary time `t` seconds. `xmax` is the maximum offshore distance in
metres. The returned named tuple contains:

  * `time`: requested time in seconds
  * `x`: offshore-positive horizontal coordinates in metres
  * `eta`: free-surface elevation in metres
  * `velocity`: shoreward-positive depth-averaged velocity in m/s
  * `shoreline_x`: shoreline coordinate in metres (negative means run-up)
  * `shoreline_eta`: shoreline elevation in metres
  * `shoreline_velocity`: shoreward-positive shoreline velocity in m/s

The expensive Fourier--Bessel spectrum is cached when the default parameters
are used, so repeated calls at different times are inexpensive.
"""
function catalina1(t::Real; xmax::Real=50_000.0, npoints::Integer=1201,
                   parameters::Parameters=Parameters())
    t >= 0 || throw(ArgumentError("time must be non-negative"))
    xmax > 0 || throw(ArgumentError("xmax must be positive"))
    npoints >= 2 || throw(ArgumentError("npoints must be at least 2"))

    isdefault = (parameters == Parameters())
    S = isdefault ? default_spectrum() : spectrum(parameters)
    A = snapshot(S, Float64(t), parameters; xmax=Float64(xmax)/parameters.reference_length,
                 npoints=Int(npoints))

    zscale, uscale, tscale = dimensional_scales(parameters)
    sx, seta, su, _ = physical_point(S, 0.0, Float64(t)/tscale)
    return (time=Float64(t),
            x=copy(A[:,1]), eta=copy(A[:,2]), velocity=copy(A[:,3]),
            shoreline_x=sx*parameters.reference_length,
            shoreline_eta=seta*zscale,
            shoreline_velocity=su*uscale)
end

catalina1(times::AbstractVector{<:Real}; kwargs...) = [catalina1(t; kwargs...) for t in times]

function write_csv(path::String, header::String, A::AbstractMatrix)
    open(path, "w") do io
        println(io, header)
        for i in axes(A,1)
            println(io, join((_g12(A[i,j]) for j in axes(A,2)), ','))
        end
    end
end

function run(outdir::String="catalina_benchmark1_output"; p=Parameters())
    mkpath(outdir)
    @info "Computing Carrier--Greenspan spectrum"
    S = p == Parameters() ? default_spectrum() : spectrum(p)
    for t in (160.0, 175.0, 220.0)
        A = snapshot(S, t, p)
        write_csv(joinpath(outdir, "surface_$(Int(t))s.csv"), "x_m,eta_m,velocity_m_s,sigma", A)
    end

    zscale, uscale, tscale = dimensional_scales(p)
    times = collect(0.0:1.0:360.0)
    shore = Matrix{Float64}(undef, length(times), 3)
    for (i, t) in pairs(times)
        x, eta, u, _ = physical_point(S, 0.0, t/tscale)
        shore[i,:] .= (t, x*p.reference_length, u*uscale)
    end
    write_csv(joinpath(outdir, "shoreline.csv"), "time_s,x_m,velocity_m_s", shore)

    println("Wrote Catalina benchmark 1 to ", abspath(outdir))
    for t in (160, 175, 220)
        A = snapshot(S, t, p; npoints=2)
        println("t=", lpad(t, 3), " s: shoreline x=", _f(A[1,1], 3, 9), " m, eta=", _f(A[1,2], 3, 8),
                " m, u=", _f(A[1,3], 3, 7), " m/s")
    end
    return nothing
end

end # module

if abspath(PROGRAM_FILE) == @__FILE__
    CatalinaBenchmark1.run(isempty(ARGS) ? "catalina_benchmark1_output" : ARGS[1])
end
