# decisiontree.jl — CART classification tree, replacing the DecisionTree.jl dependency of RemoteS.
#
# A port of the algorithm DecisionTree.jl's `DecisionTreeClassifier` runs with the settings RemoteS
# used (treeclassifier.jl `_split!`/`_fit`, DecisionTree.jl MIT, itself a port of scikit-learn's):
#   * impurity = ENTROPY (DecisionTree's default loss), split quality = -(nl*H(left) + nr*H(right))
#   * every feature is tried at every node (n_subfeatures = 0 -> all of them)
#   * min_samples_leaf = 1, min_samples_split = 2, min_purity_increase = 0, no pruning
#   * the threshold is the midpoint between the two distinct feature values the best split falls
#     between; training partitions at the LOWER value, prediction sends `x < threshold` left
#   * a new split replaces the best only when strictly better and not `isapprox` to it
# DecisionTree visits the features in a random order, which only decides between exactly-tied
# splits; here they are visited in order, so the same data always gives the same tree.
#
# The tree is stored FLAT (one vector per node attribute, children as indices) rather than as
# DecisionTree's recursive `Union{Leaf,Node}`: traversal is a plain loop over concrete types.

"""
    DecisionTreeModel{T}

A trained CART classification tree. `classes` holds the distinct training labels, sorted; the
columns of `predict_proba` follow that order.
"""
struct DecisionTreeModel{T}
	classes::Vector{T}
	feature::Vector{Int}          # 0 = leaf
	threshold::Vector{Float64}
	left::Vector{Int}
	right::Vector{Int}
	label::Vector{Int}            # index into `classes`: the leaf's majority
	counts::Matrix{Int}           # n_classes x n_nodes: training samples per class at each node
	n_features::Int
end

Base.show(io::IO, m::DecisionTreeModel) =
	print(io, "DecisionTreeModel(", length(m.classes), " classes, ", m.n_features, " features, ",
	      count(==(0), m.feature), " leaves)")

# DecisionTree's util.entropy(ns, n): log(n) - sum(k*log(k))/n
@inline function _entropy(ns::Vector{Int}, n::Int)::Float64
	s = 0.0
	@inbounds for k in ns
		k > 0 && (s += k * log(k))
	end
	return log(n) - s / n
end

