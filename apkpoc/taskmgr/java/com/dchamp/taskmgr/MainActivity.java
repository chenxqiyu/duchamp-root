package com.dchamp.taskmgr;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;

/**
 * 电视端“任务管理器”小工具。
 * 打开应用即执行 root 命令: input keyevent KEYCODE_APP_SWITCH
 * 该命令会调出系统的“最近任务 / 任务切换器”界面（电视上的任务管理器）。
 */
public class MainActivity extends Activity {
    static final String TAG = "DchampTaskMgr";
    // 要执行的命令（root 下）
    static final String CMD = "input keyevent KEYCODE_APP_SWITCH";

    private TextView status;
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 48, 48, 48);
        root.setBackgroundColor(0xFF0E0E0E);

        TextView title = new TextView(this);
        title.setText("任务管理器 / Task Manager");
        title.setTextSize(28);
        title.setTextColor(0xFFF5F5F5);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("\n点开应用即执行(root):\n  input keyevent KEYCODE_APP_SWITCH\n\n按 OK / 方向键确认 可再次触发。\n");
        hint.setTextSize(16);
        hint.setTextColor(0xFFBDBDBD);
        root.addView(hint);

        Button btn = new Button(this);
        btn.setText("打开任务切换器 (APP_SWITCH)");
        btn.setTextSize(20);
        btn.setOnClickListener(v -> fire());
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        blp.topMargin = 16;
        btn.setLayoutParams(blp);
        root.addView(btn);

        status = new TextView(this);
        status.setTextSize(14);
        status.setTextColor(0xFF80CBC4);
        status.setPadding(0, 24, 0, 0);
        root.addView(status);

        setContentView(root);

        setStatus("已启动，正在执行命令…");
        // 点开应用就执行
        fire();
    }

    private void setStatus(final String s) {
        handler.post(() -> status.setText(s));
        Log.i(TAG, s);
    }

    /** 在后台线程执行命令，避免阻塞 UI。 */
    private void fire() {
        new Thread(() -> {
            final String result = runAsRoot(CMD);
            setStatus("执行结果: " + result);
        }).start();
    }

    /**
     * 以 root 执行命令。依次尝试可用的 su 实现，
     * 注意避开 /system/xbin/su（该设备上是坏桩，会报 invalid uid/gid '-c'）。
     * 若所有 su 都不可用，则回退到直接执行（shell 具备 input 组权限时仍可工作）。
     */
    private String runAsRoot(String cmd) {
        String[] suCandidates = {
                "/sbin/su",
                "/data/adb/magisk/su",
                "su"
        };
        for (String su : suCandidates) {
            try {
                ProcessBuilder pb = new ProcessBuilder(su, "-c", cmd);
                pb.redirectErrorStream(true);
                Process p = pb.start();
                String out = readWithTimeout(p, 5000);
                int rc = p.waitFor();
                if (rc == 0) {
                    return "root OK via [" + su + "] rc=0 | " + out;
                }
                // su 存在但本次被拒绝(rc!=0)，继续尝试下一个候选
            } catch (Exception ignored) {
                // su 不存在或启动失败，尝试下一个
            }
        }
        // 回退：直接执行（无需 root）
        try {
            ProcessBuilder pb = new ProcessBuilder("sh", "-c", cmd);
            pb.redirectErrorStream(true);
            Process p = pb.start();
            String out = readWithTimeout(p, 5000);
            int rc = p.waitFor();
            return "no-su, direct rc=" + rc + " | " + out;
        } catch (Exception e) {
            return "ERROR: " + e;
        }
    }

    /** 带超时的读取，避免 su 卡住时 UI 线程无限等待。 */
    private String readWithTimeout(Process p, int timeoutMs) {
        StringBuilder sb = new StringBuilder();
        try {
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            long start = System.currentTimeMillis();
            while (System.currentTimeMillis() - start < timeoutMs) {
                if (r.ready()) {
                    String line = r.readLine();
                    if (line == null) break;
                    sb.append(line).append("\n");
                } else {
                    Thread.sleep(50);
                }
            }
        } catch (Exception ignored) {
        }
        return sb.toString().trim();
    }

    /** 遥控器 OK / 确认 / 任务切换键，均可再次触发。 */
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_DPAD_CENTER
                || keyCode == KeyEvent.KEYCODE_ENTER
                || keyCode == KeyEvent.KEYCODE_APP_SWITCH) {
            fire();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }
}
