# decompose.jl — decompose.c: bitmap -> closed pixel-edge paths, with sign and tree structure.

# ---------------------------------------------------------------------------------------------
# deterministically and efficiently hash (x,y) into a pseudo-random bit

# non-linear sequence: constant term of inverse in GF(8), mod x^8+x^4+x^3+x+1
const _DETRAND_T = UInt32[
	0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0,
	0, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1,
	1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1,
	0, 0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0,
	0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0,
	0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0,
	0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1,
	1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0,
	0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1,
	1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0,
]

function detrand(x::Int, y::Int)
	# 0x04b3e375 and 0x05a8ef93 are chosen to contain every possible 5-bit sequence
	z = ((0x04b3e375 * (x % UInt32)) ⊻ (y % UInt32)) * 0x05a8ef93
	z = _DETRAND_T[(z & 0xff) + 1] ⊻ _DETRAND_T[((z >> 8) & 0xff) + 1] ⊻
	    _DETRAND_T[((z >> 16) & 0xff) + 1] ⊻ _DETRAND_T[((z >> 24) & 0xff) + 1]
	return z != 0
end

# ---------------------------------------------------------------------------------------------
# auxiliary bitmap manipulations

# set the excess padding to 0
function bm_clearexcess!(bm::PotraceBitmap)
	r = bm.w % BM_WORDBITS
	if r != 0
		mask = BM_ALLBITS << (BM_WORDBITS - r)
		for y = 0:bm.h-1
			bm.map[_bm_word(bm, bm.w, y)] &= mask
		end
	end
	return nothing
end

# clear the bm, assuming the bounding box is set correctly (faster than clearing the whole bitmap)
function clear_bm_with_bbox!(bm::PotraceBitmap, x0::Int, x1::Int, y0::Int, y1::Int)
	imin = div(x0, BM_WORDBITS)
	imax = div(x1 + BM_WORDBITS - 1, BM_WORDBITS)
	for y = y0:y1-1, i = imin:imax-1
		bm.map[y * bm.dy + i + 1] = 0
	end
	return nothing
end

# ---------------------------------------------------------------------------------------------
# auxiliary functions

# return the "majority" value of bitmap bm at intersection (x,y). We assume that the bitmap is
# balanced at "radius" 1.
function majority(bm::PotraceBitmap, x::Int, y::Int)
	for i = 2:4                        # check at "radius" i
		ct = 0
		for a = -i+1:i-1
			ct += BM_GET(bm, x + a, y + i - 1) ? 1 : -1
			ct += BM_GET(bm, x + i - 1, y + a - 1) ? 1 : -1
			ct += BM_GET(bm, x + a - 1, y - i) ? 1 : -1
			ct += BM_GET(bm, x - i, y + a) ? 1 : -1
		end
		if ct > 0
			return true
		elseif ct < 0
			return false
		end
	end
	return false
end

# ---------------------------------------------------------------------------------------------
# decompose image into paths

# efficiently invert bits [x,infty) and [xa,infty) in line y. Here xa must be a multiple of
# BM_WORDBITS.
function xor_to_ref!(bm::PotraceBitmap, x::Int, y::Int, xa::Int)
	xhi = x & -BM_WORDBITS
	xlo = x & (BM_WORDBITS - 1)        # = x % BM_WORDBITS
	if xhi < xa
		for i = xhi:BM_WORDBITS:xa-1
			bm.map[_bm_word(bm, i, y)] ⊻= BM_ALLBITS
		end
	else
		for i = xa:BM_WORDBITS:xhi-1
			bm.map[_bm_word(bm, i, y)] ⊻= BM_ALLBITS
		end
	end
	if xlo != 0
		bm.map[_bm_word(bm, xhi, y)] ⊻= (BM_ALLBITS << (BM_WORDBITS - xlo))
	end
	return nothing
end

# a path is represented as an array of points, which are thought to lie on the corners of pixels
# (not on their centers). The path point (x,y) is the lower left corner of the pixel (x,y).

