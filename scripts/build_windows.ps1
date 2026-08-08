$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildDirectory = Join-Path $repoRoot "build"
$preferredCompiler = "C:\Program Files\MinGW\bin\g++.exe"

if (Test-Path -LiteralPath $preferredCompiler) {
    $compiler = $preferredCompiler
} else {
    $compiler = (Get-Command g++ -ErrorAction Stop).Source
}

[IO.Directory]::CreateDirectory($buildDirectory) | Out-Null

$commonArguments = @(
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-static",
    "-static-libgcc",
    "-static-libstdc++",
    "-I$($repoRoot)\include",
    "$($repoRoot)\src\cluster.cpp",
    "$($repoRoot)\src\policies.cpp",
    "$($repoRoot)\src\workload.cpp"
)

& $compiler @commonArguments "$($repoRoot)\src\main.cpp" -o "$buildDirectory\hotbucket_sim.exe"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build hotbucket_sim.exe"
}

& $compiler @commonArguments "$($repoRoot)\tests\test_main.cpp" -o "$buildDirectory\hotbucket_tests.exe"
if ($LASTEXITCODE -ne 0) {
    throw "Failed to build hotbucket_tests.exe"
}

Write-Output "Built: $buildDirectory\hotbucket_sim.exe"
Write-Output "Built: $buildDirectory\hotbucket_tests.exe"
