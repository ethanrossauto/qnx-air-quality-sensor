package ca.skryer.cabinair.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateIntAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AcUnit
import androidx.compose.material.icons.filled.DirectionsCar
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Navigation
import androidx.compose.material.icons.filled.Phone
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Warning
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ca.skryer.cabinair.CabinAirViewModel
import ca.skryer.cabinair.model.AirSample
import ca.skryer.cabinair.model.CabinAirState
import java.util.Locale

/** Color for the current air status; grey until we have data. */
private fun statusColor(sample: AirSample?): Color = when {
    sample == null -> IdleGrey
    sample.isAlert -> AlertRed
    sample.status == "WARN" -> WarnAmber
    else -> OkGreen
}

@Composable
fun CabinAirScreen(vm: CabinAirViewModel) {
    val state by vm.state.collectAsState()
    val sample = state.sample
    val alertActive = sample?.isAlert == true

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .padding(16.dp)
    ) {
        TopBar(
            state = state,
            onHostChange = vm::setHost,
            onReconnect = vm::reconnect,
            onDemoToggle = vm::setDemoMode,
            onTriggerAlert = vm::triggerDemoAlert
        )

        Spacer(Modifier.height(12.dp))

        AlertBanner(visible = alertActive, message = sample?.message.orEmpty())

        Row(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            HeroTile(
                sample = sample,
                modifier = Modifier
                    .weight(1.15f)
                    .fillMaxHeight()
            )
            Column(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight(),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Row(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    MetricCard(
                        label = "Temperature",
                        value = sample?.let { String.format(Locale.US, "%.1f", it.temp_c) } ?: "--",
                        unit = "°C",
                        modifier = Modifier.weight(1f).fillMaxHeight()
                    )
                    MetricCard(
                        label = "Humidity",
                        value = sample?.let { String.format(Locale.US, "%.0f", it.rh_pct) } ?: "--",
                        unit = "%RH",
                        modifier = Modifier.weight(1f).fillMaxHeight()
                    )
                }
                Row(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    MetricCard(
                        label = "VOC index",
                        value = sample?.voc_index?.toString() ?: "--",
                        unit = "",
                        modifier = Modifier.weight(1f).fillMaxHeight()
                    )
                    MetricCard(
                        label = "AQI",
                        value = sample?.aqi?.toString() ?: "--",
                        unit = "",
                        modifier = Modifier.weight(1f).fillMaxHeight()
                    )
                }
            }
        }

        CarDock()
    }
}

/** Decorative head-unit dock — the app-launcher row you'd see across the bottom of a car screen. */
@Composable
private fun CarDock() {
    Spacer(Modifier.height(12.dp))
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f),
        modifier = Modifier
            .fillMaxWidth()
            .height(76.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 24.dp),
            horizontalArrangement = Arrangement.SpaceEvenly,
            verticalAlignment = Alignment.CenterVertically
        ) {
            DockItem(Icons.Filled.DirectionsCar, "Car")
            DockItem(Icons.Filled.Navigation, "Nav")
            DockItem(Icons.Filled.MusicNote, "Media")
            DockItem(Icons.Filled.Phone, "Phone")
            // Climate is the screen we're on — highlight it as the active app.
            DockItem(Icons.Filled.AcUnit, "Climate", active = true)
            DockItem(Icons.Filled.Settings, "Settings")
        }
    }
}

@Composable
private fun DockItem(icon: ImageVector, label: String, active: Boolean = false) {
    val tint = if (active) OkGreen else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f)
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier
            .clip(RoundedCornerShape(12.dp))
            .clickable { /* decorative in this demo */ }
            .padding(horizontal = 18.dp, vertical = 6.dp)
    ) {
        Icon(icon, contentDescription = label, tint = tint, modifier = Modifier.size(28.dp))
        Spacer(Modifier.height(4.dp))
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = tint,
            letterSpacing = 0.5.sp
        )
    }
}

