package expo.modules.mobcamstream

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs

private const val TAG = "MobcamEngine"
private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
private const val FPS = 30


enum class Facing(val js: String) { BACK("back"), FRONT("front") }

data class EngineState(
  val active: Boolean,
  val facing: Facing,
  val lensIndex: Int,
  val lenses: List<Lens>,
  val targetHeight: Int,
  val width: Int,
  val height: Int,
  val error: String?,
)

/** Receives encoded H.264 access units (Annex-B). Keyframes carry SPS/PPS in front. */
interface FrameSink {
  fun onFrame(data: ByteArray, keyFrame: Boolean)
}

/**
 * Owns the camera and the hardware H.264 encoder.
 *
 * One Camera2 capture session feeds two surfaces: the encoder's input surface and (when
 * the preview view is on screen) a SurfaceView. No frame is ever copied on the CPU.
 *
 * Every mutable field is confined to the camera [handler] thread.
 */
class StreamEngine(context: Context) {
  private val appContext = context.applicationContext
  private val cameraManager = appContext.getSystemService(Context.CAMERA_SERVICE) as CameraManager
  private val thread = HandlerThread("MobcamCamera").apply { start() }
  private val handler = Handler(thread.looper)

  private var wantRunning = false
  private var targetHeight = 720

  /** Every selectable lens (ultra-wide, wide, telephoto, front...). */
  val lenses: List<Lens> by lazy { LensCatalog.enumerate(cameraManager) }
  private var lensIndex = -1
  private var lastBackIndex = -1
  private var zoomSupported = false
  private var sessionTargets: List<Surface> = emptyList()
  private var device: CameraDevice? = null
  private var session: CameraCaptureSession? = null
  private var sessionToken = 0
  private var sessionHasPreview = false
  private var encoder: MediaCodec? = null
  private var encoderSurface: Surface? = null
  private var drainThread: Thread? = null
  private var fpsRange: Range<Int>? = null
  private var continuousAf = false
  private var previewSurface: Surface? = null
  private var previewSize: Size? = null
  private var generation = 0
  private var error: String? = null

  @Volatile private var draining = false

  // Rotation tagging (read by the encoder thread).
  @Volatile private var sensorOrientation = 90
  @Volatile private var frontFacing = false
  @Volatile private var tagTargetHeight = 720
  @Volatile private var tagLensIndex = 0
  @Volatile private var lensListSei = ByteArray(0)

  /** Physical device orientation in degrees (0/90/180/270), fed by an OrientationEventListener. */
  @Volatile var deviceOrientation = 0

  /** Size of the current stream, or null while stopped. */
  @Volatile var currentSize: Size? = null
    private set

  @Volatile var sink: FrameSink? = null
  @Volatile var onState: ((EngineState) -> Unit)? = null

  /** Asks the preview to resize its buffer to the stream size. Invoked on the camera thread. */
  @Volatile var onPreviewSizeWanted: ((Size) -> Unit)? = null

  // ---- Public API (thread-safe; work is posted to the camera thread) ----

  fun start() {
    handler.post { wantRunning = true; restart() }
  }

  fun stop() {
    handler.post { wantRunning = false; teardown(); publish() }
  }

  /** Toggles between the last used back lens and the front camera. */
  fun switchCamera() {
    handler.post {
      ensureLens()
      val toFront = lenses.getOrNull(lensIndex)?.front != true
      setFacingLocked(if (toFront) Facing.FRONT else Facing.BACK)
    }
  }

  fun setFacing(value: Facing) {
    handler.post { ensureLens(); setFacingLocked(value) }
  }

  fun selectLens(index: Int) {
    handler.post { ensureLens(); selectLensLocked(index) }
  }

  private fun setFacingLocked(value: Facing) {
    val current = lenses.getOrNull(lensIndex)
    if (current != null && current.front == (value == Facing.FRONT)) return
    val target = if (value == Facing.FRONT) {
      lenses.indexOfFirst { it.front }
    } else {
      lastBackIndex.takeIf { lenses.getOrNull(it)?.front == false } ?: defaultBackIndex()
    }
    if (target >= 0) selectLensLocked(target)
  }

