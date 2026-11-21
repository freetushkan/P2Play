package com.freetushkan.p2play;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.util.Collections;

/**
 * Test screen of p2spike: a link, start/stop, the stream URLs, the status and the log tail.
 * Usable with a remote (Android TV): every control is a focusable button or field.
 */
public class MainActivity extends Activity {
    private static final String STREAM = "http://127.0.0.1:8095/stream";
    private static final String STATUS = "http://127.0.0.1:8095/status";
    private static final int LOG_TAIL = 6 * 1024;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private EditText mLink;
    private TextView mUrls;
    private TextView mStatus;
    private TextView mLog;
    private ScrollView mLogScroll;
    private volatile boolean mPolling;

    private final Runnable mTick = new Runnable() {
        @Override
        public void run() {
            refresh();
            mHandler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildUi();
        mLink.setText(prefs().getString("link", ""));
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 1);
        }
        handleIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleIntent(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        mHandler.post(mTick);
    }

    @Override
    protected void onPause() {
        mHandler.removeCallbacks(mTick);
        super.onPause();
    }

    // magnet from VIEW or SEND: started at once
    private void handleIntent(Intent intent) {
        if (intent == null) {
            return;
        }
        String link = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction()) && intent.getData() != null) {
            link = intent.getData().toString();
        } else if (Intent.ACTION_SEND.equals(intent.getAction())) {
            link = intent.getStringExtra(Intent.EXTRA_TEXT);
        }
        if (link != null && !link.trim().isEmpty()) {
            mLink.setText(link.trim());
            start();
        }
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("spike", MODE_PRIVATE);
    }

    private void buildUi() {
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        mLink = new EditText(this);
        mLink.setHint("magnet:?xt=urn:btih:...  or an infohash");
        mLink.setSingleLine(true);
        mLink.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        root.addView(mLink, matchWrap());

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(button("Start", v -> start()));
        buttons.addView(button("Stop", v -> stop()));
        buttons.addView(button("Play", v -> play()));
        buttons.addView(button("Paste", v -> paste()));
        root.addView(buttons, matchWrap());

        mUrls = new TextView(this);
        mUrls.setTextIsSelectable(true);
        root.addView(mUrls, matchWrap());

        mStatus = new TextView(this);
        mStatus.setPadding(0, dp(8), 0, dp(8));
        root.addView(mStatus, matchWrap());

        mLogScroll = new ScrollView(this);
        mLog = new TextView(this);
        mLog.setTypeface(Typeface.MONOSPACE);
        mLog.setTextSize(11);
        mLogScroll.addView(mLog);
        root.addView(mLogScroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
        mLink.requestFocus();
    }

    private Button button(String text, View.OnClickListener listener) {
        Button b = new Button(this);
        b.setText(text);
        b.setOnClickListener(listener);
        return b;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void start() {
        String link = mLink.getText().toString().trim();
        if (link.isEmpty()) {
            mStatus.setText("No link");
            return;
        }
        prefs().edit().putString("link", link).apply();
        Intent i = new Intent(this, SpikeService.class).setAction(SpikeService.ACTION_START)
                .putExtra(SpikeService.EXTRA_LINK, link);
        if (Build.VERSION.SDK_INT >= 26) {
            startForegroundService(i);
        } else {
            startService(i);
        }
    }

    private void stop() {
        startService(new Intent(this, SpikeService.class).setAction(SpikeService.ACTION_STOP));
    }

    private void play() {
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(Uri.parse(STREAM), "video/*");
        try {
            startActivity(Intent.createChooser(i, "Play with"));
        } catch (Exception e) {
            mStatus.setText("No player: " + e.getMessage());
        }
    }

    private void paste() {
        ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (cm != null && cm.hasPrimaryClip()) {
            ClipData clip = cm.getPrimaryClip();
            if (clip != null && clip.getItemCount() > 0 && clip.getItemAt(0).getText() != null) {
                mLink.setText(clip.getItemAt(0).getText().toString().trim());
            }
        }
    }

    private void refresh() {
        boolean running = SpikeService.isRunning();
        StringBuilder urls = new StringBuilder("Stream: ").append(STREAM);
        String ip = lanAddress();
        if (ip != null) {
            urls.append("\nLAN: http://").append(ip).append(":8095/stream");
        }
        mUrls.setText(urls);
        if (!running) {
            mStatus.setText("Stopped");
        } else if (!mPolling) {
            mPolling = true;
            new Thread(() -> {
                String text = fetchStatus();
                mHandler.post(() -> {
                    if (SpikeService.isRunning()) {
                        mStatus.setText(text);
                    }
                });
                mPolling = false;
            }, "status").start();
        }
        String log = logTail();
        if (!log.equals(mLog.getText().toString())) {
            mLog.setText(log);
            mLogScroll.post(() -> mLogScroll.fullScroll(View.FOCUS_DOWN));
        }
    }

    private static String fetchStatus() {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(STATUS).openConnection();
            c.setConnectTimeout(800);
            c.setReadTimeout(800);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
            JSONObject j = new JSONObject(out.toString("UTF-8"));
            StringBuilder sb = new StringBuilder();
            java.util.Iterator<String> keys = j.keys();
            while (keys.hasNext()) {
                String k = keys.next();
                sb.append(k).append(": ").append(j.opt(k)).append(keys.hasNext() ? "   " : "");
            }
            return sb.toString();
        } catch (Exception e) {
            return "Starting...";
        } finally {
            if (c != null) {
                c.disconnect();
            }
        }
    }

    private String logTail() {
        File f = new File(SpikeService.dataDir(this), "spike.log");
        if (!f.exists()) {
            return "";
        }
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            long len = raf.length();
            long from = Math.max(0, len - LOG_TAIL);
            byte[] buf = new byte[(int) (len - from)];
            raf.seek(from);
            raf.readFully(buf);
            return new String(buf, "UTF-8");
        } catch (Exception e) {
            return "";
        }
    }

    private static String lanAddress() {
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) {
                    continue;
                }
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (a instanceof Inet4Address && !a.isLoopbackAddress()) {
                        return a.getHostAddress();
                    }
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }
}
