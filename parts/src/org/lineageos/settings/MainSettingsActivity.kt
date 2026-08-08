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
import com.xiaomi.parts.display.CabcManager

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
                    CabcScreen(context = this)
                }
            }
        }
    }
}

@Composable
fun CabcScreen(context: Context) {
    val sharedPrefs = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    val cabcManager = remember { CabcManager() }
    
    var currentCabc by remember { 
        mutableStateOf(sharedPrefs.getInt("lcd_cabc_mode", 0)) 
    }

    val cabcModes = listOf(
        Pair("CABC Off", 0),       // LCD_CABC_OFF[span_7](start_span)[span_7](end_span)
        Pair("UI Mode", 1),        // LCD_CABC_UI_ON[span_8](start_span)[span_8](end_span)
        Pair("Movie Mode", 2),     // LCD_CABC_MOVIE_ON[span_9](start_span)[span_9](end_span)
        Pair("Still Image Mode", 3) // LCD_CABC_STILL_ON[span_10](start_span)[span_10](end_span)
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
            text = "Content Adaptive Backlight (CABC)",
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
            items(cabcModes) { (name, mode) ->
                val isSelected = currentCabc == mode
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            if (isSelected) Color(0xFF222222) else Color(0xFF121212),
                            shape = RoundedCornerShape(4.dp)
                        )
                        .clickable {
                            if (cabcManager.setCabc(mode)) {
                                currentCabc = mode
                                sharedPrefs.edit().putInt("lcd_cabc_mode", mode).apply()
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
