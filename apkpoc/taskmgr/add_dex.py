# -*- coding: utf-8 -*-
"""把 classes.dex 追加进 aapt2 生成的 APK（无 native lib 版本）。

用法: python add_dex.py <in.apk> <out.apk> <classes.dex>
"""
import shutil
import sys
import zipfile


def main():
    apk_in, apk_out, dex_path = sys.argv[1:4]
    shutil.copyfile(apk_in, apk_out)
    with zipfile.ZipFile(apk_out, "a") as z:
        z.write(dex_path, "classes.dex", zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(apk_out) as z:
        for info in z.infolist():
            print(f"{info.filename:40s} {info.file_size:>10} stored={info.compress_type == 0}")
    print("added classes.dex ->", apk_out)


if __name__ == "__main__":
    main()
