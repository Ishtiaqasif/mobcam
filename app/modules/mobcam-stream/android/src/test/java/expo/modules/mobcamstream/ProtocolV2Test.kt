package expo.modules.mobcamstream

import expo.modules.mobcamstream.Fixtures.bytes
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.math.BigInteger
import java.security.KeyFactory
import java.security.PrivateKey
import java.security.Signature
import java.security.spec.ECPrivateKeySpec

/** Key schedule and records against tests/fixtures/protocol-v2.txt (the PC is tested against it too). */
class ProtocolV2Test {
  private val v = Fixtures.load("protocol-v2.txt")

  private fun privateKey(key: String): PrivateKey =
    KeyFactory.getInstance("EC").generatePrivate(ECPrivateKeySpec(BigInteger(1, v.bytes(key)), Crypto.p256))

  @Test fun ecdhMatchesOnBothSides() {
    val shared = v.bytes("shared_secret")
    val pcPublic = Crypto.decodePoint(v.bytes("pc_ephemeral_public"))
    val phonePublic = Crypto.decodePoint(v.bytes("phone_ephemeral_public"))
    assertArrayEquals(shared, Crypto.ecdh(privateKey("phone_ephemeral_private"), pcPublic))
    assertArrayEquals(shared, Crypto.ecdh(privateKey("pc_ephemeral_private"), phonePublic))
  }

  @Test fun pointEncodingRoundTrips() {
    val point = v.bytes("phone_ephemeral_public")
    assertArrayEquals(point, Crypto.encodePoint(Crypto.decodePoint(point)))
  }

  @Test(expected = Exception::class) fun pointOffTheCurveIsRejected() {
    val bad = v.bytes("phone_ephemeral_public").also { it[40] = (it[40].toInt() xor 1).toByte() }
    Crypto.decodePoint(bad)
  }

  @Test fun sessionKeysAndCode() {
    val keys = Crypto.deriveKeys(v.bytes("transcript_hash"), v.bytes("shared_secret"))
    assertArrayEquals(v.bytes("key_pc_to_phone"), keys.pcToPhone)
    assertArrayEquals(v.bytes("key_phone_to_pc"), keys.phoneToPc)
    assertEquals(v.getValue("code").toInt(), keys.code)
    assertEquals(v.getValue("code").let { "${it.take(3)} ${it.drop(3)}" }, Crypto.formatCode(keys.code))
  }

  @Test fun pairingCodeKeepsLeadingZeros() {
    assertEquals("000 042", Crypto.formatCode(42))
  }

  @Test fun sealedVideoRecordsMatchThePcsExpectation() {
    val cipher = RecordCipher(v.bytes("key_phone_to_pc"))
    assertArrayEquals(v.bytes("video0_record"), cipher.seal(Proto.REC_VIDEO, v.bytes("video0_payload")))
    assertArrayEquals(v.bytes("video1_record"), cipher.seal(Proto.REC_VIDEO, v.bytes("video1_payload")))
  }

  @Test fun opensThePcsRecordsInOrder() {
    val cipher = RecordCipher(v.bytes("key_pc_to_phone"))
    val (type, payload) = open(cipher, v.bytes("command0_record"))
    assertEquals(Proto.REC_COMMAND, type)
    assertArrayEquals(v.bytes("command0_payload"), payload)
    val (pairedType, pairedPayload) = open(cipher, v.bytes("paired1_record"))
    assertEquals(Proto.REC_PAIRED, pairedType)
    assertEquals(0, pairedPayload.size)
  }

  @Test(expected = ProtocolException::class) fun tamperedRecordIsRejected() {
    val record = v.bytes("command0_record").also { it[6] = (it[6].toInt() xor 1).toByte() }
    open(RecordCipher(v.bytes("key_pc_to_phone")), record)
  }

  @Test(expected = ProtocolException::class) fun recordOutOfOrderIsRejected() {
    open(RecordCipher(v.bytes("key_pc_to_phone")), v.bytes("paired1_record"))
  }

  @Test fun signatureFormatsRoundTrip() {
    val message = "MOBCAM v2 phone auth".toByteArray() + v.bytes("transcript_hash")
    val der = Signature.getInstance("SHA256withECDSA").run {
      initSign(privateKey("phone_ephemeral_private"))
      update(message)
      sign()
    }
    val raw = Crypto.derToRaw(der)
    assertEquals(Proto.SIG_LEN, raw.size)
    val publicKey = Crypto.decodePoint(v.bytes("phone_ephemeral_public"))
    assertTrue(Crypto.verify(publicKey, message, raw))
    assertFalse(Crypto.verify(publicKey, message + byteArrayOf(0), raw))
  }

  private fun open(cipher: RecordCipher, record: ByteArray) =
    cipher.open(record.copyOfRange(0, 4), record.copyOfRange(4, record.size))
}
