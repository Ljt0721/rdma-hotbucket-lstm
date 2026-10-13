$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$dashboardRoot = Join-Path $repoRoot "dashboard"

& (Join-Path $PSScriptRoot "build_windows.ps1")

$nodeCommand = Get-Command node -ErrorAction SilentlyContinue
$npmCommand = Get-Command npm.cmd -ErrorAction SilentlyContinue
if (-not $nodeCommand -or -not $npmCommand) {
    throw "Node.js LTS is required. Install it and open a new terminal before retrying."
}

Push-Location $dashboardRoot
try {
    if (-not (Test-Path -LiteralPath (Join-Path $dashboardRoot "node_modules"))) {
        & $npmCommand.Source install
        if ($LASTEXITCODE -ne 0) {
            throw "npm install failed"
        }
    }

    Write-Output "Dashboard: http://127.0.0.1:5173"
    & $npmCommand.Source run dev
    if ($LASTEXITCODE -ne 0) {
        throw "Dashboard process exited with code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
