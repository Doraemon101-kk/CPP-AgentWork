param(
    [string]$Root = "$PSScriptRoot/fixtures/repair_project",
    [string]$Model = "",
    [string]$Resume = "",
    [string]$Execute = "",
    [ValidateRange(1,65535)][int]$Port = 11434,
    [string]$BuildDir = "build",
    [string]$Executable = "",
    [string]$StateDir = "$PSScriptRoot/.agent_data"
)
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
. "$PSScriptRoot/scripts/Common.ps1"
$binary = Resolve-PilotBinary $PSScriptRoot $BuildDir $Executable
$arguments = @("--workbench", $Root, "--port", "$Port", "--state", $StateDir)
if ($Model) { $arguments += @("--model", $Model) }
if ($Resume) { $arguments += @("--resume", $Resume) }
if ($Execute) { $arguments += @("--exec", $Execute) }
& $binary @arguments
exit $LASTEXITCODE
