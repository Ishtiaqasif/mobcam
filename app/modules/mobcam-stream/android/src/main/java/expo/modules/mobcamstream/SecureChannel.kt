package expo.modules.mobcamstream

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.math.BigInteger
import java.net.Socket
import java.net.SocketTimeoutException
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.PublicKey
import java.security.SecureRandom
import java.security.Signature
import java.security.interfaces.ECPublicKey
import java.security.spec.ECFieldFp
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import java.util.Locale
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** Protocol v2 constants. See docs/protocol.md. */
object Proto {
  const val VERSION = 2
  val MAGIC = "MOBCAM".toByteArray(Charsets.US_ASCII)

  const val MSG_COMMIT = 0x01
  const val MSG_HELLO = 0x02
  const val MSG_REVEAL = 0x03
  const val MSG_PC_AUTH = 0x04
  const val MSG_PAIRING = 0x05
  const val MSG_PHONE_AUTH = 0x06
  const val MSG_DENIED = 0x07

  const val REC_VIDEO = 0x10
  const val REC_COMMAND = 0x11

  /** PC → phone, empty: the PC's user confirmed the pairing code too (two-sided pairing). */
  const val REC_PAIRED = 0x12

  /** How long the phone waits for [REC_PAIRED] after its own user tapped Allow. */
  const val PC_CONFIRM_TIMEOUT_MS = 65_000

  const val DENIED_BY_USER = 1
  const val DENIED_TIMEOUT = 2
  const val DENIED_BUSY = 3
  const val DENIED_BAD_PC = 4

  const val MAX_MESSAGE = 1024
  const val MAX_RECORD = 4 * 1024 * 1024
  const val KEY_LEN = 65
  const val SIG_LEN = 64
  const val NONCE_LEN = 32
  const val MAX_NAME = 64

  val LABEL_COMMIT = "MOBCAM v2 commit".toByteArray(Charsets.US_ASCII)
  val LABEL_PC_AUTH = "MOBCAM v2 pc auth".toByteArray(Charsets.US_ASCII)
  val LABEL_PHONE_AUTH = "MOBCAM v2 phone auth".toByteArray(Charsets.US_ASCII)
  val LABEL_PC_TO_PHONE = "MOBCAM v2 pc->phone".toByteArray(Charsets.US_ASCII)
  val LABEL_PHONE_TO_PC = "MOBCAM v2 phone->pc".toByteArray(Charsets.US_ASCII)
  val LABEL_CODE = "MOBCAM v2 code".toByteArray(Charsets.US_ASCII)
}

class ProtocolException(message: String) : IOException(message)

/** The peer is not speaking protocol v2 (an old PC, or a different version). */
class VersionException(val peerVersion: Int?) : IOException("peer protocol version ${peerVersion ?: "none"}")

// ---------------------------------------------------------------- primitives

object Crypto {
  private val random = SecureRandom()

  val p256: ECParameterSpec by lazy {
    AlgorithmParameters.getInstance("EC").run {
      init(ECGenParameterSpec("secp256r1"))
      getParameterSpec(ECParameterSpec::class.java)
    }
  }

  fun randomBytes(n: Int) = ByteArray(n).also { random.nextBytes(it) }

  fun sha256(vararg parts: ByteArray): ByteArray =
    MessageDigest.getInstance("SHA-256").run { parts.forEach { update(it) }; digest() }

  fun hex(bytes: ByteArray) = bytes.joinToString("") { String.format(Locale.ROOT, "%02x", it) }

  /** Uncompressed X9.62 point, 65 bytes. */
  fun encodePoint(key: PublicKey): ByteArray {
    val w = (key as ECPublicKey).w
    return byteArrayOf(4) + unsigned32(w.affineX) + unsigned32(w.affineY)
  }

  fun decodePoint(bytes: ByteArray): PublicKey {
    if (bytes.size != Proto.KEY_LEN || bytes[0] != 4.toByte()) throw ProtocolException("bad public key")
    val x = BigInteger(1, bytes.copyOfRange(1, 33))
    val y = BigInteger(1, bytes.copyOfRange(33, 65))
    // Checked here rather than trusting the provider: not every KeyFactory validates the point
    // (the desktop JDK's doesn't), and an off-curve key would invite invalid-curve attacks.
    if (!onCurve(x, y)) throw ProtocolException("public key is not on the curve")
    return KeyFactory.getInstance("EC").generatePublic(ECPublicKeySpec(ECPoint(x, y), p256))
  }

