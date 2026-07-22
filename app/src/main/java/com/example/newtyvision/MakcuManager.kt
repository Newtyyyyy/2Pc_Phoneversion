package com.example.newtyvision

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import com.hoho.android.usbserial.driver.Ch34xSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager

/**
 * Liaison serie vers le MAKCU (emulateur de souris). VID/PID lus depuis BuildConfig
 * (config/device.properties, non versionne).
 *
 * Protocole (doc officielle makcu-docs) :
 *  - Deplacement relatif : texte "km.move(dx,dy)\r\n"  (dx,dy dans -32767..32767)
 *  - Connexion : essayer 4 Mbaud direct ; sinon 115200 -> trame magique -> 4 Mbaud.
 */
class MakcuManager(private val usbManager: UsbManager) {

    companion object {
        // VID/PID lus depuis config/device.properties (via BuildConfig, non versionne).
        val VID = BuildConfig.MAKCU_VID
        val PID = BuildConfig.MAKCU_PID
        private const val TAG = "NewtyVision_MAKCU"
        private const val BAUD_HIGH = 4_000_000
        private const val BAUD_LOW = 115_200
        // Trame de passage a 4 Mbaud : DE AD 05 00 A5 + 4000000 en little-endian (00 09 3D 00)
        private val BAUD_FRAME_4M = byteArrayOf(
            0xDE.toByte(), 0xAD.toByte(), 0x05, 0x00, 0xA5.toByte(), 0x00, 0x09, 0x3D, 0x00
        )
    }

    private var port: UsbSerialPort? = null
    val estConnecte: Boolean get() = port != null

    // Etat des boutons PHYSIQUES de la souris (lus via le stream km.buttons du MAKCU).
    // Necessite que la souris soit branchee dans le MAKCU (passthrough).
    @Volatile var boutonDroit = false
        private set
    @Volatile var boutonGauche = false
        private set

    private var ioManager: SerialInputOutputManager? = null
    // Le MAKCU streame l'etat des boutons sous la forme "km." + 1 octet de masque
    // (bit0=gauche, bit1=droit, bit2=molette...). Un octet < 0x20 apres "km." = masque ;
    // sinon c'est un echo de commande (km.move..., km.MAKCU...) qu'on ignore.
    private val marqueur = "km.".toByteArray(Charsets.US_ASCII)
    private var accum = ByteArray(0)
    private var dernierLogRx = 0L

    /** Cherche le MAKCU parmi les peripheriques USB branches. */
    fun trouver(): UsbDevice? = usbManager.deviceList.values.firstOrNull {
        it.vendorId == VID && it.productId == PID
    }

