package com.example.newtyvision

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Surface
import android.view.SurfaceHolder
import android.view.View
import android.widget.SeekBar
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.example.newtyvision.databinding.ActivityMainBinding
import kotlin.concurrent.thread

class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {

    private lateinit var binding: ActivityMainBinding
    private lateinit var usbManager: UsbManager
    private val ACTION_USB_PERMISSION = "com.example.newtyvision.USB_PERMISSION"

    // Connexion USB : on la garde en champ pour ne PAS la laisser fermer par le GC
    // (fermer la connexion invalide le file descriptor donne au C++).
    private var usbConnection: UsbDeviceConnection? = null

    // Sauvegarde persistante des reglages (survit au redemarrage de l'app)
    private val prefs by lazy { getSharedPreferences("newtyvision_config", MODE_PRIVATE) }

    // Rafraichissement periodique de l'affichage X,Y (toutes les 100 ms)
    private val uiHandler = Handler(Looper.getMainLooper())
    private val majCible = object : Runnable {
        override fun run() {
            val t = getTarget() // [trouvee, X, Y]
            binding.targetText.text =
                if (t[0] == 1) "X: ${t[1]}   Y: ${t[2]}" else "X: -   Y: -"
            uiHandler.postDelayed(this, 100)
        }
    }

    // Drapeaux anti-course : le hub genere plusieurs evenements USB (carte + hub),
    // ce qui faisait spammer requestPermission et cassait la validation.
    private var permissionEnCours = false // une demande de permission est en attente
    private var moteurLance = false       // le flux tourne deja

    // Liaison MAKCU (souris) + etat de l'aim
    private lateinit var makcu: MakcuManager
    @Volatile private var aimActif = false     // l'envoi des mouvements est-il actif ?
    @Volatile private var envoiEnMarche = true // le thread d'envoi tourne-t-il ?
    private var gain = 0.30                     // sensibilite : deplacement souris = ecart * gain

    // --- LE PONT JNI VERS TON MOTEUR C++ ---
    // On passe maintenant le file descriptor USB fourni par Android a libuvc.
    external fun startCamera(fileDescriptor: Int): Boolean
    external fun stopCamera()
    external fun setSurface(surface: Surface?)
    external fun setHsvRange(hLow: Int, sLow: Int, vLow: Int, hHigh: Int, sHigh: Int, vHigh: Int)
    external fun setClusterDistance(distance: Int)
    external fun getTarget(): IntArray   // [trouvee(0/1), X, Y]
    external fun stringFromJNI(): String

    companion object {
        init {
            System.loadLibrary("newtyvision") // Charge ton fichier C++ compile
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Plein ecran immersif : on masque barre de statut + barre de navigation.
        // Elles reapparaissent temporairement si l'utilisateur balaie depuis le bord.
        supportActionBar?.hide()
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, binding.root).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        // Test visuel de base
        binding.sampleText.text = stringFromJNI()

        // On relaie le cycle de vie de la Surface d'affichage au moteur C++
        binding.surfaceView.holder.addCallback(this)

        configurerReglagesHsv()
        uiHandler.post(majCible) // demarre l'affichage X,Y

        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        makcu = MakcuManager(usbManager)
        configurerAim()
        demarrerThreadEnvoi()

        // On ecoute la popup de permission ET le branchement a chaud de la carte
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        ContextCompat.registerReceiver(this, usbReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)

        verifierPeripheriques()
    }

    // Verifie carte de capture ET MAKCU (demande la permission de l'un puis l'autre)
    private fun verifierPeripheriques() {
        chercherCarteMacroSilicon()
        chercherMakcu()
    }

    private fun chercherMakcu() {
        if (makcu.estConnecte) return
        val device = makcu.trouver() ?: return
        if (usbManager.hasPermission(device)) {
            val ok = makcu.connecter(device)
            binding.sampleText.text = if (ok) "MAKCU connecte" else "MAKCU : echec connexion"
        } else if (!permissionEnCours) {
            permissionEnCours = true
            val flags = PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
            usbManager.requestPermission(device, PendingIntent.getBroadcast(this, 0, intent, flags))
        }
    }