@Composable
private fun TopBar(
    state: CabinAirState,
    onHostChange: (String) -> Unit,
    onReconnect: () -> Unit,
    onDemoToggle: (Boolean) -> Unit,
    onTriggerAlert: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Live dot: green when data has been seen within the freshness window.
        val dotColor by animateColorAsState(
            targetValue = if (state.connected) OkGreen else IdleGrey,
            animationSpec = tween(300),
            label = "liveDot"
        )
        Box(
            Modifier
                .size(14.dp)
                .clip(CircleShape)
                .background(dotColor)
        )
        Spacer(Modifier.width(10.dp))

        Text(
            text = "CABIN AIR",
            style = MaterialTheme.typography.titleLarge,
            fontWeight = FontWeight.Bold,
            letterSpacing = 3.sp
        )
        Spacer(Modifier.width(14.dp))

        val sourceLabel = state.sample
            ?.let { listOf(it.source, it.bus).filter(String::isNotBlank).joinToString(" · ") }
            .orEmpty()
        Text(
            text = sourceLabel,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.55f)
        )

        Spacer(Modifier.weight(1f))

        if (state.demoMode) {
            TextButton(onClick = onTriggerAlert) {
                Text("Trigger ALERT", color = AlertRed)
            }
            Spacer(Modifier.width(4.dp))
        }
        Text("DEMO", style = MaterialTheme.typography.labelMedium)
        Spacer(Modifier.width(6.dp))
        Switch(checked = state.demoMode, onCheckedChange = onDemoToggle)

        Spacer(Modifier.width(14.dp))

        OutlinedTextField(
            value = state.host,
            onValueChange = onHostChange,
            label = { Text("Pi host") },
            singleLine = true,
            enabled = !state.demoMode,
            textStyle = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.width(180.dp)
        )
        Spacer(Modifier.width(10.dp))

        Button(onClick = onReconnect, enabled = !state.demoMode) {
            Icon(Icons.Filled.Refresh, contentDescription = null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(6.dp))
            Text("Reconnect")
        }
    }
}

@Composable
private fun HeroTile(sample: AirSample?, modifier: Modifier = Modifier) {
    val statusCol by animateColorAsState(
        targetValue = statusColor(sample),
        animationSpec = tween(500),
        label = "statusColor"
    )

    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(24.dp),
        color = statusCol.copy(alpha = 0.16f),
        border = BorderStroke(2.dp, statusCol)
    ) {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            if (sample == null) {
                Text(
                    text = "Waiting for cabin sensor…",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.6f),
                    textAlign = TextAlign.Center
                )
            } else {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    // Smooth the ~4 Hz value updates instead of hard-jumping.
                    val pm by animateIntAsState(
                        targetValue = sample.pm2_5,
                        animationSpec = tween(300),
                        label = "pm25"
                    )
                    Text(
                        text = pm.toString(),
                        fontSize = 110.sp,
                        fontWeight = FontWeight.Bold,
                        color = statusCol
                    )
                    Text(
                        text = "µg/m³ PM2.5",
                        style = MaterialTheme.typography.titleMedium,
                        color = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.75f)
                    )
                    Spacer(Modifier.height(10.dp))
                    Text(
                        text = sample.status,
                        style = MaterialTheme.typography.labelLarge,
                        letterSpacing = 4.sp,
                        color = statusCol
                    )
                }
            }
        }
    }
}

@Composable
private fun MetricCard(label: String, value: String, unit: String, modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(18.dp),
        color = MaterialTheme.colorScheme.surfaceVariant
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(14.dp),
            verticalArrangement = Arrangement.Center
        ) {
            Text(
                text = label.uppercase(Locale.US),
                style = MaterialTheme.typography.labelSmall,
                letterSpacing = 1.5.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
            )
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    text = value,
                    style = MaterialTheme.typography.displaySmall,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onSurface
                )
                if (unit.isNotEmpty()) {
                    Spacer(Modifier.width(6.dp))
                    Text(
                        text = unit,
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(bottom = 6.dp)
                    )
                }
            }
        }
    }
}

@Composable
private fun AlertBanner(visible: Boolean, message: String) {
    AnimatedVisibility(
        visible = visible,
        enter = expandVertically() + fadeIn(),
        exit = shrinkVertically() + fadeOut()
    ) {
        // Spacer lives inside the animated content so it collapses with the banner.
        Column {
            Surface(
                shape = RoundedCornerShape(16.dp),
                color = AlertRed,
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector = Icons.Filled.Warning,
                        contentDescription = "Alert",
                        tint = Color.White,
                        modifier = Modifier.size(30.dp)
                    )
                    Spacer(Modifier.width(14.dp))
                    Text(
                        text = message.ifBlank { "Cabin air quality alert" },
                        style = MaterialTheme.typography.titleMedium,
                        color = Color.White,
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(Modifier.width(14.dp))
                    RecirculatePill()
                }
            }
            Spacer(Modifier.height(12.dp))
        }
    }
}

/** Visual-only recirculation toggle; nothing is actuated in this demo. */
@Composable
private fun RecirculatePill() {
    var on by rememberSaveable { mutableStateOf(false) }
    val shape = RoundedCornerShape(50)
    Box(
        modifier = Modifier
            .clip(shape)
            .background(if (on) Color.White else Color.Transparent)
            .border(BorderStroke(1.5.dp, Color.White), shape)
            .clickable { on = !on }
            .padding(horizontal = 18.dp, vertical = 8.dp)
    ) {
        Text(
            text = if (on) "RECIRC ON" else "RECIRCULATE",
            style = MaterialTheme.typography.labelLarge,
            letterSpacing = 1.sp,
            color = if (on) AlertRed else Color.White
        )
    }
}
