# trace.jl — trace.c: path -> optimal polygon -> vertex adjustment -> smoothing -> curve
# optimization. Index variables keep the C's 0-based values; arrays are read at [i+1].

const INFTY  = 10000000        # it suffices that this is longer than any path; it need not be really infinite
const COS179 = -0.999847695156 # the cosine of 179 degrees

# ---------------------------------------------------------------------------------------------
# auxiliary functions

# return a direction that is 90 degrees counterclockwise from p2-p0, but then restricted to one of
# the major wind directions (n, nw, w, etc)
@inline dorth_infty(p0::DPt, p2::DPt) = IPt(-_isign(p2.y - p0.y), _isign(p2.x - p0.x))

# return (p1-p0)x(p2-p0), the area of the parallelogram
@inline dpara(p0::DPt, p1::DPt, p2::DPt) = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y)

# ddenom/dpara have the property that the square of radius 1 centered at p1 intersects the line
# p0p2 iff |dpara(p0,p1,p2)| <= ddenom(p0,p2)
@inline function ddenom(p0::DPt, p2::DPt)
	r = dorth_infty(p0, p2)
	return r.y * (p2.x - p0.x) - r.x * (p2.y - p0.y)
end

# return true if a <= b < c < a, in a cyclic sense (mod n)
@inline cyclic(a::Int, b::Int, c::Int) = a <= c ? (a <= b && b < c) : (a <= b || b < c)

# determine the center and slope of the line i..j. Assume i<j. Needs "sums" of p to be set.
# Returns (ctr, dir).
function pointslope(pp::PotracePath, i::Int, j::Int)
	n = length(pp.pt)
	sums = pp.sums
	r = 0                              # rotations from i to j

	while j >= n
		j -= n
		r += 1
	end
	while i >= n
		i -= n
		r -= 1
	end
	while j < 0
		j += n
		r -= 1
	end
	while i < 0
		i += n
		r += 1
	end

	x  = sums[j+2].x  - sums[i+1].x  + r * sums[n+1].x
	y  = sums[j+2].y  - sums[i+1].y  + r * sums[n+1].y
	x2 = sums[j+2].x2 - sums[i+1].x2 + r * sums[n+1].x2
	xy = sums[j+2].xy - sums[i+1].xy + r * sums[n+1].xy
	y2 = sums[j+2].y2 - sums[i+1].y2 + r * sums[n+1].y2
	k  = Float64(j + 1 - i + r * n)

	ctr = DPt(x / k, y / k)

	a = (x2 - x * x / k) / k
	b = (xy - x * y / k) / k
	c = (y2 - y * y / k) / k

	lambda2 = (a + c + sqrt((a - c) * (a - c) + 4 * b * b)) / 2   # larger e.value

	# now find e.vector for lambda2
	a -= lambda2
	c -= lambda2

	dir = DPt(0.0, 0.0)
	if abs(a) >= abs(c)
		l = sqrt(a * a + b * b)
		l != 0 && (dir = DPt(-b / l, a / l))
	else
		l = sqrt(c * c + b * b)
		l != 0 && (dir = DPt(-c / l, b / l))
	end
	# when l == 0 dir stays (0,0): sometimes this can happen when k=4: the two eigenvalues coincide
	return ctr, dir
end

# Apply quadratic form Q (symmetric 3x3, affine) to vector w = (w.x,w.y,1)
@inline function quadform(Q::Matrix{Float64}, w::DPt)
	v = (w.x, w.y, 1.0)
	s = 0.0
	@inbounds for i = 1:3, j = 1:3
		s += v[i] * Q[i, j] * v[j]
	end
	return s
end

# calculate p1 x p2
@inline xprod(p1::IPt, p2::IPt) = p1.x * p2.y - p1.y * p2.x

# calculate (p1-p0)x(p3-p2)
@inline cprod(p0::DPt, p1::DPt, p2::DPt, p3::DPt) = (p1.x - p0.x) * (p3.y - p2.y) - (p3.x - p2.x) * (p1.y - p0.y)

# calculate (p1-p0)*(p2-p0)
@inline iprod(p0::DPt, p1::DPt, p2::DPt) = (p1.x - p0.x) * (p2.x - p0.x) + (p1.y - p0.y) * (p2.y - p0.y)

