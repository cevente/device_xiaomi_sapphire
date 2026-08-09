package org.lineageos.settings

import android.content.Context
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.preference.PreferenceManager
import com.xiaomi.parts.touch.TouchManager

class MainSettingsActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(
                colorScheme = darkColorScheme(
                    background = Color(0xFF0A0A0A),
                    surface = Color(0xFF141414),
                    onSurface = Color(0xFFE0E0E0)
                )
            ) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    TouchSettingsScreen(context = this)
                }
            }
        }
    }
}

@Composable
fun TouchSettingsScreen(context: Context) {
    val sharedPrefs = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    val touchManager = remember { TouchManager() }

    var dt2w by remember { mutableStateOf(sharedPrefs.getBoolean("touch_dt2w", false)) }
    var gameMode by remember { mutableStateOf(sharedPrefs.getBoolean("touch_game", false)) }
    var expertMode by remember { mutableStateOf(sharedPrefs.getBoolean("touch_expert", false)) }
    var rfResist by remember { mutableStateOf(sharedPrefs.getBoolean("touch_rf", false)) }
    var gripMode by remember { mutableStateOf(sharedPrefs.getBoolean("touch_grip", false)) }

    Column(modifier = Modifier.fillMaxSize().padding(24.dp)) {
        Text(
            text = "HARDWARE TUNING",
            style = MaterialTheme.typography.labelSmall.copy(
                fontFamily = FontFamily.Monospace,
                letterSpacing = 1.5.sp
            ),
            color = Color(0xFF666666),
            modifier = Modifier.padding(bottom = 8.dp)
        )
        Text(
            text = "Xiaomi Touch Control",
            style = MaterialTheme.typography.headlineMedium.copy(
                fontWeight = FontWeight.Bold
            ),
            color = Color(0xFFEEEEEE),
            modifier = Modifier.padding(bottom = 24.dp)
        )

        LazyColumn(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            item {
                TouchSwitchRow(
                    title = "Double-Tap to Wake",
                    subtitle = "Wake device with screen-off double tap",
                    checked = dt2w
                ) { newState ->
                    if (touchManager.setTouchMode(TouchManager.TOUCH_DOUBLETAP_MODE, newState)) {
                        dt2w = newState
                        sharedPrefs.edit().putBoolean("touch_dt2w", newState).apply()
                    }
                }
            }
            item {
                TouchSwitchRow(
                    title = "Touch Game Mode",
                    subtitle = "High-performance response profile",
                    checked = gameMode
                ) { newState ->
                    if (touchManager.setTouchMode(TouchManager.TOUCH_GAME_MODE, newState)) {
                        gameMode = newState
                        sharedPrefs.edit().putBoolean("touch_game", newState).apply()
                    }
                }
            }
            item {
                TouchSwitchRow(
                    title = "Touch Expert Mode",
                    subtitle = "Advanced custom touch filtering",
                    checked = expertMode
                ) { newState ->
                    if (touchManager.setTouchMode(TouchManager.TOUCH_EXPERT_MODE, newState)) {
                        expertMode = newState
                        sharedPrefs.edit().putBoolean("touch_expert", newState).apply()
                    }
                }
            }
            item {
                TouchSwitchRow(
                    title = "RF Interference Resistance",
                    subtitle = "Mitigate touch jitter from wireless signals",
                    checked = rfResist
                ) { newState ->
                    if (touchManager.setTouchMode(TouchManager.TOUCH_RESIST_RF, newState)) {
                        rfResist = newState
                        sharedPrefs.edit().putBoolean("touch_rf", newState).apply()
                    }
                }
            }
            item {
                TouchSwitchRow(
                    title = "Grip Suppression Mode",
                    subtitle = "Specialized palm and edge rejection",
                    checked = gripMode
                ) { newState ->
                    if (touchManager.setTouchMode(TouchManager.TOUCH_GRIP_MODE, newState)) {
                        gripMode = newState
                        sharedPrefs.edit().putBoolean("touch_grip", newState).apply()
                    }
                }
            }
        }
    }
}

@Composable
fun TouchSwitchRow(title: String, subtitle: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFF121212), shape = RoundedCornerShape(4.dp))
            .padding(horizontal = 16.dp, vertical = 16.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(text = title, color = Color.White, fontWeight = FontWeight.SemiBold)
            Text(text = subtitle, color = Color(0xFF888888), style = MaterialTheme.typography.bodySmall)
        }
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange
        )
    }
}