  /** y² = x³ + ax + b (mod p), with both coordinates in the field. */
  private fun onCurve(x: BigInteger, y: BigInteger): Boolean {
    val curve = p256.curve
    val p = (curve.field as ECFieldFp).p
    if (x.signum() < 0 || x >= p || y.signum() < 0 || y >= p) return false
    val rhs = x.pow(3).add(curve.a.multiply(x)).add(curve.b).mod(p)
    return y.multiply(y).mod(p) == rhs
  }

  fun ephemeral(): KeyPair =
    KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1"), random) }.generateKeyPair()

  /** 32-byte big-endian X coordinate of the shared point. */
  fun ecdh(own: PrivateKey, peer: PublicKey): ByteArray =
    KeyAgreement.getInstance("ECDH").run { init(own); doPhase(peer, true); generateSecret() }

  fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray = Mac.getInstance("HmacSHA256").run {
    init(SecretKeySpec(key, "HmacSHA256")); parts.forEach { update(it) }; doFinal()
  }

  /** HKDF-Expand (RFC 5869) for up to 32 bytes. */
  fun expand(prk: ByteArray, info: ByteArray, length: Int): ByteArray {
    require(length <= 32)
    return hmac(prk, info, byteArrayOf(1)).copyOf(length)
  }

  class SessionKeys(val pcToPhone: ByteArray, val phoneToPc: ByteArray, val code: Int)

  /** HKDF over the ECDH secret keyed with the transcript hash: both record keys and the pairing code. */
  fun deriveKeys(transcriptHash: ByteArray, sharedSecret: ByteArray): SessionKeys {
    val prk = hmac(transcriptHash, sharedSecret)
    val code = (BigInteger(1, expand(prk, Proto.LABEL_CODE, 4)).toLong() % 1_000_000).toInt()
    return SessionKeys(expand(prk, Proto.LABEL_PC_TO_PHONE, 32), expand(prk, Proto.LABEL_PHONE_TO_PC, 32), code)
      .also { prk.fill(0) }
  }

  /** "482 913". Locale.ROOT: ASCII digits on every device, matching the PC (not Bengali/Arabic digits). */
  fun formatCode(code: Int): String = String.format(Locale.ROOT, "%03d %03d", code / 1000, code % 1000)

  fun verify(key: PublicKey, message: ByteArray, rawSignature: ByteArray): Boolean = try {
    Signature.getInstance("SHA256withECDSA").run {
      initVerify(key)
      update(message)
      verify(rawToDer(rawSignature))
    }
  } catch (e: Exception) {
    false
  }

  private fun unsigned32(v: BigInteger): ByteArray {
    val b = v.toByteArray()
    return when {
      b.size == 32 -> b
      b.size > 32 -> b.copyOfRange(b.size - 32, b.size)
      else -> ByteArray(32 - b.size) + b
    }
  }

  /** DER ECDSA-Sig-Value -> r ‖ s (64 bytes). */
  fun derToRaw(der: ByteArray): ByteArray {
    var i = 0
    fun readLen(): Int {
      var len = der[i++].toInt() and 0xFF
      if (len and 0x80 != 0) {
        val n = len and 0x7F
        len = 0
        repeat(n) { len = (len shl 8) or (der[i++].toInt() and 0xFF) }
      }
      return len
    }
    if (der[i++] != 0x30.toByte()) throw ProtocolException("bad signature")
    readLen()
    val parts = Array(2) {
      if (der[i++] != 0x02.toByte()) throw ProtocolException("bad signature")
      val len = readLen()
      BigInteger(1, der.copyOfRange(i, i + len)).also { i += len }
    }
    return unsigned32(parts[0]) + unsigned32(parts[1])
  }

