package jp.xenia.emulator;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import android.widget.Button;
import android.widget.TextView;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;

public class LauncherActivity extends AppCompatActivity {
    private TextView statusText;
    private String selectedFilePath = null;

    private final ActivityResultContracts.OpenDocument openDocumentContract = 
        new ActivityResultContracts.OpenDocument();

    private androidx.activity.result.ActivityResultLauncher<String[]> filePickerLauncher;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_launcher);

        statusText = findViewById(R.id.status_text);
        Button pickButton = findViewById(R.id.pick_button);

        filePickerLauncher = registerForActivityResult(
            new ActivityResultContracts.OpenDocument(),
            uri -> {
                if (uri != null) {
                    selectedFilePath = getRealPathFromUri(uri);
                    if (selectedFilePath != null) {
                        statusText.setText("Selected: " + selectedFilePath + "\nBootting...");
                        launchGame(selectedFilePath);
                    } else {
                        statusText.setText("Failed to access file. Try again.");
                    }
                }
            }
        );

        pickButton.setOnClickListener(v -> openFilePicker());

        openFilePicker();
    }

    private void openFilePicker() {
        filePickerLauncher.launch(new String[]{"application/octet-stream"});
    }

    private String getRealPathFromUri(Uri uri) {
        try {
            InputStream inputStream = getContentResolver().openInputStream(uri);
            if (inputStream == null) return null;

            File tempFile = File.createTempFile("xenia_game", ".tmp", getCacheDir());
            OutputStream outputStream = new FileOutputStream(tempFile);

            byte[] buffer = new byte[1024];
            int bytesRead;
            while ((bytesRead = inputStream.read(buffer)) != -1) {
                outputStream.write(buffer, 0, bytesRead);
            }

            inputStream.close();
            outputStream.close();

            return tempFile.getAbsolutePath();
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    private void launchGame(String filePath) {
        Intent intent = new Intent(this, WindowDemoActivity.class);
        intent.putExtra("game_path", filePath);
        startActivity(intent);
        finish();
    }
}