# calculate (p1-p0)*(p3-p2)
@inline iprod1(p0::DPt, p1::DPt, p2::DPt, p3::DPt) = (p1.x - p0.x) * (p3.x - p2.x) + (p1.y - p0.y) * (p3.y - p2.y)

# calculate distance between two points
@inline ddist(p::DPt, q::DPt) = sqrt((p.x - q.x)^2 + (p.y - q.y)^2)

# calculate point of a bezier curve
@inline function bezier(t::Float64, p0::DPt, p1::DPt, p2::DPt, p3::DPt)
	s = 1 - t
	return DPt(s*s*s*p0.x + 3*(s*s*t)*p1.x + 3*(t*t*s)*p2.x + t*t*t*p3.x,
	           s*s*s*p0.y + 3*(s*s*t)*p1.y + 3*(t*t*s)*p2.y + t*t*t*p3.y)
end

# calculate the point t in [0..1] on the (convex) bezier curve (p0,p1,p2,p3) which is tangent to
# q1-q0. Return -1.0 if there is no solution in [0..1].
function tangent(p0::DPt, p1::DPt, p2::DPt, p3::DPt, q0::DPt, q1::DPt)
	# (1-t)^2 A + 2(1-t)t B + t^2 C = 0
	A = cprod(p0, p1, q0, q1)
	B = cprod(p1, p2, q0, q1)
	C = cprod(p2, p3, q0, q1)

	# a t^2 + b t + c = 0
	a = A - 2 * B + C
	b = -2 * A + 2 * B
	c = A

	d = b * b - 4 * a * c
	(a == 0 || d < 0) && return -1.0

	s = sqrt(d)
	r1 = (-b + s) / (2 * a)
	r2 = (-b - s) / (2 * a)

	if r1 >= 0 && r1 <= 1
		return r1
	elseif r2 >= 0 && r2 <= 1
		return r2
	else
		return -1.0
	end
end

# ---------------------------------------------------------------------------------------------
# Preparation: fill in the sums of a path (used for later rapid summing).
function calc_sums!(pp::PotracePath)
	n = length(pp.pt)
	pp.x0 = pp.pt[1].x
	pp.y0 = pp.pt[1].y
	sums = Vector{Sums}(undef, n + 1)
	sums[1] = Sums(0.0, 0.0, 0.0, 0.0, 0.0)
	for i = 1:n
		x = Float64(pp.pt[i].x - pp.x0)
		y = Float64(pp.pt[i].y - pp.y0)
		s = sums[i]
		sums[i+1] = Sums(s.x + x, s.y + y, s.x2 + x * x, s.xy + x * y, s.y2 + y * y)
	end
	pp.sums = sums
	return nothing
end

