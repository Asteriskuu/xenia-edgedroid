package jp.xenia.emulator;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.SurfaceView;
import android.view.SurfaceHolder;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.io.File;
import java.io.IOException;
import android.util.Log;

public class WindowDemoActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    private static final String TAG = "WindowDemoActivity";
    private boolean surfaceReady = false;
    private String gamePath = null;

    static {
        System.loadLibrary("xenia-app");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_window_demo);

        gamePath = getIntent().getStringExtra("game_path");

        SurfaceView surfaceView = findViewById(R.id.window_demo_surface_view);
        surfaceView.getHolder().addCallback(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        if (gamePath != null) {
            SurfaceView surfaceView = findViewById(R.id.window_demo_surface_view);
            surfaceView.post(() -> {
                String launchPath = gamePath;
                try {
                    if (launchPath.startsWith("content://")) {
                        Uri uri = Uri.parse(launchPath);
                        try {
                            int takeFlags = getIntent().getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                            if (takeFlags != 0) {
                                getContentResolver().takePersistableUriPermission(uri, takeFlags);
                            }
                        } catch (Exception e) {
                            Log.w(TAG, "takePersistableUriPermission failed: " + e.getMessage());
                        }
                        String copied = copyDocumentToAppCache(uri);
                        if (copied == null) {
                            Toast.makeText(this, "Failed to access the selected file", Toast.LENGTH_LONG).show();
                            Log.e(TAG, "Failed to copy content URI to app cache: " + launchPath);
                            return;
                        }
                        launchPath = copied;
                    } else if (launchPath.startsWith("file://")) {
                        Uri uri = Uri.parse(launchPath);
                        launchPath = uri.getPath();
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Error preparing game file: " + e.getMessage(), e);
                    Toast.makeText(this, "Failed to prepare game file", Toast.LENGTH_LONG).show();
                    return;
                }

                nativeBootGame(launchPath, holder.getSurface());
            });
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        nativeShutdown();
    }

    private String copyDocumentToAppCache(Uri uri) {
        Cursor cursor = null;
        String displayName = null;
        try {
            cursor = getContentResolver().query(uri, null, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (nameIndex != -1) {
                    displayName = cursor.getString(nameIndex);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed to query display name: " + e.getMessage());
        } finally {
            if (cursor != null) cursor.close();
        }
        if (displayName == null) {
            displayName = "game.xex";
        }

        File outDir = getCacheDir();
        File outFile = new File(outDir, displayName);
        int suffix = 1;
        while (outFile.exists()) {
            String base = displayName;
            String ext = "";
            int dot = displayName.lastIndexOf('.');
            if (dot != -1) {
                base = displayName.substring(0, dot);
                ext = displayName.substring(dot);
            }
            outFile = new File(outDir, base + "_" + suffix + ext);
            suffix++;
        }

        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(outFile)) {
            if (in == null) {
                Log.e(TAG, "ContentResolver.openInputStream returned null");
                return null;
            }
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            out.flush();
            Log.i(TAG, "Copied URI to cache: " + outFile.getAbsolutePath());
            return outFile.getAbsolutePath();
        } catch (IOException e) {
            Log.e(TAG, "Failed to copy content uri", e);
            return null;
        }
    }

    private native void nativeBootGame(String gamePath, android.view.Surface surface);
    private native void nativeShutdown();
    private native boolean nativeIsRunning();
}