  /** r ‖ s (64 bytes) -> DER ECDSA-Sig-Value. */
  fun rawToDer(raw: ByteArray): ByteArray {
    if (raw.size != Proto.SIG_LEN) throw ProtocolException("bad signature length")
    fun integer(bytes: ByteArray): ByteArray {
      val v = BigInteger(1, bytes).toByteArray() // minimal, with a leading 0 if the top bit is set
      return byteArrayOf(0x02, v.size.toByte()) + v
    }
    val body = integer(raw.copyOfRange(0, 32)) + integer(raw.copyOfRange(32, 64))
    return byteArrayOf(0x30, body.size.toByte()) + body
  }
}

/**
 * AES-256-GCM records: 4-byte big-endian length (also the AAD), then ciphertext and tag of
 * type ‖ payload. The nonce is the record counter, so each direction has its own instance.
 * Not thread-safe: one writer thread, one reader thread.
 */
class RecordCipher(key: ByteArray) {
  private val key = SecretKeySpec(key, "AES")
  private var counter = 0L

  /** One whole record, header included. */
  fun seal(type: Int, payload: ByteArray): ByteArray {
    val total = 1 + payload.size + 16
    val record = ByteArray(4 + total)
    record[0] = (total ushr 24).toByte(); record[1] = (total ushr 16).toByte()
    record[2] = (total ushr 8).toByte(); record[3] = total.toByte()
    val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    cipher.init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(128, nonce()))
    cipher.updateAAD(record, 0, 4)
    var n = 4 + cipher.update(byteArrayOf(type.toByte()), 0, 1, record, 4)
    n += cipher.update(payload, 0, payload.size, record, n)
    cipher.doFinal(record, n)
    return record
  }

  /** [sealed] = the record after its 4-byte [header]. Returns (type, payload); throws if it was tampered with. */
  fun open(header: ByteArray, sealed: ByteArray): Pair<Int, ByteArray> {
    val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, nonce()))
    cipher.updateAAD(header)
    val plain = try {
      cipher.doFinal(sealed)
    } catch (e: Exception) {
      throw ProtocolException("record authentication failed")
    }
    if (plain.isEmpty()) throw ProtocolException("empty record")
    return (plain[0].toInt() and 0xFF) to plain.copyOfRange(1, plain.size)
  }

  private fun nonce(): ByteArray {
    val c = counter++
    val n = ByteArray(12)
    for (i in 0 until 8) n[11 - i] = (c ushr (8 * i)).toByte()
    return n
  }
}

// ---------------------------------------------------------------- identity

/** This phone's long-term ECDSA P-256 key. The private half lives in Android Keystore. */
object Identity {
  private const val ALIAS = "mobcam-identity-v1"

  @Volatile private var cached: KeyPair? = null

  fun keyPair(): KeyPair = cached ?: synchronized(this) {
    cached ?: load().also { cached = it }
  }

  val publicKeyBytes: ByteArray get() = Crypto.encodePoint(keyPair().public)

  fun sign(message: ByteArray): ByteArray {
    val der = Signature.getInstance("SHA256withECDSA").run {
      initSign(keyPair().private)
      update(message)
      sign()
    }
    return Crypto.derToRaw(der)
  }

  private fun load(): KeyPair {
    val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    val existing = store.getEntry(ALIAS, null) as? KeyStore.PrivateKeyEntry
    if (existing != null) return KeyPair(existing.certificate.publicKey, existing.privateKey)
    val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY)
      .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
      .setDigests(KeyProperties.DIGEST_SHA256)
      .build()
    return KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore")
      .apply { initialize(spec) }
      .generateKeyPair()
  }
}

// ---------------------------------------------------------------- handshake

/** What the phone learnt about the PC during the handshake. */
class PcHello(val key: ByteArray, val name: String) {
  val id: String get() = Crypto.hex(Crypto.sha256(key))
}

/** Decides whether a PC may connect. Blocks (for the pairing prompt) and returns true to allow. */
fun interface PairingGate {
  /** @return null to allow, or a [Proto] DENIED reason. */
  fun admit(pc: PcHello, code: String, usb: Boolean, channel: SecureChannel): Int?
}

/**
 * Server side of the v2 handshake plus the encrypted record layer, for one TCP connection.
 *
 * Reads happen on one thread and writes on another after the handshake; each direction has its own
 * key and counter, so they never share mutable state.
 */
