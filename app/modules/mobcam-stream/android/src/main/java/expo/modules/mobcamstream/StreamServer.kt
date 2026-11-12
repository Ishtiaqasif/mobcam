package expo.modules.mobcamstream

import android.util.Log
import java.io.BufferedOutputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

private const val TAG = "MobcamServer"
private const val MAX_QUEUED_FRAMES = 30 // ~1 s at 30 fps

/**
 * Serves the H.264 stream to PCs over protocol v2 (docs/protocol.md): every connection runs the
 * pairing/authentication handshake in [SecureChannel], then frames go out as encrypted VIDEO
 * records and commands come in as COMMAND records. The same port serves Wi-Fi clients and USB
 * clients (via `adb forward`, which shows up as a loopback connection).
 */
class StreamServer(
  private val port: Int,
  private val phoneName: String,
  private val gate: PairingGate,
  private val onClientsChanged: () -> Unit,
  private val onKeyFrameNeeded: () -> Unit,
  /** Remote-control commands from the PC ("SWITCH", "RES 720", ...). */
  private val onCommand: (String) -> Unit,
  /** A PC with a different protocol version connected (null = an old v1 PC). */
  private val onVersionMismatch: (Int?) -> Unit,
  /** Progress of a two-sided pairing after this phone's user tapped Allow. */
  private val onPairing: (PcHello, PairingStage) -> Unit,
) : FrameSink {

  private class Client(val socket: Socket, val channel: SecureChannel) {
    val usb: Boolean = socket.inetAddress.isLoopbackAddress
    val queue = LinkedBlockingQueue<ByteArray>(MAX_QUEUED_FRAMES)
    @Volatile var synced = false // touched only by the encoder thread
    @Volatile var open = true
    /** Newly allowed PC whose user hasn't confirmed the code yet: no video, no commands. */
    @Volatile var awaitingPc = channel.pendingPairing
  }

  private val clients = CopyOnWriteArrayList<Client>()
  private val handshaking = CopyOnWriteArrayList<Socket>()
  @Volatile private var serverSocket: ServerSocket? = null
  @Volatile private var running = false

  val clientCount: Int get() = clients.count { !it.awaitingPc }
  val usbClientCount: Int get() = clients.count { it.usb && !it.awaitingPc }

  fun start() {
    if (running) return
    running = true
    thread(name = "MobcamAccept", isDaemon = true) { acceptLoop() }
  }

  fun stop() {
    running = false
    try { serverSocket?.close() } catch (_: IOException) {}
    handshaking.forEach { closeQuietly(it) }
    clients.forEach { drop(it) }
  }

  /** Called on the encoder thread for every encoded frame. */
  override fun onFrame(data: ByteArray, keyFrame: Boolean) {
    for (c in clients) {
      if (c.awaitingPc) continue
      if (!c.synced) {
        if (!keyFrame) continue // a decoder can only start on a keyframe
        c.synced = true
      }
      if (!c.queue.offer(data)) {
        // Client can't keep up (weak Wi-Fi). Drop the backlog and resync on the next keyframe.
        c.queue.clear()
        c.synced = false
        onKeyFrameNeeded()
      }
    }
  }

  private fun acceptLoop() {
    while (running) {
      try {
        ServerSocket().use { ss ->
          ss.reuseAddress = true
          ss.bind(InetSocketAddress(port))
          serverSocket = ss
          Log.i(TAG, "Listening on port $port")
          while (running) {
            val socket = ss.accept()
            socket.tcpNoDelay = true
            socket.sendBufferSize = 512 * 1024
            thread(name = "MobcamHandshake", isDaemon = true) { handshake(socket) }
          }
        }
      } catch (e: IOException) {
        if (running) {
          Log.w(TAG, "Server socket error, retrying", e)
          Thread.sleep(1000)
        }
      }
    }
  }

  /** Authenticates (and if needed pairs) the PC, then starts streaming to it. */
  private fun handshake(socket: Socket) {
    val usb = socket.inetAddress.isLoopbackAddress
    handshaking += socket
    val channel = try {
      SecureChannel(socket, phoneName).also { it.handshake(gate, usb) }
    } catch (e: VersionException) {
      Log.w(TAG, "PC ${socket.inetAddress.hostAddress} speaks protocol ${e.peerVersion ?: 1}")
      onVersionMismatch(e.peerVersion)
      closeQuietly(socket)
      return
    } catch (e: Exception) {
      Log.w(TAG, "Handshake with ${socket.inetAddress.hostAddress} failed: ${e.message}")
      closeQuietly(socket)
      return
    } finally {
      handshaking -= socket
    }
    if (!running) { closeQuietly(socket); return }

    val client = Client(socket, channel)
    if (client.awaitingPc) {
      // Give the PC's user time to confirm the code; reading PAIRED times out otherwise.
      try { socket.soTimeout = Proto.PC_CONFIRM_TIMEOUT_MS } catch (_: IOException) {}
      channel.pc?.let { onPairing(it, PairingStage.WAITING_FOR_PC) }
    }
    clients += client
    Log.i(TAG, "Client connected: ${socket.inetAddress.hostAddress} (usb=$usb, pc=${channel.pc?.name}, awaitingPc=${client.awaitingPc})")
    thread(name = "MobcamClient", isDaemon = true) { writeLoop(client) }
    thread(name = "MobcamClientIn", isDaemon = true) { readLoop(client) }
    if (!client.awaitingPc) {
      onClientsChanged()
      onKeyFrameNeeded()
    }
  }

  private fun writeLoop(client: Client) {
    try {
      val out = BufferedOutputStream(client.socket.getOutputStream(), 64 * 1024)
      while (running && client.open) {
        val frame = client.queue.poll(500, TimeUnit.MILLISECONDS) ?: continue
        client.channel.writeRecord(Proto.REC_VIDEO, frame, out)
        if (client.queue.isEmpty()) out.flush()
      }
    } catch (_: IOException) {
      // client went away
    } catch (_: InterruptedException) {
    } finally {
      drop(client)
    }
  }

  /**
   * Reads records from the PC: PAIRED (finishes a two-sided pairing) and remote-control commands.
   * EOF, a bad record or a pairing timeout means the PC is gone.
   */
  private fun readLoop(client: Client) {
    try {
      while (running && client.open) {
        val (type, payload) = client.channel.readRecord()
        if (type == Proto.REC_PAIRED && client.awaitingPc) {
          client.awaitingPc = false
          try { client.socket.soTimeout = 0 } catch (_: IOException) {}
          client.channel.pc?.let { onPairing(it, PairingStage.PAIRED) }
          Log.i(TAG, "Pairing confirmed by ${client.channel.pc?.name}")
          onClientsChanged()
          onKeyFrameNeeded()
          continue
        }
        if (client.awaitingPc) continue // not trusted yet: ignore commands
        if (type != Proto.REC_COMMAND) continue // ignore types from newer PCs
        val command = String(payload, Charsets.US_ASCII).trim()
        if (command.isNotEmpty()) {
          Log.i(TAG, "Command: $command")
          onCommand(command)
        }
      }
    } catch (_: IOException) {
    } finally {
      drop(client)
    }
  }

  private fun closeQuietly(socket: Socket) {
    try { socket.close() } catch (_: IOException) {}
  }

  private fun drop(client: Client) {
    client.open = false
    try { client.socket.close() } catch (_: IOException) {}
    if (clients.remove(client)) {
      Log.i(TAG, "Client disconnected: ${client.socket.inetAddress.hostAddress}")
      if (client.awaitingPc) {
        // The PC cancelled, timed out or vanished before its user confirmed: nothing is remembered.
        client.channel.pc?.let { onPairing(it, PairingStage.CANCELLED) }
      } else {
        onClientsChanged()
      }
    }
  }
}
