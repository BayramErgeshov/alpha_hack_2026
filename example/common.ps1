Set-StrictMode -Version Latest
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\server-common.ps1')

function Get-ExampleMarkerPath {
    param([string]$Root, [int]$Port)
    return Join-Path $Root ('work\example\server-' + $Port + '.json')
}

function Get-ExampleServer {
    param([string]$Root, [int]$Port)
    $markerPath = Get-ExampleMarkerPath $Root $Port
    if (!(Test-Path -LiteralPath $markerPath -PathType Leaf)) { return $null }
    $marker = [IO.File]::ReadAllText($markerPath) | ConvertFrom-Json
    $ownedProcess = Get-AlphaManagedProcess $marker $Root
    if ($null -eq $ownedProcess) {
        Remove-Item -LiteralPath $markerPath
        return $null
    }
    $statePath = Get-AlphaStatePath $Root $Port
    if (!(Test-Path -LiteralPath $statePath -PathType Leaf)) {
        throw 'Example ownership exists, but server state is missing. Inspect work/example and work/server.'
    }
    $state = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json
    if ([string]$state.pid -ne [string]$marker.pid -or
        [string]$state.start_time_utc_ticks -ne [string]$marker.start_time_utc_ticks -or
        ![string]::Equals((Resolve-AlphaPath $Root ([string]$state.executable)),
            (Resolve-AlphaPath $Root ([string]$marker.executable)), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The port belongs to a different managed instance. Choose another -Port; no process was stopped.'
    }
    $null = Get-AlphaManagedProcess $state $Root
    if ([string]$state.host -ne '127.0.0.1' -or [int]$state.port -ne $Port) {
        throw 'Unexpected example server address. Inspect the saved state.'
    }
    return $state
}

function Stop-ExampleServer {
    param([string]$Root, [int]$Port)
    $state = Get-ExampleServer $Root $Port
    if ($null -eq $state) {
        Write-Host "No example server is running on port $Port."
        return
    }
    & (Join-Path $Root 'scripts\stop-server.ps1') -ProjectRoot $Root -Port $Port
    $markerPath = Get-ExampleMarkerPath $Root $Port
    if (Test-Path -LiteralPath $markerPath) { Remove-Item -LiteralPath $markerPath }
}
