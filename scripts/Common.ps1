function Resolve-PilotBinary {
    param([string]$ProjectDir, [string]$BuildDir, [string]$Executable)
    if ($Executable) {
        if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Executable not found: $Executable" }
        return (Resolve-Path -LiteralPath $Executable).Path
    }
    if (![IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $ProjectDir $BuildDir }
    $binary = Join-Path $BuildDir "bin/cpp_agent.exe"
    if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Build the program first: ./build.ps1. Missing: $binary" }
    return (Resolve-Path -LiteralPath $binary).Path
}
