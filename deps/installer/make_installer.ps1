# make_installer.ps1 -- build the Windows installer: iGMT-setup-win64.exe
#
# ASCII ONLY (powershell.exe 5.1 reads a BOM-less script as ANSI; see check_dll_deps.ps1).
#
# 1. Stage a self-contained GMT tree out of the local GMT build:
#      <stage>\gmt\bin    everything in $GmtBin (symlinks followed) + the transitive closure of the
#                         non-system DLLs those files import, found in $DllDirs
#      <stage>\gmt\share  `gmt --show-sharedir` (coast, dcw, cpt, GDAL_DATA, proj, ...)
#    No GMT installer is involved: GMT.jl's own deps/build.jl finds this tree through `gmt --version`
#    on PATH, which setup.jl arranges at install time.
# 2. make_env.jl -> <stage>\env: the Project/Manifest setup.jl instantiates (GMT.jl master +
#    InteractiveGMT master, without the JLLs the staged GMT replaces). Resolved in the build depot
#    deps\build\installer_depot, kept between builds so its downloads happen once.
# 3. makensis igmt.nsi -> deps\build\iGMT-setup-win64.exe, with Julia taken from $JuliaDir.
#
#   powershell -ExecutionPolicy Bypass -File deps\installer\make_installer.ps1
#   powershell ... -File deps\installer\make_installer.ps1 -SkipStage     (keep the staged GMT)
#   powershell ... -File deps\installer\make_installer.ps1 -SkipStage -SkipEnv   (re-pack only)
#
[CmdletBinding()]
param(
    [string]$GmtBin    = "C:\programs\GMTdir",
    [string]$GmtShare  = "",                                     # default: gmt --show-sharedir
    [string[]]$DllDirs = @("C:\programs\compa_libs\DLLs_VC14_64"),
    [string]$JuliaDir  = "C:\programs\Julia-1.14",
    [string]$Makensis  = "C:\programs\NSIS\makensis.exe",
    [switch]$SkipStage,
    [switch]$SkipEnv
)

$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$deps  = Split-Path -Parent $here
$stage = Join-Path $deps "build\installer_stage"
$gbin  = Join-Path $stage "gmt\bin"
$gshr  = Join-Path $stage "gmt\share"

function Get-Dumpbin {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found -- need Visual Studio for dumpbin" }
    $vs = & $vswhere -latest -property installationPath
    $d  = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") -Directory |
          Sort-Object Name -Descending | Select-Object -First 1 |
          ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\dumpbin.exe" }
    if (-not (Test-Path $d)) { throw "dumpbin.exe not found under $vs" }
    return $d
}

# DLL names dumpbin lists as imports (normal + delay-load) of one PE file.
function Get-Imports([string]$dumpbin, [string]$file) {
    & $dumpbin /nologo /dependents $file |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '^[^\s]+\.dll$' }
}

# The VC++ redistributable sits in System32 on a developer box but NOT on a clean one: never
# treat it as part of Windows.
$redist = '^(vcruntime|msvcp|vcomp|concrt|vccorlib|mfc)'
# Debug CRT: not redistributable at all.
$debugCrt = '^(vcruntime140d|vcruntime140_1d|msvcp140d|ucrtbased|concrt140d|vcomp140d)\.dll$'
# What GMT.jl and iGMT load: an unloadable one of these is a hard error, never a dropped file.
$core = @("gmt.exe", "gmt_w64.dll", "postscriptlight_w64.dll", "gsdll64.dll", "gdal_w64.dll", "proj_w64.dll")

