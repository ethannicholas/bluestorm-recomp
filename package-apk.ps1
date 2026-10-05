# Package the Android NativeActivity library into an installable APK, and optionally
# install it and push the disc image.
#
#   .\build-android.ps1              # builds libwaverace.so
#   .\package-apk.ps1 -Install       # packages, installs, pushes the image
#
# There is no Java source and no Gradle: the framework's android.app.NativeActivity
# loads the library directly, so this is aapt2 -> zip -> zipalign -> apksigner.
param(
    [switch]$Install,
    [string]$Abi = 'arm64-v8a',
    [string]$Package = 'com.example.waverace',
    [string]$BuildDir = 'build-android'
)

$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot

$sdk = "$env:LOCALAPPDATA\Android\Sdk"
$env:JAVA_HOME = (Get-ChildItem "$env:ProgramFiles\Microsoft" -Directory -ErrorAction SilentlyContinue |
                  Where-Object Name -match 'jdk' | Select-Object -Last 1).FullName
if (-not $env:JAVA_HOME) { throw "No JDK found under $env:ProgramFiles\Microsoft. See README.md." }

$bt = Get-ChildItem "$sdk\build-tools" -Directory -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending | Select-Object -First 1
if (-not $bt) { throw "No build-tools under $sdk\build-tools. See README.md." }
$platform = Get-ChildItem "$sdk\platforms" -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1
if (-not $platform) { throw "No platform under $sdk\platforms. See README.md." }

$aapt2    = Join-Path $bt.FullName 'aapt2.exe'
$zipalign = Join-Path $bt.FullName 'zipalign.exe'
$apksigner= Join-Path $bt.FullName 'apksigner.bat'
$androidJar = Join-Path $platform.FullName 'android.jar'
$keytool  = Join-Path $env:JAVA_HOME 'bin\keytool.exe'

$so = Join-Path $BuildDir 'libwaverace.so'
if (-not (Test-Path $so)) { throw "$so not found. Run .\build-android.ps1 first." }

$out = Join-Path $BuildDir 'apk'
New-Item -ItemType Directory -Force $out | Out-Null
$unsigned = Join-Path $out 'waverace-unsigned.apk'
$aligned  = Join-Path $out 'waverace-aligned.apk'
$signed   = Join-Path $out 'waverace.apk'
foreach ($f in $unsigned, $aligned, $signed) { if (Test-Path $f) { Remove-Item $f } }

# 1. Manifest -> base APK. No resources, so aapt2 only has the manifest to link.
& $aapt2 link -o $unsigned -I $androidJar --manifest android/AndroidManifest.xml `
    --min-sdk-version 29 --target-sdk-version 34
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

# 2. Add the native libraries: ours, plus the OpenXR loader it links against, which
#    FetchContent unpacked from the Khronos AAR.
$libs = @{ "lib/$Abi/libwaverace.so" = (Resolve-Path $so) }
$loader = Get-ChildItem -Recurse -Filter 'libopenxr_loader.so' "$BuildDir/_deps" -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match [regex]::Escape($Abi) } | Select-Object -First 1
if ($loader) {
    $libs["lib/$Abi/libopenxr_loader.so"] = $loader.FullName
    Write-Host "bundling $($loader.Name)"
} else {
    Write-Warning "libopenxr_loader.so not found under $BuildDir/_deps - the app will fail to load"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open((Resolve-Path $unsigned), 'Update')
foreach ($entry in $libs.GetEnumerator()) {
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $entry.Value, $entry.Key)
}
$zip.Dispose()

# 3. Align, then sign with a local debug key (created once).
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

$ks = Join-Path $out 'debug.keystore'
if (-not (Test-Path $ks)) {
    & $keytool -genkeypair -keystore $ks -storepass android -keypass android `
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 `
        -dname "CN=Wave Race Debug, O=bluestorm-recomp, C=US"
    if ($LASTEXITCODE -ne 0) { throw "keytool failed" }
}
& $apksigner sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --ks-key-alias androiddebugkey --out $signed $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Write-Host "built $signed ($([math]::Round((Get-Item $signed).Length/1MB,1)) MB)"
if (-not $Install) { Write-Host "re-run with -Install to install on an attached device"; exit 0 }

$adb = Join-Path $sdk 'platform-tools\adb.exe'
$devices = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
if (-not $devices) { throw "No device in 'device' state." }

& $adb install -r $signed
if ($LASTEXITCODE -ne 0) { throw "adb install failed" }

# The image lives in the app's external files directory, which needs no runtime
# permission. It is the user's own disc image and is never part of the APK.
$dataDir = "/sdcard/Android/data/$Package/files"
& $adb shell mkdir -p $dataDir
$iso = (Get-ChildItem rom/*.iso | Select-Object -First 1)
if (-not $iso) { throw "No .iso in rom/" }
# stat returns nothing when the file isn't there yet, so guard against null.
$have = "$(& $adb shell "stat -c %s $dataDir/game.iso 2>/dev/null")".Trim()
if ($have -ne "$($iso.Length)") {
    Write-Host "pushing game image ($([math]::Round($iso.Length/1GB,2)) GB)..."
    & $adb push $iso.FullName "$dataDir/game.iso"
    if ($LASTEXITCODE -ne 0) { throw "pushing the image failed" }
} else {
    Write-Host "game image already in place"
}

Write-Host ""
Write-Host "Installed. On the headset: Library -> Unknown Sources -> Wave Race"
Write-Host "Logs:  adb logcat -s waverace"
