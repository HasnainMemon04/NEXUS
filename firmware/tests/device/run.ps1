$ErrorActionPreference = 'Stop'
$env:PYTHONUTF8 = '1'
$firmware = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$sums = @{}
Get-Command qemu-system-xtensa -ErrorAction Stop | Out-Null

foreach ($target in @('esp32', 'esp32s3')) {
    $build = Join-Path $firmware "build/device-$target"
    idf.py -C $PSScriptRoot -B $build "-DIDF_TARGET=$target" build
    if ($LASTEXITCODE -ne 0) { throw "$target build failed" }

    $log = Join-Path $build 'qemu-selftest.log'
    idf.py -C $PSScriptRoot -B $build "-DIDF_TARGET=$target" qemu --qemu-extra-args=-no-reboot 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $log -Pattern "^NEXUS_SELFTEST_PASS target=$target\b")) {
        throw "$target QEMU self-test failed. See $log"
    }
    $sums[$target] = (Select-String -LiteralPath $log -Pattern '^NEXUS_CHECKSUMS ').Line
}

if (!$sums['esp32'] -or $sums['esp32'] -ne $sums['esp32s3']) { throw 'Cross-chip checksum mismatch' }
Write-Output "Both emulated targets passed with matching results. $($sums['esp32'])"
