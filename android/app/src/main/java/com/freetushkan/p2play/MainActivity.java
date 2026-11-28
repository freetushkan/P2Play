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
import java.util.Collections;

/**
 * The P2Play server screen: start / stop, its addresses, the torrents, the log tail; a link
 * (typed, pasted, or opened/shared with the app) is added to the library through the
 * native P2Play API and its largest file opened in a player. Usable with Android TV.
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

    // a magnet or a torrs:// link from VIEW or SEND: added and played
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
        sDataDir = ServerService.dataDir(this);
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
        mLink.setHint("magnet:?xt=urn:btih:..., torrs://... or an infohash");
        mLink.setSingleLine(true);
        mLink.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        root.addView(mLink, matchWrap());

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(button("Add & play", v -> addAndPlay()));
        buttons.addView(button("Paste", v -> paste()));
        buttons.addView(button("Share torrs://", v -> shareTorrs()));
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

    // Import the original link so the server can parse the whole card, including its poster.
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
                // Retry readiness, not the mutating request (which may already have succeeded).
                boolean ready = false;
                for (int i = 0; i < 20 && !ready; i++) {
                    try {
                        request("GET", "/api/v1/auth/me", null);
                        ready = true;
                    } catch (Exception e) {
                        Thread.sleep(500);
                    }
                }
                if (!ready) {
                    throw new Exception("the server does not answer or owner authorization failed");
                }
                JSONObject add = new JSONObject().put("link", link);
                JSONObject torrent = new JSONObject(post("/api/v1/torrents", add.toString()))
                        .getJSONObject("torrent");
                String path = "/api/v1/torrents/" + torrent.getString("hash");
                JSONArray files = new JSONObject(request("GET", path + "/files?wait=60", null, 65000))
                        .getJSONArray("files");
                JSONObject best = null;
                for (int i = 0; i < files.length(); i++) {
                    JSONObject f = files.getJSONObject(i);
                    if (best == null || f.optLong("length") > best.optLong("length")) {
                        best = f;
                    }
                }
                if (best == null) {
                    throw new Exception("no files to play");
                }
                JSONObject share = new JSONObject(post(path + "/files/" + best.getInt("id") + "/share", "{}"));
                url = LOCAL + share.getString("url");
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

    // a line per torrent: title, state, download speed
    private static String torrents() {
        try {
            JSONArray list = new JSONObject(request("GET", "/api/v1/library/torrents", null)).getJSONArray("items");
            if (list.length() == 0) {
                return "Server running, no torrents";
            }
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < list.length(); i++) {
                JSONObject t = list.getJSONObject(i);
                sb.append(t.optString("title")).append(" - ").append(t.optString("state_text"));
                sb.append(", ").append(Math.round(t.optDouble("download_speed", 0) / 1024)).append(" KB/s");
                sb.append('\n');
            }
            return sb.toString().trim();
        } catch (Exception e) {
            return "Server starting...";
        }
    }

    // the torrs:// link of the newest torrent of the library, shared (a messenger, pPlay, TorrServe)
    private void shareTorrs() {
        new Thread(() -> {
            String link = null, title = null;
            try {
                JSONArray list = new JSONObject(request("GET", "/api/v1/library/torrents?sort=date&order=desc", null))
                        .getJSONArray("items");
                for (int i = 0; i < list.length(); i++) {
                    JSONObject t = list.getJSONObject(i);
                    JSONObject details = new JSONObject(request("GET", "/api/v1/torrents/" + t.getString("hash"), null))
                            .getJSONObject("details");
                    String candidate = details.optString("torrs");
                    if (!candidate.isEmpty()) {
                        link = candidate;
                        title = t.optString("title");
                        break;
                    }
                }
            } catch (Exception e) {
                link = null;
            }
            final String text = link, name = title;
            runOnUiThread(() -> {
                if (text == null) {
                    mStatus.setText("No torrent to share");
                    return;
                }
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType("text/plain");
                send.putExtra(Intent.EXTRA_SUBJECT, name);
                send.putExtra(Intent.EXTRA_TEXT, text);
                startActivity(Intent.createChooser(send, name));
            });
        }, "share").start();
    }

    private static String post(String path, String body) throws Exception {
        return request("POST", path, body);
    }

    // the owner's token: the server's own auth.json (this app is the owner of its server)
    private static volatile String sToken = "";
    private static File sDataDir;

    private static String token() {
        if (sToken.isEmpty() && sDataDir != null) {
            try (InputStream in = new java.io.FileInputStream(new File(sDataDir, "auth.json"))) {
                ByteArrayOutputStream buf = new ByteArrayOutputStream();
                byte[] b = new byte[8192];
                int n;
                while ((n = in.read(b)) > 0) {
                    buf.write(b, 0, n);
                }
                JSONArray users = new JSONObject(buf.toString("UTF-8")).getJSONArray("users");
                for (int i = 0; i < users.length(); i++) {
                    if ("owner".equals(users.getJSONObject(i).optString("id"))) {
                        sToken = users.getJSONObject(i).optString("api_token");
                    }
                }
            } catch (Exception ignored) {
                // the server writes it at its first start
            }
        }
        return sToken;
    }

    private static String request(String method, String path, String body) throws Exception {
        return request(method, path, body, 10000);
    }

    private static String request(String method, String path, String body, int readTimeout) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(LOCAL + path).openConnection();
        c.setRequestProperty("Authorization", "Bearer " + token());
        try {
            c.setConnectTimeout(1500);
            c.setReadTimeout(readTimeout);
            c.setRequestMethod(method);
            if (body != null) {
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/json");
                try (OutputStream out = c.getOutputStream()) {
                    out.write(body.getBytes("UTF-8"));
                }
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
