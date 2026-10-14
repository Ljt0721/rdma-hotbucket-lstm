$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$entityStore = Join-Path $repoRoot "data\entity-store\entities-100k.hbkv"

$env:ENTITY_STORE_PATH = $entityStore
$env:ENTITY_STORE_INITIAL_COUNT = "100000"

Write-Output "Entity store: $entityStore"
Write-Output "Target entity count: $($env:ENTITY_STORE_INITIAL_COUNT)"

& (Join-Path $PSScriptRoot "start_dashboard.ps1")
