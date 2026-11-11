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
) : FrameSink {

  private class Client(val socket: Socket, val channel: SecureChannel) {
    val usb: Boolean = socket.inetAddress.isLoopbackAddress
    val queue = LinkedBlockingQueue<ByteArray>(MAX_QUEUED_FRAMES)
    @Volatile var synced = false // touched only by the encoder thread
    @Volatile var open = true
  }

  private val clients = CopyOnWriteArrayList<Client>()
  private val handshaking = CopyOnWriteArrayList<Socket>()
  @Volatile private var serverSocket: ServerSocket? = null
  @Volatile private var running = false

  val clientCount: Int get() = clients.size
  val usbClientCount: Int get() = clients.count { it.usb }

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
    clients += client
    Log.i(TAG, "Client connected: ${socket.inetAddress.hostAddress} (usb=$usb, pc=${channel.pc?.name})")
    thread(name = "MobcamClient", isDaemon = true) { writeLoop(client) }
    thread(name = "MobcamClientIn", isDaemon = true) { readLoop(client) }
    onClientsChanged()
    onKeyFrameNeeded()
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

  /** Reads remote-control commands; EOF or a bad record means the PC is gone. */
  private fun readLoop(client: Client) {
    try {
      while (running && client.open) {
        val (type, payload) = client.channel.readRecord()
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
      onClientsChanged()
    }
  }
}
