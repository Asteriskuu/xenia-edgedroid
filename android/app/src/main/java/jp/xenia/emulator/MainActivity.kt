package jp.xenia.emulator

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import android.view.Surface
import android.view.SurfaceView
import android.view.SurfaceHolder

/**
 * Minimal Android launcher: will be better later on
 */
class MainActivity : AppCompatActivity(), SurfaceHolder.Callback {
    private var surfaceReady = false
    private var pickedFilePath: String? = null

    private val filePickerLauncher = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        if (uri != null) {
            // Convert URI to actual file path via ContentResolver
            pickedFilePath = getRealPathFromUri(uri)
            if (pickedFilePath != null) {
                // Once we have a path, try to launch the emulator
                if (surfaceReady) {
                    launchEmulator(pickedFilePath!!)
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Set up SurfaceView for emulator output
        val surfaceView = findViewById<SurfaceView>(R.id.surface_view)
        surfaceView.holder.addCallback(this)

        // Immediately open file picker on launch
        openFilePicker()
    }

    private fun openFilePicker() {
        // Accept common Xbox 360 ROM formats
        filePickerLauncher.launch(arrayOf("application/octet-stream"))
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        pickedFilePath?.let { launchEmulator(it) }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        // Update surface dimensions if needed
        nativeSetSurfaceSize(width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        nativeShutdownEmulator()
    }

    private fun getRealPathFromUri(uri: Uri): String? {
        return try {
            val inputStream = contentResolver.openInputStream(uri) ?: return null
            val tempFile = createTempFile("xenia_game", ".xex", cacheDir)
            inputStream.copyTo(tempFile.outputStream())
            tempFile.absolutePath
        } catch (e: Exception) {
            e.printStackTrace()
            null
        }
    }

    private fun launchEmulator(filePath: String) {
        // get the Surface from SurfaceView and pass to native code
        val surfaceView = findViewById<SurfaceView>(R.id.surface_view)
        nativeBootEmulator(filePath, surfaceView.holder.surface)
    }

    companion object {
        init {
            System.loadLibrary("xenia-android")
        }
    }

    // JNI methods
    external fun nativeBootEmulator(filePath: String, surface: Surface)
    external fun nativeSetSurfaceSize(width: Int, height: Int)
    external fun nativeShutdownEmulator()
}