# ---------------------------------------------------------------------------------------------
# Stage 1: determine the straight subpaths (Sec. 2.2.1). Fill in the "lon" component of a path
# object (based on pt/len). For each i, lon[i] is the furthest index such that a straight line can
# be drawn from i to lon[i].
#
# This algorithm depends on the fact that the existence of straight subpaths is a triplewise
# property. I.e., there exists a straight line through squares i0,...,in iff there exists a
# straight line through i,j,k, for all i0<=i<j<k<=in. A "constraint" means that future points must
# satisfy xprod(constraint[0], cur) >= 0 and xprod(constraint[1], cur) <= 0.
function calc_lon!(pp::PotracePath)
	pt = pp.pt
	n = length(pt)
	pivk = zeros(Int, n)               # pivk[n]
	nc = zeros(Int, n)                 # nc[n]: next corner
	ct = zeros(Int, 4)

	# initialize the nc data structure. Point from each point to the furthest future point to which
	# it is connected by a vertical or horizontal segment. We take advantage of the fact that there
	# is always a direction change at 0 (due to the path decomposition algorithm).
	k = 0
	for i = n-1:-1:0
		if pt[i+1].x != pt[k+1].x && pt[i+1].y != pt[k+1].y
			k = i + 1                  # necessarily i<n-1 in this case
		end
		nc[i+1] = k
	end

	lon = zeros(Int, n)

	# determine pivot points: for each i, let pivk[i] be the furthest k such that all j with i<j<k
	# lie on a line connecting i,k.
	for i = n-1:-1:0
		fill!(ct, 0)

		# keep track of "directions" that have occurred
		i1 = mod(i + 1, n)
		dir = div(3 + 3 * (pt[i1+1].x - pt[i+1].x) + (pt[i1+1].y - pt[i+1].y), 2)
		ct[dir+1] += 1

		c0 = IPt(0, 0)
		c1 = IPt(0, 0)

		# find the next k such that no straight line from i to k
		k = nc[i+1]
		k1 = i
		foundk = false
		while true
			dir = div(3 + 3 * _isign(pt[k+1].x - pt[k1+1].x) + _isign(pt[k+1].y - pt[k1+1].y), 2)
			ct[dir+1] += 1

			# if all four "directions" have occurred, cut this path
			if ct[1] != 0 && ct[2] != 0 && ct[3] != 0 && ct[4] != 0
				pivk[i+1] = k1
				foundk = true
				break
			end

			cur = IPt(pt[k+1].x - pt[i+1].x, pt[k+1].y - pt[i+1].y)

			# see if current constraint is violated
			(xprod(c0, cur) < 0 || xprod(c1, cur) > 0) && break      # -> constraint_viol

			# else, update constraint
			if !(abs(cur.x) <= 1 && abs(cur.y) <= 1)
				off = IPt(cur.x + ((cur.y >= 0 && (cur.y > 0 || cur.x < 0)) ? 1 : -1),
				          cur.y + ((cur.x <= 0 && (cur.x < 0 || cur.y < 0)) ? 1 : -1))
				xprod(c0, off) >= 0 && (c0 = off)
				off = IPt(cur.x + ((cur.y <= 0 && (cur.y < 0 || cur.x < 0)) ? 1 : -1),
				          cur.y + ((cur.x >= 0 && (cur.x > 0 || cur.y < 0)) ? 1 : -1))
				xprod(c1, off) <= 0 && (c1 = off)
			end
			k1 = k
			k = nc[k1+1]
			cyclic(k, i, k1) || break
		end
		foundk && continue

		# constraint_viol: k1 was the last "corner" satisfying the current constraint, and k is the
		# first one violating it. We now need to find the last point along k1..k which satisfied
		# the constraint.
		dk = IPt(_isign(pt[k+1].x - pt[k1+1].x), _isign(pt[k+1].y - pt[k1+1].y))
		cur = IPt(pt[k1+1].x - pt[i+1].x, pt[k1+1].y - pt[i+1].y)
		# find largest integer j such that xprod(constraint[0], cur+j*dk) >= 0 and
		# xprod(constraint[1], cur+j*dk) <= 0. Use bilinearity of xprod.
		a = xprod(c0, cur)
		b = xprod(c0, dk)
		c = xprod(c1, cur)
		d = xprod(c1, dk)
		# find largest integer j such that a+j*b>=0 and c+j*d<=0. This can be solved with integer
		# arithmetic.
		j = INFTY
		b < 0 && (j = fld(a, -b))
		d > 0 && (j = min(j, fld(-c, d)))
		pivk[i+1] = mod(k1 + j, n)
	end

	# clean up: for each i, let lon[i] be the largest k such that for all i' with i<=i'<k,
	# i'<k<=pivk[i'].
	j = pivk[n]
	lon[n] = j
	for i = n-2:-1:0
		cyclic(i + 1, pivk[i+1], j) && (j = pivk[i+1])
		lon[i+1] = j
	end

	i = n - 1
	while cyclic(mod(i + 1, n), j, lon[i+1])
		lon[i+1] = j
		i -= 1
	end

	pp.lon = lon
	return nothing
end

# ---------------------------------------------------------------------------------------------
# Stage 2: calculate the optimal polygon (Sec. 2.2.2-2.2.4).