    // Bouton AIM (active/desactive l'envoi des mouvements) + affichage etat
    private fun configurerAim() {
        binding.aimToggle.setOnClickListener {
            aimActif = !aimActif
            binding.aimToggle.text = if (aimActif) "AIM : ON" else "AIM : OFF"
        }
    }

    // Thread qui envoie les mouvements au MAKCU, uniquement sur une NOUVELLE trame
    private fun demarrerThreadEnvoi() {
        thread {
            var dernierSeq = -1
            while (envoiEnMarche) {
                if (aimActif && makcu.estConnecte) {
                    val t = getTarget() // [trouvee, X, Y, seq]
                    if (t[0] == 1 && t[3] != dernierSeq) {
                        dernierSeq = t[3]
                        val dx = (t[1] * gain).toInt()
                        val dy = (t[2] * gain).toInt()
                        if (dx != 0 || dy != 0) makcu.move(dx, dy)
                    }
                }
                try { Thread.sleep(2) } catch (_: InterruptedException) { break }
            }
        }
    }

    private fun chercherCarteMacroSilicon() {
        // Deja en marche : on ignore les evenements USB suivants (le hub en genere plusieurs)
        if (moteurLance) return

        // On cible ta puce : VID 0x345F et PID 0x2130
        val device = usbManager.deviceList.values.firstOrNull {
            it.vendorId == 0x345F && it.productId == 0x2130
        }

        if (device == null) {
            binding.sampleText.text = "Branchez la carte MacroSilicon..."
            return
        }

        if (usbManager.hasPermission(device)) {
            demarrerMoteurVision(device)
        } else if (!permissionEnCours) {
            // On ne demande la permission QU'UNE seule fois (sinon le dialogue systeme
            // redemarre a chaque evenement du hub et la validation echoue).
            permissionEnCours = true
            val flags = PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            // Intent explicite (setPackage) : obligatoire sur Android 14+ avec FLAG_MUTABLE.
            val intent = Intent(ACTION_USB_PERMISSION).setPackage(packageName)
            val permissionIntent = PendingIntent.getBroadcast(this, 0, intent, flags)
            usbManager.requestPermission(device, permissionIntent)
        }
    }

