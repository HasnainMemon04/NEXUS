param([string]$Compiler = "gcc")
$ErrorActionPreference = "Stop"

$base = $PSScriptRoot
$component = Join-Path $base "../../components/nexus_core"
$stubs = Join-Path $base "stubs"
$headers = Join-Path $component "include"
foreach ($entry in @(
    @{ Unit = "queue"; Source = "job_queue.c" },
    @{ Unit = "proto"; Source = "proto.c" }
)) {
    $output = Join-Path ([IO.Path]::GetTempPath()) ("nexus-" + $entry.Unit + "-" + [Guid]::NewGuid().ToString("N") + ".exe")
    $testSource = Join-Path $base ("test_" + $entry.Unit + ".c")
    $productionSource = Join-Path $component $entry.Source
    try {
        & $Compiler -std=c11 -O2 -g -Wall -Wextra -Werror "-I$stubs" "-I$headers" $testSource $productionSource -o $output
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $($entry.Unit)." }
        & $output
        if ($LASTEXITCODE -ne 0) { throw "Test failed for $($entry.Unit)." }
    } finally {
        if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
    }
}