# Auxiliary function: calculate the penalty of an edge from i to j in the given path. This needs
# the "lon" and "sums" data. Assumes 0<=i<j<=n.
function penalty3(pp::PotracePath, i::Int, j::Int)
	n = length(pp.pt)
	pt = pp.pt
	sums = pp.sums

	r = 0                              # rotations from i to j
	if j >= n
		j -= n
		r = 1
	end

	if r == 0
		x  = sums[j+2].x  - sums[i+1].x
		y  = sums[j+2].y  - sums[i+1].y
		x2 = sums[j+2].x2 - sums[i+1].x2
		xy = sums[j+2].xy - sums[i+1].xy
		y2 = sums[j+2].y2 - sums[i+1].y2
		k  = Float64(j + 1 - i)
	else
		x  = sums[j+2].x  - sums[i+1].x  + sums[n+1].x
		y  = sums[j+2].y  - sums[i+1].y  + sums[n+1].y
		x2 = sums[j+2].x2 - sums[i+1].x2 + sums[n+1].x2
		xy = sums[j+2].xy - sums[i+1].xy + sums[n+1].xy
		y2 = sums[j+2].y2 - sums[i+1].y2 + sums[n+1].y2
		k  = Float64(j + 1 - i + n)
	end

	px = (pt[i+1].x + pt[j+1].x) / 2.0 - pt[1].x
	py = (pt[i+1].y + pt[j+1].y) / 2.0 - pt[1].y
	ey = Float64(pt[j+1].x - pt[i+1].x)
	ex = -Float64(pt[j+1].y - pt[i+1].y)

	a = ((x2 - 2 * x * px) / k + px * px)
	b = ((xy - x * py - y * px) / k + px * py)
	c = ((y2 - 2 * y * py) / k + py * py)

	s = ex * ex * a + 2 * ex * ey * b + ey * ey * c
	return sqrt(s)
end

# find the optimal polygon. Fill in the m and po components. Non-cyclic version: assumes i=0 is in
# the polygon.
function bestpolygon!(pp::PotracePath)
	n = length(pp.pt)
	lon = pp.lon
	pen   = zeros(Float64, n + 1)      # penalty vector
	prev  = zeros(Int, n + 1)          # best path pointer vector
	clip0 = zeros(Int, n)              # longest segment pointer, non-cyclic
	clip1 = zeros(Int, n + 1)          # backwards segment pointer, non-cyclic
	seg0  = zeros(Int, n + 1)          # forward segment bounds, m<=n
	seg1  = zeros(Int, n + 1)          # backward segment bounds, m<=n

	# calculate clipped paths
	for i = 0:n-1
		c = mod(lon[mod(i - 1, n)+1] - 1, n)
		c == i && (c = mod(i + 1, n))
		clip0[i+1] = c < i ? n : c
	end

	# calculate backwards path clipping, non-cyclic. j <= clip0[i] iff clip1[j] <= i, for i,j=0..n.
	j = 1
	for i = 0:n-1
		while j <= clip0[i+1]
			clip1[j+1] = i
			j += 1
		end
	end

	# calculate seg0[j] = longest path from 0 with j segments
	i = 0
	j = 0
	while i < n
		seg0[j+1] = i
		i = clip0[i+1]
		j += 1
	end
	seg0[j+1] = n
	m = j

	# calculate seg1[j] = longest path to n with m-j segments
	i = n
	for j = m:-1:1
		seg1[j+1] = i
		i = clip1[i+1]
	end
	seg1[1] = 0

	# now find the shortest path with m segments, based on penalty3. The outer 2 loops jointly have
	# at most n iterations, thus the worst-case behavior here is quadratic. In practice, it is close
	# to linear since the inner loop tends to be short.
	pen[1] = 0
	for j = 1:m
		for i = seg1[j+1]:seg0[j+1]
			best = -1.0
			for k = seg0[j]:-1:clip1[i+1]
				thispen = penalty3(pp, k, i) + pen[k+1]
				if best < 0 || thispen < best
					prev[i+1] = k
					best = thispen
				end
			end
			pen[i+1] = best
		end
	end

	pp.m = m
	po = zeros(Int, m)

	# read off shortest path
	i = n
	j = m - 1
	while i > 0
		i = prev[i+1]
		po[j+1] = i
		j -= 1
	end
	pp.po = po
	return nothing
end

# ---------------------------------------------------------------------------------------------
# Stage 3: vertex adjustment (Sec. 2.3.1).

