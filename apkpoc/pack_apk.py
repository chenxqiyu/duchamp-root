# -*- coding: utf-8 -*-
"""把 classes.dex 与 lib/arm64-v8a/libpreload.so 追加进 aapt2 生成的 APK。

用法: python pack_apk.py <in.apk> <out.apk> <classes.dex> <libpreload.so>
so 用 ZIP_STORED 存储,交给后续 zipalign -p 4 做 4096 页对齐。
"""
import shutil
import sys
import zipfile


def main():
    apk_in, apk_out, dex_path, so_path = sys.argv[1:5]
    shutil.copyfile(apk_in, apk_out)
    with zipfile.ZipFile(apk_out, "a") as z:
        z.write(dex_path, "classes.dex", zipfile.ZIP_DEFLATED)
        z.write(so_path, "lib/arm64-v8a/libpreload.so", zipfile.ZIP_STORED)
    with zipfile.ZipFile(apk_out) as z:
        for info in z.infolist():
            print(f"{info.filename:40s} {info.file_size:>10} stored={info.compress_type == 0}")
    print("packed ->", apk_out)


if __name__ == "__main__":
    main()
