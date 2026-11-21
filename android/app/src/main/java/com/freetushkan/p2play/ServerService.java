package com.freetushkan.p2play;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Environment;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.InputStream;
import java.util.concurrent.TimeUnit;

/**
 * Runs the P2Play server (p2playd, packaged as lib/<abi>/libp2playd.so) as a foreground
 * service: the TorrServer API and the streams on port 8090 while a player is in front, as
 * TorrServe does. Stop: SIGTERM, then SIGKILL after 5 s.
 */
public class ServerService extends Service {
    static final String ACTION_START = "com.freetushkan.p2play.START";
    static final String ACTION_STOP = "com.freetushkan.p2play.STOP";
    static final int PORT = 8090;

    private static final String TAG = "P2Play";
    private static final String CHANNEL = "server";
    private static final int NOTIFICATION_ID = 1;

    private static volatile boolean sRunning = false;

    private Process mProcess;
    private PowerManager.WakeLock mWakeLock;
    private WifiManager.WifiLock mWifiLock;

    static boolean isRunning() {
        return sRunning;
    }

    // all files access: Android 11+ by its settings, older ones by the permission
    static boolean hasStorage(Context context) {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= 23) {
            return context.checkSelfPermission("android.permission.WRITE_EXTERNAL_STORAGE")
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    // /sdcard/.p2play (the library, settings, p2play.log, disk storage) as pPlay's .pplay; without the
    // access: Android/data/<package>/files
    static File dataDir(Context context) {
        if (hasStorage(context)) {
            File dir = new File(Environment.getExternalStorageDirectory(), ".p2play");
            if (dir.isDirectory() || dir.mkdirs()) {
                return dir;
            }
        }
        File ext = context.getExternalFilesDir(null);
        return ext != null ? ext : new File(context.getFilesDir(), "p2play");
    }

    static File executable(Context context) {
        return new File(context.getApplicationInfo().nativeLibraryDir, "libp2playd.so");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(action)) {
            stopServer();
            stopSelf();
            return START_NOT_STICKY;
        }
        if (sRunning) {
            return START_NOT_STICKY;
        }
        goForeground();
        startServer();
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        stopServer();
        super.onDestroy();
    }

    private void goForeground() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, "P2Play", NotificationManager.IMPORTANCE_LOW));
            builder = new Notification.Builder(this, CHANNEL);
        } else {
            builder = new Notification.Builder(this);
        }
        int piFlags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), piFlags);
        Notification notification = builder
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("P2Play")
                .setContentText("Server on port " + PORT)
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    private void startServer() {
        File dir = dataDir(this);
        dir.mkdirs();
        try {
            ProcessBuilder pb = new ProcessBuilder(executable(this).getPath(),
                    "--data", dir.getPath(), "--port", String.valueOf(PORT));
            pb.directory(dir);
            pb.redirectErrorStream(true);
            final Process process = pb.start();
            mProcess = process;
            sRunning = true;
            acquireLocks();
            // the output is in p2play.log too: the pipe only has to be drained
            Thread drain = new Thread(() -> {
                byte[] buf = new byte[4096];
                try (InputStream in = process.getInputStream()) {
                    while (in.read(buf) > 0) {
                        // discarded
                    }
                } catch (Exception ignored) {
                }
                try {
                    int code = process.waitFor();
                    Log.i(TAG, "p2playd exited: " + code);
                } catch (InterruptedException ignored) {
                }
                onExited(process);
            }, "p2play-drain");
            drain.start();
        } catch (Exception e) {
            Log.e(TAG, "can't start p2playd", e);
            sRunning = false;
            stopForegroundCompat();
            stopSelf();
        }
    }

    private synchronized void onExited(Process process) {
        if (mProcess != process) {
            return; // replaced by a new one
        }
        mProcess = null;
        sRunning = false;
        releaseLocks();
        stopForegroundCompat();
        stopSelf();
    }

    private synchronized void stopServer() {
        Process process = mProcess;
        mProcess = null;
        if (process != null) {
            process.destroy();
            try {
                if (Build.VERSION.SDK_INT >= 26) {
                    if (!process.waitFor(5, TimeUnit.SECONDS)) {
                        process.destroyForcibly();
                    }
                } else {
                    process.waitFor();
                }
            } catch (InterruptedException ignored) {
            }
        }
        sRunning = false;
        releaseLocks();
    }

    private void acquireLocks() {
        if (mWakeLock == null) {
            PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
            mWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "p2play:server");
            mWakeLock.setReferenceCounted(false);
        }
        if (mWifiLock == null) {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wm != null) {
                mWifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "p2play:server");
                mWifiLock.setReferenceCounted(false);
            }
        }
        mWakeLock.acquire();
        if (mWifiLock != null) {
            mWifiLock.acquire();
        }
    }

    private void releaseLocks() {
        if (mWakeLock != null && mWakeLock.isHeld()) {
            mWakeLock.release();
        }
        if (mWifiLock != null && mWifiLock.isHeld()) {
            mWifiLock.release();
        }
    }

    @SuppressWarnings("deprecation")
    private void stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= 24) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
    }
}
