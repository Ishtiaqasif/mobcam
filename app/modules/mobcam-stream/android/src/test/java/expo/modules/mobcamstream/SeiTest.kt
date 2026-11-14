package expo.modules.mobcamstream

import expo.modules.mobcamstream.Fixtures.bytes
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The phone's SEI bytes against tests/fixtures/sei.txt, which the PC's parser is tested against. */
class SeiTest {
  private val v = Fixtures.load("sei.txt")

  @Test fun metadataMatchesFixture() {
    for (i in 0..1) {
      val p = "metadata$i"
      val nal = Sei.metadata(
        rotation = v.getValue("$p.rotation").toInt(),
        front = v.getValue("$p.facing") == "f",
        targetHeight = v.getValue("$p.height").toInt(),
        lensIndex = v.getValue("$p.lens").toInt(),
        targetFps = v.getValue("$p.fps").toInt(),
        maxFps = v.getValue("$p.maxfps").toInt(),
      )
      assertArrayEquals(p, v.bytes("$p.nal"), nal)
    }
  }

  @Test fun lensListMatchesFixture() {
    val lenses = (0 until 4).map { i ->
      val (label, zoom, facing) = v.getValue("lens$i").split('|')
      Lens(cameraId = "0", front = facing == "f", zoom = zoom.toFloat(), label = label, equivalentFocal = 26f)
    }
    assertArrayEquals(v.bytes("lenses.nal"), Sei.lensList(lenses))
  }

  @Test fun separatorsInLabelsCannotBreakTheList() {
    val nal = Sei.lensList(listOf(Lens("0", false, 1f, "Wide; main, 1x", 26f)))
    val text = String(nal.copyOfRange(7 + 16, nal.size - 1), Charsets.US_ASCII)
    assertTrue(text, text == "Wide  main  1x,1,b")
  }

  @Test fun longLensListsStayInOneSei() {
    val many = (1..40).map { Lens("$it", false, it.toFloat(), "Telephoto lens number $it", 26f * it) }
    val nal = Sei.lensList(many)
    assertTrue((nal[6].toInt() and 0xFF) < 255)
  }

  @Test fun metadataHasNoStartCodeEmulation() {
    val nal = Sei.metadata(rotation = 0, front = false, targetHeight = 480, lensIndex = 0, targetFps = 0, maxFps = 0)
    for (i in 4 until nal.size - 2) assertTrue(!(nal[i] == 0.toByte() && nal[i + 1] == 0.toByte()))
  }
}
