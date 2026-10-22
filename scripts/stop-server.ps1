[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [ValidateRange(1,65535)][int]$Port = 8080
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'server-common.ps1')
if (!$ProjectRoot) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$root = (Get-Item -LiteralPath $ProjectRoot -ErrorAction Stop).FullName
$statePath = Get-AlphaStatePath $root $Port
if (!(Test-Path -LiteralPath $statePath -PathType Leaf)) {
    Write-Host "No managed background server is recorded for port $Port. Foreground launch: stop with Ctrl+C."
    return
}
$state = [IO.File]::ReadAllText($statePath) | ConvertFrom-Json
$process = Get-AlphaManagedProcess $state $root
if ($null -ne $process) {
    Stop-Process -InputObject $process -Force
    Write-Host "Stopped Alpha backend on port $Port (PID $($process.Id))."
} else { Write-Host "Server on port $Port was already stopped; removing stale state." }
Remove-Item -LiteralPath $statePath
