package expo.modules.mobcamstream

/**
 * MobCam's metadata in the H.264 stream: SEI "user data unregistered" NAL units that decoders
 * ignore and the MobCam PC app reads (docs/protocol.md). Pure functions, so the bytes are unit
 * tested against tests/fixtures/sei.txt, which the PC's parser is tested against too.
 */
object Sei {
  /** 16-byte UUID of the metadata SEI. */
  val METADATA_UUID: ByteArray = "MOBCAM-ROTATION!".toByteArray(Charsets.US_ASCII)

  /** 16-byte UUID of the lens list SEI. */
  val LENS_LIST_UUID: ByteArray = "MOBCAM-LENSLIST!".toByteArray(Charsets.US_ASCII)

  /** H.264 access unit delimiter (primary_pic_type = 7); ends every frame. */
  val AUD = byteArrayOf(0, 0, 0, 1, 0x09, 0xF0.toByte())

  /**
   * Metadata sent with every frame. Payload after the UUID is ASCII: rotation digit ('0'..'3',
   * quarter turns clockwise), facing ('b'/'f'), 4-digit target height ("0720"), 2-digit lens index,
   * 2-digit frame rate setting ("30"/"60") and 2-digit highest frame rate the current lens offers at
   * the current size.
   */
  fun metadata(
    rotation: Int,
    front: Boolean,
    targetHeight: Int,
    lensIndex: Int,
    targetFps: Int,
    maxFps: Int,
  ): ByteArray = userData(METADATA_UUID, metadataText(rotation, front, targetHeight, lensIndex, targetFps, maxFps))

  fun metadataText(
    rotation: Int,
    front: Boolean,
    targetHeight: Int,
    lensIndex: Int,
    targetFps: Int,
    maxFps: Int,
  ): String = "${(rotation / 90) and 3}${if (front) 'f' else 'b'}" +
    targetHeight.coerceIn(0, 9999).toString().padStart(4, '0') +
    lensIndex.coerceIn(0, 99).toString().padStart(2, '0') +
    targetFps.coerceIn(0, 99).toString().padStart(2, '0') +
    maxFps.coerceIn(0, 99).toString().padStart(2, '0')

  /**
   * "label,zoom,b|f;..." in lens-index order, e.g. "Ultra-wide,0.6,b;Wide,1,b;Front,1,f". Sent
   * with keyframes. Lenses that don't fit in one SEI (payload under 255 bytes) are left out.
   */
  fun lensList(lenses: List<Lens>): ByteArray {
    val sb = StringBuilder()
    for (lens in lenses) {
      val entry = "${lens.label.replace(
        ',',
        ' ',
      ).replace(';', ' ')},${formatZoom(lens.zoom)},${if (lens.front) 'f' else 'b'}"
      if (sb.length + entry.length + 1 > 220) break
      if (sb.isNotEmpty()) sb.append(';')
      sb.append(entry)
    }
    return userData(LENS_LIST_UUID, sb.toString())
  }

  /**
   * Annex-B SEI NAL: payload type 5, one-byte size, UUID, ASCII text, RBSP stop bit. The ASCII
   * text never contains a 00 00 byte pair, so no emulation prevention is needed.
   */
  fun userData(uuid: ByteArray, text: String): ByteArray {
    val payload = uuid + text.toByteArray(Charsets.US_ASCII)
    require(payload.size < 255) { "SEI payload too long" }
    return byteArrayOf(0, 0, 0, 1, 0x06, 0x05, payload.size.toByte()) + payload + byteArrayOf(0x80.toByte())
  }
}
