param(
    [Parameter(Mandatory = $true)][string]$ExePath,
    [string]$OutDir  = ".\test-results",
    [int]$Timeout    = 30,
    [switch]$SkipJina,
    [switch]$SkipRemote,
    [int]$RemoteDelaySec = 1,
    [string]$JinaApiKey = "",
    [switch]$FailFast
)

$ErrorActionPreference = "Continue"

if (-not (Test-Path $ExePath)) {
    Write-Host "ERROR: exe not found: $ExePath" -ForegroundColor Red
    exit 1
}

# Check for curl.exe (needed for remote mode)
$curlCmd = Get-Command curl.exe -ErrorAction SilentlyContinue
$remoteAvailable = $curlCmd -and -not $SkipRemote
if (-not $SkipRemote -and -not $curlCmd) {
    Write-Host "WARNING: curl.exe not found, remote mode disabled" -ForegroundColor Yellow
    $remoteAvailable = $false
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$urls = @(
    "https://ru.wikipedia.org/wiki/Web_scraping",
    "https://en.wikipedia.org/wiki/HTTP",
    "https://news.ycombinator.com/",
    "https://habr.com/ru/articles/",
    "https://lenta.ru/",
    "https://stackoverflow.com/questions",
    "https://github.com/microsoft/vcpkg",
    "https://docs.github.com/en/get-started",
    "https://www.rfc-editor.org/rfc/rfc2616",
    "https://go.dev/blog/",
    "https://medium.com/",
    "https://www.reddit.com/r/programming/",
    "https://example.com"
)

$results = @()

foreach ($url in $urls) {
    $safe = ($url -replace '^https?://', '' -replace '[^\w\.\-]', '_')
    Write-Host "`n=== $url ===" -ForegroundColor Cyan

    $modes = @()
    $modes += "raw"
    if (-not $SkipJina) { $modes += "jina" }
    if ($remoteAvailable) { $modes += "remote" }

    foreach ($mode in $modes) {
        $outFile = Join-Path $OutDir "$safe.$mode.md"
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $exitCode = -1
        $status = ""

        if ($mode -eq "remote") {
            # --- Remote: call real r.jina.ai via curl.exe ---
            if ($RemoteDelaySec -gt 0) { Start-Sleep -Seconds $RemoteDelaySec }

            $remoteUrl = "https://r.jina.ai/$url"
            $curlArgs = @("-sS", "-L", "-f", $remoteUrl, "-o", $outFile)
            if ($JinaApiKey) {
                $curlArgs = @("-H", "Authorization: Bearer $JinaApiKey") + $curlArgs
            }

            $errFile = Join-Path $OutDir "$safe.remote.err.txt"
            $proc = Start-Process -FilePath "curl.exe" -ArgumentList $curlArgs `
                                  -NoNewWindow -Wait -PassThru `
                                  -RedirectStandardError $errFile
            $exitCode = $proc.ExitCode
            $sw.Stop()

            if ($exitCode -eq 0) {
                $status = "OK"
            } else {
                $status = "REMOTE-ERR ($exitCode)"
                if (Test-Path $errFile) {
                    $errText = Get-Content $errFile -Raw -ErrorAction SilentlyContinue
                    if ($errText) {
                        Write-Host "    curl stderr: $($errText.Trim())" -ForegroundColor DarkRed
                    }
                }
            }
        } else {
            # --- Local: Parser.exe (raw or --jina) ---
            $argList = @($url, "-t", "$Timeout", "-o", $outFile)
            if ($mode -eq "jina") { $argList += "--jina" }

            try {
                $p = Start-Process -FilePath $ExePath -ArgumentList $argList `
                                   -NoNewWindow -Wait -PassThru -ErrorAction Stop
                $exitCode = $p.ExitCode
            } catch {
                Write-Host "  launch error: $($_.Exception.Message)" -ForegroundColor Red
            }
            $sw.Stop()

            $status = switch ($exitCode) {
                0       { "OK" }
                1       { "USAGE" }
                2       { "FETCH-ERR" }
                3       { "WRITE-ERR" }
                default { "CODE $exitCode" }
            }
        }

        $size = if (Test-Path $outFile) { (Get-Item $outFile).Length } else { 0 }
        $color = if ($exitCode -eq 0) { "Green" } else { "Red" }
        Write-Host ("  {0,-7} {1,-16} {2,8} B  {3,6} ms" -f `
                    $mode, $status, $size, $sw.ElapsedMilliseconds) -ForegroundColor $color

        $results += [PSCustomObject]@{
            URL      = $url
            Mode     = $mode
            Status   = $status
            ExitCode = $exitCode
            Size     = $size
            Ms       = $sw.ElapsedMilliseconds
            File     = $outFile
        }

        if ($FailFast -and $exitCode -ne 0 -and $mode -ne "remote") { return }
    }
}

# ---- Summary ----
Write-Host "`n=== TOTAL ===" -ForegroundColor Yellow
$results | Format-Table URL, Mode, Status, Size, Ms -AutoSize

$csvPath = Join-Path $OutDir "summary.csv"
$results | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8
Write-Host "CSV: $csvPath" -ForegroundColor Green

$ok   = @($results | Where-Object ExitCode -eq 0)
$fail = @($results | Where-Object ExitCode -ne 0)
Write-Host "OK: $($ok.Count) / $($results.Count)" -ForegroundColor Green

if ($fail.Count -gt 0) {
    Write-Host "`nFailures:" -ForegroundColor Red
    $fail | Format-Table URL, Mode, Status -AutoSize
}

# ---- Size comparison per URL (raw vs jina vs remote) ----
if (-not $SkipRemote) {
    Write-Host "`n=== SIZE COMPARISON (bytes) ===" -ForegroundColor Yellow
    $grouped = $results | Group-Object URL
    $cmp = foreach ($g in $grouped) {
        $raw    = ($g.Group | Where-Object Mode -eq 'raw'    | Select-Object -First 1).Size
        $jina   = ($g.Group | Where-Object Mode -eq 'jina'   | Select-Object -First 1).Size
        $remote = ($g.Group | Where-Object Mode -eq 'remote' | Select-Object -First 1).Size
        $diff   = if ($jina -and $remote) { "{0:P1}" -f (($jina - $remote) / [Math]::Max($remote,1)) } else { "" }
        [PSCustomObject]@{
            URL        = $g.Name
            Raw        = $raw
            LocalJina  = $jina
            RemoteJina = $remote
            DiffLocal  = $diff
        }
    }
    $cmp | Format-Table -AutoSize
}
