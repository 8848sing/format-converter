# Copy the prebuilt PDFium Android libraries into jniLibs so Gradle packages them.
$ErrorActionPreference = 'Stop'
$androidDir = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $androidDir)
$mapping = [ordered]@{
    'arm64-v8a' = 'pdfium-android-arm64-v8a'
    'x86_64'    = 'pdfium-android-x86_64'
}
foreach ($abi in $mapping.Keys) {
    $source = Join-Path $root "cpp\third_party\$($mapping[$abi])\lib\libpdfium.so"
    if (-not (Test-Path -LiteralPath $source)) {
        throw "Missing $source. Download the PDFium Android package first (see README)."
    }
    $destDir = Join-Path $androidDir "app\src\main\jniLibs\$abi"
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $destDir 'libpdfium.so') -Force
    Write-Host "Copied $abi/libpdfium.so"
}