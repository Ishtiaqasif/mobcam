package expo.modules.mobcamstream

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log

/**
 * Foreground service of type `camera` that keeps the stream running while the screen is off or
 * another app is open ("Keep streaming in background", issue #15). The camera, encoder and
 * server stay process-wide singletons owned by the module; this service only keeps the process
 * allowed to use the camera, holds the CPU and Wi-Fi awake, and shows the ongoing notification
 * with *Stop*.
 */
class StreamService : Service() {
  private var wakeLock: PowerManager.WakeLock? = null
  private var wifiLock: WifiManager.WifiLock? = null

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    if (intent?.action == ACTION_STOP) {
      onStopRequested?.invoke()
      stopSelf()
      return START_NOT_STICKY
    }
    val notification = buildNotification()
    try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
      } else {
        startForeground(NOTIFICATION_ID, notification)
      }
    } catch (e: Exception) {
      // Android 12+ refuses camera services started from the background; the module only
      // starts this while the app is visible, so this is a last-resort guard.
      Log.w(TAG, "Cannot run in the foreground: ${e.message}")
      stopSelf()
      return START_NOT_STICKY
    }
    acquireLocks()
    running = true
    return START_NOT_STICKY
  }

  override fun onDestroy() {
    running = false
    releaseLocks()
    super.onDestroy()
  }

  @SuppressLint("WakelockTimeout") // held exactly as long as the user keeps streaming
  private fun acquireLocks() {
    if (wakeLock == null) {
      wakeLock = (getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MobCam:streaming").apply { acquire() }
    }
    if (wifiLock == null) {
      val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
      @Suppress("DEPRECATION") // still the only lock that keeps Wi-Fi at full speed with the screen off
      wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "MobCam:streaming").apply {
        setReferenceCounted(false)
        acquire()
      }
    }
  }

  private fun releaseLocks() {
    wakeLock?.let { if (it.isHeld) it.release() }
    wifiLock?.let { if (it.isHeld) it.release() }
    wakeLock = null
    wifiLock = null
  }

  private fun buildNotification(): Notification {
    val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      if (manager.getNotificationChannel(CHANNEL_ID) == null) {
        manager.createNotificationChannel(
          NotificationChannel(CHANNEL_ID, "Streaming", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Shown while MobCam streams with the screen off or in the background"
            setShowBadge(false)
          },
        )
      }
      Notification.Builder(this, CHANNEL_ID)
    } else {
      @Suppress("DEPRECATION")
      Notification.Builder(this).setPriority(Notification.PRIORITY_LOW)
    }

    val open = packageManager.getLaunchIntentForPackage(packageName)?.let {
      PendingIntent.getActivity(this, 0, it, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }
    val stop = PendingIntent.getService(
      this, 1, Intent(this, StreamService::class.java).setAction(ACTION_STOP),
      PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )
    val stopAction = Notification.Action.Builder(
      Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel), "Stop", stop,
    ).build()
    return builder
      .setSmallIcon(android.R.drawable.presence_video_online)
      .setContentTitle("MobCam is streaming")
      .setContentText("The camera stays on while the screen is off or another app is open.")
      .setOngoing(true)
      .setContentIntent(open)
      .addAction(stopAction)
      .build()
  }

  companion object {
    private const val TAG = "MobcamService"
    private const val CHANNEL_ID = "streaming"
    private const val NOTIFICATION_ID = 1
    private const val ACTION_STOP = "expo.modules.mobcamstream.STOP"

    /** True between a successful start and onDestroy. */
    @Volatile var running = false
      private set

    /** The user tapped *Stop* in the notification. Called on the main thread. */
    @Volatile var onStopRequested: (() -> Unit)? = null

    /** Call while the app is visible (Android doesn't allow starting camera services otherwise). */
    fun start(context: Context) {
      val intent = Intent(context, StreamService::class.java)
      try {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(intent) else context.startService(intent)
      } catch (e: Exception) {
        Log.w(TAG, "Cannot start: ${e.message}")
      }
    }

    fun stop(context: Context) {
      context.stopService(Intent(context, StreamService::class.java))
    }
  }
}
