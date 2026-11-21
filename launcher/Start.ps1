param(
    [string]$Root = "",
    [string]$Model = "qwen3:4b-instruct-2507-q4_K_M",
    [string]$Execute = "",
    [ValidateRange(1,65535)][int]$Port = 11434,
    [switch]$NoGuide,
    [switch]$CheckOnly
)
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$package = Split-Path $PSScriptRoot -Parent
try {
    $binary = Join-Path $package "bin/cpp_agent.exe"
    if (!(Test-Path -LiteralPath $binary -PathType Leaf)) {
        $binary = Join-Path $package "build/bin/cpp_agent.exe"
    }
    if (!(Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw "未找到程序。请下载 Windows 便携包并完整解压；源码用户请先运行 build.ps1。"
    }
    if (!$NoGuide -and !$CheckOnly) { Start-Process (Join-Path $PSScriptRoot "guide.html") }
    Write-Host "CppPilot | 本地 C++ 项目助手" -ForegroundColor Cyan
    Write-Host "服务端口：$Port；模型：$Model"
    $endpoint = "http://127.0.0.1:$Port"
    $installed = $null
    try { $installed = Invoke-RestMethod "$endpoint/api/tags" -TimeoutSec 3 } catch {}
    $ollama = Get-Command ollama.exe -ErrorAction SilentlyContinue
    $ollamaPath = if ($ollama) { $ollama.Source } else { Join-Path $env:LOCALAPPDATA "Programs/Ollama/ollama.exe" }
    if ($CheckOnly) {
        [ordered]@{
            binary = $true
            ollamaInstalled = (Test-Path -LiteralPath $ollamaPath -PathType Leaf)
            serverReady = ($null -ne $installed)
            modelReady = ($null -ne $installed -and $installed.models.name -contains $Model)
            port = $Port
            cmakeAvailable = ($null -ne (Get-Command cmake -ErrorAction SilentlyContinue))
        } | ConvertTo-Json
        exit 0
    }
    if (!$installed) {
        if (!(Test-Path -LiteralPath $ollamaPath -PathType Leaf)) {
            Write-Host "请先安装 Ollama，安装后重新双击 Start.cmd。"
            if ((Read-Host "是否打开官方下载页？输入 Y 打开，回车退出") -eq "y") {
                Start-Process "https://ollama.com/download/windows"
            }
            exit 1
        }
        Write-Host "正在启动本地模型服务……"
        & (Join-Path $package "start-model.ps1") -Port $Port -OllamaPath $ollamaPath -SkipPull
        $installed = Invoke-RestMethod "$endpoint/api/tags" -TimeoutSec 3
    }
    if ($installed.models.name -notcontains $Model) {
        Write-Host "首次使用需下载模型（默认模型约 2.5 GB），需要联网并占用磁盘空间。"
        if ((Read-Host "输入 Y 确认下载；回车退出") -ne "y") { exit 0 }
        if (!(Test-Path -LiteralPath $ollamaPath -PathType Leaf)) {
            throw "服务已运行，但找不到 Ollama 命令。请安装 Ollama 客户端后重试。"
        }
        $previousHost = $env:OLLAMA_HOST
        try {
            $env:OLLAMA_HOST = $endpoint
            & $ollamaPath pull $Model
            if ($LASTEXITCODE -ne 0) { throw "模型下载失败，请检查网络后重试。" }
        } finally { $env:OLLAMA_HOST = $previousHost }
        $installed = Invoke-RestMethod "$endpoint/api/tags" -TimeoutSec 3
        if ($installed.models.name -notcontains $Model) { throw "下载完成后仍未检测到指定模型。" }
    }
    if (!$Root) { $Root = Read-Host "输入项目文件夹路径；直接回车体验自带演示" }
    if (!$Root) { $Root = Join-Path $package "fixtures/repair_project" }
    $Root = $Root.Trim().Trim('"')
    if (!(Test-Path -LiteralPath $Root -PathType Container)) { throw "项目文件夹不存在，请检查路径。" }
    if (!(Get-Command cmake -ErrorAction SilentlyContinue)) {
        Write-Host "未检测到 CMake：可以读代码和生成提案；编译/测试需要另装 CMake 与 C++ 编译器。" -ForegroundColor Yellow
    }
    Write-Host "先试：/ask 看看这个项目，入口和加法函数在哪里？"
    Write-Host "修改用 /edit，预览用 /review，退出用 /exit。"
    $arguments = @("--workbench", $Root, "--port", "$Port", "--model", $Model, "--state", (Join-Path $package ".agent_data"))
    if ($Execute) { $arguments += @("--exec", $Execute) }
    & $binary @arguments
    exit $LASTEXITCODE
} catch {
    Write-Host ("启动失败：" + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
