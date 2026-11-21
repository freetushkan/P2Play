package com.freetushkan.p2play;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.net.URLEncoder;
import java.util.Collections;

/**
 * The P2Play server screen: start / stop, its addresses, the torrents, the log tail; a link
 * (typed, pasted, a magnet opened or shared with the app) is added to the library and its
 * largest media file opened in a player. Players and Lampa use the server at
 * http://127.0.0.1:8090 as a TorrServer. Usable with a remote (Android TV).
 */
public class MainActivity extends Activity {
    private static final String LOCAL = "http://127.0.0.1:" + ServerService.PORT;
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
            mHandler.postDelayed(this, 1500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildUi();
        askPermissions();
        startServer();
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

    // a magnet from VIEW or SEND: added and played
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
            addAndPlay();
        }
    }

    // notifications (Android 13+) and all files access for /sdcard/.p2play (as pPlay)
    private void askPermissions() {
        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"}, 1);
        }
        if (ServerService.hasStorage(this)) {
            return;
        }
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                } catch (Exception ignored) {
                    // no such settings (some TVs): the data stays in Android/data
                }
            }
        } else if (Build.VERSION.SDK_INT >= 23) {
            requestPermissions(new String[]{"android.permission.READ_EXTERNAL_STORAGE",
                    "android.permission.WRITE_EXTERNAL_STORAGE"}, 2);
        }
    }

    private void buildUi() {
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        mUrls = new TextView(this);
        mUrls.setTextIsSelectable(true);
        root.addView(mUrls, matchWrap());

        LinearLayout server = new LinearLayout(this);
        server.setOrientation(LinearLayout.HORIZONTAL);
        server.addView(button("Start", v -> startServer()));
        server.addView(button("Stop", v -> stopServer()));
        root.addView(server, matchWrap());

        mLink = new EditText(this);
        mLink.setHint("magnet:?xt=urn:btih:... or an infohash");
        mLink.setSingleLine(true);
        mLink.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        root.addView(mLink, matchWrap());

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(button("Add & play", v -> addAndPlay()));
        buttons.addView(button("Paste", v -> paste()));
        root.addView(buttons, matchWrap());

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

    private void startServer() {
        Intent i = new Intent(this, ServerService.class).setAction(ServerService.ACTION_START);
        if (Build.VERSION.SDK_INT >= 26) {
            startForegroundService(i);
        } else {
            startService(i);
        }
    }

    private void stopServer() {
        startService(new Intent(this, ServerService.class).setAction(ServerService.ACTION_STOP));
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

    // POST /torrents add (into the library), wait for the file list, open the largest media file
    private void addAndPlay() {
        final String link = mLink.getText().toString().trim();
        if (link.isEmpty()) {
            mStatus.setText("No link");
            return;
        }
        startServer();
        mStatus.setText("Adding...");
        new Thread(() -> {
            String error = null;
            String url = null;
            try {
                JSONObject add = new JSONObject().put("action", "add").put("link", link).put("save_to_db", true);
                JSONObject st = null;
                // the server may be starting
                for (int i = 0; i < 20 && st == null; i++) {
                    try {
                        st = new JSONObject(post("/torrents", add.toString()));
                    } catch (Exception e) {
                        Thread.sleep(500);
                    }
                }
                if (st == null) {
                    throw new Exception("the server does not answer");
                }
                String hash = st.getString("hash");
                JSONObject get = new JSONObject().put("action", "get").put("hash", hash);
                for (int i = 0; i < 120 && !st.has("file_stats"); i++) {
                    Thread.sleep(500);
                    st = new JSONObject(post("/torrents", get.toString()));
                }
                if (!st.has("file_stats")) {
                    throw new Exception("no metadata in 60 s (no peers?)");
                }
                JSONArray files = st.getJSONArray("file_stats");
                JSONObject best = null;
                for (int i = 0; i < files.length(); i++) {
                    JSONObject f = files.getJSONObject(i);
                    if (best == null || f.optLong("length") > best.optLong("length")) {
                        best = f;
                    }
                }
                String path = best.getString("path");
                String name = path.substring(path.lastIndexOf('/') + 1);
                url = LOCAL + "/stream/" + URLEncoder.encode(name, "UTF-8").replace("+", "%20") + "?link=" + hash
                        + "&index=" + best.getInt("id") + "&play";
            } catch (Exception e) {
                error = e.getMessage();
            }
            final String fError = error;
            final String fUrl = url;
            mHandler.post(() -> {
                if (fError != null) {
                    mStatus.setText("Failed: " + fError);
                    return;
                }
                Intent i = new Intent(Intent.ACTION_VIEW);
                i.setDataAndType(Uri.parse(fUrl), "video/*");
                try {
                    startActivity(Intent.createChooser(i, "Play with"));
                } catch (Exception e) {
                    mStatus.setText("No player: " + e.getMessage());
                }
            });
        }, "add").start();
    }

    private void refresh() {
        StringBuilder urls = new StringBuilder("Server: ").append(LOCAL);
        String ip = lanAddress();
        if (ip != null) {
            urls.append("\nLAN: http://").append(ip).append(':').append(ServerService.PORT);
        }
        urls.append("\nData: ").append(ServerService.dataDir(this).getPath());
        mUrls.setText(urls);
        if (!ServerService.isRunning()) {
            mStatus.setText("Server stopped");
        } else if (!mPolling) {
            mPolling = true;
            new Thread(() -> {
                String text = torrents();
                mHandler.post(() -> {
                    if (ServerService.isRunning() && !mStatus.getText().toString().startsWith("Adding")) {
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

    // a line a torrent: title, state, peers, speed
    private static String torrents() {
        try {
            JSONArray list = new JSONArray(post("/torrents", "{\"action\":\"list\"}"));
            if (list.length() == 0) {
                return "Server running, no torrents";
            }
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < list.length(); i++) {
                JSONObject t = list.getJSONObject(i);
                sb.append(t.optString("title")).append(" - ").append(t.optString("stat_string"));
                if (t.has("active_peers")) {
                    sb.append(", ").append(t.optInt("active_peers")).append(" peers, ")
                            .append(Math.round(t.optDouble("download_speed") / 1024)).append(" KB/s");
                }
                sb.append('\n');
            }
            return sb.toString().trim();
        } catch (Exception e) {
            return "Server starting...";
        }
    }

    private static String post(String path, String body) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(LOCAL + path).openConnection();
        try {
            c.setConnectTimeout(1500);
            c.setReadTimeout(10000);
            c.setDoOutput(true);
            c.setRequestMethod("POST");
            c.setRequestProperty("Content-Type", "application/json");
            try (OutputStream out = c.getOutputStream()) {
                out.write(body.getBytes("UTF-8"));
            }
            InputStream in = c.getResponseCode() < 400 ? c.getInputStream() : c.getErrorStream();
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            byte[] b = new byte[8192];
            int n;
            while (in != null && (n = in.read(b)) > 0) {
                buf.write(b, 0, n);
            }
            String text = buf.toString("UTF-8");
            if (c.getResponseCode() >= 400) {
                throw new Exception(text);
            }
            return text;
        } finally {
            c.disconnect();
        }
    }

    private String logTail() {
        File f = new File(ServerService.dataDir(this), "p2play.log");
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
