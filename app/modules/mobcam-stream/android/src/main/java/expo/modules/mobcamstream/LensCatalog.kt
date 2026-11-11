package expo.modules.mobcamstream

import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.os.Build
import android.util.Log
import android.util.Range
import java.util.Locale
import kotlin.math.abs
import kotlin.math.hypot

/**
 * One selectable lens. [zoom] is the CONTROL_ZOOM_RATIO to apply on [cameraId]; for a logical
 * multi-camera (several physical lenses behind one ID) the HAL switches to the matching
 * physical lens on its own (e.g. 0.6x -> ultra-wide, 3x -> telephoto).
 */
data class Lens(
  val cameraId: String,
  val front: Boolean,
  val zoom: Float,
  val label: String,
  val equivalentFocal: Float,
) {
  /** Short chip text: "0.6×", "1×", "3.4×" or "Front". */
  val shortLabel: String
    get() = if (front) label else "${formatZoom(zoom)}×"
}

fun formatZoom(z: Float): String {
  val s = String.format(Locale.US, if (z < 1f) "%.2f" else "%.1f", z).trimEnd('0').trimEnd('.')
  return if (s.isEmpty()) "1" else s
}

object LensCatalog {
  private const val TAG = "MobcamLenses"

  fun enumerate(manager: CameraManager): List<Lens> {
    val ids = try { manager.cameraIdList.toList() } catch (e: Exception) { emptyList() }
    val chars = ids.mapNotNull { id -> runCatching { id to manager.getCameraCharacteristics(id) }.getOrNull() }.toMap()

    // Physical members of a listed logical camera are reached through its zoom ratio instead.
    val members = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      chars.values.flatMap { it.physicalCameraIds }.toSet()
    } else emptySet()

    val found = mutableListOf<Lens>()
    for ((id, c) in chars) {
      if (id in members) continue
      val facing = c.get(CameraCharacteristics.LENS_FACING) ?: continue
      if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) continue
      val front = facing == CameraCharacteristics.LENS_FACING_FRONT
      val base = equivalentFocal(c) ?: 26f

      val zoomRange: Range<Float>? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) else null
      val physical = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) c.physicalCameraIds else emptySet()

      if (physical.size > 1 && zoomRange != null) {
        // Logical multi-camera: one option per physical lens, expressed as a zoom ratio.
        val zooms = physical.mapNotNull { pid ->
          runCatching { equivalentFocal(manager.getCameraCharacteristics(pid)) }.getOrNull()
        }.map { (it / base).coerceIn(zoomRange.lower, zoomRange.upper) } + 1f
        for (z in distinctZooms(zooms)) found += Lens(id, front, z, "", base * z)
      } else {
        found += Lens(id, front, 1f, "", base)
      }
    }

    // Same facing and nearly the same field of view = the same lens listed twice.
    val unique = mutableListOf<Lens>()
    for (lens in found.sortedWith(compareBy<Lens>({ it.front }, { it.equivalentFocal }, { it.cameraId }))) {
      val dup = unique.any { it.front == lens.front && abs(it.equivalentFocal / lens.equivalentFocal - 1f) < 0.08f }
      if (!dup) unique += lens
    }

    val labelled = label(unique)
    Log.i(TAG, "Lenses: " + labelled.joinToString { "${it.label} ${formatZoom(it.zoom)}x (id ${it.cameraId}, ${it.equivalentFocal.toInt()}mm)" })
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

  private fun distinctZooms(zooms: List<Float>): List<Float> {
    val out = mutableListOf<Float>()
    for (z in zooms.sorted()) {
      if (out.isEmpty() || z / out.last() > 1.15f) out += z
      else if (abs(z - 1f) < 0.05f) out[out.lastIndex] = 1f  // prefer exactly 1x for the main lens
    }
    return out
  }

  private fun label(lenses: List<Lens>): List<Lens> {
    val base = lenses.map { l ->
      when {
        l.front -> "Front"
        l.equivalentFocal < 20f -> "Ultra-wide"
        l.equivalentFocal < 40f -> "Wide"
        else -> "Telephoto"
      }
    }
    return lenses.mapIndexed { i, l ->
      val name = base[i]
      val clash = base.count { it == name } > 1
      val label = when {
        !clash -> name
        l.front -> "Front ${lenses.take(i + 1).count { it.front }}"
        else -> "$name ${formatZoom(l.zoom)}×"
      }
      l.copy(label = label)
    }
  }
}
