package jp.xenia.emulator

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import android.widget.Button
import android.widget.TextView

class LauncherActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private var selectedFilePath: String? = null

    private val filePickerLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            selectedFilePath = getRealPathFromUri(uri)
            if (selectedFilePath != null) {
                statusText.text = "Selected: $selectedFilePath\nBootting..."
                launchGame(selectedFilePath!!)
            } else {
                statusText.text = "Failed to access file. Try again."
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launcher)

        statusText = findViewById(R.id.status_text)
        val pickButton = findViewById<Button>(R.id.pick_button)

        pickButton.setOnClickListener {
            openFilePicker()
        }

        openFilePicker()
    }

    private fun openFilePicker() {
        filePickerLauncher.launch(arrayOf("application/octet-stream"))
    }

    private fun getRealPathFromUri(uri: Uri): String? {
        return try {
            val inputStream = contentResolver.openInputStream(uri) ?: return null
            val tempFile = createTempFile("xenia_game", ".tmp", cacheDir)
            inputStream.use { input ->
                tempFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
            tempFile.absolutePath
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    private fun launchGame(filePath: String) {
        val intent = Intent(this, WindowDemoActivity::class.java)
        intent.putExtra("game_path", filePath)
        startActivity(intent)
        finish()
    }
}