if (-not $SkipStage) {
    if (-not (Test-Path (Join-Path $GmtBin "gmt.exe"))) { throw "no gmt.exe in $GmtBin" }
    if (-not $GmtShare) { $GmtShare = (& (Join-Path $GmtBin "gmt.exe") --show-sharedir).Trim() }
    if (-not (Test-Path $GmtShare)) { throw "GMT share dir not found: $GmtShare" }

    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force $gbin | Out-Null

    # --- bin: copy, following symlinks (gdal_w64.dll / proj_w64.dll point into compa_libs) ---
    $skip = '\.(pdb|bak|old|sln|ilk|exp|lib|m|mexw64)$'
    $search = New-Object System.Collections.Generic.List[string]
    foreach ($d in $DllDirs) { $search.Add($d) }
    Get-ChildItem $GmtBin -Recurse -File | Where-Object { $_.Name -notmatch $skip } | ForEach-Object {
        $rel = $_.FullName.Substring($GmtBin.TrimEnd('\').Length + 1)
        $dst = Join-Path $gbin $rel
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        $src = $_.FullName
        if ($_.LinkType -eq "SymbolicLink") {
            $src = @($_.Target)[0]
            $search.Add((Split-Path $src))
        }
        [IO.File]::Copy($src, $dst, $true)
    }
    Write-Host "bin: copied $GmtBin"

    # --- DLL closure ---
    $dumpbin = Get-Dumpbin
    $sys32   = Join-Path $env:SystemRoot "System32"
    $have    = @{}
    Get-ChildItem $gbin -Recurse -File -Include *.dll | ForEach-Object { $have[$_.Name.ToLower()] = 1 }
    $queue   = New-Object System.Collections.Generic.Queue[string]
    Get-ChildItem $gbin -Recurse -File -Include *.dll, *.exe | ForEach-Object { $queue.Enqueue($_.FullName) }
    $missing = @{}
    $added   = @()
    $fromSys = @{}
    $imports = @{}
    while ($queue.Count -gt 0) {
        $f = $queue.Dequeue()
        $imports[$f] = @(Get-Imports $dumpbin $f | ForEach-Object { $_.ToLower() })
        foreach ($dll in $imports[$f]) {
            $k = $dll.ToLower()
            if ($have.ContainsKey($k) -or $missing.ContainsKey($k)) { continue }
            if ($k -match $debugCrt) { $missing[$k] = $f; continue }    # never redistributable
            if ($k -match '^(api|ext)-ms-') { continue }
            $isRedist = $k -match $redist
            if (-not $isRedist -and (Test-Path (Join-Path $sys32 $dll))) {
                $vi = (Get-Item (Join-Path $sys32 $dll)).VersionInfo
                if ($vi.CompanyName -notmatch 'Microsoft') { $fromSys[$dll] = $vi.CompanyName }
                continue
            }
            $src = $null
            foreach ($d in (@($search) + @($sys32))) {
                $c = Join-Path $d $dll
                if (Test-Path $c) { $src = (Get-Item $c).FullName; break }
            }
            if (-not $src) { $missing[$k] = $f; continue }
            $it = Get-Item $src
            if ($it.LinkType -eq "SymbolicLink") { $src = @($it.Target)[0] }
            $dst = Join-Path $gbin $dll
            [IO.File]::Copy($src, $dst, $true)
            $have[$k] = 1
            $added += $dll
            $queue.Enqueue($dst)
        }
    }
    Write-Host ("closure: added {0} DLLs: {1}" -f $added.Count, ($added -join ", "))
    if ($fromSys.Count) {
        Write-Warning "imports resolved from System32 that are NOT Microsoft's (a clean machine may lack them):"
        $fromSys.GetEnumerator() | ForEach-Object { Write-Warning ("  {0}  ({1})" -f $_.Key, $_.Value) }
    }
    # A file with an unresolvable import (or one on the debug CRT) cannot load on this machine
    # either -- leftovers in the build tree. Drop it, and anything that imports it, transitively.
    # If that reaches a core file the bundle would be broken: stop.
    $broken = @{}
    foreach ($k in $missing.Keys) { $broken[$k] = 1 }
    do {
        $more = $false
        foreach ($f in @($imports.Keys)) {
            $bad = @($imports[$f] | Where-Object { $broken.ContainsKey($_) })
            if (-not $bad.Count) { continue }
            $name = (Split-Path $f -Leaf).ToLower()
            if ($core -contains $name -or $f -like "*\gmt_plugins\*") {
                throw "core file $name cannot load: needs $($bad -join ', ') -- add its folder to -DllDirs"
            }
            Write-Warning "dropping $name (needs $($bad -join ', '))"
            Remove-Item $f
            $imports.Remove($f)
            $broken[$name] = 1
            $more = $true
        }
    } while ($more)

    # --- share ---
    & robocopy $GmtShare $gshr /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy of $GmtShare failed ($LASTEXITCODE)" }
    $LASTEXITCODE = 0
    Write-Host "share: copied $GmtShare"

    # --- smoke test: the staged gmt runs with ONLY its own folder + Windows on PATH ---
    $env:PATH = "$gbin;$sys32;$env:SystemRoot"
    $env:GMT_SHAREDIR = $gshr
    $ver = & (Join-Path $gbin "gmt.exe") --version
    if ($LASTEXITCODE -ne 0) { throw "staged gmt.exe does not run" }
    & (Join-Path $gbin "gmt.exe") grdmath -R0/1/0/1 -I1 1 = (Join-Path $stage "smoke.nc")
    if ($LASTEXITCODE -ne 0) { throw "staged gmt grdmath failed" }
    Remove-Item (Join-Path $stage "smoke.nc")
    Write-Host "staged GMT $ver runs standalone"
}

# --- environment: resolved with the staged GMT first on PATH, in the private build depot ---
if (-not $SkipEnv) {
    if (-not (Test-Path (Join-Path $gbin "gmt.exe"))) { throw "no staged GMT in $gbin -- run without -SkipStage" }
    $julia = Join-Path $JuliaDir "bin\julia.exe"
    $sys32 = Join-Path $env:SystemRoot "System32"
    $saved = @{ PATH = $env:PATH; JULIA_DEPOT_PATH = $env:JULIA_DEPOT_PATH; GMT_SHAREDIR = $env:GMT_SHAREDIR
                JULIA_PKG_PRECOMPILE_AUTO = $env:JULIA_PKG_PRECOMPILE_AUTO; JULIA_PROJECT = $env:JULIA_PROJECT }
    try {
        $env:PATH = "$gbin;$sys32;$env:SystemRoot;$(Join-Path $env:SystemRoot 'System32\Wbem')"
        $env:JULIA_DEPOT_PATH = "$(Join-Path $deps 'build\installer_depot');"   # ';' keeps Julia's bundled depots
        $env:GMT_SHAREDIR = $gshr
        $env:JULIA_PKG_PRECOMPILE_AUTO = "0"     # only resolving here; precompiling is setup.jl's job
        $env:JULIA_PROJECT = $null
        & $julia --startup-file=no --history-file=no (Join-Path $here "make_env.jl") $stage
        if ($LASTEXITCODE -ne 0) { throw "make_env.jl failed ($LASTEXITCODE)" }
    } finally {
        foreach ($k in $saved.Keys) {
            if ($null -eq $saved[$k]) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
            else { Set-Item "env:$k" $saved[$k] }
        }
    }
}
foreach ($f in "Project.toml", "Manifest.toml", "igmt_commit.txt") {
    if (-not (Test-Path (Join-Path $stage "env\$f"))) { throw "missing $stage\env\$f -- run without -SkipEnv" }
}

# --- GMT.jl shipped in the installer: the version the Manifest pins, from the build depot ---
# Always run, also with -SkipEnv: it is a plain copy, and it must match the Manifest being packed.
$saveddepot = $env:JULIA_DEPOT_PATH
try {
    $env:JULIA_DEPOT_PATH = "$(Join-Path $deps 'build\installer_depot');"
    & (Join-Path $JuliaDir "bin\julia.exe") --startup-file=no --history-file=no (Join-Path $here "ship_gmtjl.jl") $stage
    if ($LASTEXITCODE -ne 0) { throw "ship_gmtjl.jl failed ($LASTEXITCODE)" }
} finally {
    if ($null -eq $saveddepot) { Remove-Item env:JULIA_DEPOT_PATH -ErrorAction SilentlyContinue }
    else { $env:JULIA_DEPOT_PATH = $saveddepot }
}

# --- pack ---
$igmt = Join-Path $deps "build\igmt.exe"
if (-not (Test-Path $igmt))  { throw "launcher not built: $igmt" }
if (-not (Test-Path $Makensis)) { throw "makensis not found: $Makensis" }
& $Makensis /V3 "/DJULIA_SRC=$JuliaDir" "/DSTAGE=$stage" "/DBUILD=$(Join-Path $deps 'build')" (Join-Path $here "igmt.nsi")
if ($LASTEXITCODE -ne 0) { throw "makensis failed ($LASTEXITCODE)" }
Write-Host "done: $(Join-Path $deps 'build\iGMT-setup-win64.exe')"
