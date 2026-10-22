[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [ValidateRange(1,65535)][int]$Port = 18082,
    [switch]$WriteExamples
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$ProjectRoot) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$root = (Get-Item -LiteralPath $ProjectRoot).FullName
$script:checks = 0
function Check([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw "FAIL: $Message" }
    $script:checks++
}
function ErrorStatus([scriptblock]$Request) {
    try { & $Request | Out-Null; return 200 }
    catch {
        if ($null -eq $_.Exception.Response) { throw }
        return [int]$_.Exception.Response.StatusCode
    }
}
function SaveExample([string]$Name, $Value) {
    if ($WriteExamples) {
        $path = Join-Path $root ('docs\examples\' + $Name)
        [IO.File]::WriteAllText($path, ($Value | ConvertTo-Json -Depth 40), (New-Object Text.UTF8Encoding($false)))
    }
}
$testRoot = Join-Path $root 'work\launcher-tests'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$fixture = Join-Path $testRoot ('config-' + [Guid]::NewGuid().ToString('N') + '.env')
$token = 'local-fixture-token-not-a-real-secret'
[IO.File]::WriteAllText($fixture, "ALFA_API_TOKEN=$token`nALFA_LLM_API_KEY=dummy-disabled-by-NoLlm`nALFA_HOST=127.0.0.1`nALFA_DATA_DIR=data/input`nALFA_ANALYTICS_DIR=data/analytics`n", (New-Object Text.UTF8Encoding($false)))
$oldGatewayKey = [Environment]::GetEnvironmentVariable('ALFA_LLM_API_KEY', 'Process')
$oldOtherKey = [Environment]::GetEnvironmentVariable('OPENAI_API_KEY', 'Process')
$started = $false
$base = "http://127.0.0.1:$Port"
$headers = @{ Authorization = "Bearer $token" }
try {
    [Environment]::SetEnvironmentVariable('ALFA_LLM_API_KEY','parent-fixture-gateway','Process')
    [Environment]::SetEnvironmentVariable('OPENAI_API_KEY','parent-fixture-other','Process')
    & (Join-Path $root 'scripts\start-server.ps1') -ProjectRoot $root -EnvironmentFile $fixture -NoLlm -Background -Port $Port
    $started = $true
    Check ([Environment]::GetEnvironmentVariable('ALFA_LLM_API_KEY','Process') -eq 'parent-fixture-gateway') 'launcher restores parent gateway key'
    Check ([Environment]::GetEnvironmentVariable('OPENAI_API_KEY','Process') -eq 'parent-fixture-other') 'launcher restores parent alias key'
    $health = Invoke-RestMethod ($base + '/health') -TimeoutSec 5
    Check ($health.status -eq 'ok') 'background server is ready'
    Check ((ErrorStatus { Invoke-RestMethod ($base + '/api/v1/models') -TimeoutSec 5 }) -eq 401) 'API rejects missing bearer token'
    $models = Invoke-RestMethod ($base + '/api/v1/models') -Headers $headers -TimeoutSec 5
    Check (!$models.configured) 'NoLlm excludes both gateway and alias credentials'
    $company = Invoke-RestMethod ($base + '/api/v1/companies/U_9250') -Headers $headers -TimeoutSec 5
    Check ($company.id -eq 'U_9250') 'company profile contract'
    SaveExample 'company-response.json' $company
    $query = '%D0%B8%D0%BD%D1%84%D0%BB%D1%8F%D1%86%D0%B8%D1%8F'
    $analytics = Invoke-RestMethod ($base + '/api/v1/analytics/search?q=' + $query + '&limit=3') -Headers $headers -TimeoutSec 5
    Check (@($analytics.items).Count -gt 0) 'analytics search returns evidence'
    foreach ($item in $analytics.items) { Check ($item.page -gt 0 -and $item.text_extraction -eq 'windows_ocr') 'evidence retains source page and OCR origin' }
    SaveExample 'analytics-search-response.json' $analytics
    $body = [IO.File]::ReadAllText((Join-Path $root 'docs\examples\demo-news.json'), [Text.Encoding]::UTF8)
    $insight = Invoke-RestMethod ($base + '/api/v1/insights') -Headers $headers -Method Post -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body)) -TimeoutSec 15
    Check ($insight.generation.method -eq 'deterministic_fallback') 'offline insight uses deterministic fallback'
    $sourceIds = @($insight.sources | ForEach-Object { $_.id })
    Check ($sourceIds -contains 'N1' -and $sourceIds -contains 'C1') 'news and company sources exist'
    foreach ($claim in $insight.interpretation.model_conclusions) {
        foreach ($reference in $claim.evidence_ids) { Check ($sourceIds -contains $reference) 'claim reference resolves to a source' }
    }
    SaveExample 'insight-response.json' $insight
    $followPath = $base + '/api/v1/insights/' + $insight.id + '/questions'
    Check ((ErrorStatus { Invoke-RestMethod $followPath -Headers $headers -Method Post -ContentType 'application/json' -Body '{"question":""}' -TimeoutSec 5 }) -eq 400) 'empty follow-up question is HTTP 400'
    Check ((ErrorStatus { Invoke-RestMethod ($base + '/api/v1/insights/missing/questions') -Headers $headers -Method Post -ContentType 'application/json' -Body '{"question":"Explain"}' -TimeoutSec 5 }) -eq 404) 'unknown insight is HTTP 404'
    $follow = Invoke-RestMethod $followPath -Headers $headers -Method Post -ContentType 'application/json' -Body '{"question":"Explain the available sources"}' -TimeoutSec 5
    Check ($follow.method -eq 'deterministic_fallback' -and ![string]::IsNullOrEmpty($follow.answer)) 'offline follow-up has a nonempty answer'
    SaveExample 'followup-response.json' $follow
    $duplicateRejected = $false
    try { & (Join-Path $root 'scripts\start-server.ps1') -ProjectRoot $root -EnvironmentFile $fixture -NoLlm -Background -Port $Port }
    catch { $duplicateRejected = $true }
    Check $duplicateRejected 'duplicate managed startup is rejected'
    & (Join-Path $root 'scripts\stop-server.ps1') -ProjectRoot $root -Port $Port
    $started = $false
    Check (!(Test-Path -LiteralPath (Join-Path $root "work\server\server-$Port.json"))) 'stop removes managed state'
    $closed = $false
    try { Invoke-RestMethod ($base + '/health') -TimeoutSec 2 | Out-Null }
    catch { $closed = $true }
    Check $closed 'stop closes server port'
    [IO.File]::WriteAllText((Join-Path $testRoot 'latest-result.json'), (@{passed=$true; checks=$script:checks; external_llm_calls=0} | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    Write-Output ("PASS: $script:checks local server lifecycle and API checks; no LLM calls.")
} finally {
    if ($started) { & (Join-Path $root 'scripts\stop-server.ps1') -ProjectRoot $root -Port $Port }
    [Environment]::SetEnvironmentVariable('ALFA_LLM_API_KEY',$oldGatewayKey,'Process')
    [Environment]::SetEnvironmentVariable('OPENAI_API_KEY',$oldOtherKey,'Process')
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture }
}
