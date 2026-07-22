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
    @Volatile private var aimActif = false     // interrupteur maitre (bouton AIM)
    @Volatile private var gachetteDroit = false // si vrai : viser seulement clic droit maintenu
    @Volatile private var envoiEnMarche = true // le thread d'envoi tourne-t-il ?
    @Volatile private var vitesseX = 0.40      // rattrapage horizontal
    @Volatile private var vitesseY = 0.30      // rattrapage vertical
    @Volatile private var expo = 1.8           // courbe : >1 = plus on est loin, plus c'est rapide

    // --- LE PONT JNI VERS TON MOTEUR C++ ---
    // On passe maintenant le file descriptor USB fourni par Android a libuvc.
    external fun startCamera(fileDescriptor: Int): Boolean
    external fun stopCamera()
    external fun setSurface(surface: Surface?)
    external fun setHsvRange(hLow: Int, sLow: Int, vLow: Int, hHigh: Int, sHigh: Int, vHigh: Int)
    external fun setClusterDistance(distance: Int)
    external fun setAimOffset(x: Int, y: Int)  // decalage du point de consigne (px du crop)
    external fun setLissage(pct: Int)          // 0 = tres lisse, 100 = brut
    external fun setMinPixels(px: Int)         // seuil mini de pixels d'un blob (anti-parasites)
    external fun setPrediction(ms: Int)        // avance de prediction (ms) anti-latence
    external fun setZoneSize(pctX: Int, pctY: Int) // taille de la zone detectee (% largeur/hauteur)
    external fun getCenterHsv(): IntArray      // [H, S, V] au centre du viseur (pipette)
    external fun getTarget(): IntArray   // [trouvee(0/1), X, Y, seq, boxW, boxH]
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

    // Bouton AIM (interrupteur maitre) + bouton Gachette (viser au clic droit maintenu)
    private fun configurerAim() {
        binding.aimToggle.setOnClickListener {
            aimActif = !aimActif
            binding.aimToggle.text = if (aimActif) "AIM : ON" else "AIM : OFF"
        }
        gachetteDroit = prefs.getBoolean("gachette", false)
        majTexteGachette()
        binding.gachetteToggle.setOnClickListener {
            gachetteDroit = !gachetteDroit
            majTexteGachette()
            prefs.edit().putBoolean("gachette", gachetteDroit).apply()
        }
    }

    private fun majTexteGachette() {
        binding.gachetteToggle.text =
            if (gachetteDroit) "Gachette : Clic droit" else "Gachette : OFF"
    }

    // --- Envoi : proportionnel PAR IMAGE, mais GLISSE en continu (fluide, sans a-coups) ---
    // A chaque nouvelle image : budget de mouvement = ecart x vitesse (loin -> gros, pres -> petit).
    // Ce budget est ensuite depense en douceur a 500 Hz (fraction GLISSE par tick) -> mouvement
    // fluide, et comme le budget est borne par image, aucune embardee / depassement.
    private val TICK_MS = 2L
    private val GLISSE = 0.30  // douceur du geste : fraction du budget restant depensee par tick
    private val REF = 40.0     // distance de reference pour la courbe expo (px)

    // Reponse non-lineaire : loin -> vitesse qui monte (expo>1), pres -> doux/precis.
    // A expo=1 c'est lineaire (= ecart * vitesse). Renvoie le budget de mouvement (counts).
    private fun reponse(offset: Int, vit: Double): Double {
        val o = offset.toDouble()
        if (o == 0.0) return 0.0
        val n = Math.abs(o) / REF
        return Math.signum(o) * Math.pow(n, expo) * REF * vit
    }

    private fun demarrerThreadEnvoi() {
        thread {
            var dernierSeq = -1
            var resteX = 0.0
            var resteY = 0.0
            while (envoiEnMarche) {
                val actif = aimActif && (!gachetteDroit || makcu.boutonDroit)
                if (actif && makcu.estConnecte) {
                    val t = getTarget() // [trouvee, X, Y, seq, ...]
                    if (t[0] == 1) {
                        if (t[3] != dernierSeq) {          // nouvelle image -> nouveau budget
                            dernierSeq = t[3]
                            resteX = reponse(t[1], vitesseX)
                            resteY = reponse(t[2], vitesseY)
                        }
                        val mx = Math.round(resteX * GLISSE).toInt()
                        val my = Math.round(resteY * GLISSE).toInt()
                        if (mx != 0 || my != 0) {
                            makcu.move(mx, my)
                            resteX -= mx
                            resteY -= my
                        }
                    } else { resteX = 0.0; resteY = 0.0 }
                } else { resteX = 0.0; resteY = 0.0 }
                try { Thread.sleep(TICK_MS) } catch (_: InterruptedException) { break }
            }
        }
    }

    private fun chercherCarteMacroSilicon() {
        // Deja en marche : on ignore les evenements USB suivants (le hub en genere plusieurs)
        if (moteurLance) return

        // VID/PID de la carte lus depuis config/device.properties (via BuildConfig).
        val device = usbManager.deviceList.values.firstOrNull {
            it.vendorId == BuildConfig.CARD_VID && it.productId == BuildConfig.CARD_PID
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
        // NB : le flux carte est MJPEG compresse -> couleurs desaturees/decalees vs l'ecran PC.
        // Il faut des plages LARGES (Sat/Val mini bas), sinon la detection rate les pixels.
        binding.hLow.progress = prefs.getInt("hLow", 140)
        binding.hHigh.progress = prefs.getInt("hHigh", 160)
        binding.sLow.progress = prefs.getInt("sLow", 120)
        binding.sHigh.progress = prefs.getInt("sHigh", 200)
        binding.vLow.progress = prefs.getInt("vLow", 180)
        binding.vHigh.progress = prefs.getInt("vHigh", 255)
        binding.cluster.progress = prefs.getInt("cluster", 10)
        binding.minPixels.progress = prefs.getInt("minPixels", 30) // seuil anti-parasites
        binding.offsetY.progress = prefs.getInt("offsetY", 36)     // 36 -> offset 0 (-36..+36)
        binding.vitesseX.progress = prefs.getInt("vitesseX", 40)   // 40 -> 0.40 (max 100 -> 1.0)
        binding.vitesseY.progress = prefs.getInt("vitesseY", 30)   // 30 -> 0.30 (Y souvent + doux)
        binding.expo.progress = prefs.getInt("expo", 40)           // 40 -> expo 1.8 (max 100 -> 3.0)
        binding.lissage.progress = prefs.getInt("lissage", 60)     // 60 -> lissage moyen (max 100)
        binding.prediction.progress = prefs.getInt("prediction", 60) // 60 ms d'avance (max 300)
        binding.zoneX.progress = prefs.getInt("zoneX", 28)         // % largeur detectee (max 100)
        binding.zoneY.progress = prefs.getInt("zoneY", 16)         // % hauteur detectee (max 100)

        val ecouteur = object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) = appliquerHsv()
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        }
        listOf(binding.cluster, binding.minPixels, binding.offsetY, binding.vitesseX, binding.vitesseY,
               binding.expo, binding.lissage, binding.prediction, binding.zoneX, binding.zoneY,
               binding.hLow, binding.hHigh, binding.sLow, binding.sHigh, binding.vLow, binding.vHigh)
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
                .putInt("minPixels", binding.minPixels.progress)
                .putInt("offsetY", binding.offsetY.progress)
                .putInt("vitesseX", binding.vitesseX.progress)
                .putInt("vitesseY", binding.vitesseY.progress)
                .putInt("expo", binding.expo.progress)
                .putInt("lissage", binding.lissage.progress)
                .putInt("prediction", binding.prediction.progress)
                .putInt("zoneX", binding.zoneX.progress)
                .putInt("zoneY", binding.zoneY.progress)
                .apply()
            binding.sampleText.text = "Config sauvegardee ✓"
        }

        // PIPETTE CUMULATIVE : la couleur de la cible varie (distance, lumiere...).
        //  - Appui LONG = repartir a zero sur la couleur visee (1re cible).
        //  - Tap court = ELARGIR la plage pour inclure la couleur visee (autres distances).
        // -> vise de pres, tap ; vise de loin, tap ; etc. -> la plage couvre toutes les nuances.
        fun pipetteReset() {
            val c = getCenterHsv(); val h = c[0]; val s = c[1]; val v = c[2]
            binding.hLow.progress = (h - 8).coerceIn(0, 179)
            binding.hHigh.progress = (h + 8).coerceIn(0, 179)
            binding.sLow.progress = (s - 30).coerceIn(0, 255); binding.sHigh.progress = 255
            binding.vLow.progress = (v - 30).coerceIn(0, 255); binding.vHigh.progress = 255
            appliquerHsv()
            binding.sampleText.text = "Pipette RESET : H$h S$s V$v"
        }
        binding.pipette.setOnClickListener {
            val c = getCenterHsv(); val h = c[0]; val s = c[1]; val v = c[2]
            binding.hLow.progress = minOf(binding.hLow.progress, (h - 8).coerceIn(0, 179))
            binding.hHigh.progress = maxOf(binding.hHigh.progress, (h + 8).coerceIn(0, 179))
            binding.sLow.progress = minOf(binding.sLow.progress, (s - 30).coerceIn(0, 255))
            binding.vLow.progress = minOf(binding.vLow.progress, (v - 30).coerceIn(0, 255))
            appliquerHsv()
            binding.sampleText.text = "Pipette + : H$h S$s V$v (plage elargie)"
        }
        binding.pipette.setOnLongClickListener { pipetteReset(); true }

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
        setMinPixels(binding.minPixels.progress)

        // Offset de consigne (surtout Y) : progress 0..72 -> -36..+36 px
        val offY = binding.offsetY.progress - 36
        setAimOffset(0, offY)
        // Vitesse X/Y : progress 0..100 -> 0.00..1.00 (fraction de l'ecart rattrapee par image)
        vitesseX = binding.vitesseX.progress / 100.0
        vitesseY = binding.vitesseY.progress / 100.0
        // Expo : progress 0..100 -> 1.0..3.0 (1 = lineaire, plus haut = loin bcp + rapide)
        expo = 1.0 + binding.expo.progress / 100.0 * 2.0
        // Lissage : 0 (tres lisse) .. 100 (brut)
        setLissage(binding.lissage.progress)
        // Prediction : avance en ms (compense la latence telephone<->PC)
        setPrediction(binding.prediction.progress)
        // Zone detectee (% largeur / hauteur)
        setZoneSize(binding.zoneX.progress, binding.zoneY.progress)

        binding.tZoneX.text = "Zone X : ${binding.zoneX.progress} %"
        binding.tZoneY.text = "Zone Y : ${binding.zoneY.progress} %"
        binding.tOffsetY.text = "Offset Y : $offY"
        binding.tVitesseX.text = "Vitesse X : %.2f".format(vitesseX)
        binding.tVitesseY.text = "Vitesse Y : %.2f".format(vitesseY)
        binding.tExpo.text = "Expo (loin=rapide) : %.1f".format(expo)
        binding.tLissage.text = "Lissage : ${binding.lissage.progress}"
        binding.tPrediction.text = "Prediction : ${binding.prediction.progress} ms"
        binding.tMinPixels.text = "Min pixels : ${binding.minPixels.progress}"
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