# Adjust vertices of optimal polygon: calculate the intersection of the two "optimal" line
# segments, then move it into the unit square if it lies outside.
function adjust_vertices!(pp::PotracePath)
	m = pp.m
	po = pp.po
	n = length(pp.pt)
	pt = pp.pt
	x0 = pp.x0
	y0 = pp.y0

	ctr = Vector{DPt}(undef, m)
	dir = Vector{DPt}(undef, m)
	q = zeros(Float64, 3, 3, m)
	v = zeros(Float64, 3)

	pp.curve = PrivCurve(m)

	# calculate "optimal" point-slope representation for each line segment
	for i = 0:m-1
		j = po[mod(i + 1, m)+1]
		j = mod(j - po[i+1], n) + po[i+1]
		ctr[i+1], dir[i+1] = pointslope(pp, po[i+1], j)
	end

	# represent each line segment as a singular quadratic form; the distance of a point (x,y) from
	# the line segment will be (x,y,1)Q(x,y,1)^t, where Q=q[i].
	for i = 1:m
		d = dir[i].x^2 + dir[i].y^2
		if d != 0.0
			v[1] = dir[i].y
			v[2] = -dir[i].x
			v[3] = -v[2] * ctr[i].y - v[1] * ctr[i].x
			for l = 1:3, k = 1:3
				q[l, k, i] = v[l] * v[k] / d
			end
		end
	end

	# now calculate the "intersections" of consecutive segments. Instead of using the actual
	# intersection, we find the point within a given unit square which minimizes the square
	# distance to the two lines.
	Q = zeros(Float64, 3, 3)
	for i = 0:m-1
		# let s be the vertex, in coordinates relative to x0/y0
		s = DPt(pt[po[i+1]+1].x - x0, pt[po[i+1]+1].y - y0)

		# intersect segments i-1 and i
		j = mod(i - 1, m)

		# add quadratic forms
		for l = 1:3, k = 1:3
			Q[l, k] = q[l, k, j+1] + q[l, k, i+1]
		end

		local w::DPt
		while true
			# minimize the quadratic form Q on the unit square: find intersection
			det = Q[1, 1] * Q[2, 2] - Q[1, 2] * Q[2, 1]
			if det != 0.0
				w = DPt((-Q[1, 3] * Q[2, 2] + Q[2, 3] * Q[1, 2]) / det,
				        ( Q[1, 3] * Q[2, 1] - Q[2, 3] * Q[1, 1]) / det)
				break
			end

			# matrix is singular - lines are parallel. Add another, orthogonal axis, through the
			# center of the unit square
			if Q[1, 1] > Q[2, 2]
				v[1] = -Q[1, 2]
				v[2] = Q[1, 1]
			elseif Q[2, 2] != 0
				v[1] = -Q[2, 2]
				v[2] = Q[2, 1]
			else
				v[1] = 1
				v[2] = 0
			end
			d = v[1]^2 + v[2]^2
			v[3] = -v[2] * s.y - v[1] * s.x
			for l = 1:3, k = 1:3
				Q[l, k] += v[l] * v[k] / d
			end
		end
		dx = abs(w.x - s.x)
		dy = abs(w.y - s.y)
		if dx <= 0.5 && dy <= 0.5
			pp.curve.vertex[i+1] = DPt(w.x + x0, w.y + y0)
			continue
		end

		# the minimum was not in the unit square; now minimize quadratic on boundary of square
		qmin = quadform(Q, s)
		xmin = s.x
		ymin = s.y

		if Q[1, 1] != 0.0
			for z = 0:1                # value of the y-coordinate
				wy = s.y - 0.5 + z
				wx = -(Q[1, 2] * wy + Q[1, 3]) / Q[1, 1]
				dx = abs(wx - s.x)
				cand = quadform(Q, DPt(wx, wy))
				if dx <= 0.5 && cand < qmin
					qmin = cand
					xmin = wx
					ymin = wy
				end
			end
		end
		# fixx:
		if Q[2, 2] != 0.0
			for z = 0:1                # value of the x-coordinate
				wx = s.x - 0.5 + z
				wy = -(Q[2, 1] * wx + Q[2, 3]) / Q[2, 2]
				dy = abs(wy - s.y)
				cand = quadform(Q, DPt(wx, wy))
				if dy <= 0.5 && cand < qmin
					qmin = cand
					xmin = wx
					ymin = wy
				end
			end
		end
		# corners: check four corners
		for l = 0:1, k = 0:1
			wx = s.x - 0.5 + l
			wy = s.y - 0.5 + k
			cand = quadform(Q, DPt(wx, wy))
			if cand < qmin
				qmin = cand
				xmin = wx
				ymin = wy
			end
		end

		pp.curve.vertex[i+1] = DPt(xmin + x0, ymin + y0)
	end
	return nothing
