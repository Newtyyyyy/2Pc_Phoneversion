package com.example.newtyvision

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.util.Log
import com.hoho.android.usbserial.driver.Ch34xSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber

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
            if (validerA(BAUD_HIGH)) { Log.i(TAG, "connecte (deja 4M)"); return true }

            // 2. Negociation : 115200 -> trame magique -> 4 Mbaud
            p.setParameters(BAUD_LOW, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            purge(p)
            p.write(BAUD_FRAME_4M, 200)  // sans \r\n
            Thread.sleep(100)
            if (validerA(BAUD_HIGH)) { Log.i(TAG, "connecte (4M apres negociation)"); return true }

            // 3. Dernier recours : certains firmwares repondent encore a 115200
            if (validerA(BAUD_LOW)) { Log.i(TAG, "connecte (115200)"); return true }

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
        try { port?.close() } catch (_: Exception) {}
        port = null
    }
}
