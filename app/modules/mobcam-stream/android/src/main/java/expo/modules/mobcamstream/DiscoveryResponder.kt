package expo.modules.mobcamstream

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import kotlin.concurrent.thread

private const val TAG = "MobcamDiscovery"
const val DISCOVER_REQUEST = "MOBCAM_DISCOVER"
const val DISCOVER_REPLY = "MOBCAM_HERE"

/**
 * Answers UDP broadcasts so the PC can find the phone on the LAN without typing an IP.
 *
 * Request:  "MOBCAM_DISCOVER"
 * Reply:    "MOBCAM_HERE <streamPort> <device name>"
 */
class DiscoveryResponder(context: Context, private val port: Int, private val streamPort: Int) {
  private val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
  private var lock: WifiManager.MulticastLock? = null
  @Volatile private var socket: DatagramSocket? = null
  @Volatile private var running = false

  fun start() {
    if (running) return
    running = true
    // Many devices filter broadcast packets unless a multicast lock is held.
    lock = wifi.createMulticastLock("mobcam-discovery").apply {
      setReferenceCounted(false)
      acquire()
    }
    thread(name = "MobcamDiscovery", isDaemon = true) { loop() }
  }

  fun stop() {
    running = false
    socket?.close()
    lock?.let { if (it.isHeld) it.release() }
  }

  private fun loop() {
    val name = "${Build.MANUFACTURER} ${Build.MODEL}"
    while (running) {
      try {
        DatagramSocket(null).use { s ->
          s.reuseAddress = true
          s.broadcast = true
          s.bind(InetSocketAddress(port))
          socket = s
          val buf = ByteArray(256)
          while (running) {
            val packet = DatagramPacket(buf, buf.size)
            s.receive(packet)
            val msg = String(packet.data, 0, packet.length, Charsets.UTF_8).trim()
            if (msg == DISCOVER_REQUEST) {
              val reply = "$DISCOVER_REPLY $streamPort $name".toByteArray(Charsets.UTF_8)
              s.send(DatagramPacket(reply, reply.size, packet.address, packet.port))
            }
          }
        }
      } catch (e: IOException) {
        if (running) {
          Log.w(TAG, "Discovery socket error, retrying", e)
          Thread.sleep(1000)
        }
      }
    }
  }
}
