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
 * Runs the p2spike executable (lib/<abi>/libp2spike.so) as a foreground service, so that
 * it keeps serving while a player is in front. Stop: SIGTERM, then SIGKILL after 5 s.
 */
public class SpikeService extends Service {
    static final String ACTION_START = "com.freetushkan.p2play.START";
    static final String ACTION_STOP = "com.freetushkan.p2play.STOP";
    static final String EXTRA_LINK = "link";

    private static final String TAG = "P2Spike";
    private static final String CHANNEL = "spike";
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

    // /sdcard/.p2play (magnet.txt, spike.log, downloads) as pPlay's .pplay; without the
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
        return new File(context.getApplicationInfo().nativeLibraryDir, "libp2spike.so");
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(action)) {
            stopSpike();
            stopSelf();
            return START_NOT_STICKY;
        }
        // no link: the one of magnet.txt
        String link = intent != null ? intent.getStringExtra(EXTRA_LINK) : null;
        goForeground();
        stopSpike();
        startSpike(link != null ? link.trim() : "");
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        stopSpike();
        super.onDestroy();
    }

    private void goForeground() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL, "p2spike", NotificationManager.IMPORTANCE_LOW));
            builder = new Notification.Builder(this, CHANNEL);
        } else {
            builder = new Notification.Builder(this);
        }
        int piFlags = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), piFlags);
        Notification notification = builder
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("P2Play spike")
                .setContentText("Streaming")
                .setContentIntent(open)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }

    private void startSpike(String link) {
        File dir = dataDir(this);
        dir.mkdirs();
        try {
            ProcessBuilder pb = link.isEmpty()
                    ? new ProcessBuilder(executable(this).getPath())
                    : new ProcessBuilder(executable(this).getPath(), link);
            pb.directory(dir);
            pb.environment().put("P2SPIKE_DATA", dir.getPath());
            pb.redirectErrorStream(true);
            final Process process = pb.start();
            mProcess = process;
            sRunning = true;
            acquireLocks();
            // the output is in spike.log too: the pipe only has to be drained
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
                    Log.i(TAG, "p2spike exited: " + code);
                } catch (InterruptedException ignored) {
                }
                onExited(process);
            }, "p2spike-drain");
            drain.start();
        } catch (Exception e) {
            Log.e(TAG, "can't start p2spike", e);
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

    private synchronized void stopSpike() {
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
            mWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "p2play:spike");
            mWakeLock.setReferenceCounted(false);
        }
        if (mWifiLock == null) {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wm != null) {
                mWifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "p2play:spike");
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
