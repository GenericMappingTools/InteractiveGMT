// 74_ephem.cpp -- the ephemeris of T & L > "Solar system" (74_solarsys.cpp), "Sky viewer"
// (74_skyview.cpp) and "Earth-Moon system" (74_eclipse.cpp). ONE place for every position the demos draw:
// they never compute a body twice.
//
// Ported from astroterm (https://github.com/da-luce/astroterm, MIT, (c) 2024 Dalton Luce), whose
// terminal renderer is not used -- only its sources of numbers:
//
//   * PLANETS: NASA JPL "Keplerian elements for approximate positions of the major planets"
//     (E. M. Standish, Table 2a/2b, valid 3000 BC -- 3000 AD): six elements + linear rates per Julian
//     century, plus the b/c/s/f correction to the mean anomaly of Jupiter..Neptune. Heliocentric,
//     J2000 ecliptic. Arcminute-class, which is what a picture needs.
//   * MOON: NOT astroterm's (a bare Kepler ellipse, wrong by a degree or more). Meeus, "Astronomical
//     Algorithms" 2nd ed., chapter 47 (ELP-2000/82 truncated): the 32 largest longitude/distance and 20
//     largest latitude terms, good to ~0.01 deg.
//   * STARS: the Yale Bright Star Catalog, 5th ed. (BSC5, Hoffleit & Warren 1991), Harvard's binary
//     file as distributed (data/bsc5, J2000, 9110 entries); star names (data/bsc5_names.txt, IAU) and
//     constellation figures (data/bsc5_constellations.txt, Stellarium's "modern" sky culture with its
//     Hipparcos numbers converted to BSC5 by astroterm), both from astroterm's data/.
//
// FRAMES: everything is handed out in the J2000 EQUATORIAL frame (x to the March equinox of J2000, z to
// the J2000 celestial pole), except ephPlanetHelio, which is J2000 ECLIPTIC (the solar-system view's own
// frame). The sky viewer turns J2000 into "of date" with ephPrecession (IAU 1976) and then into the
// observer's horizon with the local sidereal time.
//
// TIME: the ephemerides run on Terrestrial Time; the Earth's turn (sidereal time) runs on UT. The user
// types UT; ephDeltaT (Espenak & Meeus polynomials) bridges the two -- minutes in antiquity, so it is
// not a detail for a sky of 500 BC.

static const double kEphRad = 3.14159265358979323846 / 180.0;
static const double kEphObliq = 23.4392911;        // obliquity of the ecliptic at J2000 (deg)
static const double kEphAUkm = 149597870.7;
static const double kEphEarthRkm = 6378.137;

// The eight planets, then the three dwarf planets (Pluto, Ceres, Eris). The dwarfs are NOT in JPL's
// approximate-position tables, so their rows were made here the way Standish made the planets': a
// least-squares straight line through each of JPL Horizons' heliocentric osculating elements (J2000
// ecliptic, every 60 days, 1800-2200 AD; L = Omega + omega + M and varpi = Omega + omega fitted, then
// written in this table's M/omega form). Against Horizons over 1800-2200 the largest error is 0.05 deg
// (Pluto), 0.18 deg (Eris), 0.6 deg (Ceres, pulled about by Jupiter); outside that span they extrapolate
// and drift. A single osculating orbit run as pure Kepler was tried first: Ceres was 2 deg off after 26
// years, 5 deg after 74.
enum EphPlanet { EPH_MERCURY, EPH_VENUS, EPH_EMB, EPH_MARS, EPH_JUPITER, EPH_SATURN, EPH_URANUS, EPH_NEPTUNE,
                 EPH_PLUTO, EPH_CERES, EPH_ERIS, EPH_NPLANETS };
static const int kEphMajor = EPH_PLUTO;    // the first dwarf planet: 0..kEphMajor-1 are the planets
static const char *kEphPlanetName[EPH_NPLANETS] = {"Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn",
                                                   "Uranus", "Neptune", "Pluto", "Ceres", "Eris"};
// Mean radius (km) of each body, for the drawn sizes.
static const double kEphPlanetRkm[EPH_NPLANETS] = {2439.7, 6051.8, 6371.0, 3389.5, 69911.0, 58232.0, 25362.0,
                                                   24622.0, 1188.3, 469.7, 1163.0};

