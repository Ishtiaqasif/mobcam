package expo.modules.mobcamstream

import android.content.Context
import android.os.Build
import java.net.Inet4Address
import java.net.NetworkInterface

/** Process-wide singletons shared by the module and the preview view. */
object Mobcam {
  const val STREAM_PORT = 5000
  const val DISCOVERY_PORT = 5001

  /** "OnePlus CPH2747": shown to PCs in discovery and during pairing. */
  val deviceName: String get() = "${Build.MANUFACTURER} ${Build.MODEL}"

  @Volatile private var engine: StreamEngine? = null

  fun engine(context: Context): StreamEngine =
    engine ?: synchronized(this) {
      engine ?: StreamEngine(context.applicationContext).also { engine = it }
    }

  private val ignoredInterfaces = listOf("rmnet", "ccmni", "dummy", "tun", "v4-", "ip6tnl", "p2p", "lo")

  /** IPv4 addresses a PC on the same network could reach, Wi-Fi first. */
  fun localAddresses(): List<String> = try {
    NetworkInterface.getNetworkInterfaces().toList()
      .filter { nif -> nif.isUp && !nif.isLoopback && ignoredInterfaces.none { nif.name.startsWith(it) } }
      .sortedBy { if (it.name.startsWith("wlan")) 0 else 1 }
      .flatMap { nif -> nif.inetAddresses.toList().filterIsInstance<Inet4Address>().mapNotNull { it.hostAddress } }
      .distinct()
  } catch (e: Exception) {
    emptyList()
  }
}
