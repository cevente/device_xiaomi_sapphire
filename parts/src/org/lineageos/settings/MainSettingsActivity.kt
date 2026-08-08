package org.lineageos.settings

import android.content.Context
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
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
import com.xiaomi.parts.display.ColorProfileManager

class MainSettingsActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        setContent {
            MaterialTheme(
                colorScheme = darkColorScheme(
                    background = Color(0xFF0A0A0A), // Deep industrial black
                    surface = Color(0xFF141414),
                    onSurface = Color(0xFFE0E0E0)
                )
            ) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    ColorProfileScreen(context = this)
                }
            }
        }
    }
}

@Composable
fun ColorProfileScreen(context: Context) {
    val sharedPrefs = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    val colorManager = remember { ColorProfileManager() }
    
    var currentProfile by remember { 
        mutableStateOf(sharedPrefs.getInt("hardware_crc_mode", 0)) 
    }

    val profiles = listOf(
        Pair("SYS_DEFAULT", 0),
        Pair("SRGB_STANDARD", 1),
        Pair("DCI_P3_WIDE", 2),
        Pair("DCI_P3_D65", 3),
        Pair("DCI_P3_FLAT", 4),
        Pair("SRGB_D65", 5)
    )

    Column(modifier = Modifier.fillMaxSize().padding(24.dp)) {
        Text(
            text = "HARDWARE CONFIGURATION",
            style = MaterialTheme.typography.labelSmall.copy(
                fontFamily = FontFamily.Monospace,
                letterSpacing = 1.5.sp
            ),
            color = Color(0xFF666666),
            modifier = Modifier.padding(bottom = 8.dp)
        )
        Text(
            text = "Display Colorimetry",
            style = MaterialTheme.typography.headlineMedium.copy(
                fontWeight = FontWeight.Bold
            ),
            color = Color(0xFFEEEEEE),
            modifier = Modifier.padding(bottom = 32.dp)
        )

        LazyColumn(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            items(profiles) { (name, mode) ->
                val isSelected = currentProfile == mode
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            if (isSelected) Color(0xFF222222) else Color(0xFF121212),
                            shape = RoundedCornerShape(4.dp) // Sharp, industrial edges
                        )
                        .clickable {
                            if (colorManager.setProfile(mode)) {
                                currentProfile = mode
                                sharedPrefs.edit().putInt("hardware_crc_mode", mode).apply()
                            }
                        }
                        .padding(horizontal = 16.dp, vertical = 20.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = name,
                        color = if (isSelected) Color.White else Color(0xFF888888),
                        style = MaterialTheme.typography.bodyLarge.copy(
                            fontFamily = FontFamily.Monospace,
                            fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal
                        )
                    )
                    if (isSelected) {
                        Text(
                            text = "[ ACTIVE ]", 
                            color = Color(0xFFFFFFFF), 
                            style = MaterialTheme.typography.labelMedium.copy(
                                fontFamily = FontFamily.Monospace
                            )
                        )
                    }
                }
            }
        }
    }
}