    /** Ouvre le port et negocie la vitesse. Renvoie true si le MAKCU repond. */
    fun connecter(device: UsbDevice): Boolean {
        return try {
            val connection = usbManager.openDevice(device) ?: run {
                Log.e(TAG, "openDevice a renvoye null"); return false
            }
            // Pilote auto : le CH343 du MAKCU a un init different du CH340. Forcer
            // Ch34xSerialDriver echoue ("Expected 0x0 byte, but get 0x8"). On laisse
            // le prober choisir ; repli sur Ch34x seulement s'il ne reconnait rien.
            val driver = UsbSerialProber.getDefaultProber().probeDevice(device)
                ?: Ch34xSerialDriver(device)
            Log.i(TAG, "pilote MAKCU : ${driver.javaClass.simpleName}")
            val p = driver.ports[0]

            // L'init CH34x est capricieux (surtout avec la carte de capture qui sature
            // le bus USB2) : on retente l'ouverture quelques fois avant d'abandonner.
            var ouvert = false
            for (essai in 1..4) {
                try { p.open(connection); ouvert = true; break }
                catch (e: Exception) {
                    Log.w(TAG, "open MAKCU essai $essai : ${e.message}")
                    try { p.close() } catch (_: Exception) {}
                    Thread.sleep(150)
                }
            }
            if (!ouvert) { Log.e(TAG, "open MAKCU a echoue apres 4 essais"); return false }
            port = p

            // 1. Le MAKCU est peut-etre deja en 4 Mbaud (session precedente)
            if (validerA(BAUD_HIGH)) return succes("connecte (deja 4M)")

            // 2. Negociation : 115200 -> trame magique -> 4 Mbaud
            p.setParameters(BAUD_LOW, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            purge(p)
            p.write(BAUD_FRAME_4M, 200)  // sans \r\n
            Thread.sleep(100)
            if (validerA(BAUD_HIGH)) return succes("connecte (4M apres negociation)")

            // 3. Dernier recours : certains firmwares repondent encore a 115200
            if (validerA(BAUD_LOW)) return succes("connecte (115200)")

            Log.e(TAG, "aucune reponse du MAKCU")
            fermer(); false
        } catch (e: Exception) {
            Log.e(TAG, "echec connexion : ${e.message}"); fermer(); false
        }
    }

    /** Configure le baud, envoie km.version() et verifie la reponse "makcu". */
    private fun validerA(baud: Int): Boolean {
        val p = port ?: return false
        return try {
            p.setParameters(baud, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            Thread.sleep(30)
            purge(p)
            p.write("km.version()\r\n".toByteArray(), 200)
            Thread.sleep(60)
            val buf = ByteArray(256)
            val n = p.read(buf, 200)
            val resp = if (n > 0) String(buf, 0, n) else ""
            Log.i(TAG, "reponse @${baud} : \"${resp.trim()}\"")
            resp.contains("makcu", ignoreCase = true) || resp.contains(">>>")
        } catch (e: Exception) {
            Log.e(TAG, "validerA($baud) : ${e.message}"); false
        }
    }

    /** Connexion reussie : demarre la lecture des boutons physiques, puis renvoie true. */
    private fun succes(msg: String): Boolean {
        Log.i(TAG, msg)
        demarrerBoutons()
        return true
    }

    /** Active le stream des boutons physiques du MAKCU et lit en continu. */
    private fun demarrerBoutons() {
        val p = port ?: return
        try {
            // Active le stream des boutons physiques (masque brut sur changement d'etat).
            p.write("km.buttons(1)\r\n".toByteArray(), 100)
        } catch (e: Exception) {
            Log.w(TAG, "activation boutons : ${e.message}")
        }
        val io = SerialInputOutputManager(p, object : SerialInputOutputManager.Listener {
            override fun onNewData(data: ByteArray) = parserBoutons(data)
            override fun onRunError(e: Exception) { Log.w(TAG, "lecture boutons : ${e.message}") }
        })
        ioManager = io
        Thread(io, "makcu-boutons").start()
    }

    /** Parse le flux entrant : cherche "km.buttons" + 1 octet de masque (bit0=gauche, bit1=droit). */
    private fun parserBoutons(data: ByteArray) {
        // Debug (throttle) : voir le format brut reellement envoye par le MAKCU (hex + ASCII).
        val now = System.currentTimeMillis()
        if (now - dernierLogRx > 150) {
            dernierLogRx = now
            val hex = data.take(32).joinToString(" ") { "%02X".format(it) }
            val txt = String(data.take(32).toByteArray(), Charsets.ISO_8859_1)
                .map { if (it.code in 32..126) it else '.' }.joinToString("")
            Log.i(TAG, "RX hex[$hex] txt[$txt]")
        }
        accum += data
        if (accum.size > 512) accum = accum.copyOfRange(accum.size - 512, accum.size)
        var consomme = 0
        var i = 0
        while (true) {
            val idx = indexOf(accum, marqueur, i)
            if (idx < 0 || idx + marqueur.size >= accum.size) break
            val b = accum[idx + marqueur.size].toInt() and 0xFF
            if (b < 0x20) {
                // Masque de boutons : bit0=gauche, bit1=droit, bit2=molette, bit3/4=cotes.
                val droit = (b and 0x02) != 0
                if (droit != boutonDroit) Log.i(TAG, "clic droit = %b (mask=0x%02X)".format(droit, b))
                boutonDroit = droit
                boutonGauche = (b and 0x01) != 0
                i = idx + marqueur.size + 1
            } else {
                // Echo de commande (km.move..., km.MAKCU...) : on saute le "km.".
                i = idx + marqueur.size
            }
            consomme = i
        }
        if (consomme > 0) accum = accum.copyOfRange(consomme, accum.size)
    }

    private fun indexOf(h: ByteArray, n: ByteArray, from: Int): Int {
        if (h.size < n.size) return -1
        outer@ for (s in from..h.size - n.size) {
            for (j in n.indices) if (h[s + j] != n[j]) continue@outer
            return s
        }
        return -1
    }

    /** Deplacement relatif de la souris. */
    fun move(dx: Int, dy: Int) {
        val p = port ?: return
        try {
            p.write("km.move($dx,$dy)\r\n".toByteArray(), 20)
        } catch (e: Exception) {
            Log.e(TAG, "erreur move : ${e.message}")
        }
    }

    private fun purge(p: UsbSerialPort) {
        try { p.purgeHwBuffers(true, true) } catch (_: Exception) {}
    }

    fun fermer() {
        try { ioManager?.stop() } catch (_: Exception) {}
        ioManager = null
        boutonDroit = false
        boutonGauche = false
        try { port?.close() } catch (_: Exception) {}
        port = null
    }
}