end

# ---------------------------------------------------------------------------------------------
# Stage 4: smoothing and corner analysis (Sec. 2.3.3)

# reverse orientation of a path
reverse_curve!(curve::PrivCurve) = (reverse!(curve.vertex); nothing)

function smooth!(curve::PrivCurve, alphamax::Float64)
	m = curve.n
	vx = curve.vertex

	# examine each vertex and find its best fit
	for i = 0:m-1
		j = mod(i + 1, m)
		k = mod(i + 2, m)
		p4 = interval(0.5, vx[k+1], vx[j+1])

		denom = ddenom(vx[i+1], vx[k+1])
		if denom != 0.0
			dd = abs(dpara(vx[i+1], vx[j+1], vx[k+1]) / denom)
			alpha = dd > 1 ? (1 - 1.0 / dd) : 0.0
			alpha = alpha / 0.75
		else
			alpha = 4 / 3.0
		end
		curve.alpha0[j+1] = alpha      # remember "original" value of alpha

		if alpha >= alphamax           # pointed corner
			curve.tag[j+1] = CORNER
			curve.c[j+1, 2] = vx[j+1]
			curve.c[j+1, 3] = p4
		else
			if alpha < 0.55
				alpha = 0.55
			elseif alpha > 1
				alpha = 1.0
			end
			p2 = interval(0.5 + 0.5 * alpha, vx[i+1], vx[j+1])
			p3 = interval(0.5 + 0.5 * alpha, vx[k+1], vx[j+1])
			curve.tag[j+1] = CURVETO
			curve.c[j+1, 1] = p2
			curve.c[j+1, 2] = p3
			curve.c[j+1, 3] = p4
		end
		curve.alpha[j+1] = alpha       # store the "cropped" value of alpha
		curve.beta[j+1] = 0.5
	end
	curve.alphacurve = true
	return nothing
end

# ---------------------------------------------------------------------------------------------
# Stage 5: Curve optimization (Sec. 2.4)

# the result of opti_penalty
struct Opti
	pen::Float64                       # penalty
	c0::DPt                            # curve parameters
	c1::DPt
	t::Float64                         # curve parameters
	s::Float64
	alpha::Float64                     # curve parameter
end
const _OPTI0 = Opti(0.0, DPt(0.0, 0.0), DPt(0.0, 0.0), 0.0, 0.0, 0.0)

