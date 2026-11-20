# 构建 Android APK（Debug / Release / 单元测试）
#
# 为什么要这个脚本：
#   1. AGP 拒绝非 ASCII 工程路径，NDK clang 在中文/空格路径下会报
#      "unable to open output file: Permission denied"，
#      所以通过目录联接把项目映射成 ASCII 路径再构建。
#   2. 顺带把 APK 回写到 dist\，保持交付目录整洁。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build_android.ps1 -Task assembleDebug
#   powershell -ExecutionPolicy Bypass -File build_android.ps1 -Task assembleRelease
#   powershell -ExecutionPolicy Bypass -File build_android.ps1 -Task testDebugUnitTest
#
# 需要的环境（未设置时脚本会尝试下面这些常见位置）：
#   JAVA_HOME          JDK 17
#   ANDROID_HOME       Android SDK
#   ANDROID_NDK_PATH   NDK 27.x
#   GRADLE_USER_HOME   Gradle 缓存目录（可选）
#   以及 gradle 可执行文件（PATH 里能找到，或用 -Gradle 指定）
param(
    [ValidateSet('assembleDebug', 'assembleRelease', 'testDebugUnitTest')]
    [string]$Task = 'assembleDebug',
    [string]$ProjectRoot = '',
    [string]$Gradle = '',
    [string]$LinkRoot = 'C:\fcproj'
)

$ErrorActionPreference = 'Stop'

# 脚本可能被 -File 直接执行，也可能被脚本块方式调用，这里逐级兜底取项目根。
$projectRoot = $ProjectRoot
if (-not $projectRoot -and $PSScriptRoot) { $projectRoot = $PSScriptRoot }
if (-not $projectRoot -and $MyInvocation.MyCommand.Path) {
    $projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if (-not $projectRoot) { $projectRoot = (Get-Location).Path }
if (-not (Test-Path (Join-Path $projectRoot 'cpp\CMakeLists.txt'))) {
    throw "项目根不正确：$projectRoot（请从仓库根目录运行，或用 -ProjectRoot 指定）"
}
$projectRoot = (Resolve-Path $projectRoot).Path

$androidDir = Join-Path $LinkRoot 'cpp\android'
$distDir = Join-Path $projectRoot 'dist'

# 1) 目录联接：把项目映射到纯 ASCII 路径（已存在则复用）
if (-not (Test-Path $LinkRoot)) {
    cmd /c mklink /J $LinkRoot $projectRoot | Out-Null
    Write-Host "已创建目录联接 $LinkRoot -> $projectRoot"
} else {
    Write-Host "复用已有目录联接 $LinkRoot"
}

# 2) 工具链：优先用环境变量，其次猜常见安装位置
if (-not $env:JAVA_HOME) {
    $studios = @(
        'C:\Program Files\Android\Android Studio\jbr',
        "$env:LOCALAPPDATA\Programs\Android Studio\jbr"
    )
    foreach ($candidate in $studios) {
        if (Test-Path (Join-Path $candidate 'bin\java.exe')) { $env:JAVA_HOME = $candidate; break }
    }
}
if (-not $env:JAVA_HOME) { throw '未找到 JDK 17，请先设置 JAVA_HOME（Android Studio 自带 jbr 也可以）' }

if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }
if (-not (Test-Path $env:ANDROID_HOME)) { throw "找不到 Android SDK：$env:ANDROID_HOME" }
$env:ANDROID_SDK_ROOT = $env:ANDROID_HOME

if (-not $env:ANDROID_NDK_PATH) {
    $ndkDir = Join-Path $env:ANDROID_HOME 'ndk'
    if (Test-Path $ndkDir) {
        $ndk = Get-ChildItem $ndkDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($ndk) { $env:ANDROID_NDK_PATH = $ndk.FullName }
    }
}
if (-not $env:ANDROID_NDK_PATH) {
    throw '找不到 NDK，请设置 ANDROID_NDK_PATH（需要 NDK 27.x）'
}

$gradleExe = $Gradle
if (-not $gradleExe) {
    $cmd = Get-Command gradle -ErrorAction SilentlyContinue
    if ($cmd) { $gradleExe = $cmd.Source }
}
if (-not $gradleExe -or -not (Test-Path $gradleExe)) {
    throw '找不到 gradle，请把 Gradle 8.x 加到 PATH，或用 -Gradle 指定 gradle.bat 路径'
}

Write-Host "JAVA_HOME        = $env:JAVA_HOME"
Write-Host "ANDROID_HOME     = $env:ANDROID_HOME"
Write-Host "ANDROID_NDK_PATH = $env:ANDROID_NDK_PATH"
Write-Host "gradle           = $gradleExe"

# 3) 构建
Write-Host "构建 :app:$Task ..."
Push-Location $androidDir
try {
    & $gradleExe ":app:$Task" --console=plain --no-daemon
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) {
    throw "Gradle 构建失败（exit $code），日志见上方输出"
}

# 4) 回写 dist
function Copy-Artifacts([string]$buildType, [string]$targetName) {
    $dir = Join-Path $androidDir "app\build\outputs\apk\$buildType"
    if (-not (Test-Path $dir)) { return }
    # ABI 拆分后优先取 universal 包
    $apk = Get-ChildItem $dir -Filter 'app-universal-*.apk' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch 'unsigned' } | Select-Object -First 1
    if (-not $apk) {
        $apk = Get-ChildItem $dir -Filter 'app-*.apk' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch 'unsigned' } | Select-Object -First 1
    }
    if ($apk) {
        New-Item -ItemType Directory -Force $distDir | Out-Null
        $dest = Join-Path $distDir $targetName
        Copy-Item $apk.FullName $dest -Force
        $hash = (Get-FileHash $dest -Algorithm SHA256).Hash
        Write-Host ("已输出 {0}  ({1:N1} MB)  SHA256={2}" -f $dest, ($apk.Length / 1MB), $hash)
    }
}

switch ($Task) {
    'assembleDebug' { Copy-Artifacts 'debug' '格式转换工具-android-debug.apk' }
    'assembleRelease' { Copy-Artifacts 'release' '格式转换工具-android-release.apk' }
}

Write-Host "完成。"