class SecureChannel(private val socket: Socket, private val phoneName: String) {
  private val input = DataInputStream(socket.getInputStream().buffered(64 * 1024))
  private val output: OutputStream = socket.getOutputStream()
  private val transcript = ByteArrayOutputStream()

  private lateinit var sendCipher: RecordCipher
  private lateinit var recvCipher: RecordCipher

  var pc: PcHello? = null
    private set

  /**
   * True when this phone's user allowed a new PC but the PC's user hasn't confirmed the code yet.
   * Set by the [PairingGate]; the stream server then holds video and trust until [Proto.REC_PAIRED].
   */
  @Volatile var pendingPairing = false

  /**
   * Runs the handshake. Throws on any failure (the caller just closes the socket).
   * Returns normally only when the PC is authenticated and allowed.
   */
  fun handshake(gate: PairingGate, usb: Boolean) {
    // Preambles. A v2 PC sends its preamble immediately. A v1 PC sends nothing, waits for video
    // and hangs up after ~3 s, so silence or a hang-up before the preamble means an old PC.
    socket.soTimeout = 2_500
    val pcPreamble = ByteArray(8)
    try {
      input.readFully(pcPreamble)
    } catch (e: SocketTimeoutException) {
      throw VersionException(null)
    } catch (e: EOFException) {
      throw VersionException(null)
    }
    socket.soTimeout = 10_000
    if (!pcPreamble.copyOfRange(0, 6).contentEquals(Proto.MAGIC)) throw VersionException(null)
    transcript.write(pcPreamble)
    val pcVersion = ((pcPreamble[6].toInt() and 0xFF) shl 8) or (pcPreamble[7].toInt() and 0xFF)
    val mine = Proto.MAGIC + byteArrayOf(0, Proto.VERSION.toByte())
    sendRaw(mine)
    if (pcVersion != Proto.VERSION) {
      output.flush()
      throw VersionException(pcVersion)
    }

    // COMMIT
    val commit = readMessage(Proto.MSG_COMMIT)
    if (commit.size < Proto.KEY_LEN + 32) throw ProtocolException("short COMMIT")
    val pcKeyBytes = commit.copyOfRange(0, Proto.KEY_LEN)
    val pcKey = Crypto.decodePoint(pcKeyBytes)
    val commitment = commit.copyOfRange(Proto.KEY_LEN, Proto.KEY_LEN + 32)
    val pcName = String(commit, Proto.KEY_LEN + 32, commit.size - Proto.KEY_LEN - 32, Charsets.UTF_8).take(64)

    // HELLO
    val eph = Crypto.ephemeral()
    val nPhone = Crypto.randomBytes(Proto.NONCE_LEN)
    val name = phoneName.toByteArray(Charsets.UTF_8).let {
      if (it.size >
        Proto.MAX_NAME
      ) {
        it.copyOf(Proto.MAX_NAME)
      } else {
        it
      }
    }
    sendMessage(Proto.MSG_HELLO, Identity.publicKeyBytes + Crypto.encodePoint(eph.public) + nPhone + name)

    // REVEAL
    val reveal = readMessage(Proto.MSG_REVEAL)
    if (reveal.size != Proto.KEY_LEN + Proto.NONCE_LEN) throw ProtocolException("bad REVEAL")
    val ePcBytes = reveal.copyOfRange(0, Proto.KEY_LEN)
    val nPc = reveal.copyOfRange(Proto.KEY_LEN, reveal.size)
    val h = Crypto.sha256(transcript.toByteArray())

    // PC_AUTH (not part of H)
    val sigPc = readMessage(Proto.MSG_PC_AUTH, inTranscript = false)
    val commitOk = MessageDigest.isEqual(Crypto.sha256(Proto.LABEL_COMMIT, ePcBytes, nPc), commitment)
    if (!commitOk || sigPc.size != Proto.SIG_LEN || !Crypto.verify(pcKey, Proto.LABEL_PC_AUTH + h, sigPc)) {
      deny(Proto.DENIED_BAD_PC)
      throw ProtocolException("PC authentication failed")
    }
    val ePc = Crypto.decodePoint(ePcBytes)

    // Keys and code.
    val keys = Crypto.deriveKeys(h, Crypto.ecdh(eph.private, ePc))
    recvCipher = RecordCipher(keys.pcToPhone)
    sendCipher = RecordCipher(keys.phoneToPc)
    val code = Crypto.formatCode(keys.code)

    val hello = PcHello(pcKeyBytes, pcName.ifBlank { "PC" })
    pc = hello
    val denied = gate.admit(hello, code, usb, this)
    if (denied != null) {
      deny(denied)
      throw ProtocolException("PC denied (reason $denied)")
    }
    sendMessage(Proto.MSG_PHONE_AUTH, Identity.sign(Proto.LABEL_PHONE_AUTH + h), inTranscript = false)
    output.flush()
    socket.soTimeout = 0
  }

