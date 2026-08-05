package ca.skryer.cabinair.source

import ca.skryer.cabinair.model.AirSample
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import kotlinx.serialization.json.Json
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.sse.EventSource
import okhttp3.sse.EventSourceListener
import okhttp3.sse.EventSources

/**
 * Connects to GET http://<host>:8090/stream (text/event-stream) and emits one
 * AirSample per SSE "data:" line. Reconnects forever with exponential backoff.
 *
 * OkHttp's SSE support already strips the "data: " framing and silently drops
 * comment/keepalive lines (": ..."), so onEvent only ever sees payload text.
 * flowOn(Dispatchers.IO) keeps connection handling and JSON parsing off the
 * main thread.
 */
class SseSource(private val host: String) : AirSource {

    private val json = Json {
        ignoreUnknownKeys = true   // future fields on the QNX side won't break us
        isLenient = true
        coerceInputValues = true   // nulls / out-of-range enums fall back to defaults
    }

    private val client = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        // A long-lived stream must not hit the default 10s read timeout.
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .retryOnConnectionFailure(true)
        .build()

    override fun samples(): Flow<AirSample> = flow {
        var backoffMs = 1_000L
        while (currentCoroutineContext().isActive) {
            try {
                rawEvents().collect { data ->
                    backoffMs = 1_000L // healthy stream: reset backoff
                    parse(data)?.let { emit(it) }
                }
                // Server closed the stream cleanly; fall through and reconnect.
            } catch (ce: CancellationException) {
                throw ce // never swallow cancellation
            } catch (_: Throwable) {
                // Connection refused / dropped / bad host string: retry below.
            }
            delay(backoffMs)
            backoffMs = (backoffMs * 2).coerceAtMost(15_000L)
        }
    }.flowOn(Dispatchers.IO)

    /** One SSE connection as a flow of raw event payload strings. */
    private fun rawEvents(): Flow<String> = callbackFlow {
        val request = Request.Builder()
            .url("http://$host:8090/stream") // throws on garbage host -> caught by retry loop
            .header("Accept", "text/event-stream")
            .build()

        val listener = object : EventSourceListener() {
            override fun onEvent(eventSource: EventSource, id: String?, type: String?, data: String) {
                trySend(data)
            }

            override fun onClosed(eventSource: EventSource) {
                close() // normal end of stream
            }

            override fun onFailure(eventSource: EventSource, t: Throwable?, response: Response?) {
                close(t ?: IOException("SSE connection failed (HTTP ${response?.code ?: "?"})"))
            }
        }

        val eventSource = EventSources.createFactory(client).newEventSource(request, listener)
        awaitClose { eventSource.cancel() }
    }

    /** Tolerant parse: blank lines, non-JSON noise or bad fields just yield null. */
    private fun parse(data: String): AirSample? {
        val trimmed = data.trim()
        if (trimmed.isEmpty() || !trimmed.startsWith("{")) return null
        return try {
            json.decodeFromString<AirSample>(trimmed)
        } catch (_: Exception) {
            null
        }
    }
}