    // Le recepteur qui s'active quand tu cliques sur "Autoriser" ou quand tu branches la carte
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_USB_PERMISSION -> {
                    synchronized(this) {
                        permissionEnCours = false
                        if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                            // On re-verifie les deux peripheriques (carte + MAKCU)
                            verifierPeripheriques()
                        } else {
                            binding.sampleText.text = "Permission refusee."
                        }
                    }
                }
                // Detecte carte/MAKCU si branches apres le lancement de l'app
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    verifierPeripheriques()
                }
            }
        }
    }

    private fun demarrerMoteurVision(device: UsbDevice) {
        // Si un flux tourne deja, on le coupe proprement avant d'en relancer un
        arreterMoteurVision()

        // Securite : on verifie la permission sur CETTE instance juste avant d'ouvrir
        // (le device de l'intent pouvait etre perime a cause des evenements du hub).
        if (!usbManager.hasPermission(device)) {
            chercherCarteMacroSilicon() // relance proprement la demande de permission
            return
        }

        // Ouverture de la connexion USB -> Android nous donne un file descriptor
        val connection = usbManager.openDevice(device)
        if (connection == null) {
            binding.sampleText.text = "Impossible d'ouvrir la connexion USB."
            return
        }
        usbConnection = connection

        binding.sampleText.text = "Permission OK. Lancement du C++..."

        // libuvc gere son propre thread interne : pas besoin de boucle Kotlin ici.
        // On lui transmet le file descriptor de la connexion USB.
        val succes = startCamera(connection.fileDescriptor)
        if (!succes) {
            binding.sampleText.text = "Erreur d'ouverture du flux UVC (voir Logcat)."
            arreterMoteurVision()
        } else {
            moteurLance = true
            binding.sampleText.text = "Flux video en cours de traitement..."
        }
    }

    private fun arreterMoteurVision() {
        moteurLance = false
        stopCamera()          // Stoppe le streaming libuvc cote C++
        usbConnection?.close() // Libere le file descriptor cote Android
        usbConnection = null
    }

    // --- Reglage HSV en direct (sliders) ---
    private fun configurerReglagesHsv() {
        // Valeurs chargees depuis la sauvegarde, sinon defauts (magenta/violet, tolerant distance)
        binding.hLow.progress = prefs.getInt("hLow", 130)
        binding.hHigh.progress = prefs.getInt("hHigh", 168)
        binding.sLow.progress = prefs.getInt("sLow", 55)
        binding.sHigh.progress = prefs.getInt("sHigh", 255)
        binding.vLow.progress = prefs.getInt("vLow", 75)
        binding.vHigh.progress = prefs.getInt("vHigh", 255)
        binding.cluster.progress = prefs.getInt("cluster", 10)
        binding.gain.progress = prefs.getInt("gain", 30) // 30 -> gain 0.30

        val ecouteur = object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) = appliquerHsv()
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        }
        listOf(binding.gain, binding.cluster, binding.hLow, binding.hHigh, binding.sLow, binding.sHigh, binding.vLow, binding.vHigh)
            .forEach { it.setOnSeekBarChangeListener(ecouteur) }

        // Bouton : sauvegarde les valeurs courantes pour le prochain demarrage
        binding.saveConfig.setOnClickListener {
            prefs.edit()
                .putInt("hLow", binding.hLow.progress)
                .putInt("hHigh", binding.hHigh.progress)
                .putInt("sLow", binding.sLow.progress)
                .putInt("sHigh", binding.sHigh.progress)
                .putInt("vLow", binding.vLow.progress)
                .putInt("vHigh", binding.vHigh.progress)
                .putInt("cluster", binding.cluster.progress)
                .putInt("gain", binding.gain.progress)
                .apply()
            binding.sampleText.text = "Config sauvegardee ✓"
        }

        // Toucher la video affiche/masque le panneau de reglage
        binding.surfaceView.setOnClickListener {
            binding.tuningPanel.visibility =
                if (binding.tuningPanel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }

        appliquerHsv()
    }

    private fun appliquerHsv() {
        setHsvRange(
            binding.hLow.progress, binding.sLow.progress, binding.vLow.progress,
            binding.hHigh.progress, binding.sHigh.progress, binding.vHigh.progress
        )
        setClusterDistance(binding.cluster.progress)
        gain = binding.gain.progress / 100.0
        binding.tGain.text = "Gain : %.2f".format(gain)
        binding.tCluster.text = "Distance clusters : ${binding.cluster.progress}"
        binding.tHLow.text = "Teinte bas : ${binding.hLow.progress}"
        binding.tHHigh.text = "Teinte haut : ${binding.hHigh.progress}"
        binding.tSLow.text = "Saturation bas : ${binding.sLow.progress}"
        binding.tSHigh.text = "Saturation haut : ${binding.sHigh.progress}"
        binding.tVLow.text = "Valeur bas : ${binding.vLow.progress}"
        binding.tVHigh.text = "Valeur haut : ${binding.vHigh.progress}"
    }

    // --- Cycle de vie de la Surface d'affichage ---
    override fun surfaceCreated(holder: SurfaceHolder) {
        setSurface(holder.surface) // Le C++ peut maintenant dessiner dessus
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        setSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        setSurface(null) // On detache avant que la Surface ne soit detruite
    }

    override fun onDestroy() {
        super.onDestroy()
        envoiEnMarche = false // stoppe le thread d'envoi
        uiHandler.removeCallbacks(majCible)
        arreterMoteurVision()
        makcu.fermer()
        unregisterReceiver(usbReceiver)
    }
}
