package expo.modules.mobcamstream

import android.content.Context
import android.os.Build
import android.view.OrientationEventListener
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

private const val STATUS_EVENT = "onStatus"

class MobcamStreamModule : Module() {
  private var engine: StreamEngine? = null
  private var server: StreamServer? = null
  private var discovery: DiscoveryResponder? = null
  private var orientation: OrientationEventListener? = null
  @Volatile private var engineState: EngineState? = null
  private var started = false

  private val context: Context
    get() = requireNotNull(appContext.reactContext) { "React context is gone" }.applicationContext

  override fun definition() = ModuleDefinition {
    Name("MobcamStream")

    Events(STATUS_EVENT)

    /** Starts the camera, encoder, TCP server and discovery responder. Idempotent. */
    Function("start") {
      val ctx = context
      val eng = engine ?: Mobcam.engine(ctx).also { engine = it }
      if (server == null) {
        server = StreamServer(
          port = Mobcam.STREAM_PORT,
          onClientsChanged = { emitStatus() },
          onKeyFrameNeeded = { eng.requestKeyFrame() },
          onCommand = { command -> handleCommand(eng, command) },
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

    Function("getStatus") { statusMap() }

    OnActivityEntersForeground {
      // Android takes the camera away from background apps; grab it back on return.
      if (started) {
        engine?.start()
        orientation?.enable()
        configureWindow()
      }
    }

    OnActivityEntersBackground {
      if (started) {
        engine?.stop()
        orientation?.disable()
      }
    }

    OnDestroy {
      started = false
      orientation?.disable()
      orientation = null
      engine?.let { it.stop(); it.sink = null; it.onState = null }
      server?.stop()
      discovery?.stop()
      server = null
      discovery = null
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

  private fun emitStatus() {
    sendEvent(STATUS_EVENT, statusMap())
  }

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
      "clients" to total,
      "usbClients" to usb,
      "wifiClients" to total - usb,
      "addresses" to Mobcam.localAddresses(),
      "port" to Mobcam.STREAM_PORT,
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
