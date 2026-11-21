param(
    [string]$BuildDir = "build",
    [string]$Compiler = "",
    [string]$Generator = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")][string]$Configuration = "Release",
    [switch]$NoTests
)
$ErrorActionPreference = "Stop"
if (!(Get-Command cmake -ErrorAction SilentlyContinue)) { throw "Install CMake 3.26+ and add it to PATH." }
if (![IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $PSScriptRoot $BuildDir }
$cacheExists = Test-Path (Join-Path $BuildDir "CMakeCache.txt")
$options = @("-S", $PSScriptRoot, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=$Configuration")
if ($NoTests) { $options += "-DBUILD_TESTING=OFF" } else { $options += "-DBUILD_TESTING=ON" }
if (!$cacheExists -and !$Compiler -and !$Generator -and !$env:CXX -and !(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $gcc = Get-Command g++.exe -ErrorAction SilentlyContinue
    if ($gcc) { $Compiler = $gcc.Source }
}
if ($Compiler) {
    $resolved = Get-Command $Compiler -ErrorAction SilentlyContinue
    if (!$resolved) { throw "Compiler not found: $Compiler" }
    $Compiler = $resolved.Source
    if ([IO.Path]::GetFileName($Compiler) -match 'g\+\+(\.exe)?$') {
        $version = & $Compiler -dumpversion
        if ($LASTEXITCODE -ne 0 -or [int]($version.Split('.')[0]) -lt 9) { throw "GCC 9+ is required. Use -Compiler with a newer g++ path, or -Generator with MSVC." }
        if (!$Generator -and !$cacheExists) {
            $make = Join-Path (Split-Path $Compiler) "mingw32-make.exe"
            if (Test-Path $make) { $Generator = "MinGW Makefiles"; $options += "-DCMAKE_MAKE_PROGRAM=$make" }
            elseif (Get-Command ninja -ErrorAction SilentlyContinue) { $Generator = "Ninja" }
            else { throw "Add mingw32-make or Ninja to PATH, or specify -Generator." }
        }
    }
    $options += "-DCMAKE_CXX_COMPILER=$Compiler"
}
if ($Generator) { $options += @("-G", $Generator) }
& cmake @options
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $BuildDir --config $Configuration --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Built: $BuildDir/bin/cpp_agent.exe"
Write-Host "Tests: ctest --test-dir `"$BuildDir`" -C $Configuration --output-on-failure"
