package expo.modules.mobcamstream

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.OrientationEventListener
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

private const val STATUS_EVENT = "onStatus"
private const val PAIRING_EVENT = "onPairingRequest"
private const val PAIRING_CLOSED_EVENT = "onPairingClosed"
private const val PAIRED_CHANGED_EVENT = "onPairedPcsChanged"
private const val NOTICES_ASSET = "THIRD-PARTY-NOTICES.txt"

private const val SETTINGS = "mobcam-settings"
private const val KEY_BACKGROUND = "backgroundStreaming"
/** A PC that drops off may reconnect; keep the background service this long without one. */
private const val IDLE_STOP_MS = 60_000L

class MobcamStreamModule : Module() {
  private var engine: StreamEngine? = null
  private var server: StreamServer? = null
  private var discovery: DiscoveryResponder? = null
  private var broker: PairingBroker? = null
  private var orientation: OrientationEventListener? = null
  @Volatile private var engineState: EngineState? = null
  /** Shown under the connection line, e.g. "A PC with an old MobCam version tried to connect". */
  @Volatile private var notice: String? = null
  private var started = false

  private val context: Context
    get() = requireNotNull(appContext.reactContext) { "React context is gone" }.applicationContext

  private val store: PairedPcStore by lazy { PairedPcStore(context) }

  // ---- "Keep streaming in background" (StreamService). All of this runs on the main thread.
  private val main = Handler(Looper.getMainLooper())
  /** Application context captured by start(), still usable in OnDestroy. */
  private var appCtx: Context? = null
  @Volatile private var inForeground = true
  /** The foreground service was started and not stopped since. */
  @Volatile private var serviceOn = false
  private var backgroundSetting: Boolean? = null
  private var keepStreaming: Boolean
    get() = backgroundSetting
      ?: context.getSharedPreferences(SETTINGS, Context.MODE_PRIVATE).getBoolean(KEY_BACKGROUND, false)
        .also { backgroundSetting = it }
    set(value) {
      backgroundSetting = value
      context.getSharedPreferences(SETTINGS, Context.MODE_PRIVATE).edit().putBoolean(KEY_BACKGROUND, value).apply()
    }
  private val idleStop = Runnable { if ((server?.clientCount ?: 0) == 0) stopBackgroundService() }

  override fun definition() = ModuleDefinition {
    Name("MobcamStream")

    Events(STATUS_EVENT, PAIRING_EVENT, PAIRING_CLOSED_EVENT, PAIRED_CHANGED_EVENT)

    /** Starts the camera, encoder, TCP server and discovery responder. Idempotent. */
    Function("start") {
      val ctx = context
      appCtx = ctx
      StreamService.onStopRequested = { onNotificationStop() }
      val eng = engine ?: Mobcam.engine(ctx).also { engine = it }
      if (server == null) {
        val gate = PairingBroker(
          store,
          onPrompt = { sendEvent(PAIRING_EVENT, pairingMap(it)) },
          onPromptClosed = { sendEvent(PAIRING_CLOSED_EVENT, mapOf("id" to it.id)) },
          onPairedChanged = { sendEvent(PAIRED_CHANGED_EVENT, mapOf<String, Any>()) },
        ).also { broker = it }
        server = StreamServer(
          port = Mobcam.STREAM_PORT,
          phoneName = Mobcam.deviceName,
          gate = gate,
          onClientsChanged = {
            notice = null
            emitStatus()
            main.post { updateBackgroundService() }
          },
          onKeyFrameNeeded = { eng.requestKeyFrame() },
          onCommand = { command -> handleCommand(eng, command) },
          onVersionMismatch = { version ->
            notice = if (version != null && version > Proto.VERSION) {
              "A newer MobCam PC app tried to connect. Update this app."
            } else {
              "A PC with an old MobCam version tried to connect. Update MobCam on the PC."
            }
            emitStatus()
          },
          onPairing = { pc, stage ->
            when (stage) {
              PairingStage.WAITING_FOR_PC -> notice = "Now confirm the code on ${pc.name} to finish pairing."
              PairingStage.PAIRED -> {
                gate.confirm(pc)
                notice = null
              }
              PairingStage.CANCELLED -> notice = "Pairing with ${pc.name} was not confirmed on the PC. Nothing was saved."
            }
            emitStatus()
          },
        ).also { it.start() }
        discovery = DiscoveryResponder(ctx, Mobcam.DISCOVERY_PORT, Mobcam.STREAM_PORT).also { it.start() }
      }
      if (orientation == null) {
        // Physical orientation, independent of the (landscape-locked) UI. The PC uses it to
        // turn the picture upright however the phone is mounted.
        orientation = object : OrientationEventListener(ctx) {
          override fun onOrientationChanged(degrees: Int) {
            if (degrees != ORIENTATION_UNKNOWN) eng.deviceOrientation = ((degrees + 45) / 90 * 90) % 360
          }
        }
      }
      orientation?.enable()
      eng.sink = server
      eng.onState = { engineState = it; emitStatus() }
      started = true
      eng.start()
      configureWindow()
    }

    Function("switchCamera") { engine?.switchCamera() }

    Function("selectLens") { index: Int -> engine?.selectLens(index) }

    Function("setResolution") { height: Int -> engine?.setResolution(height) }

    /**
     * "Keep streaming in background": while a PC is connected, a foreground service with an
     * ongoing notification keeps the camera running with the screen off or another app open.
     */
    Function("setBackgroundStreaming") { on: Boolean ->
      keepStreaming = on
      main.post { updateBackgroundService() }
      emitStatus()
    }

    Function("getStatus") { statusMap() }

    /** Answers the open pairing prompt (see onPairingRequest). */
    Function("respondPairing") { id: Int, allow: Boolean -> broker?.respond(id, allow) }

    /** The prompt that is open right now, or null (for a UI that mounts late). */
    Function("getPendingPairing") { broker?.pending?.let { pairingMap(it) } }

    Function("getPairedPcs") {
      store.list().map { mapOf("id" to it.id, "name" to it.name, "pairedAt" to it.pairedAt.toDouble()) }
    }

    /** Forgets a PC; it has to pair again (already connected sessions keep running). */
    Function("forgetPc") { id: String ->
      store.remove(id)
      sendEvent(PAIRED_CHANGED_EVENT, mapOf<String, Any>())
    }

    /**
     * THIRD-PARTY-NOTICES.txt from the APK's assets, or null in builds made without
     * scripts/notices.ps1 (development builds).
     */
    AsyncFunction("readNotices") {
      runCatching { context.assets.open(NOTICES_ASSET).bufferedReader().use { it.readText() } }.getOrNull()
    }

    OnActivityEntersForeground {
      inForeground = true
      // Android takes the camera away from background apps; grab it back on return.
      if (started) {
        engine?.start()
        orientation?.enable()
        configureWindow()
        main.post { updateBackgroundService() }
      }
    }

    OnActivityEntersBackground {
      inForeground = false
      // With the background service running the camera may stay on; otherwise stop it.
      if (started && !serviceOn) {
        engine?.stop()
        orientation?.disable()
      }
    }

    OnDestroy {
      main.removeCallbacks(idleStop)
      StreamService.onStopRequested = null
      appCtx?.let { StreamService.stop(it) }
      serviceOn = false
      started = false
      orientation?.disable()
      orientation = null
      engine?.let { it.stop(); it.sink = null; it.onState = null }
      server?.stop()
      discovery?.stop()
      server = null
      discovery = null
      broker = null
    }

    View(MobcamPreviewView::class) {}
  }