# xor the given pixmap with the interior of the given path. Note: the path must be within the
# dimensions of the pixmap.
function xor_path!(bm::PotraceBitmap, p::PotracePath)
	pt = p.pt
	isempty(pt) && return nothing      # a path of length 0 is silly, but legal
	y1 = pt[end].y
	xa = pt[1].x & -BM_WORDBITS
	for k = 1:length(pt)
		x = pt[k].x
		y = pt[k].y
		if y != y1
			# efficiently invert the rectangle [x,xa] x [y,y1]
			xor_to_ref!(bm, x, min(y, y1), xa)
			y1 = y
		end
	end
	return nothing
end

# Find the bounding box of a given path -> (x0, x1, y0, y1). Path is assumed to be of non-zero length.
function setbbox_path(p::PotracePath)
	x0 = typemax(Int32); x1 = 0
	y0 = typemax(Int32); y1 = 0
	for q in p.pt
		q.x < x0 && (x0 = q.x)
		q.x > x1 && (x1 = q.x)
		q.y < y0 && (y0 = q.y)
		q.y > y1 && (y1 = q.y)
	end
	return x0, x1, y0, y1
end

# compute a path in the given pixmap, separating black from white. Start path at the point
# (x0,x1), which must be an upper left corner of the path. Also compute the area enclosed by the
# path. Sign is required for correct interpretation of turnpolicies.
function findpath(bm::PotraceBitmap, x0::Int, y0::Int, sign::Char, turnpolicy::Int)
	x = x0
	y = y0
	dirx = 0
	diry = -1
	pt = IPt[]
	area = 0

	while true
		# add point to path
		push!(pt, IPt(x, y))

		# move to next point
		x += dirx
		y += diry
		area += x * diry

		# path complete?
		(x == x0 && y == y0) && break

		# determine next direction
		c = BM_GET(bm, x + div(dirx + diry - 1, 2), y + div(diry - dirx - 1, 2))
		d = BM_GET(bm, x + div(dirx - diry - 1, 2), y + div(diry + dirx - 1, 2))

		if c && !d                     # ambiguous turn
			if turnpolicy == TURNPOLICY_RIGHT ||
			   (turnpolicy == TURNPOLICY_BLACK && sign == '+') ||
			   (turnpolicy == TURNPOLICY_WHITE && sign == '-') ||
			   (turnpolicy == TURNPOLICY_RANDOM && detrand(x, y)) ||
			   (turnpolicy == TURNPOLICY_MAJORITY && majority(bm, x, y)) ||
			   (turnpolicy == TURNPOLICY_MINORITY && !majority(bm, x, y))
				tmp = dirx             # right turn
				dirx = diry
				diry = -tmp
			else
				tmp = dirx             # left turn
				dirx = -diry
				diry = tmp
			end
		elseif c                       # right turn
			tmp = dirx
			dirx = diry
			diry = -tmp
		elseif !d                      # left turn
			tmp = dirx
			dirx = -diry
			diry = tmp
		end
	end

	# the C accumulates the area in a uint64_t and clamps it to INT_MAX
	a = (area < 0 || area > typemax(Int32)) ? Int(typemax(Int32)) : area
	return PotracePath(pt, a, sign)
end