// JPL Table 2a: a (AU), e, I, M = L - varpi, omega = varpi - Omega, Omega (deg), and their rates per
// Julian century (astroterm's data/keplerian_elements.c, which holds them in this M/omega form).
struct EphElems { double a, e, I, M, w, O; };
static const EphElems kEphEl[EPH_NPLANETS] = {
	{0.38709843, 0.20563661, 7.00559432, 174.79394829, 29.11810076, 48.33961819},
	{0.72332102, 0.00676399, 3.39777545, 50.21215137, 55.09494217, 76.67261496},
	{1.00000018, 0.01673163, -0.00054346, -2.46314313, 108.04266274, -5.11260389},
	{1.52371243, 0.09336511, 1.85181869, 19.34931620, -73.63065768, 49.71320984},
	{5.20248019, 0.04853590, 1.29861416, 20.05983908, -86.01787410, 100.29282654},
	{9.54149883, 0.05550825, 2.49424102, -42.78564734, -20.77862639, 113.63998702},
	{19.18797948, 0.04685740, 0.77298127, 141.76872184, 98.47154226, 73.96250215},
	{30.06952752, 0.00895439, 1.77005520, 257.54130563, -85.10477129, 131.78635853},
	{39.53602135, 0.24885599, 17.14129809, 14.83935291, 113.76270952, 110.30869514},    // Pluto
	{2.76707248, 0.07781749, 10.59511580, 7.65990147, 72.52726481, 80.55677676},         // Ceres
	{67.89138987, 0.43799640, 43.99332121, 193.85501706, 151.21075275, 35.97616239}};    // Eris
static const EphElems kEphRate[EPH_NPLANETS] = {
	{0.00000000, 0.00002123, -0.00590158, 149472.51546610, 0.28154195, -0.12214182},
	{-0.00000026, -0.00005107, 0.00043494, 58517.75880612, 0.32953822, -0.27274174},
	{-0.00000003, -0.00003661, -0.01337178, 35999.05511069, 0.55919116, -0.24123856},
	{0.00000097, 0.00009149, -0.00724757, 19139.84710618, 0.72076056, -0.26852431},
	{-0.00002864, 0.00018026, -0.00322699, 3034.72172561, 0.05174577, 0.13024619},
	{-0.00003065, -0.00032044, 0.00451969, 1221.57315246, 0.79194480, -0.25015002},
	{-0.00020455, -0.00001550, -0.00180155, 428.40245610, 0.03527286, 0.05739699},
	{0.00006447, 0.00000818, 0.00022400, 218.45505376, 0.01616240, -0.00606302},
	{0.00157819, 0.00012420, 0.00052248, 145.24160120, -0.03656056, -0.00725730},     // Pluto
	{-0.00000199, -0.00041112, -0.01680614, 7817.78163014, 3.19273139, -1.49225274},    // Ceres
	{0.00180248, 0.00009995, 0.00110506, 64.57253426, -0.08148418, 0.00290767}};        // Eris