# calculate best fit from i+.5 to j+.5. Assume i<j (cyclically). Returns the fit, or `nothing` if
# impossible.
function opti_penalty(pp::PotracePath, i::Int, j::Int, opttolerance::Float64, convc::Vector{Int},
                      areac::Vector{Float64})
	cv = pp.curve
	m = cv.n
	vx = cv.vertex
	C = cv.c

	# check convexity, corner-freeness, and maximum bend < 179 degrees
	i == j && return nothing           # sanity - a full loop can never be an opticurve

	k = i
	i1 = mod(i + 1, m)
	k1 = mod(k + 1, m)
	conv = convc[k1+1]
	conv == 0 && return nothing
	d = ddist(vx[i+1], vx[i1+1])
	k = k1
	while k != j
		k1 = mod(k + 1, m)
		k2 = mod(k + 2, m)
		convc[k1+1] != conv && return nothing
		_isign(cprod(vx[i+1], vx[i1+1], vx[k1+1], vx[k2+1])) != conv && return nothing
		if iprod1(vx[i+1], vx[i1+1], vx[k1+1], vx[k2+1]) < d * ddist(vx[k1+1], vx[k2+1]) * COS179
			return nothing
		end
		k = k1
	end

	# the curve we're working in:
	p0 = C[mod(i, m)+1, 3]
	p1 = vx[mod(i + 1, m)+1]
	p2 = vx[mod(j, m)+1]
	p3 = C[mod(j, m)+1, 3]

	# determine its area
	area = areac[j+1] - areac[i+1]
	area -= dpara(vx[1], C[i+1, 3], C[j+1, 3]) / 2
	i >= j && (area += areac[m+1])

	# find intersection o of p0p1 and p2p3. Let t,s such that o = interval(t,p0,p1) =
	# interval(s,p3,p2). Let A be the area of the triangle (p0,o,p3).
	A1 = dpara(p0, p1, p2)
	A2 = dpara(p0, p1, p3)
	A3 = dpara(p0, p2, p3)
	A4 = A1 + A3 - A2                  # = dpara(p1, p2, p3)

	A2 == A1 && return nothing         # this should never happen

	t = A3 / (A3 - A4)
	s = A2 / (A2 - A1)
	A = A2 * t / 2.0

	A == 0.0 && return nothing         # this should never happen

	R = area / A                       # relative area
	alpha = 2 - sqrt(4 - R / 0.3)      # overall alpha for p0-o-p3 curve

	rc0 = interval(t * alpha, p0, p1)
	rc1 = interval(s * alpha, p3, p2)
	rt, rs = t, s

	p1 = rc0
	p2 = rc1                           # the proposed curve is now (p0,p1,p2,p3)

	pen = 0.0

	# calculate penalty: check tangency with edges
	k = mod(i + 1, m)
	while k != j
		k1 = mod(k + 1, m)
		t = tangent(p0, p1, p2, p3, vx[k+1], vx[k1+1])
		t < -0.5 && return nothing
		pt = bezier(t, p0, p1, p2, p3)
		d = ddist(vx[k+1], vx[k1+1])
		d == 0.0 && return nothing     # this should never happen
		d1 = dpara(vx[k+1], vx[k1+1], pt) / d
		abs(d1) > opttolerance && return nothing
		(iprod(vx[k+1], vx[k1+1], pt) < 0 || iprod(vx[k1+1], vx[k+1], pt) < 0) && return nothing
		pen += d1^2
		k = k1
	end

	# check corners
	k = i
	while k != j
		k1 = mod(k + 1, m)
		t = tangent(p0, p1, p2, p3, C[k+1, 3], C[k1+1, 3])
		t < -0.5 && return nothing
		pt = bezier(t, p0, p1, p2, p3)
		d = ddist(C[k+1, 3], C[k1+1, 3])
		d == 0.0 && return nothing     # this should never happen
		d1 = dpara(C[k+1, 3], C[k1+1, 3], pt) / d
		d2 = dpara(C[k+1, 3], C[k1+1, 3], vx[k1+1]) / d
		d2 *= 0.75 * cv.alpha[k1+1]
		if d2 < 0
			d1 = -d1
			d2 = -d2
		end
		d1 < d2 - opttolerance && return nothing
		d1 < d2 && (pen += (d1 - d2)^2)
		k = k1
	end

	return Opti(pen, rc0, rc1, rt, rs, alpha)
end

