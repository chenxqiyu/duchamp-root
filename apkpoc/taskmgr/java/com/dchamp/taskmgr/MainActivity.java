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
import java.util.concurrent.TimeUnit;

/**
 * 电视端“任务管理器”小工具。
 * 打开应用即按顺序执行(root):
 *   1) input keyevent KEYCODE_APP_SWITCH   // 调出任务切换器
 *   2) input keyevent 20                   // DPAD_DOWN
 *   3) input tap 1600 80                   // 点击坐标
 * 各步之间留 ~800ms 间隔。
 */
public class MainActivity extends Activity {
    static final String TAG = "DchampTaskMgr";

    // 依次执行的命令（均通过 su 以 root 执行）
    static final String[] CMDS = {
            "input keyevent KEYCODE_APP_SWITCH",
            "input keyevent 20",
            "input tap 1600 80"
    };
    // 每步之间的间隔(毫秒)
    static final long STEP_INTERVAL_MS = 800;

    // 可用的 su 实现（避开 /system/xbin/su 坏桩）
    static final String[] SU_CANDIDATES = {
            "/sbin/su",
            "/data/adb/magisk/su",
            "su"
    };

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
        hint.setText("\n点开应用即执行(root, 逐步):\n"
                + "  1) input keyevent KEYCODE_APP_SWITCH\n"
                + "  2) input keyevent 20\n"
                + "  3) input tap 1600 80\n\n按 OK / 方向键确认 可再次触发。\n");
        hint.setTextSize(16);
        hint.setTextColor(0xFFBDBDBD);
        root.addView(hint);

        Button btn = new Button(this);
        btn.setText("执行任务管理器序列");
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

        setStatus("已启动，准备执行命令…");
        // 点开应用就执行
        fire();
    }

    private void setStatus(final String s) {
        handler.post(() -> status.setText(s));
        Log.i(TAG, s.replace("\n", " | "));
    }

    /** 在后台线程逐步执行命令序列，步骤间留间隔。 */
    private void fire() {
        new Thread(() -> {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < CMDS.length; i++) {
                String r = runAsRoot(CMDS[i]);
                sb.append("[step ").append(i + 1).append("] ").append(r).append("\n");
                setStatus("进度:\n" + sb);
                Log.i(TAG, "step " + (i + 1) + " -> " + r);
                if (i < CMDS.length - 1) {
                    try {
                        Thread.sleep(STEP_INTERVAL_MS);
                    } catch (InterruptedException ignored) {
                    }
                }
            }
            setStatus("完成:\n" + sb);
            Log.i(TAG, "SEQUENCE DONE");
        }).start();
    }

    /** 以 root 执行单条命令；依次尝试可用 su，全部失败则回退直接执行。 */
    private String runAsRoot(String cmd) {
        for (String su : SU_CANDIDATES) {
            try {
                Process p = new ProcessBuilder(su, "-c", cmd)
                        .redirectErrorStream(true)
                        .start();
                String out = readFully(p, 8000);
                try {
                    if (p.exitValue() == 0) {
                        return "root OK via [" + su + "] | " + out;
                    }
                } catch (IllegalThreadStateException e) {
                    p.destroy();
                }
            } catch (Exception ignored) {
                // su 不存在/启动失败，尝试下一个
            }
        }
        // 回退：直接执行（无 root）
        try {
            Process p = new ProcessBuilder("sh", "-c", cmd)
                    .redirectErrorStream(true)
                    .start();
            String out = readFully(p, 8000);
            int rc;
            try {
                rc = p.exitValue();
            } catch (IllegalThreadStateException e) {
                p.destroy();
                rc = -1;
            }
            return "no-su, direct rc=" + rc + " | " + out;
        } catch (Exception e) {
            return "ERROR: " + e;
        }
    }

    /** 读满进程输出，最多等 timeoutMs；进程退出即返回。 */
    private String readFully(Process p, long timeoutMs) {
        StringBuilder sb = new StringBuilder();
        Thread t = new Thread(() -> {
            try {
                BufferedReader r = new BufferedReader(
                        new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = r.readLine()) != null) {
                    sb.append(line).append("\n");
                }
            } catch (Exception ignored) {
            }
        });
        t.start();
        try {
            p.waitFor(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (Exception ignored) {
        }
        try {
            t.join(500);
        } catch (InterruptedException ignored) {
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