// JPL Table 2b: M += b T^2 + c cos(f T) + s sin(f T), Jupiter..Neptune.
struct EphExtra { double b, c, s, f; };
static const EphExtra kEphExtra[EPH_NPLANETS] = {
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
	{-0.00012452, 0.06064060, -0.35635438, 38.35125000},
	{0.00025899, -0.13434469, 0.87320147, 38.35125000},
	{0.00058331, -0.97731848, 0.17689245, 7.67025000},
	{-0.00041348, 0.68346318, -0.10162547, 7.67025000},
	{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

// The Titius-Bode law: a = 0.4 + 0.3 * 2^n AU (Mercury n = -inf, Venus 0, Earth 1, Mars 2, Ceres 3,
// Jupiter 4, Saturn 5, Uranus 6, Neptune 7, Pluto 8; Eris, found long after, given the next slot, 9).
static double ephTitiusBode(int p) {
	static const int n[EPH_NPLANETS] = {-99, 0, 1, 2, 4, 5, 6, 7, 8, 3, 9};
	return n[p] == -99 ? 0.4 : 0.4 + 0.3 * std::pow(2.0, n[p]);
}

// Julian Date of a UTC date-time (proleptic Gregorian calendar, as QDate counts) and back.
static double ephJD(const QDateTime &utc) {
	const QDateTime u = utc.toUTC();
	return double(u.date().toJulianDay()) - 0.5 + u.time().msecsSinceStartOfDay() / 86400000.0;
}
static QDateTime ephDateTime(double jd) {
	const double j = jd + 0.5;
	const qint64 day = qint64(std::floor(j));
	const qint64 ms = qint64(std::llround((j - double(day)) * 86400000.0));
	QDateTime t(QDate::fromJulianDay(day), QTime(0, 0), Qt::UTC);
	return t.addMSecs(ms);
}

// TT - UT (s) for a decimal year: Espenak & Meeus (NASA, Five Millennium Canon of Solar Eclipses).
static double ephDeltaT(double y) {
	auto u2 = [](double y) { const double u = (y - 1820.0) / 100.0; return -20.0 + 32.0 * u * u; };
	double t;
	if (y < -500.0) return u2(y);
	if (y < 500.0) {
		const double u = y / 100.0;
		return 10583.6 + u * (-1014.41 + u * (33.78311 + u * (-5.952053 + u * (-0.1798452 + u * (0.022174192 +
		       u * 0.0090316521)))));
	}
	if (y < 1600.0) {
		const double u = (y - 1000.0) / 100.0;
		return 1574.2 + u * (-556.01 + u * (71.23472 + u * (0.319781 + u * (-0.8503463 + u * (-0.005050998 +
		       u * 0.0083572073)))));
	}
	if (y < 1700.0) { t = y - 1600.0; return 120.0 - 0.9808 * t - 0.01532 * t * t + t * t * t / 7129.0; }
	if (y < 1800.0) {
		t = y - 1700.0;
		return 8.83 + t * (0.1603 + t * (-0.0059285 + t * (0.00013336 - t / 1174000.0)));
	}
	if (y < 1860.0) {
		t = y - 1800.0;
		return 13.72 + t * (-0.332447 + t * (0.0068612 + t * (0.0041116 + t * (-0.00037436 + t * (0.0000121272 +
		       t * (-0.0000001699 + t * 0.000000000875))))));
	}
	if (y < 1900.0) {
		t = y - 1860.0;
		return 7.62 + t * (0.5737 + t * (-0.251754 + t * (0.01680668 + t * (-0.0004473624 + t / 233174.0))));
	}
	if (y < 1920.0) {
		t = y - 1900.0;
		return -2.79 + t * (1.494119 + t * (-0.0598939 + t * (0.0061966 - 0.000197 * t)));
	}
	if (y < 1941.0) { t = y - 1920.0; return 21.20 + t * (0.84493 + t * (-0.076100 + 0.0020936 * t)); }
	if (y < 1961.0) { t = y - 1950.0; return 29.07 + 0.407 * t - t * t / 233.0 + t * t * t / 2547.0; }
	if (y < 1986.0) { t = y - 1975.0; return 45.45 + 1.067 * t - t * t / 260.0 - t * t * t / 718.0; }
	if (y < 2005.0) {
		t = y - 2000.0;
		return 63.86 + t * (0.3345 + t * (-0.060374 + t * (0.0017275 + t * (0.000651814 + t * 0.00002373599))));
	}
	if (y < 2050.0) { t = y - 2000.0; return 62.92 + 0.32217 * t + 0.005589 * t * t; }
	if (y < 2150.0) return u2(y) - 0.5628 * (2150.0 - y);
	return u2(y);
}

// Terrestrial Time of a UT Julian Date.
static double ephTT(double jdUT) {
	return jdUT + ephDeltaT(2000.0 + (jdUT - 2451545.0) / 365.25) / 86400.0;
}

// Greenwich mean sidereal time (deg, 0..360) of a UT Julian Date (IAU 1982; Meeus 12.4).
static double ephGMST(double jdUT) {
	const double d = jdUT - 2451545.0, T = d / 36525.0;
	double g = 280.46061837 + 360.98564736629 * d + 0.000387933 * T * T - T * T * T / 38710000.0;
	g = std::fmod(g, 360.0);
	return g < 0.0 ? g + 360.0 : g;
}

// J2000 ecliptic -> J2000 equatorial (a turn of the obliquity about x).
static void ephEclToEqu(const double e[3], double q[3]) {
	const double c = std::cos(kEphObliq * kEphRad), s = std::sin(kEphObliq * kEphRad);
	const double y = c * e[1] - s * e[2], z = s * e[1] + c * e[2];
	q[0] = e[0];
	q[1] = y;
	q[2] = z;
}
static void ephEquToEcl(const double q[3], double e[3]) {
	const double c = std::cos(kEphObliq * kEphRad), s = std::sin(kEphObliq * kEphRad);
	const double y = c * q[1] + s * q[2], z = -s * q[1] + c * q[2];
	e[0] = q[0];
	e[1] = y;
	e[2] = z;
}

// Precession J2000 -> mean equator and equinox of date (IAU 1976, Lieske; Meeus 21.2/21.4): v_date = P v.
static void ephPrecession(double jdTT, double P[3][3]) {
	const double T = (jdTT - 2451545.0) / 36525.0;
	const double as = kEphRad / 3600.0;
	const double zeta = (2306.2181 * T + 0.30188 * T * T + 0.017998 * T * T * T) * as;
	const double z = (2306.2181 * T + 1.09468 * T * T + 0.018203 * T * T * T) * as;
	const double th = (2004.3109 * T - 0.42665 * T * T - 0.041833 * T * T * T) * as;
	const double cz = std::cos(zeta), sz = std::sin(zeta), cZ = std::cos(z), sZ = std::sin(z);
	const double ct = std::cos(th), st = std::sin(th);
	P[0][0] = cz * ct * cZ - sz * sZ;  P[0][1] = -sz * ct * cZ - cz * sZ;  P[0][2] = -st * cZ;
	P[1][0] = cz * ct * sZ + sz * cZ;  P[1][1] = -sz * ct * sZ + cz * cZ;  P[1][2] = -st * sZ;
	P[2][0] = cz * st;                 P[2][1] = -sz * st;                 P[2][2] = ct;
}

// Kepler's equation E - e sin E = M (radians).
static double ephKepler(double M, double e) {
	double E = M + e * std::sin(M);
	for (int i = 0; i < 50; ++i) {
		const double dE = (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
		E -= dE;
		if (std::fabs(dE) < 1e-14) break;
	}
	return E;
}

// Heliocentric position (AU, J2000 ECLIPTIC) from the six elements of the instant: a (AU), e, and the
// angles I, w, O (radians) and M (deg). THE ellipse-to-XYZ of every body (the planets, Apophis).
static void ephElemsHelio(double a, double e, double I, double M, double w, double O, double xyz[3]) {
	const double E = ephKepler(std::remainder(M, 360.0) * kEphRad, e);
	const double xp = a * (std::cos(E) - e), yp = a * std::sqrt(1.0 - e * e) * std::sin(E);
	const double cw = std::cos(w), sw = std::sin(w), cO = std::cos(O), sO = std::sin(O);
	const double cI = std::cos(I), sI = std::sin(I);
	xyz[0] = (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp;
	xyz[1] = (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp;
	xyz[2] = (sw * sI) * xp + (cw * sI) * yp;
}

// Heliocentric position (AU, J2000 ECLIPTIC) of planet `p` at `jdTT`. `Mforce` (deg) replaces the
// date's mean anomaly -- the orbit drawn through the date's elements; NaN = the planet's own.
static void ephPlanetHelioM(int p, double jdTT, double Mforce, double xyz[3]) {
	const double T = (jdTT - 2451545.0) / 36525.0;
	const EphElems &e0 = kEphEl[p], &r = kEphRate[p];
	const EphExtra &x = kEphExtra[p];
	const double a = e0.a + r.a * T, e = e0.e + r.e * T;
	const double I = (e0.I + r.I * T) * kEphRad, w = (e0.w + r.w * T) * kEphRad, O = (e0.O + r.O * T) * kEphRad;
	double M = e0.M + r.M * T + x.b * T * T + x.c * std::cos(x.f * T * kEphRad) + x.s * std::sin(x.f * T * kEphRad);
	if (!std::isnan(Mforce)) M = Mforce;
	ephElemsHelio(a, e, I, M, w, O, xyz);
}
static void ephPlanetHelio(int p, double jdTT, double xyz[3]) {
	ephPlanetHelioM(p, jdTT, std::nan(""), xyz);
}

// The Moon, geocentric (Meeus ch. 47, truncated): ecliptic longitude/latitude of date (deg) and distance
// (km); `equ` (km, J2000 EQUATORIAL) when not null.
static void ephMoon(double jdTT, double &lambda, double &beta, double &distKm, double *equ = nullptr) {
	const double T = (jdTT - 2451545.0) / 36525.0;
	auto nd = [](double d) { d = std::fmod(d, 360.0); return d < 0 ? d + 360.0 : d; };
	const double Lp = nd(218.3164477 + 481267.88123421 * T - 0.0015786 * T * T + T * T * T / 538841.0 -
	                     T * T * T * T / 65194000.0);
	const double D = nd(297.8501921 + 445267.1114034 * T - 0.0018819 * T * T + T * T * T / 545868.0 -
	                    T * T * T * T / 113065000.0);
	const double M = nd(357.5291092 + 35999.0502909 * T - 0.0001536 * T * T + T * T * T / 24490000.0);
	const double Mp = nd(134.9633964 + 477198.8675055 * T + 0.0087414 * T * T + T * T * T / 69699.0 -
	                     T * T * T * T / 14712000.0);
	const double F = nd(93.2720950 + 483202.0175233 * T - 0.0036539 * T * T - T * T * T / 3526000.0 +
	                    T * T * T * T / 863310000.0);
	const double A1 = nd(119.75 + 131.849 * T), A2 = nd(53.09 + 479264.290 * T), A3 = nd(313.45 + 481266.484 * T);
	const double Ec = 1.0 - 0.002516 * T - 0.0000074 * T * T;
	// Table 47.A: D, M, M', F, longitude (1e-6 deg), distance (1e-3 km).
	static const int lr[32][6] = {
		{0, 0, 1, 0, 6288774, -20905355}, {2, 0, -1, 0, 1274027, -3699111}, {2, 0, 0, 0, 658314, -2955968},
		{0, 0, 2, 0, 213618, -569925},    {0, 1, 0, 0, -185116, 48888},     {0, 0, 0, 2, -114332, -3149},
		{2, 0, -2, 0, 58793, 246158},     {2, -1, -1, 0, 57066, -152138},   {2, 0, 1, 0, 53322, -170733},
		{2, -1, 0, 0, 45758, -204586},    {0, 1, -1, 0, -40923, -129620},   {1, 0, 0, 0, -34720, 108743},
		{0, 1, 1, 0, -30383, 104755},     {2, 0, 0, -2, 15327, 10321},      {0, 0, 1, 2, -12528, 0},
		{0, 0, 1, -2, 10980, 79661},      {4, 0, -1, 0, 10675, -34782},     {0, 0, 3, 0, 10034, -23210},
		{4, 0, -2, 0, 8548, -21636},      {2, 1, -1, 0, -7888, 24208},      {2, 1, 0, 0, -6766, 30824},
		{1, 0, -1, 0, -5163, -8379},      {1, 1, 0, 0, 4987, -16675},       {2, -1, 1, 0, 4036, -12831},
		{2, 0, 2, 0, 3994, -10445},       {4, 0, 0, 0, 3861, -11650},       {2, 0, -3, 0, 3665, 14403},
		{0, 1, -2, 0, -2689, -7003},      {2, 0, -1, 2, -2602, 0},          {2, -1, -2, 0, 2390, 10056},
		{1, 0, 1, 0, -2348, 6322},        {2, -2, 0, 0, 2236, -9884}};
	// Table 47.B: D, M, M', F, latitude (1e-6 deg).
	static const int bt[20][5] = {
		{0, 0, 0, 1, 5128122}, {0, 0, 1, 1, 280602}, {0, 0, 1, -1, 277693}, {2, 0, 0, -1, 173237},
		{2, 0, -1, 1, 55413},  {2, 0, -1, -1, 46271}, {2, 0, 0, 1, 32573},  {0, 0, 2, 1, 17198},
		{2, 0, 1, -1, 9266},   {0, 0, 2, -1, 8822},   {2, -1, 0, -1, 8216}, {2, 0, -2, -1, 4324},
		{2, 0, 1, 1, 4200},    {2, 1, 0, -1, -3359},  {2, -1, -1, 1, 2463}, {2, -1, 0, 1, 2211},
		{2, -1, -1, -1, 2065}, {0, 1, -1, -1, -1870}, {4, 0, -1, -1, 1828}, {0, 1, 0, 1, -1794}};
	double sl = 0.0, sr = 0.0, sb = 0.0;
	for (const auto &t : lr) {
		const double arg = (t[0] * D + t[1] * M + t[2] * Mp + t[3] * F) * kEphRad;
		const double ef = std::abs(t[1]) == 1 ? Ec : (std::abs(t[1]) == 2 ? Ec * Ec : 1.0);
		sl += t[4] * ef * std::sin(arg);
		sr += t[5] * ef * std::cos(arg);
	}
	for (const auto &t : bt) {
		const double arg = (t[0] * D + t[1] * M + t[2] * Mp + t[3] * F) * kEphRad;
		const double ef = std::abs(t[1]) == 1 ? Ec : (std::abs(t[1]) == 2 ? Ec * Ec : 1.0);
		sb += t[4] * ef * std::sin(arg);
	}
	sl += 3958.0 * std::sin(A1 * kEphRad) + 1962.0 * std::sin((Lp - F) * kEphRad) + 318.0 * std::sin(A2 * kEphRad);
	sb += -2235.0 * std::sin(Lp * kEphRad) + 382.0 * std::sin(A3 * kEphRad) + 175.0 * std::sin((A1 - F) * kEphRad) +
	      175.0 * std::sin((A1 + F) * kEphRad) + 127.0 * std::sin((Lp - Mp) * kEphRad) -
	      115.0 * std::sin((Lp + Mp) * kEphRad);
	lambda = nd(Lp + sl / 1e6);
	beta = sb / 1e6;
	distKm = 385000.56 + sr / 1000.0;
	if (equ) {
		// Of-date ecliptic -> J2000 ecliptic: take the general precession in longitude back out (the
		// ecliptic itself moves < 1" a year; ignored), then to the J2000 equator.
		const double l = (lambda - 1.3969713 * T) * kEphRad, b = beta * kEphRad;
		const double e[3] = {distKm * std::cos(b) * std::cos(l), distKm * std::cos(b) * std::sin(l),
		                     distKm * std::sin(b)};
		ephEclToEqu(e, equ);
	}
}

static const double kEphMoonRkm = 1737.4;
static const double kEphSunRkm = 695700.0;          // IAU 2015 nominal solar radius
static const double kEphEarthMoonMass = 81.30056;   // M(Earth) / M(Moon)
static const double kEphLightDay = 0.0057755183;    // light time for 1 AU, days

// The Earth's own heliocentric position (AU, J2000 ecliptic at `jdTT`): JPL's elements give the
// Earth-Moon BARYCENTRE, which the Moon pulls up to 4700 km off the Earth's centre.
static void ephEarthHelio(double jdTT, double xyz[3]) {
	double emb[3], mq[3], me[3], l, b, d;
	ephPlanetHelio(EPH_EMB, jdTT, emb);
	ephMoon(jdTT, l, b, d, mq);
	ephEquToEcl(mq, me);
	for (int i = 0; i < 3; ++i) xyz[i] = emb[i] - me[i] / kEphAUkm / (1.0 + kEphEarthMoonMass);
}

// Geocentric APPARENT position (AU, J2000 EQUATORIAL) of a sky body at `jdTT`: 0 = Sun, 1 = Moon, 2.. =
// the planets Mercury, Venus, Mars, Jupiter, Saturn, Uranus, Neptune. Light time and annual aberration
// are in (below); nutation is not (it moves every body alike by < 20").
enum EphSkyBody { SKY_SUN, SKY_MOON, SKY_MERCURY, SKY_VENUS, SKY_MARS, SKY_JUPITER, SKY_SATURN, SKY_URANUS,
                  SKY_NEPTUNE, SKY_NBODIES };
static const char *kSkyBodyName[SKY_NBODIES] = {"Sun", "Moon", "Mercury", "Venus", "Mars", "Jupiter", "Saturn",
                                                "Uranus", "Neptune"};
static int ephSkyPlanet(int b) {
	static const int map[SKY_NBODIES] = {-1, -1, EPH_MERCURY, EPH_VENUS, EPH_MARS, EPH_JUPITER, EPH_SATURN,
	                                     EPH_URANUS, EPH_NEPTUNE};
	return map[b];
}
static void ephGeocentric(int b, double jdTT, double q[3]) {
	if (b == SKY_MOON) {
		double l, be, d;
		ephMoon(jdTT, l, be, d, q);
		for (int i = 0; i < 3; ++i) q[i] /= kEphAUkm;
		return;
	}
	// APPARENT place: the body where it was when its light left (light time tau), seen from where the
	// Earth was tau earlier too -- which folds in the annual aberration to first order (20" on the Sun:
	// 40 s on an eclipse's timing). Two passes: tau from the geometric distance, then the real one.
	auto rel = [&](double t, double out[3]) {
		double E[3], P[3] = {0, 0, 0};
		ephEarthHelio(t, E);
		if (b != SKY_SUN) ephPlanetHelio(ephSkyPlanet(b), t, P);
		for (int i = 0; i < 3; ++i) out[i] = P[i] - E[i];
	};
	double e[3];
	rel(jdTT, e);
	rel(jdTT - std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) * kEphLightDay, e);
	ephEclToEqu(e, q);
}

// ---- an observer on the Earth ------------------------------------------------------------------
// One place for "where is it from HERE": the Sky viewer and the Earth-Moon system's "Moon from a place"
// both go through these, never through a copy of their own.

// Local sidereal time (deg, 0..360) at east longitude `lonDeg`, for a UT Julian Date.
static double ephLST(double jdUT, double lonDeg) {
	double l = std::fmod(ephGMST(jdUT) + lonDeg, 360.0);
	return l < 0 ? l + 360.0 : l;
}

// The horizon of a place: M takes a J2000 equatorial vector to (east, north, up) -- precession to the
// equator of date, then the local sidereal time and the latitude.
static void ephHorizonMatrix(double jdUT, double jdTT, double latDeg, double lonDeg, double M[3][3]) {
	double P[3][3];
	ephPrecession(jdTT, P);
	const double L = ephLST(jdUT, lonDeg) * kEphRad, f = latDeg * kEphRad;
	const double H[3][3] = {
		{-std::sin(L), std::cos(L), 0.0},
		{-std::sin(f) * std::cos(L), -std::sin(f) * std::sin(L), std::cos(f)},
		{std::cos(f) * std::cos(L), std::cos(f) * std::sin(L), std::sin(f)}};
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j) M[i][j] = H[i][0] * P[0][j] + H[i][1] * P[1][j] + H[i][2] * P[2][j];
}

// A geocentric J2000 equatorial vector (AU) made topocentric: the place's own position on the (spherical)
// Earth taken out. Matters for the Moon only (up to ~1 deg); the rest move by seconds of arc.
static void ephTopocentric(double jdUT, double jdTT, double latDeg, double lonDeg, double q[3]) {
	double P[3][3];
	ephPrecession(jdTT, P);
	const double L = ephLST(jdUT, lonDeg) * kEphRad, f = latDeg * kEphRad;
	const double od[3] = {std::cos(f) * std::cos(L), std::cos(f) * std::sin(L), std::sin(f)};
	for (int i = 0; i < 3; ++i)
		q[i] -= kEphEarthRkm / kEphAUkm * (P[0][i] * od[0] + P[1][i] * od[1] + P[2][i] * od[2]);
}

// Altitude and azimuth (deg; azimuth from north through east) of a horizon-frame (east, north, up) vector.
static void ephAltAz(const double w[3], double &alt, double &az) {
	const double n = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	alt = std::asin(std::clamp(w[2] / n, -1.0, 1.0)) / kEphRad;
	az = std::atan2(w[0], w[1]) / kEphRad;
	if (az < 0) az += 360.0;
}

// The cities a place can be picked from (data/wcity_major.dat: lon lat Name), sorted by name; read once.
struct EphCity { QString name; double lat, lon; };
static const std::vector<EphCity> &ephCities() {
	static std::vector<EphCity> c;
	static bool loaded = false;
	if (loaded) return c;
	loaded = true;
	std::vector<std::tuple<QString, double, double>> t;
	QFile f(QDir(gmtvtkDataDir()).filePath("wcity_major.dat"));
	if (f.open(QIODevice::ReadOnly | QIODevice::Text))
		while (!f.atEnd()) {
			const QStringList w = QString::fromUtf8(f.readLine()).simplified().split(' ');
			if (w.size() >= 3)
				t.emplace_back(w.mid(2).join(' ').replace('_', ' '), w[1].toDouble(), w[0].toDouble());
		}
	std::sort(t.begin(), t.end());
	for (const auto &x : t) c.push_back({std::get<0>(x), std::get<1>(x), std::get<2>(x)});
	return c;
}

// Apparent visual magnitude of body `p` at heliocentric distance r, geocentric distance d (AU) and
// phase angle `i` (deg): Astronomical Almanac (Meeus 41) formulae; Saturn without its rings' share.
static double ephPlanetMag(int p, double r, double d, double i) {
	const double k = 5.0 * std::log10(r * d), x = i / 100.0;
	switch (p) {
	case EPH_MERCURY: return -0.42 + k + 3.80 * x - 2.73 * x * x + 2.00 * x * x * x;
	case EPH_VENUS:   return -4.40 + k + 0.09 * x + 2.39 * x * x - 0.65 * x * x * x;
	case EPH_MARS:    return -1.52 + k + 0.016 * i;
	case EPH_JUPITER: return -9.40 + k + 0.005 * i;
	case EPH_SATURN:  return -8.88 + k;
	case EPH_URANUS:  return -7.19 + k;
	case EPH_NEPTUNE: return -6.87 + k;
	// The dwarf planets: SBDB absolute magnitude H, no phase term (their phase angle stays small).
	case EPH_PLUTO:   return -0.55 + k;
	case EPH_CERES:   return 3.34 + k;
	case EPH_ERIS:    return -1.26 + k;
	}
	return 0.0;
}

// ---- the star catalogue -------------------------------------------------------------------------

struct EphStar {
	int hr;                  // BSC5 (Harvard Revised) number
	double ra, dec;          // J2000, radians
	double pmRa, pmDec;      // proper motion, radians per year
	float mag;               // visual magnitude
	char sp;                 // first letter of the spectral type (O B A F G K M ...)
};
struct EphConstellation {
	QString abbr, name;
	std::vector<std::pair<int, int>> segs;   // pairs of HR numbers
};
struct EphCatalog {
	bool loaded = false;
	QString error;
	std::vector<EphStar> stars;
	std::vector<int> byHr;                    // HR -> index into `stars`, -1 = none
	std::map<int, QString> names;             // HR -> proper name
	std::vector<EphConstellation> constellations;
};

static QString ephConstellationName(const QString &abbr) {
	static const std::map<QString, QString> n = {
		{"And", "Andromeda"}, {"Ant", "Antlia"}, {"Aps", "Apus"}, {"Aqr", "Aquarius"}, {"Aql", "Aquila"},
		{"Ara", "Ara"}, {"Ari", "Aries"}, {"Aur", "Auriga"}, {"Boo", "Boötes"}, {"Cae", "Caelum"},
		{"Cam", "Camelopardalis"}, {"Cnc", "Cancer"}, {"CVn", "Canes Venatici"}, {"CMa", "Canis Major"},
		{"CMi", "Canis Minor"}, {"Cap", "Capricornus"}, {"Car", "Carina"}, {"Cas", "Cassiopeia"},
		{"Cen", "Centaurus"}, {"Cep", "Cepheus"}, {"Cet", "Cetus"}, {"Cha", "Chamaeleon"}, {"Cir", "Circinus"},
		{"Col", "Columba"}, {"Com", "Coma Berenices"}, {"CrA", "Corona Australis"}, {"CrB", "Corona Borealis"},
		{"Crv", "Corvus"}, {"Crt", "Crater"}, {"Cru", "Crux"}, {"Cyg", "Cygnus"}, {"Del", "Delphinus"},
		{"Dor", "Dorado"}, {"Dra", "Draco"}, {"Equ", "Equuleus"}, {"Eri", "Eridanus"}, {"For", "Fornax"},
		{"Gem", "Gemini"}, {"Gru", "Grus"}, {"Her", "Hercules"}, {"Hor", "Horologium"}, {"Hya", "Hydra"},
		{"Hyi", "Hydrus"}, {"Ind", "Indus"}, {"Lac", "Lacerta"}, {"Leo", "Leo"}, {"LMi", "Leo Minor"},
		{"Lep", "Lepus"}, {"Lib", "Libra"}, {"Lup", "Lupus"}, {"Lyn", "Lynx"}, {"Lyr", "Lyra"}, {"Men", "Mensa"},
		{"Mic", "Microscopium"}, {"Mon", "Monoceros"}, {"Mus", "Musca"}, {"Nor", "Norma"}, {"Oct", "Octans"},
		{"Oph", "Ophiuchus"}, {"Ori", "Orion"}, {"Pav", "Pavo"}, {"Peg", "Pegasus"}, {"Per", "Perseus"},
		{"Phe", "Phoenix"}, {"Pic", "Pictor"}, {"Psc", "Pisces"}, {"PsA", "Piscis Austrinus"}, {"Pup", "Puppis"},
		{"Pyx", "Pyxis"}, {"Ret", "Reticulum"}, {"Sge", "Sagitta"}, {"Sgr", "Sagittarius"}, {"Sco", "Scorpius"},
		{"Scl", "Sculptor"}, {"Sct", "Scutum"}, {"Ser", "Serpens"}, {"Sex", "Sextans"}, {"Tau", "Taurus"},
		{"Tel", "Telescopium"}, {"Tri", "Triangulum"}, {"TrA", "Triangulum Australe"}, {"Tuc", "Tucana"},
		{"UMa", "Ursa Major"}, {"UMi", "Ursa Minor"}, {"Vel", "Vela"}, {"Vir", "Virgo"}, {"Vol", "Volans"},
		{"Vul", "Vulpecula"}};
	auto it = n.find(abbr);
	return it == n.end() ? abbr : it->second;
}

// The catalogue, read once per session from the package's data/ (gmtvtkDataDir).
static const EphCatalog &ephCatalog() {
	static EphCatalog c;
	if (c.loaded) return c;
	c.loaded = true;
	const QDir dd(gmtvtkDataDir());
	// BSC5 binary: a 28-byte header (STARN < 0 = J2000), then 32-byte little-endian records: XNO float,
	// SRA0 double, SDEC0 double, IS char[2], MAG int16 (x100), XRPM float, XDPM float.
	QFile f(dd.filePath("bsc5"));
	if (!f.open(QIODevice::ReadOnly)) {
		c.error = "cannot open " + f.fileName();
		return c;
	}
	const QByteArray buf = f.readAll();
	auto rd = [&](qsizetype off, void *dst, size_t n) { std::memcpy(dst, buf.constData() + off, n); };
	if (buf.size() < 28) {
		c.error = f.fileName() + " is too short";
		return c;
	}
	qint32 starn = 0;
	rd(8, &starn, 4);
	const int n = std::abs(starn);
	if (buf.size() < 28 + qsizetype(n) * 32) {
		c.error = f.fileName() + " is truncated";
		return c;
	}
	c.byHr.assign(size_t(n) + 2, -1);
	for (int i = 0; i < n; ++i) {
		const qsizetype o = 28 + qsizetype(i) * 32;
		float xno, pmr, pmd;
		double ra, dec;
		qint16 mag;
		rd(o, &xno, 4);
		rd(o + 4, &ra, 8);
		rd(o + 12, &dec, 8);
		rd(o + 22, &mag, 2);
		rd(o + 24, &pmr, 4);
		rd(o + 28, &pmd, 4);
		if (ra == 0.0 && dec == 0.0) continue;   // the catalogue's withdrawn entries (novae, clusters)
		EphStar s;
		s.hr = int(std::lround(xno));
		s.ra = ra;
		s.dec = dec;
		s.pmRa = pmr;
		s.pmDec = pmd;
		s.mag = mag / 100.0f;
		s.sp = buf.at(o + 20);
		if (s.hr > 0 && s.hr < int(c.byHr.size())) c.byHr[size_t(s.hr)] = int(c.stars.size());
		c.stars.push_back(s);
	}
	// Names: "HR,Name" per line.
	QFile fn(dd.filePath("bsc5_names.txt"));
	if (fn.open(QIODevice::ReadOnly | QIODevice::Text))
		while (!fn.atEnd()) {
			const QString l = QString::fromUtf8(fn.readLine()).trimmed();
			const int k = l.indexOf(',');
			if (k > 0) c.names[l.left(k).toInt()] = l.mid(k + 1);
		}
	// Figures: "Abr nseg hr hr hr hr ...".
	QFile fc(dd.filePath("bsc5_constellations.txt"));
	if (fc.open(QIODevice::ReadOnly | QIODevice::Text))
		while (!fc.atEnd()) {
			const QStringList w = QString::fromUtf8(fc.readLine()).simplified().split(' ', Qt::SkipEmptyParts);
			if (w.size() < 4) continue;
			EphConstellation k;
			k.abbr = w[0];
			k.name = ephConstellationName(w[0]);
			for (int i = 2; i + 1 < w.size(); i += 2) k.segs.push_back({w[i].toInt(), w[i + 1].toInt()});
			c.constellations.push_back(k);
		}
	return c;
}

// A star's J2000 unit vector at `years` from J2000 (linear proper motion).
static void ephStarVec(const EphStar &s, double years, double v[3]) {
	const double ra = s.ra + s.pmRa * years, de = s.dec + s.pmDec * years;
	v[0] = std::cos(de) * std::cos(ra);
	v[1] = std::cos(de) * std::sin(ra);
	v[2] = std::sin(de);
}

// RA (h) / Dec (deg) as text.
static QString ephRaText(double raDeg) {
	double h = std::fmod(raDeg / 15.0, 24.0);
	if (h < 0) h += 24.0;
	const int hh = int(h), mm = int((h - hh) * 60.0);
	const double ss = ((h - hh) * 60.0 - mm) * 60.0;
	return QString("%1h %2m %3s").arg(hh, 2, 10, QChar('0')).arg(mm, 2, 10, QChar('0')).arg(ss, 4, 'f', 1, QChar('0'));
}
static QString ephDecText(double dec) {
	const double a = std::fabs(dec);
	const int dd = int(a), mm = int((a - dd) * 60.0);
	const int ss = int(std::lround(((a - dd) * 60.0 - mm) * 60.0));
	return QString("%1%2° %3′ %4″").arg(dec < 0 ? "−" : "+").arg(dd, 2, 10, QChar('0'))
		.arg(mm, 2, 10, QChar('0')).arg(std::min(ss, 59), 2, 10, QChar('0'));
}
