Set-Location $PSScriptRoot

$clang = "C:\Users\Administrator\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"

New-Item -ItemType Directory -Force -Path "build\shennong\bin" | Out-Null

$srcs = "src/main.c src/targets/shennong/util.c src/targets/shennong/slide.c src/fops.c src/pipe.c src/root.c src/preload.c src/ksud_blob.S"
$args = "--target=aarch64-linux-android35 -fPIC -O2 -g0 -Wall -Wextra -Isrc -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function -DTARGET_CONFIG_H=\`"targets/shennong/target.h\`" $srcs -shared -o build\shennong\bin\preload.so -pthread -llog"

cmd /d /c "`"$clang`" $args"

if ($LASTEXITCODE -ne 0) {
  Write-Host "BUILD FAILED" -ForegroundColor Red
  exit 1
}

Get-FileHash build/shennong/bin/preload.so -Algorithm SHA256 | Format-List
Write-Host "BUILD OK: build\shennong\bin\preload.so" -ForegroundColor Green
