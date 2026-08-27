Set-Location $PSScriptRoot

# ---- 工具链路径（与参考 apkpoc 一致） ----
$sdk     = "C:\Users\Administrator\AppData\Local\Android\Sdk"
$bt      = "$sdk\build-tools\36.0.0"
$android = "$sdk\platforms\android-35\android.jar"
$aapt2   = "$bt\aapt2.exe"
$d8      = "$bt\d8.bat"
$zipalign= "$bt\zipalign.exe"
$apksign = "$bt\apksigner.bat"
$javac   = "F:\core\java\tool\jdk-17.0.2\bin\javac.exe"
$keytool = "F:\core\java\tool\jdk-17.0.2\bin\keytool.exe"
$python  = "python"

$ks      = "taskmgr.keystore"
$alias   = "taskmgr"
$out     = "taskmgr.apk"

# ---- 清理工作目录 ----
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue obj, dexout, unsigned.apk, packed.apk, aligned.apk, $out
New-Item -ItemType Directory -Force obj, dexout | Out-Null

# ---- 1. javac ----
& $javac --release 17 -encoding UTF-8 -cp $android -d obj "java\com\dchamp\taskmgr\MainActivity.java"
if ($LASTEXITCODE -ne 0) { Write-Host "JAVAC FAILED" -ForegroundColor Red; exit 1 }

# ---- 2. d8 ----
cmd /d /c "`"$d8`" --release --lib `"$android`" --min-api 21 --output dexout obj\com\dchamp\taskmgr\*.class"
if ($LASTEXITCODE -ne 0) { Write-Host "D8 FAILED" -ForegroundColor Red; exit 1 }

# ---- 3. aapt2 link(资源=只有 manifest) ----
& $aapt2 link -o unsigned.apk -I $android --manifest AndroidManifest.xml `
    --min-sdk-version 21 --target-sdk-version 28 `
    --version-code 1 --version-name 1.0
if ($LASTEXITCODE -ne 0) { Write-Host "AAPT2 FAILED" -ForegroundColor Red; exit 1 }

# ---- 4. 注入 classes.dex（无 native lib） ----
& $python add_dex.py unsigned.apk packed.apk dexout\classes.dex
if ($LASTEXITCODE -ne 0) { Write-Host "ADD_DEX FAILED" -ForegroundColor Red; exit 1 }

# ---- 5. zipalign ----
& $zipalign -f -p 4 packed.apk aligned.apk
if ($LASTEXITCODE -ne 0) { Write-Host "ZIPALIGN FAILED" -ForegroundColor Red; exit 1 }

# ---- 6. 签名(一次性生成 keystore) ----
if (-not (Test-Path $ks)) {
    & $keytool -genkeypair -keystore $ks -storetype PKCS12 -alias $alias `
        -keyalg RSA -keysize 2048 -validity 10000 `
        -storepass android -keypass android `
        -dname "CN=Dchamp TaskMgr,O=Dchamp,C=US" 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Host "KEYTOOL FAILED" -ForegroundColor Red; exit 1 }
}
cmd /d /c "`"$apksign`" sign --ks $ks --ks-pass pass:android --key-pass pass:android --ks-key-alias $alias --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true --out $out aligned.apk"
if ($LASTEXITCODE -ne 0) { Write-Host "SIGN FAILED" -ForegroundColor Red; exit 1 }

# ---- 7. 验证 ----
cmd /d /c "`"$apksign`" verify --print-certs -v $out"
Write-Host ""
Write-Host "BUILD OK: $PSScriptRoot\$out" -ForegroundColor Green
Get-Item $out | Select-Object Length, FullName | Format-List
