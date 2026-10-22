[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [ValidateRange(1,65535)][int]$Port = 18080,
    [switch]$IncludeWebExample,
    [switch]$KeepServer
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (!$ProjectRoot) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$root = (Get-Item -LiteralPath $ProjectRoot -ErrorAction Stop).FullName
$exampleDirectory = Join-Path $root 'example'
$baseUrl = "http://127.0.0.1:$Port"
$candidates = @((Join-Path $exampleDirectory 'bin\AlphaExampleClient.exe'),
    (Join-Path $exampleDirectory 'build\AlphaExampleClient.exe'),
    (Join-Path $root 'build\Release\AlphaExampleClient.exe'),
    (Join-Path $root 'build\AlphaExampleClient.exe'))
$client = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (!$client) { throw 'Example client is missing. Build AlphaBackend.sln (Release | x64), including AlphaExampleClient.' }

$outputRoot = Join-Path $exampleDirectory 'output'
$runName = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$runDirectory = Join-Path $outputRoot $runName
$null = New-Item -ItemType Directory -Path $runDirectory -Force
$report = [ordered]@{
    run_directory = (ConvertTo-AlphaPortablePath $root $runDirectory)
    summary_file = (ConvertTo-AlphaPortablePath $root (Join-Path $runDirectory 'summary.json'))
    server_url = $baseUrl
    port = $Port
    started_utc = [DateTime]::UtcNow.ToString('o')
    finished_utc = $null
    completed = $false
    server_kept_running = $false
    include_web_example = [bool]$IncludeWebExample
    external_llm_enabled = $false
}
$startedHere = $false
$serverAvailable = $false
$failure = $null
$savedToken = [Environment]::GetEnvironmentVariable('ALFA_EXAMPLE_API_TOKEN', 'Process')
try {
    $state = Get-ExampleServer $root $Port
    if ($null -eq $state) {
        & (Join-Path $root 'scripts\start-server.ps1') -ProjectRoot $root `
            -EnvironmentFile (Join-Path $exampleDirectory 'service.env') -Port $Port `
            -BindAddress '127.0.0.1' -NoLlm -Background
        $startedHere = $true
        $statePath = Get-AlphaStatePath $root $Port
        $state = $null
        try {
            $state = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json
            $process = Get-AlphaManagedProcess $state $root
            if ($null -eq $process) { throw 'The example server exited after startup.' }
            $markerPath = Get-ExampleMarkerPath $root $Port
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $markerPath) -Force
            [IO.File]::WriteAllText($markerPath, ($state | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
        } catch {
            # State was just obtained from this launcher. Stop only this exact process.
            if ($null -ne $state) {
                $current = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json
                if ([string]$current.pid -eq [string]$state.pid -and
                    [string]$current.start_time_utc_ticks -eq [string]$state.start_time_utc_ticks) {
                    & (Join-Path $root 'scripts\stop-server.ps1') -ProjectRoot $root -Port $Port
                }
            }
            throw
        }
    } else { Write-Host "Reusing the example server: $baseUrl" }
    $serverAvailable = $true
    [Environment]::SetEnvironmentVariable('ALFA_EXAMPLE_API_TOKEN', '', 'Process')
    $clientArguments = @('--base-url', $baseUrl, '--output-dir', $runDirectory,
        '--requests-dir', (Join-Path $exampleDirectory 'requests'))
    if ($IncludeWebExample) { $clientArguments += '--include-web' }
    & $client @clientArguments
    if ($LASTEXITCODE -ne 0) { throw "Example client exited with code $LASTEXITCODE. See $($report.summary_file)" }
    $report.completed = $true
} catch { $failure = $_ }
finally {
    [Environment]::SetEnvironmentVariable('ALFA_EXAMPLE_API_TOKEN', $savedToken, 'Process')
    if ($serverAvailable -and (!$KeepServer -or ($null -ne $failure -and $startedHere))) {
        try { Stop-ExampleServer $root $Port; $serverAvailable = $false }
        catch { if ($null -eq $failure) { $failure = $_ } else { Write-Warning $_.Exception.Message } }
    }
    $report.server_kept_running = $serverAvailable
    $report.finished_utc = [DateTime]::UtcNow.ToString('o')
    $index = $report | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $runDirectory 'run.json'), $index, (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $outputRoot 'latest.json'), $index, (New-Object Text.UTF8Encoding($false)))
}
Write-Host "Responses: $($report.run_directory)"
if ($serverAvailable) {
    Write-Host "Example service is available: $baseUrl"
    Write-Host "Stop: .\example\stop.cmd -Port $Port"
}
if ($null -ne $failure) { throw $failure }
