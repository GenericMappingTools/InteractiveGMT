# check_dll_deps.ps1 -- refuse to publish a gmtvtk.dll the installed runtime cannot load.
#
# ASCII ONLY, deliberately: powershell.exe 5.1 reads a BOM-less script as ANSI, so a UTF-8 em dash
# lands mid-string as three bytes and the file no longer parses. Keep every character 7-bit.
#
# Run BEFORE `gh release upload dll-latest gmtvtk-win64.zip --clobber`. Exit 0 = safe to publish.
#
# The two release artifacts move at different speeds (see deps/build.jl): the full VTK/Qt bundle
# (iGMT-win64-full.zip, tag in deps/RUNTIME_VERSION) is fetched when the pinned tag changes, while
# the dll-only zip is re-fetched constantly. Link one new VTK module into the C++ and the small zip
# hands every existing install a DLL whose dependency is simply absent -- LoadLibrary fails, and
# Julia reports it as a missing gmtvtk_* symbol, which points at nothing.
#
# This walks gmtvtk.dll's transitive imports with dumpbin and subtracts the file list of the full
# bundle plus whatever the dll zip itself carries. Anything left over is a machine that will not
# start. Happened 2026-07-29 (vtkIOHDF + vtkhdf5 + vtkhdf5_hl + vtkFiltersTemporal).
#
#   powershell -ExecutionPolicy Bypass -File deps\check_dll_deps.ps1
#   powershell ... -File deps\check_dll_deps.ps1 -DllZip build\gmtvtk-win64.zip
#
[CmdletBinding()]
param(
    [string]$Dll      = "",                                    # default: deps\build\gmtvtk.dll
    [string]$FullZip  = "",                                    # default: deps\build\iGMT-win64-full.zip
    [string]$DllZip   = "",                                    # optional: the zip about to be uploaded
    [string[]]$ToolchainBins = @(
        "C:\programs\compa_libs\VTK-9.6.2\compileds\bin",
        "C:\programs\Qt6\6.11.1\msvc2022_64\bin",
        "C:\programs\compa_libs\oneTBB\compileds\bin")
)

$ErrorActionPreference = "Stop"
$tar = Join-Path $env:SystemRoot "System32\tar.exe"

# Resolved here, not as a param() default: $PSScriptRoot is not populated while param defaults are
# evaluated under `powershell.exe -File`, so a default built on it binds an empty string.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Dll)     { $Dll     = Join-Path $here "build\gmtvtk.dll" }
if (-not $FullZip) { $FullZip = Join-Path $here "build\iGMT-win64-full.zip" }

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

if (-not (Test-Path $Dll))     { throw "dll not found: $Dll" }
if (-not (Test-Path $FullZip)) { throw "full runtime bundle not found: $FullZip (it is the baseline this checks against)" }

$dumpbin = Get-Dumpbin

# What a target machine will have: the full bundle, plus anything the dll zip itself ships.
$available = @(& $tar -tf $FullZip | ForEach-Object { Split-Path $_ -Leaf })
if ($DllZip) {
    if (-not (Test-Path $DllZip)) { throw "dll zip not found: $DllZip" }
    $available += @(& $tar -tf $DllZip | ForEach-Object { Split-Path $_ -Leaf })
}
$available = $available | Where-Object { $_ -like "*.dll" } | Sort-Object -Unique

# Transitive closure over the modules we actually ship (vtk*/Qt6*/tbb*); system DLLs are the OS's
# problem, not ours.
$seen = @{}; $queue = New-Object System.Collections.Queue; $queue.Enqueue($Dll)
$missing = @()
while ($queue.Count -gt 0) {
    $path = $queue.Dequeue()
    $name = Split-Path $path -Leaf
    if ($seen.ContainsKey($name)) { continue }
    $seen[$name] = $true
    if (-not (Test-Path $path)) { continue }
    & $dumpbin /dependents $path | Where-Object { $_ -match '^\s+\S+\.dll$' } | ForEach-Object {
        $m = $_.Trim()
        if ($m -match '^(vtk|Qt6|tbb)') {
            if ($available -notcontains $m) { $missing += $m }
            foreach ($b in $ToolchainBins) {
                $c = Join-Path $b $m
                if (Test-Path $c) { $queue.Enqueue($c); break }
            }
        }
    }
}

"modules walked : $($seen.Count)"
"baseline       : $(Split-Path $FullZip -Leaf)$(if ($DllZip) { ' + ' + (Split-Path $DllZip -Leaf) })"
$missing = $missing | Sort-Object -Unique
if ($missing.Count -eq 0) {
    "RESULT         : OK -- every dependency is present on a target machine"
    exit 0
}
"RESULT         : FAIL -- these modules exist on NO target machine:"
$missing | ForEach-Object { "                 $_" }
""
"Fix by either bumping deps/RUNTIME_VERSION and publishing a new iGMT-win64-full.zip, or adding"
"them to _GMTVTK_DLL_EXTRA_MODULES in deps/CMakeLists.txt so the dll-only zip carries them."
exit 1
