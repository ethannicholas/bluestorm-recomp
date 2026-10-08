# Cross-compile for an Android/arm64 device (a Quest headset, say) and optionally run
# on it over adb.
#
# Two targets, neither of which is an APK:
#
#   waverace_bench  no renderer, no SDL -- measures whether the device's CPU can
#                   sustain the game's 30 fps.   .\build-android.ps1 -Run
#   waverace_egl    the real renderer on a headless EGL pbuffer, writing frames as
#                   PNGs -- validates the GL ES back end on the device before there is
#                   any VR frontend.             .\build-android.ps1 -Render
#
# See docs/dev/diagnostics.md for both; the README covers the installable app.
param(
    [switch]$Run,
    [switch]$Render,
    [int]$Seconds = 40,
    [int]$Scale = 1,
    [int]$DumpEvery = 20,
    [string]$InputScript,
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
if (-not $ndk) { throw "No NDK found under $ndkRoot. See the Quest section of README.md." }

$toolchain = Join-Path $ndk.FullName 'build\cmake\android.toolchain.cmake'
if (-not (Test-Path $toolchain)) { throw "NDK toolchain file not found: $toolchain" }
Write-Host "using NDK $($ndk.Name)"

if (-not (Test-Path gcn-recomp/CMakeLists.txt)) {
    git submodule update --init
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed" }
}

$buildDir = 'build-android'
if (-not (Test-Path "$buildDir/build.ninja")) {
    # ANDROID_STL=c++_static keeps the binaries self-contained, so there is no
    # libc++_shared.so to push alongside them. GCN_BENCH_ONLY skips SDL2 and the desktop
    # renderer targets; waverace_egl is added separately and brings its own frontend.
    cmake -S . -B $buildDir -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DANDROID_ABI=$Abi" `
        "-DANDROID_PLATFORM=android-$ApiLevel" `
        "-DANDROID_STL=c++_static" `
        "-DGCN_BENCH_ONLY=ON" `
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}
ninja -C $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed" }
foreach ($t in 'waverace_bench', 'waverace_egl') {
    $p = "$buildDir/$t"
    if (Test-Path $p) { Write-Host "built $p ($([math]::Round((Get-Item $p).Length/1MB,1)) MB)" }
}

if (-not ($Run -or $Render)) {
    Write-Host "re-run with -Run (benchmark) or -Render (frame capture) to use an attached device"
    exit 0
}

$adb = Join-Path $sdk 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) { throw "adb not found at $adb" }

$devices = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
if (-not $devices) {
    throw "No device in 'device' state. Enable developer mode, accept the USB debugging prompt in the headset, and check 'adb devices'."
}
Write-Host "device: $($devices[0])"

$iso = (Get-ChildItem rom/*.iso | Select-Object -First 1)
if (-not $iso) { throw "No .iso in rom/" }

& $adb shell mkdir -p $DeviceDir | Out-Null

# The image is ~1.4 GB; only push it when it isn't already there at the right size.
$have = (& $adb shell "stat -c %s $DeviceDir/game.iso 2>/dev/null").Trim()
if ($have -ne "$($iso.Length)") {
    Write-Host "pushing game image ($([math]::Round($iso.Length/1GB,2)) GB), this takes a few minutes..."
    & $adb push $iso.FullName "$DeviceDir/game.iso"
    if ($LASTEXITCODE -ne 0) { throw "adb push of the image failed" }
} else {
    Write-Host "game image already on device"
}

$exe = if ($Render) { 'waverace_egl' } else { 'waverace_bench' }
& $adb push "$buildDir/$exe" "$DeviceDir/$exe"
if ($LASTEXITCODE -ne 0) { throw "adb push failed" }
& $adb shell chmod 755 "$DeviceDir/$exe"

# GCN_INPUT drives the game without a controller: "frame:BUTTON:duration,..." against
# the presented-frame count. Needed to reach anything past the title screen.
$envPrefix = if ($InputScript) { "GCN_INPUT='$InputScript' " } else { "" }

if ($Render) {
    & $adb shell "rm -rf $DeviceDir/frames; mkdir -p $DeviceDir/frames"
    Write-Host "--- rendering ---"
    & $adb shell "cd $DeviceDir && $envPrefix./waverace_egl --scale=$Scale --dump-dir=frames --dump-every=$DumpEvery --seconds=$Seconds ./game.iso > run.log 2>&1"
    & $adb shell "grep -E 'GL_|presented|done|FATAL|ERROR' $DeviceDir/run.log | head -30"
    $out = "$buildDir/frames"
    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $out | Out-Null
    & $adb pull "$DeviceDir/frames" $out 2>&1 | Select-Object -Last 1
    Write-Host "frames in $out"
} else {
    Write-Host "--- benchmark ---"
    & $adb shell "cd $DeviceDir && $envPrefix./waverace_bench --seconds=$Seconds ./game.iso"
}
