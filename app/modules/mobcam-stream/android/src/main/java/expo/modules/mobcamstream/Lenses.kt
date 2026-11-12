package expo.modules.mobcamstream

import java.util.Locale
import kotlin.math.abs

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

/**
 * How camera hardware becomes the lens list the user sees. No Android calls, so it is unit tested
 * on the JVM; [LensCatalog] feeds it what Camera2 reports.
 */
object LensRules {
  /** Zoom stops of a logical multi-camera, nearly equal ones merged; the main lens is exactly 1×. */
  fun distinctZooms(zooms: List<Float>): List<Float> {
    val out = mutableListOf<Float>()
    for (z in zooms.sorted()) {
      if (out.isEmpty() || z / out.last() > 1.15f) out += z
      else if (abs(z - 1f) < 0.05f) out[out.lastIndex] = 1f
    }
    return out
  }

  /** Same facing and nearly the same field of view = the same lens listed twice. Sorted back first, wide to tele. */
  fun dedupe(found: List<Lens>): List<Lens> {
    val unique = mutableListOf<Lens>()
    for (lens in found.sortedWith(compareBy<Lens>({ it.front }, { it.equivalentFocal }, { it.cameraId }))) {
      val dup = unique.any { it.front == lens.front && abs(it.equivalentFocal / lens.equivalentFocal - 1f) < 0.08f }
      if (!dup) unique += lens
    }
    return unique
  }

  /** "Ultra-wide" / "Wide" / "Telephoto" / "Front", numbered or zoom-tagged when names repeat. */
  fun label(lenses: List<Lens>): List<Lens> {
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