  private fun selectLensLocked(index: Int) {
    if (index !in lenses.indices || index == lensIndex) return
    val previous = lenses.getOrNull(lensIndex)
    val next = lenses[index]
    lensIndex = index
    if (!next.front) lastBackIndex = index
    tagLensIndex = index
    when {
      !wantRunning -> publish()
      // Same camera, different lens: just change the zoom ratio, no restart.
      previous?.cameraId == next.cameraId && session != null && zoomSupported -> {
        frontFacing = next.front
        applyRequest()
        publish()
      }
      else -> restart()
    }
  }

  private fun defaultBackIndex(): Int {
    val back = lenses.withIndex().filter { !it.value.front }
    return (back.firstOrNull { abs(it.value.zoom - 1f) < 0.01f } ?: back.firstOrNull())?.index
      ?: if (lenses.isNotEmpty()) 0 else -1
  }

  private fun ensureLens() {
    if (lensIndex < 0) {
      lensIndex = defaultBackIndex()
      if (lenses.getOrNull(lensIndex)?.front == false) lastBackIndex = lensIndex
      tagLensIndex = lensIndex.coerceAtLeast(0)
    }
  }

  fun setResolution(height: Int) {
    handler.post {
      targetHeight = height.coerceIn(240, 1080)
      if (wantRunning) restart() else publish()
    }
  }

  fun requestKeyFrame() {
    handler.post {
      try {
        encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
      } catch (e: Exception) {
        Log.w(TAG, "Key frame request failed", e)
      }
    }
  }

  fun attachPreview(surface: Surface, width: Int, height: Int) {
    handler.post {
      previewSurface = surface
      previewSize = Size(width, height)
      val size = currentSize
      if (size != null && size != previewSize) onPreviewSizeWanted?.invoke(size)
      if (device != null && previewUsable() != sessionHasPreview) createSession()
    }
  }

  /** Blocks until the camera stops drawing into the preview surface (required by SurfaceHolder). */
  fun detachPreview() {
    val done = CountDownLatch(1)
    handler.post {
      previewSurface = null
      previewSize = null
      if (sessionHasPreview && device != null) createSession()
      done.countDown()
    }
    done.await(2, TimeUnit.SECONDS)
  }

  // ---- Camera thread internals ----

  private fun previewUsable(): Boolean =
    previewSurface?.isValid == true && previewSize != null && previewSize == currentSize

