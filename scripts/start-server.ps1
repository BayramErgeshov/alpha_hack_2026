[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$EnvironmentFile = '.env',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$BindAddress,
    [ValidateRange(1,65535)][int]$Port = 8080,
    [ValidateRange(5,300)][int]$ReadyTimeoutSeconds = 60,
    [switch]$Background,
    [switch]$NoLlm
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'server-common.ps1')
if (!$ProjectRoot) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$root = (Get-Item -LiteralPath $ProjectRoot -ErrorAction Stop).FullName
$environmentPath = Resolve-AlphaPath $root $EnvironmentFile
if ($PSBoundParameters.ContainsKey('EnvironmentFile') -and !(Test-Path -LiteralPath $environmentPath -PathType Leaf)) {
    throw 'The explicitly selected environment file does not exist.'
}
$settings = Read-AlphaEnvironment $environmentPath
foreach ($name in @('ALFA_HOST','ALFA_PORT','ALFA_DATA_DIR','ALFA_ANALYTICS_DIR','ALFA_API_TOKEN',
    'ALFA_LLM_API_KEY','ALFA_LLM_BASE_URL','ALFA_LLM_MODEL','ALFA_LLM_TIMEOUT_SECONDS','ALFA_AGENT_SELF_CHECK',
        'ALFA_LLM_MODEL_FAST','ALFA_LLM_MODEL_REVIEW','ALFA_LLM_DAILY_TOKEN_LIMIT','ALFA_NEWS_SEED','ALFA_WORK_DIR',
        'ALFA_STATIC_DIR','ALFA_CORS_ORIGIN','ALFA_NEWS_REFRESH_MINUTES')) {
    if (!$settings.ContainsKey($name)) {
        $inherited = [Environment]::GetEnvironmentVariable($name, 'Process')
        if ($null -ne $inherited) { $settings[$name] = $inherited }
    }
}
if ($PSBoundParameters.ContainsKey('Port')) { $settings['ALFA_PORT'] = $Port.ToString() }
if (!$settings.ContainsKey('ALFA_PORT') -or !$settings['ALFA_PORT']) { $settings['ALFA_PORT'] = '8080' }
$parsedPort = 0
if (![int]::TryParse($settings['ALFA_PORT'], [ref]$parsedPort) -or $parsedPort -lt 1 -or $parsedPort -gt 65535) {
    throw 'ALFA_PORT must be an integer from 1 to 65535.'
}
$Port = $parsedPort
if ($PSBoundParameters.ContainsKey('BindAddress')) { $settings['ALFA_HOST'] = $BindAddress }
if (!$settings.ContainsKey('ALFA_HOST') -or !$settings['ALFA_HOST']) { $settings['ALFA_HOST'] = '127.0.0.1' }
$ip = $null
if (![Net.IPAddress]::TryParse($settings['ALFA_HOST'], [ref]$ip) -or
    $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'ALFA_HOST / -BindAddress must be an IPv4 address.'
}
foreach ($pair in @(@('ALFA_DATA_DIR','data\input'), @('ALFA_ANALYTICS_DIR','data\analytics'))) {
    if (!$settings.ContainsKey($pair[0]) -or !$settings[$pair[0]]) { $settings[$pair[0]] = $pair[1] }
    $settings[$pair[0]] = Resolve-AlphaPath $root $settings[$pair[0]]
}
foreach ($file in @('df_Uid_data_hack.csv','df_Uid_payment_hack.csv','df_Fid_Uid_link_hack.csv')) {
    if (!(Test-Path -LiteralPath (Join-Path $settings['ALFA_DATA_DIR'] $file) -PathType Leaf)) {
        throw "Required dataset is missing: $file. Run python scripts/prepare_data.py . --data-only from the project root."
    }
}
if (!(Test-Path -LiteralPath $settings['ALFA_ANALYTICS_DIR'] -PathType Container)) {
    Write-Warning 'Analytics directory is missing; bank evidence will be unavailable until data preparation and restart.'
}
$candidates = @((Join-Path $root "x64\$Configuration\AlphaBackend.exe"),
    (Join-Path $root "build\$Configuration\alpha_backend.exe"), (Join-Path $root 'build\alpha_backend.exe'))
