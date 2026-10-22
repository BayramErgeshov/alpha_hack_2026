Set-StrictMode -Version Latest

function Resolve-AlphaPath {
    param([string]$Root, [string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function ConvertTo-AlphaPortablePath {
    param([string]$Root, [string]$Path)
    $absolute = Resolve-AlphaPath $Root $Path
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if ($absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        return $absolute.Substring($prefix.Length).Replace('\','/')
    }
    return $absolute
}

function Read-AlphaEnvironment {
    param([string]$Path)
    $values = @{}
    $allowed = @('ALFA_HOST','ALFA_PORT','ALFA_DATA_DIR','ALFA_ANALYTICS_DIR','ALFA_API_TOKEN',
        'ALFA_LLM_API_KEY','ALFA_LLM_BASE_URL','ALFA_LLM_MODEL','ALFA_LLM_TIMEOUT_SECONDS','ALFA_AGENT_SELF_CHECK')
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $values }
    $lineNumber = 0
    foreach ($line in [IO.File]::ReadAllLines($Path, [Text.Encoding]::UTF8)) {
        $lineNumber++
        $text = $line.Trim()
        if (!$text -or $text.StartsWith('#')) { continue }
        if ($text -notmatch '^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$') {
            throw "Invalid environment-file assignment at line $lineNumber. Use NAME=value."
        }
        $name = $Matches[1]
        $value = $Matches[2].Trim()
        if ($allowed -notcontains $name) {
            if ($name.StartsWith('ALFA_')) { throw "Unsupported ALFA setting at line $lineNumber (check the variable name)." }
            continue
        }
        if ($value.StartsWith('"') -or $value.StartsWith("'")) {
            if ($value.Length -lt 2 -or $value[$value.Length - 1] -ne $value[0]) {
                throw "Unclosed quoted environment value at line $lineNumber."
            }
            $value = $value.Substring(1, $value.Length - 2)
        }
        $values[$name] = $value
    }
    return $values
}

function Get-AlphaStatePath {
    param([string]$Root, [int]$Port)
    return Join-Path $Root ('work\server\server-' + $Port + '.json')
}

function Get-AlphaManagedProcess {
    param($State, [string]$Root)
    $executable = Resolve-AlphaPath $Root ([string]$State.executable)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    if (!$executable.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        @('AlphaBackend.exe','alpha_backend.exe') -notcontains [IO.Path]::GetFileName($executable)) {
        throw 'Managed process state contains an unexpected executable. No process was stopped.'
    }
    $managed = Get-Process -Id ([int]$State.pid) -ErrorAction SilentlyContinue
    if ($null -eq $managed) { return $null }
    if (![string]::Equals($managed.Path, $executable, [StringComparison]::OrdinalIgnoreCase) -or
        $managed.StartTime.ToUniversalTime().Ticks.ToString() -ne [string]$State.start_time_utc_ticks) {
        throw 'The PID belongs to a different process. No process was stopped; inspect the saved state.'
    }
    return $managed
}
