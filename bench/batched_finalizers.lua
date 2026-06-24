-- Synthetic benchmark for the experimental batched direct C userdata finalizer path.
-- It varies metatable count and distinct zero-upvalue C finalizer identities.

local function fail(msg)
  io.stderr:write("batched_finalizers benchmark: ", msg, "\n")
  os.exit(1)
end

local function usage()
  io.write([[
usage: src/luajit bench/batched_finalizers.lua [options]

options:
  --objects N          userdata objects allocated per burst (default: 10000)
  --bursts N           measured bursts per metatable count (default: 50)
  --k LIST             comma-separated metatable counts (default: 1,2,4,8)
  --functions N        distinct zero-upvalue C finalizer functions (default: 4)
  --ineligible-every N make every Nth metatable use a C closure finalizer (default: 0)
  --mode MODE          collection mode: collect or step (default: collect)
  --step N             collectgarbage("step", N) argument for step mode (default: 0)
  --cc PATH            C compiler used for the helper (env: CC, default: cc)
  --build-dir DIR      helper build directory (default: /tmp/luajit-batched-finalizers-$USER)
  --no-compile         load an already-built batchedfinalizers module from package.cpath
  -h, --help           show this help

Output is CSV-ish and intended for build-to-build comparison. GCStats-enabled
builds report whether batched-finalizer counters are available. Telemetry builds
include counter overhead; compare timing against like-for-like telemetry
settings only. Helper compilation assumes a POSIX-like shell and a compiler
accepting -shared/-fPIC.
]])
end

local function getenv(name)
  return os.getenv("BATCHED_FINALIZERS_" .. name) or os.getenv(name)
end

local function parse_nonnegative_int(value, name)
  local n = tonumber(value)
  if not n or n < 0 or n % 1 ~= 0 then
    fail(name .. " must be a non-negative integer")
  end
  return n
end

local function parse_positive_int(value, name)
  local n = parse_nonnegative_int(value, name)
  if n < 1 then fail(name .. " must be a positive integer") end
  return n
end

local function required_arg(argv, i, opt)
  local value = argv[i + 1]
  if value == nil or value == "" then fail(opt .. " requires a value") end
  return value
end

