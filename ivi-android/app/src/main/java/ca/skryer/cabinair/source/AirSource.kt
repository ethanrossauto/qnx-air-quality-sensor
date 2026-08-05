package ca.skryer.cabinair.source

import ca.skryer.cabinair.model.AirSample
import kotlinx.coroutines.flow.Flow

/**
 * A stream of cabin-air samples. Implemented by the real SSE client (SseSource)
 * and a local simulator (FakeSource) so the UI/ViewModel never care where data
 * comes from.
 */
interface AirSource {
    /** Cold flow of parsed samples. Collect to start, cancel to stop. */
    fun samples(): Flow<AirSample>
}
