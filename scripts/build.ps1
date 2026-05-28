$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Sources = @(
  (Join-Path $Root "src/main.cpp"),
  (Join-Path $Root "src/dataset.cpp"),
  (Join-Path $Root "src/brute_force.cpp"),
  (Join-Path $Root "src/dynamic_index.cpp"),
  (Join-Path $Root "src/graph_index.cpp"),
  (Join-Path $Root "src/metrics.cpp")
)

$Output = Join-Path $BuildDir "agentmem_flow.exe"
g++ -std=c++17 -O3 -Wall -Wextra -pedantic -I (Join-Path $Root "include") @Sources -o $Output
if ($LASTEXITCODE -ne 0) {
  throw "g++ failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Output"