local function parse_k_list(value)
  local ks = {}
  for part in tostring(value):gmatch("[^,]+") do
    ks[#ks + 1] = parse_positive_int(part, "k")
  end
  if #ks == 0 then fail("k list must not be empty") end
  return ks
end

local objects = parse_positive_int(getenv("OBJECTS") or "10000", "objects")
local bursts = parse_positive_int(getenv("BURSTS") or "50", "bursts")
local ks = parse_k_list(getenv("K") or "1,2,4,8")
local finalizer_functions = parse_positive_int(getenv("FUNCTIONS") or "4", "functions")
local ineligible_every = parse_nonnegative_int(getenv("INELIGIBLE_EVERY") or "0", "ineligible-every")
local mode = getenv("MODE") or "collect"
local step_arg = tonumber(getenv("STEP") or "0") or 0
local cc = getenv("CC") or "cc"
local build_dir = getenv("BUILD_DIR")
local no_compile = false

local i = 1
while i <= #arg do
  local a = arg[i]
  if a == "-h" or a == "--help" then
    usage()
    os.exit(0)
  elseif a == "--objects" then
    objects = parse_positive_int(required_arg(arg, i, a), "objects")
    i = i + 1
  elseif a == "--bursts" then
    bursts = parse_positive_int(required_arg(arg, i, a), "bursts")
    i = i + 1
  elseif a == "--k" then
    ks = parse_k_list(required_arg(arg, i, a))
    i = i + 1
  elseif a == "--functions" then
    finalizer_functions = parse_positive_int(required_arg(arg, i, a), "functions")
    i = i + 1
  elseif a == "--ineligible-every" then
    ineligible_every = parse_nonnegative_int(required_arg(arg, i, a), "ineligible-every")
    i = i + 1
  elseif a == "--mode" then
    mode = required_arg(arg, i, a)
    i = i + 1
  elseif a == "--step" then
    step_arg = tonumber(required_arg(arg, i, a)) or fail("step must be numeric")
    i = i + 1
  elseif a == "--cc" then
    cc = required_arg(arg, i, a)
    i = i + 1
  elseif a == "--build-dir" then
    build_dir = required_arg(arg, i, a)
    i = i + 1
  elseif a == "--no-compile" then
    no_compile = true
  else
    fail("unknown option: " .. tostring(a))
  end
  i = i + 1
end

if mode ~= "collect" and mode ~= "step" then
  fail("mode must be collect or step")
end

local function shell_quote(s)
  return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function command_ok(cmd)
  local ok = os.execute(cmd)
  return ok == true or ok == 0
end

local function dirname(path)
  local dir = tostring(path):match("^(.*)/[^/]*$")
  return dir and dir ~= "" and dir or "."
end

local script_dir = dirname(arg[0] or "bench/batched_finalizers.lua")
local repo_root = dirname(script_dir)
build_dir = build_dir or ("/tmp/luajit-batched-finalizers-" .. (os.getenv("USER") or "user"))

if not no_compile then
  local src = script_dir .. "/batched_finalizers_helper.c"
  local out = build_dir .. "/batchedfinalizers.so"
  local mkdir_cmd = "mkdir -p " .. shell_quote(build_dir)
  if not command_ok(mkdir_cmd) then fail("cannot create build dir: " .. build_dir) end
  local cmd = table.concat({
    cc, " -shared -fPIC -I", shell_quote(repo_root .. "/src"),
    " -o ", shell_quote(out), " ", shell_quote(src)
  })
  if not command_ok(cmd) then fail("cannot compile helper with: " .. cmd) end
  package.cpath = build_dir .. "/?.so;" .. package.cpath
end

local m = require "batchedfinalizers"
if finalizer_functions > m.max_finalizers then
  fail("functions exceeds helper max of " .. tostring(m.max_finalizers))
end

local timer_name = "os.clock"
local now = os.clock
do
  local ok, ffi = pcall(require, "ffi")
  if ok then
    ffi.cdef[[
      typedef long time_t;
      typedef struct timespec { time_t tv_sec; long tv_nsec; } timespec;
      int clock_gettime(int clk_id, struct timespec *tp);
    ]]
    local ts = ffi.new("struct timespec[1]")
    local CLOCK_MONOTONIC = 1
    now = function()
      if ffi.C.clock_gettime(CLOCK_MONOTONIC, ts) ~= 0 then return os.clock() end
      return tonumber(ts[0].tv_sec) + tonumber(ts[0].tv_nsec) / 1000000000
    end
    timer_name = "clock_gettime(CLOCK_MONOTONIC)"
  end
end

local has_gcstats = jit and jit.gcstats
local initial_gcstats = has_gcstats and jit.gcstats() or nil
local batch_stats_available = initial_gcstats and initial_gcstats.finalizer_direct_cfunc_batches ~= nil

local function full_gc()
  collectgarbage("restart")
  collectgarbage("collect")
  collectgarbage("collect")
end

local function counters()
  local count, upvalue_count, stack_errors = m.counters()
  if stack_errors ~= 0 then fail("helper observed finalizer stack errors: " .. tostring(stack_errors)) end
  return count, upvalue_count
end

local function total_count()
  local count, upvalue_count = counters()
  return count + upvalue_count
end

local function drain(expected)
  if mode == "collect" then
    for _ = 1, 128 do
      collectgarbage("collect")
      if total_count() >= expected then return end
    end
  else
    for _ = 1, 100000 do
      collectgarbage("step", step_arg)
      if total_count() >= expected then return end
    end
  end
  fail("could not drain finalizers: got " .. tostring(total_count()) ..
       " expected " .. tostring(expected))
end

local function rank(sorted, p)
  local n = #sorted
  local idx = math.ceil(p * n)
  if idx < 1 then idx = 1 end
  if idx > n then idx = n end
  return sorted[idx]
end

local function summarize(samples)
  local sorted = {}
  local sum = 0
  for i = 1, #samples do
    sorted[i] = samples[i]
    sum = sum + samples[i]
  end
  table.sort(sorted)
  local worst_n = math.max(1, math.ceil(#sorted * 0.001))
  local worst_sum = 0
  for j = #sorted - worst_n + 1, #sorted do worst_sum = worst_sum + sorted[j] end
  return {
    samples = #sorted,
    avg = sum / #sorted,
    median = rank(sorted, 0.50),
    p90 = rank(sorted, 0.90),
    p95 = rank(sorted, 0.95),
    p99 = rank(sorted, 0.99),
    p999 = rank(sorted, 0.999),
    max = sorted[#sorted],
    worst001 = worst_sum / worst_n,
  }
end

local function stat_delta(before, after, noise, collections, name)
  if not before or not after or before[name] == nil or after[name] == nil then return "" end
  local value = after[name] - before[name]
  if noise and noise[name] then value = value - noise[name] * collections end
  return tostring(value)
end

local function stat_highwater(after, name)
  if not after or after[name] == nil then return "" end
  return tostring(after[name])
end

local function finalizer_counts_string()
  local counts = m.finalizer_counts()
  local parts = {}
  for i = 1, finalizer_functions do
    parts[i] = tostring(counts[i] or 0)
  end
  return table.concat(parts, ";")
end

local function print_row(k, phase, summary, before, after, noise, collections,
                         direct_finalizers, upvalue_finalizers, function_counts)
  io.write(table.concat({
    tostring(k), phase, tostring(summary.samples),
    string.format("%.6f", summary.avg * 1000),
    string.format("%.6f", summary.median * 1000),
    string.format("%.6f", summary.p90 * 1000),
    string.format("%.6f", summary.p95 * 1000),
    string.format("%.6f", summary.p99 * 1000),
    string.format("%.6f", summary.p999 * 1000),
    string.format("%.6f", summary.max * 1000),
    string.format("%.6f", summary.worst001 * 1000),
    tostring(direct_finalizers),
    tostring(upvalue_finalizers),
    function_counts,
    stat_delta(before, after, noise, collections, "finalizer_queued"),
    stat_delta(before, after, noise, collections, "finalizer_calls"),
    stat_delta(before, after, noise, collections, "finalizer_cfunc_nup0_calls"),
    stat_delta(before, after, noise, collections, "finalizer_cfunc_upvalue_calls"),
    stat_delta(before, after, noise, collections, "finalizer_direct_cfunc_calls"),
    stat_delta(before, after, noise, collections, "finalizer_direct_cfunc_batches"),
    stat_delta(before, after, noise, collections, "finalizer_direct_cfunc_batched_calls"),
    stat_highwater(after, "finalizer_direct_cfunc_batch_max"),
    stat_delta(before, after, noise, collections, "finalizer_nonresurrecting_cfunc_frees"),
  }, ","), "\n")
end

io.write("# benchmark=batched_finalizers\n")
io.write("# objects_per_burst=", tostring(objects), "\n")
io.write("# bursts=", tostring(bursts), "\n")
io.write("# mode=", mode, "\n")
io.write("# step=", tostring(step_arg), "\n")
io.write("# timer=", timer_name, "\n")
io.write("# finalizer_functions=", tostring(finalizer_functions), "\n")
io.write("# ineligible_every=", tostring(ineligible_every), "\n")
io.write("# gcstats=", has_gcstats and "available" or "unavailable", "\n")
if has_gcstats then
  io.write("# batched_finalizer_counters=", batch_stats_available and "available" or "unavailable", "\n")
  io.write("# note=GCStats telemetry is enabled and adds overhead; compare against other GCStats builds only.\n")
  io.write("# note=additive counter columns subtract one empty full-collection baseline per burst; high-water columns report measured-run values.\n")
else
  io.write("# batched_finalizer_counters=unknown\n")
  io.write("# note=GCStats telemetry is unavailable; counter columns are blank.\n")
end
io.write("k,phase,samples,avg_ms,median_ms,p90_ms,p95_ms,p99_ms,p999_ms,max_ms,worst_0_1pct_avg_ms,direct_finalizers,upvalue_finalizers,function_counts,finalizer_queued,finalizer_calls,cfunc_nup0_calls,cfunc_upvalue_calls,direct_cfunc_calls,direct_cfunc_batches,direct_cfunc_batched_calls,direct_cfunc_batch_max,nonresurrecting_cfunc_frees\n")

for _, k in ipairs(ks) do
  full_gc()
  m.reset()
  m.ensure_metatables(k, finalizer_functions, ineligible_every)
  local noise = nil
  if has_gcstats then
    jit.gcstats(true)
    collectgarbage("collect")
    noise = jit.gcstats(true)
  end
  local before = has_gcstats and jit.gcstats() or nil
  local alloc_samples, gc_samples, total_samples = {}, {}, {}
  for burst = 1, bursts do
    local expected = burst * objects
    collectgarbage("stop")
    local t0 = now()
    if k == 1 then
      m.alloc_homogeneous(objects)
    else
      m.alloc_mixed(objects, k)
    end
    local t1 = now()
    collectgarbage("restart")
    drain(expected)
    local t2 = now()
    alloc_samples[burst] = t1 - t0
    gc_samples[burst] = t2 - t1
    total_samples[burst] = t2 - t0
  end
  local after = has_gcstats and jit.gcstats() or nil
  local direct_finalizers, upvalue_finalizers = counters()
  local function_counts = finalizer_counts_string()
  print_row(k, "alloc", summarize(alloc_samples), before, after, noise, bursts,
            direct_finalizers, upvalue_finalizers, function_counts)
  print_row(k, "gc_finalizer", summarize(gc_samples), before, after, noise, bursts,
            direct_finalizers, upvalue_finalizers, function_counts)
  print_row(k, "total", summarize(total_samples), before, after, noise, bursts,
            direct_finalizers, upvalue_finalizers, function_counts)
end
