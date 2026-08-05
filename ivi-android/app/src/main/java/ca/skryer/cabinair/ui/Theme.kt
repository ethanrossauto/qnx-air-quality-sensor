package ca.skryer.cabinair.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// Status colors used across the screen.
val OkGreen = Color(0xFF3DDC84)
val WarnAmber = Color(0xFFFFB300)
val AlertRed = Color(0xFFE53935)
val IdleGrey = Color(0xFF5C6B78)

private val IviDarkColors = darkColorScheme(
    primary = Color(0xFF7FD1FF),
    onPrimary = Color(0xFF00344A),
    background = Color(0xFF0B0F14),
    onBackground = Color(0xFFE4EAF0),
    surface = Color(0xFF141A21),
    onSurface = Color(0xFFE4EAF0),
    surfaceVariant = Color(0xFF1B232C),
    onSurfaceVariant = Color(0xFFB8C4CF),
    outline = Color(0xFF39434D),
    error = AlertRed
)

/** Always-dark Material3 theme: this is a car screen, day and night. */
@Composable
fun CabinAirTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = IviDarkColors,
        content = content
    )
}
