package ca.skryer.cabinair.source

import ca.skryer.cabinair.model.AirSample
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt
import kotlin.random.Random
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive

/**
 * Local simulator for demoing the UI without the Pi: PM2.5 does a gentle random
 * walk around a clean baseline at ~4 samples/sec. triggerAlert() drives it into
 * an ALERT spike for ~10 seconds, then it decays back to baseline.
 */
class FakeSource : AirSource {

    private val alertRequested = AtomicBoolean(false)

    fun triggerAlert() {
        alertRequested.set(true)
    }

    override fun samples(): Flow<AirSample> = flow {
        var pm = 9.0
        var seq = 0
        var alertUntil = 0L

        while (currentCoroutineContext().isActive) {
            val now = System.currentTimeMillis()
            if (alertRequested.getAndSet(false)) alertUntil = now + 10_000L
            val alertPhase = now < alertUntil

            // Exponential pull toward the target plus a little noise.
            val target = if (alertPhase) 95.0 else 9.0
            pm += (target - pm) * 0.08 + Random.nextDouble(-1.5, 1.5)
            pm = pm.coerceIn(2.0, 150.0)

            val pmInt = pm.roundToInt()
            val status = when {
                pmInt >= 55 -> "ALERT"
                pmInt >= 25 -> "WARN"
                else -> "OK"
            }

            emit(
                AirSample(
                    seq = seq++,
                    t_us = now * 1000L,
                    pm1_0 = (pmInt * 0.6).roundToInt(),
                    pm2_5 = pmInt,
                    pm10 = (pmInt * 1.4).roundToInt(),
                    no2_ppb = 12 + Random.nextInt(-2, 3),
                    co2_ppm = 620 + Random.nextInt(-15, 16),
                    voc_index = if (alertPhase) 240 + Random.nextInt(-10, 11) else 98 + Random.nextInt(-5, 6),
                    temp_c = 22.4 + Random.nextDouble(-0.2, 0.2),
                    rh_pct = 41.0 + Random.nextDouble(-0.8, 0.8),
                    aqi = (pmInt * 4).coerceAtMost(300),
                    status = status,
                    alert = status == "ALERT",
                    message = if (status == "ALERT") "PM2.5 spike detected. Close vents and recirculate." else "",
                    source = "demo-sim",
                    bus = "local"
                )
            )
            delay(250)
        }
    }.flowOn(Dispatchers.Default)
}
