# runparallel.jl — the FULL suite (GUI tier included), split across several Julia processes.
#
#     julia test/runparallel.jl            (INTERACTIVEGMT_TEST_JOBS=n picks the shard count, default 6)
#
# Each shard is an ordinary `julia test/runtests.jl gui` run, restricted to a set of WHOLE test files
# (INTERACTIVEGMT_TEST_FILES) — never split inside a file, because some GUI items only behave with the
# rest of their file ahead of them. Files are dealt out by their duration in the previous run
# (longest first, each to the least loaded shard), read off the time stamps in test/last_run.log.
# Every shard writes its own run log (test/last_run_shard<k>.log) and runs in its own scratch
# directory, so GMT's gmt.history / gmt.conf never collide.
#
# The files that press the REAL mouse run ALONE, after the shards: another process raising a window
# while the OS cursor is held down would take the press.
#
# Progress is printed as it happens ("[done/total] s<k> elapsed  item", plus every FAIL/ERROR line),
# and the merged result goes to test/last_run.log, ending in the same "FAILED ITEMS" / "NO FAILED
# ITEMS" verdict a single-process run writes. Exit code 1 when anything failed or a shard died.

const TESTDIR = @__DIR__
const SERIAL  = ["test-aquamoto-transport-gui.jl"]          # real OS mouse input: run alone
const NJOBS   = max(1, parse(Int, strip(get(ENV, "INTERACTIVEGMT_TEST_JOBS", "6"))))
const T0      = time()

stamp(t) = (s = round(Int, t); string(s ÷ 60, "m", lpad(s % 60, 2, '0'), "s"))
secs(m)  = parse(Int, m[1]) * 60 + parse(Int, m[2])

# Per-file seconds from a run log: an item's start stamp to the next stamp (item or END) in the same
# process. A stamp that goes BACKWARDS is the next shard's log starting over, never a duration.
function file_times(path)
	d = Dict{String,Float64}()
	isfile(path) || return d
	file = ""; prev = nothing
	for ln in eachline(path)
		if (m = match(r"^── END (\d+)m(\d+)s", ln)) !== nothing
			t = secs(m)
			prev !== nothing && t >= prev[2] && (d[prev[1]] = get(d, prev[1], 0.0) + t - prev[2])
			prev = nothing
		elseif (m = match(r"^── (test-[^ ]+\.jl)", ln)) !== nothing
			file = m[1]
		elseif (m = match(r"^\[\d+/\d+\] (\d+)m(\d+)s", ln)) !== nothing
			t = secs(m)
			prev !== nothing && t >= prev[2] && (d[prev[1]] = get(d, prev[1], 0.0) + t - prev[2])
			prev = (file, t)
		end
	end
	return d
end

files = sort(filter(f -> startswith(f, "test-") && endswith(f, ".jl") && !occursin("pngquant", f), readdir(TESTDIR)))
times = file_times(joinpath(TESTDIR, "last_run.log"))
par   = filter(f -> !(f in SERIAL), files)
ser   = filter(f -> f in SERIAL, files)

# Longest first, each to the least loaded shard. A file with no recorded time counts 10 s.
shards = [String[] for _ in 1:min(NJOBS, length(par))]
load   = zeros(length(shards))
for f in sort(par, by = f -> -get(times, f, 10.0))
	k = argmin(load)
	push!(shards[k], f); load[k] += get(times, f, 10.0)
end

println("Parallel test run: ", length(shards), " shards + ", length(ser), " file(s) alone afterwards")
for (k, s) in enumerate(shards)
	println("  s", k, "  ~", stamp(load[k]), "  ", length(s), " files")
end
flush(stdout)

mutable struct Shard
	id::String
	files::Vector{String}
	log::String
	out::String
	proc::Any
	pos::Int          # bytes of the run log already read
	done::Int         # items started
	total::Int        # items this shard runs (0 until its first progress line)
end

