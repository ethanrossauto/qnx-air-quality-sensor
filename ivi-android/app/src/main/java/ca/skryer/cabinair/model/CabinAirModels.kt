package ca.skryer.cabinair.model

import kotlinx.serialization.Serializable

/**
 * One sensor event as published by the QNX air-quality node over SSE.
 *
 * Every field has a default so a partially-populated (or evolving) JSON payload
 * still decodes instead of crashing. Property names deliberately match the wire
 * keys (pm2_5 etc.) so no @SerialName annotations are needed.
 */
@Serializable
data class AirSample(
    val seq: Int = 0,
    val t_us: Long = 0L,
    val pm1_0: Int = 0,
    val pm2_5: Int = 0,
    val pm10: Int = 0,
    val no2_ppb: Int = 0,
    val co2_ppm: Int = 0,
    val voc_index: Int = 0,
    val temp_c: Double = 0.0,
    val rh_pct: Double = 0.0,
    val aqi: Int = 0,
    val status: String = "OK",       // "OK" | "WARN" | "ALERT"
    val alert: Boolean = false,
    val message: String = "",
    val source: String = "",
    val bus: String = ""
) {
    /** True when either signal says we are in an alert condition. */
    val isAlert: Boolean get() = alert || status == "ALERT"
}

/** Everything the screen needs, exposed as a single StateFlow from the ViewModel. */
data class CabinAirState(
    val host: String = "",
    val demoMode: Boolean = false,
    /** True when a sample arrived within the last ~2 seconds. */
    val connected: Boolean = false,
    /** Last sample seen; null until the first event arrives after (re)connect. */
    val sample: AirSample? = null
)
