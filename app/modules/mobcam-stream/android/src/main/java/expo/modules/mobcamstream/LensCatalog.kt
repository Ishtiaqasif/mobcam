package expo.modules.mobcamstream

import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Build
import android.util.Log
import android.util.Range
import kotlin.math.hypot

object LensCatalog {
  private const val TAG = "MobcamLenses"

  fun enumerate(manager: CameraManager): List<Lens> {
    val ids = try { manager.cameraIdList.toList() } catch (e: Exception) { emptyList() }
    val chars = ids.mapNotNull { id -> runCatching { id to manager.getCameraCharacteristics(id) }.getOrNull() }.toMap()

    // Physical members of a listed logical camera are reached through its zoom ratio instead.
    val members = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      chars.values.flatMap { it.physicalCameraIds }.toSet()
    } else {
      emptySet()
    }

    val found = mutableListOf<Lens>()
    for ((id, c) in chars) {
      if (id in members) continue
      val facing = c.get(CameraCharacteristics.LENS_FACING) ?: continue
      if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) continue
      val front = facing == CameraCharacteristics.LENS_FACING_FRONT
      val base = equivalentFocal(c) ?: 26f

      val zoomRange: Range<Float>? =
        if (Build.VERSION.SDK_INT >=
          Build.VERSION_CODES.R
        ) {
          c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
        } else {
          null
        }
      val physical = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) c.physicalCameraIds else emptySet()

      if (physical.size > 1 && zoomRange != null) {
        // Logical multi-camera: one option per physical lens, expressed as a zoom ratio.
        val zooms = physical.mapNotNull { pid ->
          runCatching { equivalentFocal(manager.getCameraCharacteristics(pid)) }.getOrNull()
        }.map { (it / base).coerceIn(zoomRange.lower, zoomRange.upper) } + 1f
        for (z in LensRules.distinctZooms(zooms)) found += Lens(id, front, z, "", base * z)
      } else {
        found += Lens(id, front, 1f, "", base)
      }
    }

    val labelled = LensRules.label(LensRules.dedupe(found))
    Log.i(
      TAG,
      "Lenses: " +
        labelled.joinToString {
          "${it.label} ${formatZoom(it.zoom)}x (id ${it.cameraId}, ${it.equivalentFocal.toInt()}mm)"
        },
    )
    return labelled
  }

  /** 35mm-equivalent focal length, a field-of-view measure comparable across sensors. */
  private fun equivalentFocal(c: CameraCharacteristics): Float? {
    val focal = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.minOrNull() ?: return null
    val size = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: return null
    val diagonal = hypot(size.width, size.height)
    if (diagonal <= 0f) return null
    return focal * 43.27f / diagonal
  }
}
