package expo.modules.mobcamstream

import android.content.Context
import android.util.Size
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import expo.modules.kotlin.AppContext
import expo.modules.kotlin.views.ExpoView

/**
 * Live camera preview. A SurfaceView lets the system compositor handle rotation and
 * front-camera mirroring; the streamed video itself is never mirrored.
 */
class MobcamPreviewView(context: Context, appContext: AppContext) :
  ExpoView(context, appContext),
  SurfaceHolder.Callback {

  override val shouldUseAndroidLayout = true

  private val engine = Mobcam.engine(context)
  private val surfaceView = SurfaceView(context)
  private val sizeListener: (Size) -> Unit = { size ->
    post { surfaceView.holder.setFixedSize(size.width, size.height) }
  }

  init {
    addView(
      surfaceView,
      ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
    )
    surfaceView.holder.addCallback(this)
  }

  override fun onAttachedToWindow() {
    super.onAttachedToWindow()
    engine.onPreviewSizeWanted = sizeListener
    engine.currentSize?.let { surfaceView.holder.setFixedSize(it.width, it.height) }
  }

  override fun onDetachedFromWindow() {
    if (engine.onPreviewSizeWanted === sizeListener) engine.onPreviewSizeWanted = null
    super.onDetachedFromWindow()
  }

  override fun surfaceCreated(holder: SurfaceHolder) = Unit

  override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
    engine.attachPreview(holder.surface, width, height)
  }

  override fun surfaceDestroyed(holder: SurfaceHolder) {
    engine.detachPreview()
  }
}