$executable = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (!$executable) { throw 'Backend executable is missing. Build AlphaBackend.sln (Release | x64), then retry.' }
$statePath = Get-AlphaStatePath $root $Port
if (Test-Path -LiteralPath $statePath -PathType Leaf) {
    $state = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json
    $existing = Get-AlphaManagedProcess $state $root
    if ($null -ne $existing) { throw "A managed backend is already running on port $Port. Stop it with scripts/stop-server.ps1 -Port $Port." }
    Remove-Item -LiteralPath $statePath
}
$listener = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners() |
    Where-Object { $_.Port -eq $Port } | Select-Object -First 1
if ($listener) { throw "Port $Port is already in use. Choose -Port with a free port or stop the original server." }
# The launcher uses only the dedicated gateway credential, not an unrelated key
# inherited from another OpenAI-compatible application.
$settings['OPENAI_API_KEY'] = ''
if ($NoLlm) { $settings['ALFA_LLM_API_KEY'] = '' }
$savedEnvironment = @{}
foreach ($name in $settings.Keys) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$probeHost = if ($settings['ALFA_HOST'] -eq '0.0.0.0') { '127.0.0.1' } else { $settings['ALFA_HOST'] }
$baseUrl = "http://${probeHost}:$Port"
try {
    foreach ($name in $settings.Keys) { [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process') }
    if (!$Background) {
        Write-Host "Starting Alpha backend: $baseUrl"
        Write-Host 'Initial dataset loading can take several seconds. Stop: Ctrl+C.'
        if ($NoLlm) { Write-Host 'LLM calls are disabled for this launch.' }
        Push-Location -LiteralPath $root
        try {
            & $executable
            if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 130 -and $LASTEXITCODE -ne -1073741510) {
                throw "Backend exited with code $LASTEXITCODE."
            }
        } finally { Pop-Location }
        return
    }
    $logDirectory = Split-Path -Parent $statePath
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $stdout = Join-Path $logDirectory "server-$Port.stdout.log"
    $stderr = Join-Path $logDirectory "server-$Port.stderr.log"
    $process = $null
    try {
        $process = Start-Process -FilePath $executable -WorkingDirectory $root -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $state = @{
            pid = $process.Id; executable = (ConvertTo-AlphaPortablePath $root $executable)
            start_time_utc_ticks = $process.StartTime.ToUniversalTime().Ticks.ToString()
            host = $settings['ALFA_HOST']; port = $Port
            stdout_log = (ConvertTo-AlphaPortablePath $root $stdout)
            stderr_log = (ConvertTo-AlphaPortablePath $root $stderr)
        }
        [IO.File]::WriteAllText($statePath, ($state | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
        $ready = $false
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($watch.Elapsed.TotalSeconds -lt $ReadyTimeoutSeconds) {
            $process.Refresh()
            if ($process.HasExited) { throw "Backend exited during startup (code $($process.ExitCode)); see $stderr" }
            try {
                $health = Invoke-RestMethod -Uri ($baseUrl + '/health') -TimeoutSec 2
                if ($health.status -eq 'ok' -and $health.service -eq 'alpha-news-backend') { $ready = $true; break }
            } catch { Start-Sleep -Milliseconds 250 }
        }
        if (!$ready) { throw "Backend readiness timed out; see $stderr" }
        Write-Host "Server ready: $baseUrl (PID $($process.Id))"
        Write-Host ('Logs: ' + (ConvertTo-AlphaPortablePath $root $logDirectory))
        Write-Host "Stop: .\scripts\stop-server.ps1 -Port $Port"
    } catch {
        if ($null -ne $process -and !$process.HasExited) { Stop-Process -InputObject $process -Force }
        if (Test-Path -LiteralPath $statePath) { Remove-Item -LiteralPath $statePath }
        throw
    }
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
