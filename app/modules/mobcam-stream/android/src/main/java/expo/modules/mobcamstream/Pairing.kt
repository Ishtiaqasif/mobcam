package expo.modules.mobcamstream

import android.content.Context
import android.util.Base64
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicReference

private const val PREFS = "mobcam-paired-pcs"
private const val PROMPT_TIMEOUT_MS = 60_000L

data class PairedPc(val id: String, val name: String, val pairedAt: Long)

/** PCs the user allowed on Wi-Fi, keyed by the SHA-256 of their identity key. */
class PairedPcStore(context: Context) {
  private val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

  @Synchronized
  fun isPaired(pc: PcHello): Boolean {
    val json = prefs.getString(pc.id, null) ?: return false
    // The id is a hash of the key; compare the key itself too.
    return runCatching { Base64.decode(JSONObject(json).getString("key"), Base64.NO_WRAP) }
      .getOrNull()?.contentEquals(pc.key) == true
  }

  @Synchronized
  fun add(pc: PcHello) {
    val json = JSONObject()
      .put("key", Base64.encodeToString(pc.key, Base64.NO_WRAP))
      .put("name", pc.name)
      .put("pairedAt", System.currentTimeMillis())
    prefs.edit().putString(pc.id, json.toString()).apply()
  }

  /** Keeps the stored name current when a paired PC is renamed. */
  @Synchronized
  fun touch(pc: PcHello) {
    val json = prefs.getString(pc.id, null)?.let { runCatching { JSONObject(it) }.getOrNull() } ?: return
    if (json.optString("name") != pc.name) prefs.edit().putString(pc.id, json.put("name", pc.name).toString()).apply()
  }

  @Synchronized
  fun remove(id: String) {
    prefs.edit().remove(id).apply()
  }

  @Synchronized
  fun list(): List<PairedPc> = prefs.all.mapNotNull { (id, value) ->
    val json = (value as? String)?.let { runCatching { JSONObject(it) }.getOrNull() } ?: return@mapNotNull null
    PairedPc(id, json.optString("name", "PC"), json.optLong("pairedAt"))
  }.sortedByDescending { it.pairedAt }
}

/** Where a two-sided pairing stands after this phone's user tapped Allow. */
enum class PairingStage { WAITING_FOR_PC, PAIRED, CANCELLED }

/** A pairing question waiting for the user's answer. */
class PairingRequest(val id: Int, val pc: PcHello, val code: String) {
  internal val answer = AtomicReference<Boolean?>(null)
}

/**
 * The [PairingGate] used by the stream server: lets paired PCs and USB in, and asks the user
 * (through [onPrompt] / [onPromptClosed]) about everything else. One prompt at a time.
 */
class PairingBroker(
  private val store: PairedPcStore,
  private val onPrompt: (PairingRequest) -> Unit,
  private val onPromptClosed: (PairingRequest) -> Unit,
  private val onPairedChanged: () -> Unit,
) : PairingGate {
  private val current = AtomicReference<PairingRequest?>(null)
  private var nextId = 1

  val pending: PairingRequest? get() = current.get()

  override fun admit(pc: PcHello, code: String, usb: Boolean, channel: SecureChannel): Int? {
    if (usb) return null // only PCs the user authorised for USB debugging can reach loopback
    if (store.isPaired(pc)) {
      store.touch(pc)
      return null
    }

    val request = synchronized(this) { PairingRequest(nextId++, pc, code) }
    if (!current.compareAndSet(null, request)) return Proto.DENIED_BUSY
    try {
      channel.announcePairing()
      onPrompt(request)
      val deadline = System.currentTimeMillis() + PROMPT_TIMEOUT_MS
      while (true) {
        request.answer.get()?.let { allowed ->
          if (!allowed) return Proto.DENIED_BY_USER
          // Two-sided pairing: the PC is trusted only once its user confirmed the code too
          // (Proto.REC_PAIRED, handled by the stream server, which then calls confirm()).
          channel.pendingPairing = true
          return null
        }
        if (System.currentTimeMillis() >= deadline) return Proto.DENIED_TIMEOUT
        // Blocks up to 250 ms; doubles as the wait between checks.
        if (!channel.peerStillThere()) throw ProtocolException("PC left during pairing")
      }
    } finally {
      current.compareAndSet(request, null)
      onPromptClosed(request)
    }
  }

  /** Both users confirmed the code: remember the PC. */
  fun confirm(pc: PcHello) {
    store.add(pc)
    onPairedChanged()
  }

  /** Called from the UI. Ignored if [id] is not the open prompt. */
  fun respond(id: Int, allow: Boolean) {
    val request = current.get() ?: return
    if (request.id == id) request.answer.compareAndSet(null, allow)
  }
}
