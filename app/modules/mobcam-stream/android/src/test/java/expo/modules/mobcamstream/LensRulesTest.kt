package expo.modules.mobcamstream

import org.junit.Assert.assertEquals
import org.junit.Test

class LensRulesTest {
  private fun back(focal: Float, zoom: Float = 1f, id: String = "0") = Lens(id, false, zoom, "", focal)
  private fun front(focal: Float, id: String = "1") = Lens(id, true, 1f, "", focal)

  @Test fun zoomStopsMergeNearDuplicatesAndSnapTheMainLensTo1x() {
    assertEquals(listOf(0.6f, 1f, 3.4f), LensRules.distinctZooms(listOf(3.5f, 0.6f, 1.02f, 3.4f, 1f)))
    // A logical camera whose main lens reports 0.97x: LensCatalog always adds 1x.
    assertEquals(listOf(0.6f, 1f), LensRules.distinctZooms(listOf(0.6f, 0.97f, 1f)))
  }

  @Test fun duplicateLensesAreDroppedAndBackLensesComeFirst() {
    val result = LensRules.dedupe(
      listOf(front(24f), back(77f, id = "2"), back(26f, id = "0"), back(27f, id = "3"), back(13f, id = "4")),
    )
    assertEquals(listOf("4", "0", "2", "1"), result.map { it.cameraId })
  }

  @Test fun typicalPhone() {
    val labels = LensRules.label(listOf(back(13f, 0.6f), back(26f), back(77f, 3f), front(24f))).map { it.label }
    assertEquals(listOf("Ultra-wide", "Wide", "Telephoto", "Front"), labels)
  }

  @Test fun repeatedNamesGetZoomOrNumber() {
    val labels = LensRules.label(listOf(back(70f, 3f), back(120f, 5f), front(24f), front(30f, "5"))).map { it.label }
    assertEquals(listOf("Telephoto 3×", "Telephoto 5×", "Front 1", "Front 2"), labels)
  }

  @Test fun chipText() {
    assertEquals("0.6×", back(13f, 0.6f).shortLabel)
    assertEquals("1×", back(26f).shortLabel)
    assertEquals("3.4×", back(88f, 3.4f).shortLabel)
    assertEquals("Front", front(24f).copy(label = "Front").shortLabel)
  }

  @Test fun zoomFormatting() {
    assertEquals("0.67", formatZoom(0.67f))
    assertEquals("0.6", formatZoom(0.6f))
    assertEquals("1", formatZoom(1f))
    assertEquals("2", formatZoom(2f))
    assertEquals("3.4", formatZoom(3.4f))
  }
}
