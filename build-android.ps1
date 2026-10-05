# Cross-compile the headless benchmark for an Android/arm64 device (a Quest headset,
# say) and optionally push and run it over adb.
#
# This builds ONLY `waverace_bench`: no renderer, no SDL, no APK. Its purpose is to
# measure whether a device's CPU can sustain the game's 30 fps before committing to an
# OpenGL ES port of the renderer. See the Benchmarking section of README.md.
#
#   .\build-android.ps1            # build only
#   .\build-android.ps1 -Run       # build, push to the device, run the benchmark
param(
    [switch]$Run,
    [int]$Seconds = 40,
    [string]$Abi = 'arm64-v8a',
    [int]$ApiLevel = 29,
    [string]$DeviceDir = '/data/local/tmp/waverace'
)

# 'Continue', not 'Stop': cmake, ninja and adb all write progress to stderr, which
# would otherwise be turned into fatal errors. Failures are caught via $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot

$sdk = "$env:LOCALAPPDATA\Android\Sdk"
$ndkRoot = Join-Path $sdk 'ndk'
$ndk = Get-ChildItem $ndkRoot -Directory -ErrorAction SilentlyContinue |
       Sort-Object Name -Descending | Select-Object -First 1
if (-not $ndk) { throw "No NDK found under $ndkRoot. See the Benchmarking section of README.md." }

$toolchain = Join-Path $ndk.FullName 'build\cmake\android.toolchain.cmake'
if (-not (Test-Path $toolchain)) { throw "NDK toolchain file not found: $toolchain" }
Write-Host "using NDK $($ndk.Name)"

$buildDir = 'build-android'
if (-not (Test-Path "$buildDir/build.ninja")) {
    # ANDROID_STL=c++_static keeps the binary self-contained, so there is no
    # libc++_shared.so to push alongside it.
    cmake -S . -B $buildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$Abi" `
        "-DANDROID_PLATFORM=android-$ApiLevel" `
        "-DANDROID_STL=c++_static" `
        "-DWR_BENCH_ONLY=ON" `
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}
ninja -C $buildDir waverace_bench
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = "$buildDir/waverace_bench"
Write-Host "built $exe ($([math]::Round((Get-Item $exe).Length/1MB,1)) MB)"

if (-not $Run) {
    Write-Host "re-run with -Run to push and benchmark on an attached device"
    exit 0
}

$adb = Join-Path $sdk 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }

$devices = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
if (-not $devices) {
    throw "No device in 'device' state. Enable developer mode, allow USB debugging on the headset, and check 'adb devices'."
}
Write-Host "device: $($devices[0])"

$iso = (Get-ChildItem rom/*.iso | Select-Object -First 1).FullName
if (-not $iso) { throw "No .iso in rom/" }

& $adb shell mkdir -p $DeviceDir | Out-Null
& $adb push $exe "$DeviceDir/waverace_bench"
if ($LASTEXITCODE -ne 0) { throw "adb push failed" }
& $adb shell chmod 755 "$DeviceDir/waverace_bench"

# The image is ~1.4 GB; only push it when it isn't already there and the right size.
$want = (Get-Item $iso).Length
$have = (& $adb shell "stat -c %s $DeviceDir/game.iso 2>/dev/null").Trim()
if ($have -ne "$want") {
    Write-Host "pushing game image ($([math]::Round($want/1GB,2)) GB), this takes a few minutes..."
    & $adb push $iso "$DeviceDir/game.iso"
    if ($LASTEXITCODE -ne 0) { throw "adb push of the image failed" }
} else {
    Write-Host "game image already on device"
}

Write-Host "--- benchmark ---"
& $adb shell "cd $DeviceDir && ./waverace_bench --seconds=$Seconds ./game.iso"
