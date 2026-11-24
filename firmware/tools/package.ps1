param([switch]$IncludeS3Worker)

$ErrorActionPreference = 'Stop'

$firmware = Split-Path -Parent $PSScriptRoot
$release = Join-Path $firmware 'release'
New-Item -ItemType Directory -Path $release -Force | Out-Null

$targets = @(
    @{ Role = 'master'; Build = 'master-esp32s3'; Chip = 'esp32s3'; App = 'nexus_master' }
    @{ Role = 'worker'; Build = 'worker-esp32'; Chip = 'esp32'; App = 'nexus_worker' }
)
if ($IncludeS3Worker) {
    $targets += @{ Role = 'worker'; Build = 'worker-esp32s3'; Chip = 'esp32s3'; App = 'nexus_worker' }
}
$manifest = foreach ($target in $targets) {
    $build = Join-Path $firmware ('build/' + $target.Build)
    $image = Join-Path $build ($target.App + '.bin')
    $description = Get-Content -LiteralPath (Join-Path $build 'project_description.json') -Raw | ConvertFrom-Json
    if ($description.target -ne $target.Chip -or $description.project_name -ne $target.App) {
        throw "Wrong target or role in $build"
    }
    if (-not (Test-Path -LiteralPath $image -PathType Leaf)) { throw "Missing firmware image $image" }
    $appImage = Get-Item -LiteralPath $image
    $role = Join-Path $firmware $target.Role
    $sources = @(
        Get-ChildItem -LiteralPath (Join-Path $role 'main'), (Join-Path $firmware 'components') -Recurse -File
        Get-ChildItem -LiteralPath $role, $firmware -File |
            Where-Object { $_.Name -match '^(CMakeLists\.txt|sdkconfig\.(common|defaults)(\..+)?|partitions\.csv)$' }
    )
    $newer = $sources | Where-Object LastWriteTimeUtc -GT $appImage.LastWriteTimeUtc | Select-Object -First 1
    if ($newer) { throw "Rebuild $($target.Build) before packaging. Source is newer: $($newer.FullName)" }
    $file = 'nexus-' + $target.Build + '-flash.bin'
    $out = Join-Path $release $file
    Push-Location -LiteralPath $build
    try {
        python -m esptool --chip $target.Chip merge-bin -o $out '@flash_args' | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "esptool merge-bin failed for $($target.Build)" }
    } finally {
        Pop-Location
    }
    [ordered]@{
        image = $file
        chip = $target.Chip
        flash_offset = '0x0'
        app_bytes = $appImage.Length
        sha256 = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding utf8