# optimize the path p, replacing sequences of Bezier segments by a single segment when possible.
function opticurve!(pp::PotracePath, opttolerance::Float64)
	cv = pp.curve
	m = cv.n
	vx = cv.vertex
	pt  = zeros(Int, m + 1)
	pen = zeros(Float64, m + 1)
	len = zeros(Int, m + 1)
	opt = fill(_OPTI0, m + 1)
	convc = zeros(Int, m)              # pre-computed convexities
	areac = zeros(Float64, m + 1)      # cache for fast area computation

	# pre-calculate convexity: +1 = right turn, -1 = left turn, 0 = corner
	for i = 0:m-1
		if cv.tag[i+1] == CURVETO
			convc[i+1] = _isign(dpara(vx[mod(i - 1, m)+1], vx[i+1], vx[mod(i + 1, m)+1]))
		end
	end

	# pre-calculate areas
	area = 0.0
	areac[1] = 0.0
	p0 = vx[1]
	for i = 0:m-1
		i1 = mod(i + 1, m)
		if cv.tag[i1+1] == CURVETO
			alpha = cv.alpha[i1+1]
			area += 0.3 * alpha * (4 - alpha) * dpara(cv.c[i+1, 3], vx[i1+1], cv.c[i1+1, 3]) / 2
			area += dpara(p0, cv.c[i+1, 3], cv.c[i1+1, 3]) / 2
		end
		areac[i+2] = area
	end

	pt[1] = -1
	pen[1] = 0
	len[1] = 0

	# we always start from a fixed point -- should find the best curve cyclically (C: Fixme)
	for j = 1:m
		# calculate best path from 0 to j
		pt[j+1] = j - 1
		pen[j+1] = pen[j]
		len[j+1] = len[j] + 1

		for i = j-2:-1:0
			o = opti_penalty(pp, i, mod(j, m), opttolerance, convc, areac)
			o === nothing && break
			if len[j+1] > len[i+1] + 1 || (len[j+1] == len[i+1] + 1 && pen[j+1] > pen[i+1] + o.pen)
				pt[j+1] = i
				pen[j+1] = pen[i+1] + o.pen
				len[j+1] = len[i+1] + 1
				opt[j+1] = o
			end
		end
	end
	om = len[m+1]
	oc = PrivCurve(om)
	s = zeros(Float64, om)
	t = zeros(Float64, om)

	j = m
	for i = om-1:-1:0
		jm = mod(j, m) + 1
		if pt[j+1] == j - 1
			oc.tag[i+1]    = cv.tag[jm]
			oc.c[i+1, 1]   = cv.c[jm, 1]
			oc.c[i+1, 2]   = cv.c[jm, 2]
			oc.c[i+1, 3]   = cv.c[jm, 3]
			oc.vertex[i+1] = cv.vertex[jm]
			oc.alpha[i+1]  = cv.alpha[jm]
			oc.alpha0[i+1] = cv.alpha0[jm]
			oc.beta[i+1]   = cv.beta[jm]
			s[i+1] = t[i+1] = 1.0
		else
			o = opt[j+1]
			oc.tag[i+1]    = CURVETO
			oc.c[i+1, 1]   = o.c0
			oc.c[i+1, 2]   = o.c1
			oc.c[i+1, 3]   = cv.c[jm, 3]
			oc.vertex[i+1] = interval(o.s, cv.c[jm, 3], cv.vertex[jm])
			oc.alpha[i+1]  = o.alpha
			oc.alpha0[i+1] = o.alpha
			s[i+1] = o.s
			t[i+1] = o.t
		end
		j = pt[j+1]
	end

	# calculate beta parameters
	for i = 0:om-1
		i1 = mod(i + 1, om)
		oc.beta[i+1] = s[i+1] / (s[i+1] + t[i1+1])
	end
	oc.alphacurve = true
	pp.ocurve = oc
	return nothing
end

# ---------------------------------------------------------------------------------------------
# process_path + potrace_trace (potracelib.c)

function process_path!(plist::Union{PotracePath,Nothing}, param::PotraceParams)
	p = plist
	while p !== nothing
		calc_sums!(p)
		calc_lon!(p)
		bestpolygon!(p)
		adjust_vertices!(p)
		p.sign == '-' && reverse_curve!(p.curve)   # reverse orientation of negative paths
		smooth!(p.curve, param.alphamax)
		if param.opticurve
			opticurve!(p, param.opttolerance)
			p.fcurve = p.ocurve
		else
			p.fcurve = p.curve
		end
		p = p.next
	end
	return nothing
end

"""
    res = potrace(bm::PotraceBitmap, param::PotraceParams=PotraceParams())
    res = potrace(M; threshold=nothing, invert=false, turdsize=2, turnpolicy=:minority,
                  alphamax=1.0, opticurve=true, opttolerance=0.2)

Trace a bitmap into closed curves (corners + cubic Béziers). `M` is a `Bool`/`BitMatrix` or a
numeric matrix, laid out as displayed (`M[1,1]` top-left); see `PotraceBitmap` for thresholding.
Write the result with `potrace_svg`, `potrace_eps` or `potrace_gmtds`.
"""
function potrace(bm::PotraceBitmap, param::PotraceParams=PotraceParams())
	plist = bm_to_pathlist(bm, param)
	process_path!(plist, param)
	return PotraceResult(bm.w, bm.h, plist)
end

function potrace(M::Union{BitMatrix, Matrix{Bool}}; kw...)
	return potrace(PotraceBitmap(M), PotraceParams(; kw...))
end

function potrace(M::Matrix{<:Real}; threshold::Union{Nothing,Real}=nothing, invert::Bool=false, kw...)
	return potrace(PotraceBitmap(M; threshold=threshold, invert=invert), PotraceParams(; kw...))
end
