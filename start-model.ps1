param(
    [string]$Model = "qwen3:4b-instruct-2507-q4_K_M",
    [ValidateRange(1,65535)][int]$Port = 11434,
    [string]$OllamaPath = "",
    [string]$ModelsDir = ""
)
$ErrorActionPreference = "Stop"
$endpoint = "http://127.0.0.1:$Port"
$logDir = Join-Path $PSScriptRoot ".agent_data/runtime"
$ready = $false
try { $installed = Invoke-RestMethod "$endpoint/api/tags" -TimeoutSec 3; $ready = $true } catch {}
if (!$ready) {
    if (!$OllamaPath) {
        $command = Get-Command ollama.exe -ErrorAction SilentlyContinue
        if ($command) { $OllamaPath = $command.Source }
        elseif (Test-Path "$env:LOCALAPPDATA/Programs/Ollama/ollama.exe") { $OllamaPath = "$env:LOCALAPPDATA/Programs/Ollama/ollama.exe" }
    }
    if (!$OllamaPath -or !(Test-Path -LiteralPath $OllamaPath -PathType Leaf)) { throw "Install Ollama from https://ollama.com/download/windows, or pass -OllamaPath. No server at $endpoint." }
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $oldHost = $env:OLLAMA_HOST; $oldModels = $env:OLLAMA_MODELS
    try {
        $env:OLLAMA_HOST = "127.0.0.1:$Port"
        if ($ModelsDir) { $env:OLLAMA_MODELS = [IO.Path]::GetFullPath($ModelsDir) }
        $server = Start-Process -FilePath $OllamaPath -ArgumentList "serve" -WindowStyle Hidden -PassThru -RedirectStandardOutput "$logDir/server-$Port.out.log" -RedirectStandardError "$logDir/server-$Port.err.log"
        @{ pid = $server.Id; executable = (Resolve-Path -LiteralPath $OllamaPath).Path; port = $Port } | ConvertTo-Json | Set-Content "$logDir/server-$Port.json" -Encoding UTF8
    } finally { $env:OLLAMA_HOST = $oldHost; $env:OLLAMA_MODELS = $oldModels }
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        try { $installed = Invoke-RestMethod "$endpoint/api/tags" -TimeoutSec 1; $ready = $true; break } catch {}
    }
    if (!$ready) { throw "Ollama failed to start. Inspect $logDir/server-$Port.err.log" }
}
if ($installed.models.name -notcontains $Model) {
    Write-Host "Downloading $Model (first run only)..."
    $body = @{ model = $Model; stream = $false } | ConvertTo-Json
    Invoke-RestMethod "$endpoint/api/pull" -Method Post -ContentType "application/json" -Body $body -TimeoutSec 1800 | Out-Null
}
Write-Host "Ollama ready at $endpoint; model: $Model"
