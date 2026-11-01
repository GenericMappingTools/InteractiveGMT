# Astronomy demos — accuracy and future improvements

T & L > **Solar system**, **Sky viewer**, **Earth-Moon system** (`deps/src/74_solarsys.cpp`,
`74_skyview.cpp`, `74_eclipse.cpp`). All three draw from one ephemeris, `deps/src/74_ephem.cpp`.
Tests: `test/test-sky-gui.jl`, `test/test-eclipse-gui.jl`.

## Sources

| Quantity | Source | Valid |
|---|---|---|
| Planets | NASA JPL "Keplerian elements for approximate positions" (Standish), Table 2a/2b, via astroterm | 3000 BC – 3000 AD |
| Pluto, Ceres, Eris | Linear mean elements fitted here to JPL Horizons osculating elements, every 60 days, 1800–2200 | 1800 – 2200 |
| Moon | Meeus, *Astronomical Algorithms* ch. 47 (ELP-2000/82), the 32 largest longitude/distance and 20 largest latitude terms | centuries around 2000 |
| Stars | Yale Bright Star Catalog 5th ed. (`data/bsc5`), linear proper motion | |
| Constellation figures, star names | astroterm `data/` (figures: Stellarium "modern" sky culture) | |
| TT − UT (ΔT) | Espenak & Meeus polynomials (NASA Five Millennium Canon) | |
| Sidereal time | IAU 1982 GMST (Meeus 12.4) | |
| Precession | IAU 1976 (Lieske) | |

Sun and planets are **apparent** places: light time plus annual aberration (the observer's position
taken one light time earlier). The Earth is offset from the Earth–Moon barycentre (up to 4700 km).

## Measured accuracy (from the tests)

| Check | Reference | Result |
|---|---|---|
| GMST, 1987-04-10 0h UT | Meeus 12.a | < 1e-5 deg |
| Moon λ, β, Δ, 1992-04-12 | Meeus 47.a | < 0.01 deg, < 0.01 deg, < 50 km |
| Sun RA/Dec, 1992-10-13 | Meeus 25.a (apparent) | < 0.02 deg |
| Venus RA/Dec, 1992-12-20 | Meeus 33.a (apparent) | < 0.05 deg |
| Pluto / Eris / Ceres heliocentric direction, 2000–2100 | JPL Horizons | < 0.07 / < 0.25 / < 0.7 deg |
| Sun's culmination, Lisbon, 2026 June solstice | 90 − φ + ε | az 180 ± 1 deg, alt ± 0.3 deg |
| 7 eclipses (1999–2025): greatest eclipse | NASA catalogue | within 2 min (measured: 0.1–1.5 min) |
| same: γ | NASA | within 0.012 (measured: 0.001–0.008) |
| same, lunar: umbral magnitude | NASA | within 0.02 (measured: 0.001–0.013) |
| Eclipses of 2024: dates and kinds | NASA | all four exact |
| Eclipse counts 2001–2100 | NASA: 224 solar, 228 lunar | 222 solar, 226 lunar |

The two missing eclipses on each side are grazing ones, at the edge of a definition.

## What is not modelled

- **Nutation** (< 20″). It moves the Sun and Moon alike, so it does not change an eclipse, but it
  does shift sky positions slightly.
- **Atmospheric refraction**: the Sky viewer's horizon is about 0.5 deg too low near rise and set.
- **Earth's flattening**: the eclipse geometry treats the Earth as a sphere. Central eclipses at
  high latitude are slightly misjudged, and NASA's γ limit (0.9972) is taken as 1.
- **Hybrid solar eclipses** are filed as total or annular, from greatest eclipse only.
- **Local circumstances**: the eclipse list is global (greatest eclipse, γ). It gives no contact
  times or magnitude for one place, and no path of totality.
- **Moon**: the series is truncated (≈ 10–30″). The Moon is plain grey: no texture, no libration.
- **ΔT** is a fit. Back in antiquity its uncertainty (tens of minutes to hours) moves the
  longitudes where an eclipse was seen, whatever the ephemeris.
- **Dwarf planets** outside 1800–2200 extrapolate their fitted rates and drift.
- **Saturn's magnitude** ignores the rings.

## Future improvements

1. **Nutation** (IAU 1980, the main terms) in `74_ephem.cpp`, for true apparent places.
2. **Local circumstances and paths**: Besselian elements, then the path of totality or annularity
   and the partial-eclipse limits, drawn with GMT on a map (a natural GMT.jl hand-off), plus
   contact times for the Sky viewer's place.
3. **Lunar eclipse contacts** (P1, U1 … U4, P4) and durations in the list.
4. **Moon texture** from GMT's `@moon_relief` remote grid (the Julia side already serves the
   Earth's picture), with libration from the IAU rotation model.
5. **Refraction** at the Sky viewer's horizon (Bennett's formula), and twilight/extinction dimming.
6. **The full ELP-2000 / VSOP87** (or JPL DE ephemerides when present) as an optional
   high-precision source behind the same `ephGeocentric` call.
7. **Earth's flattening** in the eclipse geometry (γ limit 0.9972, umbra on an ellipsoid).
8. **More small bodies** (Haumea, Makemake, Vesta …) by the same Horizons fit.
9. A **"next eclipse visible from here"** search, combining the eclipse list with the Sky
   viewer's place and the bodies' altitude.
