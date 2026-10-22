[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [ValidateRange(1,65535)][int]$Port = 18080
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (!$ProjectRoot) { $ProjectRoot = Split-Path -Parent $PSScriptRoot }
$root = (Get-Item -LiteralPath $ProjectRoot -ErrorAction Stop).FullName
Stop-ExampleServer $root $Port
