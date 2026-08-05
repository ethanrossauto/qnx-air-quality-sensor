package ca.skryer.cabinair

import android.os.SystemClock
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import ca.skryer.cabinair.model.CabinAirState
import ca.skryer.cabinair.source.AirSource
import ca.skryer.cabinair.source.FakeSource
import ca.skryer.cabinair.source.SseSource
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Default LAN address of the Raspberry Pi bridging the QNX sensor node. */
const val DEFAULT_PI_IP = "192.168.1.10"   // placeholder; set the Pi host in the app

/** How long after the last sample the connection dot stays green. */
private const val FRESH_WINDOW_MS = 2_000L

class CabinAirViewModel : ViewModel() {

    private val fakeSource = FakeSource()

    private val _state = MutableStateFlow(CabinAirState(host = DEFAULT_PI_IP))
    val state: StateFlow<CabinAirState> = _state.asStateFlow()

    private var streamJob: Job? = null

    /** Monotonic timestamp of the last sample; 0 = nothing since (re)connect. */
    @Volatile
    private var lastSampleAt = 0L

    init {
        connect()

        // Freshness ticker: the green "live" dot means data in the last ~2s,
        // which also covers the case where the server silently stops sending.
        viewModelScope.launch {
            while (isActive) {
                val fresh = lastSampleAt != 0L &&
                    SystemClock.elapsedRealtime() - lastSampleAt < FRESH_WINDOW_MS
                if (_state.value.connected != fresh) {
                    _state.update { it.copy(connected = fresh) }
                }
                delay(500)
            }
        }
    }

    fun setHost(host: String) {
        _state.update { it.copy(host = host) }
    }

    fun reconnect() = connect()

    fun setDemoMode(enabled: Boolean) {
        _state.update { it.copy(demoMode = enabled) }
        connect()
    }

    fun triggerDemoAlert() = fakeSource.triggerAlert()

    /** (Re)start collection from the currently selected source. */
    private fun connect() {
        streamJob?.cancel()
        lastSampleAt = 0L
        _state.update { it.copy(connected = false, sample = null) }

        val source: AirSource =
            if (_state.value.demoMode) fakeSource
            else SseSource(_state.value.host.trim())

        // viewModelScope is cancelled in onCleared(), which tears down the SSE
        // connection via the callbackFlow's awaitClose.
        streamJob = viewModelScope.launch {
            source.samples().collect { sample ->
                lastSampleAt = SystemClock.elapsedRealtime()
                _state.update { it.copy(sample = sample, connected = true) }
            }
        }
    }
}