function launch(id, fs)
	log = joinpath(TESTDIR, "last_run_shard$(id).log")
	out = joinpath(TESTDIR, "last_run_shard$(id).stdout.log")
	rm(log; force = true)
	env = copy(ENV)
	env["INTERACTIVEGMT_TEST_GUI"]   = "1"
	env["INTERACTIVEGMT_TEST_FILES"] = join(fs, ';')
	env["INTERACTIVEGMT_TEST_LOG"]   = log
	cmd = setenv(`$(Base.julia_cmd()) $(joinpath(TESTDIR, "runtests.jl")) gui`, env; dir = mktempdir())
	p = run(pipeline(cmd; stdout = out, stderr = out); wait = false)
	return Shard(id, fs, log, out, p, 0, 0, 0)
end

# New lines of each shard's run log -> the terminal. Progress lines are renumbered over the whole run.
function pump(shards)
	for s in shards
		isfile(s.log) || continue
		open(s.log) do io
			seek(io, s.pos)
			while !eof(io)
				ln = readline(io; keep = true)
				endswith(ln, '\n') || break            # half-written line: read it whole next time
				s.pos = position(io)
				ln = chomp(ln)
				if (m = match(r"^\[(\d+)/(\d+)\] \S+  (.*)$", ln)) !== nothing
					s.done = parse(Int, m[1]); s.total = parse(Int, m[2])
					tot = all(x -> x.total > 0, shards) ? string(sum(x -> x.total, shards)) : "?"
					println("[", sum(x -> x.done, shards), "/", tot, "] s", s.id, " ", stamp(time() - T0), "  ", m[3])
				elseif startswith(ln, "!!! ") || startswith(ln, "Test Failed") || startswith(ln, "Error")
					println("s", s.id, " ", ln)
				end
			end
		end
	end
	flush(stdout)
end

function run_all(shards)
	while any(s -> process_running(s.proc), shards)
		pump(shards); sleep(1)
	end
	pump(shards)
end

running = [launch(string(k), s) for (k, s) in enumerate(shards)]
run_all(running)
if !isempty(ser)
	println("── alone: ", join(ser, ", ")); flush(stdout)
	alone = launch("A", ser)
	run_all([alone])
	push!(running, alone)
end

# The merged log: each shard's own log in turn (its per-shard verdict line dropped), then ONE verdict.
failed = String[]
open(joinpath(TESTDIR, "last_run.log"), "w") do io
	println(io, "InteractiveGMT test run  ", Libc.strftime("%Y-%m-%d %H:%M:%S", T0), "  gui=true  PARALLEL ",
	        length(shards), " shards, wall ", stamp(time() - T0))
	for s in running
		println(io, "\n=== shard ", s.id, " (exit ", s.proc.exitcode, "): ", join(s.files, ", "))
		lines = isfile(s.log) ? readlines(s.log) : String[]
		infail = false
		for ln in lines
			if startswith(ln, "FAILED ITEMS")
				infail = true; continue
			elseif ln == "NO FAILED ITEMS"
				continue
			end
			if infail
				(m = match(r"^  (.*)$", ln)) !== nothing ? push!(failed, m[1]) : (infail = false; println(io, ln))
			else
				println(io, ln)
			end
		end
		# A shard that died before writing its verdict is a failure of everything it was given.
		if s.proc.exitcode != 0 && !any(l -> startswith(l, "FAILED ITEMS"), lines)
			push!(failed, "SHARD $(s.id) DIED (exit $(s.proc.exitcode)) — see $(basename(s.out))")
		end
	end
	if isempty(failed)
		println(io, "\nNO FAILED ITEMS")
	else
		println(io, "\nFAILED ITEMS (", length(failed), "):")
		foreach(f -> println(io, "  ", f), failed)
	end
end

println("\nWall time ", stamp(time() - T0), ", ", sum(s -> s.done, running), " items.")
if isempty(failed)
	println("NO FAILED ITEMS")
else
	println("FAILED ITEMS (", length(failed), "):")
	foreach(f -> println("  ", f), failed)
end
exit(isempty(failed) ? 0 : 1)
