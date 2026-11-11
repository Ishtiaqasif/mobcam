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
 * Serves the raw H.264 Annex-B elementary stream over TCP.
 *
 * No framing: a client just reads bytes and feeds them to a decoder
 * (`ffplay -f h264 tcp://<phone>:5000` works as-is). The same port serves Wi-Fi clients and
 * USB clients (via `adb forward`, which shows up as a loopback connection).
 */
class StreamServer(
  private val port: Int,
  private val onClientsChanged: () -> Unit,
  private val onKeyFrameNeeded: () -> Unit,
  /** Text commands from the PC, one per line ("SWITCH", "RES 720", ...). */
  private val onCommand: (String) -> Unit,
) : FrameSink {

  private class Client(val socket: Socket) {
    val usb: Boolean = socket.inetAddress.isLoopbackAddress
    val queue = LinkedBlockingQueue<ByteArray>(MAX_QUEUED_FRAMES)
    @Volatile var synced = false // touched only by the encoder thread
    @Volatile var open = true
  }

  private val clients = CopyOnWriteArrayList<Client>()
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
            val client = Client(socket)
            clients += client
            Log.i(TAG, "Client connected: ${socket.inetAddress.hostAddress} (usb=${client.usb})")
            thread(name = "MobcamClient", isDaemon = true) { writeLoop(client) }
            thread(name = "MobcamClientIn", isDaemon = true) { readLoop(client) }
            onClientsChanged()
            onKeyFrameNeeded()
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

  private fun writeLoop(client: Client) {
    try {
      val out = BufferedOutputStream(client.socket.getOutputStream(), 64 * 1024)
      while (running && client.open) {
        val frame = client.queue.poll(500, TimeUnit.MILLISECONDS) ?: continue
        out.write(frame)
        if (client.queue.isEmpty()) out.flush()
      }
    } catch (_: IOException) {
      // client went away
    } catch (_: InterruptedException) {
    } finally {
      drop(client)
    }
  }

  /** Reads remote-control commands; EOF means the PC closed the connection. */
  private fun readLoop(client: Client) {
    try {
      val reader = client.socket.getInputStream().bufferedReader(Charsets.US_ASCII)
      while (running && client.open) {
        val line = reader.readLine() ?: break
        val command = line.trim()
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

  private fun drop(client: Client) {
    client.open = false
    try { client.socket.close() } catch (_: IOException) {}
    if (clients.remove(client)) {
      Log.i(TAG, "Client disconnected: ${client.socket.inetAddress.hostAddress}")
      onClientsChanged()
    }
  }
}