  @SuppressLint("MissingPermission")
  private fun restart() {
    teardown()
    error = null
    val gen = generation
    try {
      if (appContext.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
        throw IllegalStateException("Camera permission not granted")
      }
      ensureLens()
      val lens = lenses.getOrNull(lensIndex) ?: throw IllegalStateException("No camera found on this device")
      val cameraId = lens.cameraId
      val chars = cameraManager.getCameraCharacteristics(cameraId)

      sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
      frontFacing = lens.front
      tagTargetHeight = targetHeight
      tagLensIndex = lensIndex
      zoomSupported = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
        chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null
      if (lensListSei.isEmpty()) lensListSei = Sei.lensList(lenses)

      val codec = MediaCodec.createEncoderByType(MIME)
      encoder = codec
      val size = chooseSize(chars, codec)
      codec.configure(encoderFormat(size), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
      encoderSurface = codec.createInputSurface()
      codec.start()
      startDrain(codec)

      fpsRange = chooseFpsRange(chars)
      continuousAf = chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
        ?.contains(CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO) == true
      currentSize = size
      onPreviewSizeWanted?.invoke(size)
      Log.i(TAG, "Starting ${lens.label} (camera $cameraId, ${formatZoom(lens.zoom)}x) at ${size.width}x${size.height}")

      cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
        override fun onOpened(camera: CameraDevice) {
          if (gen != generation) { camera.close(); return }
          device = camera
          createSession()
        }

        override fun onDisconnected(camera: CameraDevice) {
          camera.close()
          if (gen == generation) { device = null; fail("Camera disconnected (in use by another app?)") }
        }

        override fun onError(camera: CameraDevice, code: Int) {
          camera.close()
          if (gen == generation) { device = null; fail("Camera error $code") }
        }
      }, handler)
    } catch (e: Exception) {
      fail(e.message ?: e.toString())
      return
    }
    publish()
  }

  @Suppress("DEPRECATION")
  private fun createSession() {
    val camera = device ?: return
    val encSurface = encoderSurface ?: return
    closeSession()

    val withPreview = previewUsable()
    val targets = if (withPreview) listOf(encSurface, previewSurface!!) else listOf(encSurface)
    val token = ++sessionToken
    sessionHasPreview = withPreview
    sessionTargets = targets

    try {
      camera.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(s: CameraCaptureSession) {
          if (token != sessionToken || device !== camera) { s.close(); return }
          session = s
          if (applyRequest()) {
            error = null
            publish()
          }
        }

        override fun onConfigureFailed(s: CameraCaptureSession) {
          if (token == sessionToken) fail("Camera rejected the stream configuration")
        }
      }, handler)
    } catch (e: Exception) {
      fail("Could not create capture session: ${e.message}")
    }
  }

  /** (Re)issues the repeating capture request, including the current lens' zoom ratio. */
  private fun applyRequest(): Boolean {
    val camera = device ?: return false
    val s = session ?: return false
    return try {
      val request = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
        sessionTargets.forEach { addTarget(it) }
        fpsRange?.let { set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
        if (continuousAf) set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        if (zoomSupported && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
          set(CaptureRequest.CONTROL_ZOOM_RATIO, lenses.getOrNull(lensIndex)?.zoom ?: 1f)
        }
      }
      s.setRepeatingRequest(request.build(), null, handler)
      true
    } catch (e: Exception) {
      fail("Could not start capture: ${e.message}")
      false
    }
  }

  private fun closeSession() {
    session?.let {
      try { it.stopRepeating(); it.abortCaptures() } catch (_: Exception) {}
      try { it.close() } catch (_: Exception) {}
    }
    session = null
    sessionHasPreview = false
  }

  private fun teardown() {
    generation++
    sessionToken++
    closeSession()
    device?.close()
    device = null

    draining = false
    drainThread?.join(1000)
    drainThread = null
    encoder?.let {
      try { it.stop() } catch (_: Exception) {}
      try { it.release() } catch (_: Exception) {}
    }
    encoder = null
    encoderSurface?.release()
    encoderSurface = null
    currentSize = null
  }

  private fun fail(message: String) {
    Log.e(TAG, message)
    teardown()
    error = message
    publish()
  }

  private fun publish() {
    val size = currentSize
    ensureLens()
    val lens = lenses.getOrNull(lensIndex)
    onState?.invoke(
      EngineState(
        active = wantRunning && error == null,
        facing = if (lens?.front == true) Facing.FRONT else Facing.BACK,
        lensIndex = lensIndex,
        lenses = lenses,
        targetHeight = targetHeight,
        width = size?.width ?: 0,
        height = size?.height ?: 0,
        error = error,
      )
    )
  }

  /** Largest landscape size with height <= [targetHeight], preferring 16:9. Never above 1080p. */
  private fun chooseSize(chars: CameraCharacteristics, codec: MediaCodec): Size {
    val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
      ?: throw IllegalStateException("Camera reports no stream configurations")
    val caps = codec.codecInfo.getCapabilitiesForType(MIME).videoCapabilities
    val previewSizes = map.getOutputSizes(SurfaceHolder::class.java)?.toSet() ?: emptySet()
    val maxWidth = targetHeight * 16 / 9 + 16

    val candidates = (map.getOutputSizes(MediaCodec::class.java) ?: emptyArray())
      .filter { it.height <= targetHeight && it.width <= maxWidth && it.width >= it.height }
      .filter { caps?.isSizeSupported(it.width, it.height) != false }
    if (candidates.isEmpty()) throw IllegalStateException("No usable size up to ${targetHeight}p")

    val wide = candidates.filter { abs(it.width.toFloat() / it.height - 16f / 9f) < 0.05f }
    val pool = wide.ifEmpty { candidates }
    val previewable = pool.filter { it in previewSizes }.ifEmpty { pool }
    return previewable.maxBy { it.width * it.height }
  }

  private fun chooseFpsRange(chars: CameraCharacteristics): Range<Int>? {
    val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
    return ranges.filter { it.upper == FPS }.maxByOrNull { it.lower }
      ?: ranges.filter { it.upper <= FPS }.maxByOrNull { it.upper }
  }

  private fun encoderFormat(size: Size): MediaFormat {
    val bitrate = when {
      size.height >= 1080 -> 8_000_000
      size.height >= 720 -> 4_000_000
      else -> 2_000_000
    }
    return MediaFormat.createVideoFormat(MIME, size.width, size.height).apply {
      setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
      setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
      setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
      setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
      setInteger(MediaFormat.KEY_PRIORITY, 0) // realtime
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0) // B-frames add latency
      }
    }
  }

  private fun startDrain(codec: MediaCodec) {
    draining = true
    drainThread = Thread({ drain(codec) }, "MobcamEncoder").apply { start() }
  }

  /** Runs on its own thread for the lifetime of one encoder instance. */
  private fun drain(codec: MediaCodec) {
    val info = MediaCodec.BufferInfo()
    var config: ByteArray? = null
    try {
      while (draining) {
        val index = codec.dequeueOutputBuffer(info, 20_000)
        if (index >= 0) {
          val buffer = codec.getOutputBuffer(index)
          if (buffer != null && info.size > 0) {
            val bytes = ByteArray(info.size)
            buffer.position(info.offset)
            buffer.get(bytes, 0, info.size)
            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
              config = bytes
            } else {
              val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
              val cfg = config
              val sei = rotationSei(currentRotation())
              // Trailing access unit delimiter marks the frame as complete for the receiver.
              // Keyframes also carry the lens list so a newly connected PC learns it within ~1 s.
              val frame = if (key && cfg != null) cfg + lensListSei + sei + bytes + Sei.AUD else sei + bytes + Sei.AUD
              sink?.onFrame(frame, key)
            }
          }
          codec.releaseOutputBuffer(index, false)
        } else if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED && config == null) {
          val format = codec.outputFormat
          val sps = format.getByteBuffer("csd-0")
          val pps = format.getByteBuffer("csd-1")
          if (sps != null && pps != null) config = sps.toBytes() + pps.toBytes()
        }
      }
    } catch (e: IllegalStateException) {
      // Encoder was stopped underneath us during teardown.
    } catch (e: Exception) {
      Log.e(TAG, "Encoder drain failed", e)
    }
  }

  /**
   * Clockwise rotation (0/90/180/270) the receiver must apply to make the image upright.
   * Same formula Android uses for JPEG_ORIENTATION.
   */
  private fun currentRotation(): Int {
    val device = if (frontFacing) -deviceOrientation else deviceOrientation
    return ((sensorOrientation + device) % 360 + 360) % 360
  }

  private var seiKey = ""
  private var seiBytes = ByteArray(0)

  /** Metadata SEI for this frame (see [Sei.metadata]); rebuilt only when something changed. */
  private fun rotationSei(rotation: Int): ByteArray {
    val info = Sei.metadataText(rotation, frontFacing, tagTargetHeight, tagLensIndex)
    if (info == seiKey) return seiBytes
    seiBytes = Sei.userData(Sei.METADATA_UUID, info)
    seiKey = info
    return seiBytes
  }

  private fun ByteBuffer.toBytes(): ByteArray {
    val copy = duplicate()
    copy.rewind()
    return ByteArray(copy.remaining()).also { copy.get(it) }
  }
}
