<#
.SYNOPSIS
  Interleaved A/B: alternates scenario runs between two builds so that drift in
  the machine's state (background load, GPU/driver warm-up) hits both equally.
  Only one instance runs at a time; each switch is a clean quit + start.
#>
param(
    [Parameter(Mandatory)] [string]$ExeA,
    [Parameter(Mandatory)] [string]$ExeB,
    [string]$Scenario = 'S2',
    [int]$Pairs = 3,
    [string]$Label = 'ab'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\PerfCommon.ps1"

function Stop-All {
    foreach ($p in Get-SvmProcess) {
        [void][SvmPerf.Native]::QuitInstance($p.Id)
        if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
    }
}

for ($i = 1; $i -le $Pairs; ++$i) {
    foreach ($side in 'A', 'B') {
        $exe = if ($side -eq 'A') { $ExeA } else { $ExeB }
        Stop-All
        Write-Host "== pair $i, side $side"
        & "$PSScriptRoot\Run-Scenarios.ps1" -Exe $exe -Scenarios $Scenario -Runs 1 -Label "$Label-$side"
    }
}