"""
    fit_tree(X::Matrix{Float64}, y::Vector{T}; max_depth::Int=-1) -> DecisionTreeModel{T}

Train a classification tree on the samples in the rows of `X` with labels `y`.
`max_depth = -1` means no depth limit.
"""
function fit_tree(X::Matrix{Float64}, y::Vector{T}; max_depth::Int=-1) where T
	n_samples, n_features = size(X)
	n_samples == length(y) || error("fit_tree: $(n_samples) samples but $(length(y)) labels")
	n_samples == 0 && error("fit_tree: no training samples")
	maxd = max_depth < 0 ? typemax(Int) : max_depth

	classes = sort(unique(y))
	cls_ind = Dict{T,Int}(c => i for (i, c) in enumerate(classes))
	Y = Int[cls_ind[v] for v in y]
	nclass = length(classes)

	feature = Int[];  threshold = Float64[];  left = Int[];  right = Int[];  label = Int[]
	counts = Vector{Vector{Int}}()

	indX = collect(1:n_samples)
	Xf = Vector{Float64}(undef, n_samples)
	perm = Vector{Int}(undef, n_samples)
	nc  = zeros(Int, nclass);  ncl = zeros(Int, nclass);  ncr = zeros(Int, nclass)

	newnode!() = (push!(feature, 0); push!(threshold, 0.0); push!(left, 0); push!(right, 0);
	              push!(label, 0); push!(counts, zeros(Int, nclass)); length(feature))

	# Stack of (node id, sample range, depth). Nodes are created when pushed.
	stack = Tuple{Int,UnitRange{Int},Int}[(newnode!(), 1:n_samples, 0)]
	while !isempty(stack)
		id, region, depth = pop!(stack)
		n = length(region)
		fill!(nc, 0)
		@inbounds for i in region  nc[Y[indX[i]]] += 1  end
		counts[id] .= nc
		label[id] = argmax(nc)
		(n < 2 || depth >= maxd || nc[label[id]] == n) && continue      # leaf

		node_imp = n * _entropy(nc, n)
		best = -Inf;  best_f = 0;  thr_lo = 0.0;  thr_hi = 0.0
		r0 = first(region) - 1
		for f in 1:n_features
			@inbounds for i in 1:n  Xf[i] = X[indX[r0+i], f]  end
			sp = sortperm(view(Xf, 1:n))
			@inbounds for i in 1:n  perm[i] = indX[r0+sp[i]]  end
			fill!(ncl, 0);  copyto!(ncr, nc)
			lo = 1
			@inbounds while lo <= n
				v = X[perm[lo], f]
				hi = lo
				while hi < n && X[perm[hi+1], f] == v  hi += 1  end
				if lo > 1                                     # a split between lo-1 and lo
					nl = lo - 1;  nr = n - nl
					q = -(nl * _entropy(ncl, nl) + nr * _entropy(ncr, nr))
					if q > best && !isapprox(q, best)
						best = q;  best_f = f;  thr_lo = X[perm[lo-1], f];  thr_hi = v
					end
				end
				for i in lo:hi
					c = Y[perm[i]];  ncl[c] += 1;  ncr[c] -= 1
				end
				lo = hi + 1
			end
		end
		(best_f == 0 || best + node_imp < 0.0) && continue               # unsplittable -> leaf

		# Partition the region: feature value <= thr_lo to the left, order within each side kept.
		nleft = 0
		@inbounds for i in region
			X[indX[i], best_f] <= thr_lo && (nleft += 1)
		end
		a = r0;  b = r0 + nleft
		tmp = indX[region]
		@inbounds for s in tmp
			if X[s, best_f] <= thr_lo  indX[a += 1] = s
			else                       indX[b += 1] = s
			end
		end
		feature[id] = best_f
		threshold[id] = (thr_lo + thr_hi) / 2.0
		l = newnode!();  r = newnode!()
		left[id] = l;  right[id] = r
		push!(stack, (r, (r0 + nleft + 1):last(region), depth + 1))
		push!(stack, (l, first(region):(r0 + nleft), depth + 1))
	end

	cmat = Matrix{Int}(undef, nclass, length(counts))
	for (j, c) in enumerate(counts)  cmat[:, j] = c  end
	return DecisionTreeModel{T}(classes, feature, threshold, left, right, label, cmat, n_features)
end

# The leaf a sample (one row of features, read by the accessor `x(f)`) falls into.
@inline function _leaf(m::DecisionTreeModel, x::F)::Int where F
	id = 1
	@inbounds while m.feature[id] != 0
		id = x(m.feature[id]) < m.threshold[id] ? m.left[id] : m.right[id]
	end
	return id
end

"""
    predict(model, X::Matrix{Float64}) -> Vector

The class of each row of `X`.
"""
function predict(m::DecisionTreeModel{T}, X::Matrix{Float64}) where T
	size(X, 2) == m.n_features || error("predict: model has $(m.n_features) features, X has $(size(X,2))")
	out = Vector{T}(undef, size(X, 1))
	@inbounds for i in axes(X, 1)
		out[i] = m.classes[m.label[_leaf(m, f -> X[i, f])]]
	end
	return out
end

"""
    predict_proba(model, X::Matrix{Float64}) -> Matrix{Float64}

Per-class probabilities of each row of `X` (rows sum to 1; columns in `model.classes` order): the
class fractions of the training samples in the leaf the row falls into.
"""
function predict_proba(m::DecisionTreeModel, X::Matrix{Float64})::Matrix{Float64}
	size(X, 2) == m.n_features || error("predict_proba: model has $(m.n_features) features, X has $(size(X,2))")
	nclass = length(m.classes)
	out = Matrix{Float64}(undef, size(X, 1), nclass)
	@inbounds for i in axes(X, 1)
		id = _leaf(m, f -> X[i, f])
		tot = 0
		for c in 1:nclass  tot += m.counts[c, id]  end
		for c in 1:nclass  out[i, c] = m.counts[c, id] / tot  end
	end
	return out
end