  /** Remote control from the PC app. The phone UI follows via the normal status events. */
  private fun handleCommand(eng: StreamEngine, command: String) {
    val parts = command.split(' ', limit = 2)
    when (parts[0].uppercase()) {
      "SWITCH" -> eng.switchCamera()
      "FACING" -> eng.setFacing(if (parts.getOrNull(1)?.trim()?.lowercase() == "front") Facing.FRONT else Facing.BACK)
      "RES" -> parts.getOrNull(1)?.trim()?.toIntOrNull()?.let { eng.setResolution(it) }
      "LENS" -> parts.getOrNull(1)?.trim()?.toIntOrNull()?.let { eng.selectLens(it) }
    }
  }

  /**
   * Runs the foreground service while the setting is on and a PC is connected. It can only be
   * started while the app is visible, which is when the user connects and turns the screen off.
   */
  private fun updateBackgroundService() {
    val ctx = appCtx ?: return
    val connected = (server?.clientCount ?: 0) > 0
    if (started && keepStreaming && connected) {
      main.removeCallbacks(idleStop)
      if (!serviceOn && inForeground) {
        StreamService.start(ctx)
        serviceOn = true
        emitStatus()
      }
    } else if (serviceOn) {
      if (!started || !keepStreaming) {
        stopBackgroundService()
      } else {
        main.removeCallbacks(idleStop)
        main.postDelayed(idleStop, IDLE_STOP_MS)
      }
    }
  }

  private fun stopBackgroundService() {
    main.removeCallbacks(idleStop)
    if (!serviceOn) return
    serviceOn = false
    appCtx?.let { StreamService.stop(it) }
    if (!inForeground) {
      engine?.stop()
      orientation?.disable()
    }
    emitStatus()
  }

  /** *Stop* in the notification: stop for now; opening MobCam again resumes as usual. */
  private fun onNotificationStop() {
    stopBackgroundService()
    if (!inForeground) {
      notice = "Background streaming was stopped from the notification."
      emitStatus()
    }
  }

  private fun emitStatus() {
    sendEvent(STATUS_EVENT, statusMap())
  }

  private fun pairingMap(r: PairingRequest): Map<String, Any> =
    mapOf("id" to r.id, "pcName" to r.pc.name, "code" to r.code)

  private fun statusMap(): Map<String, Any?> {
    val s = engineState
    val total = server?.clientCount ?: 0
    val usb = server?.usbClientCount ?: 0
    return mapOf(
      "active" to (s?.active ?: false),
      "facing" to (s?.facing?.js ?: "back"),
      "lens" to (s?.lensIndex ?: -1),
      "lenses" to (s?.lenses ?: emptyList()).map { l ->
        mapOf("label" to l.label, "short" to l.shortLabel, "zoom" to l.zoom.toDouble(), "front" to l.front)
      },
      "targetHeight" to (s?.targetHeight ?: 720),
      "width" to (s?.width ?: 0),
      "height" to (s?.height ?: 0),
      "error" to s?.error,
      "notice" to notice,
      "clients" to total,
      "usbClients" to usb,
      "wifiClients" to total - usb,
      "addresses" to Mobcam.localAddresses(),
      "port" to Mobcam.STREAM_PORT,
      "backgroundStreaming" to keepStreaming,
      "backgroundActive" to serviceOn,
    )
  }

  /** Keeps the screen awake and hides the status/navigation bars (swipe to reveal). */
  private fun configureWindow() {
    val activity = appContext.currentActivity ?: return
    activity.runOnUiThread {
      val window = activity.window
      window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
        window.insetsController?.let {
          it.hide(WindowInsets.Type.systemBars())
          it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
      } else {
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = (
          View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
          )
      }
    }
  }
}
