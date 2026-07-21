package com.example.newtyvision

import java.util.Random
import kotlin.math.hypot
import kotlin.math.roundToInt

/**
 * Generateur "maison" de trajectoire de souris facon humaine (aucun ML).
 *
 * Courbe de Bezier cubique (0,0) -> (cibleX,cibleY) avec :
 *  - un arc perpendiculaire (approche courbe, pas en ligne droite),
 *  - un profil de vitesse ease-in-out (depart et arrivee lents),
 *  - un leger overshoot puis retour (on depasse un peu, on se recale),
 *  - un micro-jitter qui s'attenue vers la fin (tremblement humain).
 *
 * Sortie = liste de deplacements INCREMENTAUX entiers (dx,dy) a envoyer via
 * km.move. La somme des deplacements vaut exactement (cibleX,cibleY) : on
 * atterrit pile sur la cible malgre l'arrondi et l'overshoot.
 */
class BezierAim(private val rnd: Random = Random()) {

    // --- Reglages (modifiables en direct) ---
    var arc = 0.18        // amplitude de l'arc (fraction de la distance ; 0 = ligne droite)
    var jitter = 1.2      // amplitude du micro-tremblement (counts souris)
    var overshoot = 0.06  // depassement puis retour (fraction ; 0 = aucun)
    var pasParUnite = 0.6 // densite de sous-pas (plus grand = courbe plus fine)
    var pasMin = 6
    var pasMax = 40

    /** Renvoie les deplacements incrementaux (dx,dy) vers la cible (en counts souris). */
    fun trajet(cibleX: Int, cibleY: Int): List<IntArray> {
        val ex = cibleX.toDouble()
        val ey = cibleY.toDouble()
        val dist = hypot(ex, ey)
        if (dist < 0.5) return emptyList()

        val n = (dist * pasParUnite).roundToInt().coerceIn(pasMin, pasMax)

        // Direction perpendiculaire (pour l'arc) + sens aleatoire (gauche/droite)
        val perpX = -ey / dist
        val perpY = ex / dist
        val sens = if (rnd.nextBoolean()) 1.0 else -1.0
        val a = arc * dist * sens

        // Poignees de la Bezier, un peu variables pour ne jamais tracer 2x la meme courbe
        val h1 = 0.25 + rnd.nextDouble() * 0.15
        val h2 = 0.65 + rnd.nextDouble() * 0.15
        val p1x = ex * h1 + perpX * a
        val p1y = ey * h1 + perpY * a
        val p2x = ex * h2 + perpX * a * 0.6
        val p2y = ey * h2 + perpY * a * 0.6

        // Point d'arrivee de la courbe = cible + overshoot ; on corrige a la fin.
        val cx = ex * (1.0 + overshoot)
        val cy = ey * (1.0 + overshoot)

        val sortie = ArrayList<IntArray>(n + 1)
        var prevIx = 0
        var prevIy = 0
        for (i in 1..n) {
            val tLin = i.toDouble() / n
            val t = easeInOut(tLin)
            val bx = bezier(t, 0.0, p1x, p2x, cx)
            val by = bezier(t, 0.0, p1y, p2y, cy)
            // jitter attenue en fin de course (0 a l'arrivee)
            val att = 1.0 - tLin
            val jx = (rnd.nextDouble() - 0.5) * jitter * att
            val jy = (rnd.nextDouble() - 0.5) * jitter * att
            val ix = (bx + jx).roundToInt()
            val iy = (by + jy).roundToInt()
            val dx = ix - prevIx
            val dy = iy - prevIy
            if (dx != 0 || dy != 0) sortie.add(intArrayOf(dx, dy))
            prevIx = ix
            prevIy = iy
        }
        // Correction finale : atterrissage exact sur la cible (annule overshoot/arrondi)
        val corrDx = cibleX - prevIx
        val corrDy = cibleY - prevIy
        if (corrDx != 0 || corrDy != 0) sortie.add(intArrayOf(corrDx, corrDy))
        return sortie
    }

    // Profil de vitesse : cubique, lent au depart ET a l'arrivee.
    private fun easeInOut(t: Double): Double =
        if (t < 0.5) 4.0 * t * t * t
        else 1.0 - Math.pow(-2.0 * t + 2.0, 3.0) / 2.0

    // Bezier cubique 1D : P0=0, P1, P2, P3.
    private fun bezier(t: Double, p0: Double, p1: Double, p2: Double, p3: Double): Double {
        val u = 1.0 - t
        return u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3
    }
}