  /** Tells the PC the phone is asking the user. Called by the gate before it shows the prompt. */
  fun announcePairing() {
    sendMessage(Proto.MSG_PAIRING, ByteArray(0), inTranscript = false)
    output.flush()
  }

  /**
   * While the pairing prompt is open: waits up to 250 ms and returns false once the PC has hung
   * up. The PC sends nothing during pairing, so a byte (like EOF) ends the connection.
   */
  fun peerStillThere(): Boolean {
    val old = socket.soTimeout
    return try {
      socket.soTimeout = 250
      input.read()
      false // EOF or an unexpected byte
    } catch (_: SocketTimeoutException) {
      true
    } catch (_: IOException) {
      false
    } finally {
      try { socket.soTimeout = old } catch (_: IOException) {}
    }
  }

  private fun deny(reason: Int) {
    try {
      sendMessage(Proto.MSG_DENIED, byteArrayOf(reason.toByte()), inTranscript = false)
      output.flush()
    } catch (_: IOException) {}
  }

  // ---- records (after the handshake)

  /** Encrypts and writes one record. Writer thread only. The caller flushes. */
  fun writeRecord(type: Int, payload: ByteArray, out: OutputStream) {
    out.write(sendCipher.seal(type, payload))
  }

  /** Reads and decrypts one record: returns (type, payload). Reader thread only. */
  fun readRecord(): Pair<Int, ByteArray> {
    val header = ByteArray(4)
    input.readFully(header)
    val total = toInt(header)
    if (total < 17 || total > Proto.MAX_RECORD) throw ProtocolException("bad record length $total")
    val sealed = ByteArray(total)
    input.readFully(sealed)
    return recvCipher.open(header, sealed)
  }

  // ---- handshake framing

  private fun sendRaw(bytes: ByteArray) {
    transcript.write(bytes)
    output.write(bytes)
  }

  private fun sendMessage(type: Int, body: ByteArray, inTranscript: Boolean = true) {
    val msg = intBytes(1 + body.size) + byteArrayOf(type.toByte()) + body
    if (inTranscript) transcript.write(msg)
    output.write(msg)
  }

  private fun readMessage(expected: Int, inTranscript: Boolean = true): ByteArray {
    val header = readExactly(4)
    val len = toInt(header)
    if (len < 1 || len > Proto.MAX_MESSAGE) throw ProtocolException("bad message length $len")
    val msg = readExactly(len)
    if ((msg[0].toInt() and 0xFF) != expected) throw ProtocolException("expected message $expected, got ${msg[0]}")
    if (inTranscript) { transcript.write(header); transcript.write(msg) }
    return msg.copyOfRange(1, msg.size)
  }

  private fun readExactly(n: Int): ByteArray {
    val buf = ByteArray(n)
    try {
      input.readFully(buf)
    } catch (e: EOFException) {
      throw ProtocolException("connection closed during handshake")
    }
    return buf
  }

  private fun intBytes(v: Int) =
    byteArrayOf((v ushr 24).toByte(), (v ushr 16).toByte(), (v ushr 8).toByte(), v.toByte())

  private fun toInt(b: ByteArray) =
    ((b[0].toInt() and 0xFF) shl 24) or ((b[1].toInt() and 0xFF) shl 16) or ((b[2].toInt() and 0xFF) shl 8) or
      (b[3].toInt() and 0xFF)
}