# Give a tree structure to the given path list, based on "insideness" testing. I.e., path A is
# considered "below" path B if it is inside path B. The input pathlist is assumed to be ordered so
# that "outer" paths occur before "inner" paths. The tree structure is stored in the "childlist"
# and "sibling" components of the path. The linked list structure is also changed so that
# negative path components are listed immediately after their positive parent. We assume that in
# the input, point 0 of each path is an "upper left" corner of the path, as returned by
# bm_to_pathlist. The bm argument should be a bitmap of the correct size (large enough to hold all
# the paths), and will be used as scratch space. Returns the new list head.
function pathlist_to_tree!(plist::Union{PotracePath,Nothing}, bm::PotraceBitmap)
	fill!(bm.map, 0)

	# save original "next" pointers
	p = plist
	while p !== nothing
		p.sibling = p.next
		p.childlist = nothing
		p = p.next
	end

	heap = plist

	# the heap holds a list of lists of paths. Use "childlist" field for outer list, "next" field
	# for inner list. Each of the sublists is to be turned into a tree. Each path is rendered
	# exactly once. The heap gives a tail recursive algorithm: it holds a list of pathlists which
	# still need to be transformed.
	while heap !== nothing
		# unlink first sublist
		cur = heap
		heap = heap.childlist
		cur.childlist = nothing

		# unlink first path
		head = cur
		cur = cur.next
		head.next = nothing

		# render path
		xor_path!(bm, head)
		bx0, bx1, by0, by1 = setbbox_path(head)

		# now do insideness test for each element of cur; append it to head.childlist if it's
		# inside head, else append it to head.next.
		hook_in  = _Hook(head, :childlist)
		hook_out = _Hook(head, :next)
		p = cur                        # list_forall_unlink(p, cur)
		while p !== nothing
			cur = p.next
			p.next = nothing
			if p.pt[1].y <= by0
				hook_out = _insert_beforehook!(p, hook_out)
				# append the remainder of the list to hook_out
				_hset!(hook_out, cur)
				break
			end
			if BM_GET(bm, p.pt[1].x, p.pt[1].y - 1)
				hook_in = _insert_beforehook!(p, hook_in)
			else
				hook_out = _insert_beforehook!(p, hook_out)
			end
			p = cur
		end

		# clear bm
		clear_bm_with_bbox!(bm, bx0, bx1, by0, by1)

		# now schedule head.childlist and head.next for further processing
		if head.next !== nothing
			head.next.childlist = heap
			heap = head.next
		end
		if head.childlist !== nothing
			head.childlist.childlist = heap
			heap = head.childlist
		end
	end

	# copy sibling structure from "next" to "sibling" component
	p = plist
	while p !== nothing
		p1 = p.sibling
		p.sibling = p.next
		p = p1
	end

	# reconstruct a new linked list ("next") structure from tree ("childlist", "sibling")
	# structure. The heap contains a list of childlists which still need to be processed.
	heap = plist
	heap !== nothing && (heap.next = nothing)   # heap is a linked list of childlists
	out = _Head(nothing)
	plist_hook = _hook(out)
	while heap !== nothing
		heap1 = heap.next
		p = heap
		while p !== nothing
			# p is a positive path: append to linked list
			plist_hook = _insert_beforehook!(p, plist_hook)
			# go through its children
			p1 = p.childlist
			while p1 !== nothing
				plist_hook = _insert_beforehook!(p1, plist_hook)
				# append its childlist to heap, if non-empty
				p1.childlist !== nothing && (heap1 = _list_append(heap1, p1.childlist))
				p1 = p1.sibling
			end
			p = p.sibling
		end
		heap = heap1
	end
	return out.p
end

# find the next set pixel in a row <= y. Pixels are searched first left-to-right, then top-down.
# In other words, (x,y)<(x',y') if y>y' or y=y' and x<x'. Returns (found, x, y). Assumes the excess
# bits have been cleared with bm_clearexcess!.
function findnext(bm::PotraceBitmap, xp::Int, yp::Int)
	x0 = xp & ~(BM_WORDBITS - 1)
	for y = yp:-1:0
		x = x0
		while x < bm.w && x >= 0
			if bm.map[_bm_word(bm, x, y)] != 0
				while !BM_GET(bm, x, y)
					x += 1
				end
				return true, x, y      # found
			end
			x += BM_WORDBITS
		end
		x0 = 0
	end
	return false, xp, yp               # not found
end

# Decompose the given bitmap into paths. Returns a linked list of paths with the fields pt, area,
# sign filled in, in tree order (see pathlist_to_tree!).
function bm_to_pathlist(bm::PotraceBitmap, param::PotraceParams)
	bm1 = bm_dup(bm)

	# be sure the byte padding on the right is set to 0, as the fast pixel search below relies on it
	bm_clearexcess!(bm1)

	head = _Head(nothing)
	plist_hook = _hook(head)

	# iterate through components
	x = 0
	y = bm1.h - 1
	while true
		found, x, y = findnext(bm1, x, y)
		found || break

		# calculate the sign by looking at the original
		sign = BM_GET(bm, x, y) ? '+' : '-'

		# calculate the path
		p = findpath(bm1, x, y + 1, sign, param.turnpolicy)

		# update buffered image
		xor_path!(bm1, p)

		# if it's a turd, eliminate it, else append it to the list
		p.area > param.turdsize && (plist_hook = _insert_beforehook!(p, plist_hook))
	end

	return pathlist_to_tree!(head.p, bm1)
end
