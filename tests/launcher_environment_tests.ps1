[CmdletBinding()]
param([string]$ProjectRoot)

$ErrorActionPreference = 'Stop'
if (!$ProjectRoot) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
. (Join-Path $ProjectRoot 'scripts\server-common.ps1')

# The fixture values are made up. This suite never reads a real .env or DOCX.
$fixtureDirectory = Join-Path ([IO.Path]::GetTempPath()) ('alpha-env-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
$fixturePath = Join-Path $fixtureDirectory 'fixture.env'
$utf8 = New-Object Text.UTF8Encoding($false)
$script:checks = 0

function Assert-AlphaFixture {
    param([bool]$Condition, [string]$Name)
    if (!$Condition) { throw "Fixture assertion failed: $Name" }
    $script:checks++
}

function Set-AlphaFixture {
    param([string]$Text)
    [IO.File]::WriteAllText($fixturePath, $Text, $utf8)
}

function Assert-AlphaFixtureFailure {
    param([string]$Text, [string]$ExpectedMessage, [string]$Name)
    Set-AlphaFixture $Text
    $failure = $null
    try { $null = Read-AlphaEnvironment $fixturePath } catch { $failure = $_.Exception.Message }
    Assert-AlphaFixture ($null -ne $failure) ($Name + ': rejected')
    Assert-AlphaFixture ($failure -like $ExpectedMessage) ($Name + ': diagnostic')
    Assert-AlphaFixture (!$failure.Contains('DUMMY_PRIVATE_VALUE')) ($Name + ': no value in diagnostic')
}

$originalToken = [Environment]::GetEnvironmentVariable('ALFA_API_TOKEN', 'Process')
try {
    $missing = Read-AlphaEnvironment (Join-Path $fixtureDirectory 'missing.env')
    Assert-AlphaFixture ($missing -is [hashtable] -and $missing.Count -eq 0) 'Missing file returns empty settings'

    Set-AlphaFixture @'
# Dummy fixtures only; no credentials from project files.

ALFA_PORT=8099
ALFA_HOST = 127.0.0.1
ALFA_DATA_DIR='data with spaces/input'
export ALFA_LLM_MODEL="model=value"
ALFA_API_TOKEN="DUMMY_PRIVATE_VALUE_=$literal;`text#inside"
ALFA_LLM_API_KEY=''
ALFA_AGENT_SELF_CHECK=0
UNRELATED_SETTING=DUMMY_PRIVATE_VALUE_ignored
OPENAI_API_KEY=DUMMY_PRIVATE_VALUE_ignored
'@
    [Environment]::SetEnvironmentVariable('ALFA_API_TOKEN', 'DUMMY_PROCESS_SENTINEL', 'Process')
    $values = Read-AlphaEnvironment $fixturePath
    Assert-AlphaFixture ($values.Count -eq 7) 'Only allowed variables accepted'
    Assert-AlphaFixture ($values['ALFA_PORT'] -ceq '8099') 'Simple assignment'
    Assert-AlphaFixture ($values['ALFA_HOST'] -ceq '127.0.0.1') 'Assignment whitespace'
    Assert-AlphaFixture ($values['ALFA_DATA_DIR'] -ceq 'data with spaces/input') 'Single quoted spaces'
    Assert-AlphaFixture ($values['ALFA_LLM_MODEL'] -ceq 'model=value') 'Export and equals in value'
    Assert-AlphaFixture ($values['ALFA_API_TOKEN'] -ceq 'DUMMY_PRIVATE_VALUE_=$literal;`text#inside') 'Value parsed without expansion or execution'
    Assert-AlphaFixture ($values.ContainsKey('ALFA_LLM_API_KEY') -and $values['ALFA_LLM_API_KEY'] -ceq '') 'Explicit empty value preserved'
    Assert-AlphaFixture ($values['ALFA_AGENT_SELF_CHECK'] -ceq '0') 'Boolean-like value remains string'
    Assert-AlphaFixture (!$values.ContainsKey('UNRELATED_SETTING') -and !$values.ContainsKey('OPENAI_API_KEY')) 'Unrelated environment ignored'
    Assert-AlphaFixture ([Environment]::GetEnvironmentVariable('ALFA_API_TOKEN', 'Process') -ceq 'DUMMY_PROCESS_SENTINEL') 'Reading settings does not mutate environment'

    Set-AlphaFixture "ALFA_PORT=8081`r`nALFA_PORT=8082`r`n"
    Assert-AlphaFixture ((Read-AlphaEnvironment $fixturePath)['ALFA_PORT'] -ceq '8082') 'Duplicate assignment uses last value'
    Set-AlphaFixture 'ALFA_API_TOKEN=DUMMY_PRIVATE_VALUE#literal'
    Assert-AlphaFixture ((Read-AlphaEnvironment $fixturePath)['ALFA_API_TOKEN'] -ceq 'DUMMY_PRIVATE_VALUE#literal') 'Hash in unquoted value remains literal'

    Assert-AlphaFixtureFailure 'ALFA_API_TOKEN' '*assignment at line 1*' 'Missing equals'
    Assert-AlphaFixtureFailure 'ALFA_LLM_MODLE=DUMMY_PRIVATE_VALUE' '*Unsupported ALFA setting at line 1*' 'Misspelled ALFA setting'
    Assert-AlphaFixtureFailure 'ALFA_API_TOKEN="DUMMY_PRIVATE_VALUE' '*Unclosed quoted environment value at line 1*' 'Unclosed quote'
    Assert-AlphaFixtureFailure "ALFA_API_TOKEN='DUMMY_PRIVATE_VALUE' # trailing comment" '*Unclosed quoted environment value at line 1*' 'Unsupported trailing quoted comment'

    Write-Host ("PASS: " + $script:checks + ' environment parser fixture checks (dummy values only).')
} finally {
    [Environment]::SetEnvironmentVariable('ALFA_API_TOKEN', $originalToken, 'Process')
    if (Test-Path -LiteralPath $fixturePath) { Remove-Item -LiteralPath $fixturePath }
    # Nonrecursive removal affects only the newly created, now empty fixture directory.
    if (Test-Path -LiteralPath $fixtureDirectory) { Remove-Item -LiteralPath $fixtureDirectory }
}
