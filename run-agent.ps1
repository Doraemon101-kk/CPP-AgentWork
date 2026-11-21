param(
    [string]$Question = "看看这个项目的代码，告诉我入口在哪里，哪些函数还没实现。",
    [string]$Root = "$PSScriptRoot/fixtures/sample_project",
    [string]$Model = "qwen3:1.7b",
    [ValidateRange(1,65535)][int]$Port = 11434,
    [string]$BuildDir = "build",
    [string]$Executable = ""
)
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
. "$PSScriptRoot/scripts/Common.ps1"
$binary = Resolve-PilotBinary $PSScriptRoot $BuildDir $Executable
& $binary --agent $Root $Question $Model $Port
exit $LASTEXITCODE
