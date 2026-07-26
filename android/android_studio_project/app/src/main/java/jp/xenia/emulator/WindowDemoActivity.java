package jp.xenia.emulator;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.OpenableColumns;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.io.File;
import java.io.IOException;
import android.util.Log;

public class WindowDemoActivity extends AppCompatActivity implements TextureView.SurfaceTextureListener, View.OnLayoutChangeListener {
    private static final String TAG = "WindowDemoActivity";
    private TextureView textureView;
    private HandlerThread rendererThread;
    private Handler rendererHandler;
    private String gamePath = null;
    private boolean surfaceReady = false;
    private boolean layoutReady = false;
    private int surfaceWidth = 0;
    private int surfaceHeight = 0;

    static {
        System.loadLibrary("xenia-app");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_window_demo);

        gamePath = getIntent().getStringExtra("game_path");

        textureView = findViewById(R.id.window_demo_texture_view);
        if (textureView == null) {
            Log.e(TAG, "TextureView not found in layout!");
            finish();
            return;
        }

        textureView.setSurfaceTextureListener(this);
        textureView.addOnLayoutChangeListener(this);
    }

    @Override
    public void onSurfaceTextureAvailable(android.graphics.SurfaceTexture surfaceTexture, int width, int height) {
        Log.i(TAG, "onSurfaceTextureAvailable: " + width + "x" + height);
        
        surfaceWidth = width;
        surfaceHeight = height;
        surfaceReady = true;

        if (rendererThread == null) {
            rendererThread = new HandlerThread("XeniaRenderer");
            rendererThread.start();
            rendererHandler = new Handler(rendererThread.getLooper());
        }

        if (layoutReady && gamePath != null) {
            launchGame(new Surface(surfaceTexture));
        }
    }

    @Override
    public void onSurfaceTextureSizeChanged(android.graphics.SurfaceTexture surface, int width, int height) {
        Log.i(TAG, "onSurfaceTextureSizeChanged: " + width + "x" + height);
        surfaceWidth = width;
        surfaceHeight = height;
    }

    @Override
    public boolean onSurfaceTextureDestroyed(android.graphics.SurfaceTexture surface) {
        Log.i(TAG, "onSurfaceTextureDestroyed");
        surfaceReady = false;
        nativeShutdown();
        return true;
    }

    @Override
    public void onSurfaceTextureUpdated(android.graphics.SurfaceTexture surface) {
    }

    @Override
    public void onLayoutChange(android.view.View v, int left, int top, int right, int bottom,
                             int oldLeft, int oldTop, int oldRight, int oldBottom) {
        int width = right - left;
        int height = bottom - top;

        if (width > 0 && height > 0 && !layoutReady) {
            Log.i(TAG, "Layout finalized: " + width + "x" + height);
            layoutReady = true;
            surfaceWidth = width;
            surfaceHeight = height;

            if (surfaceReady && gamePath != null && textureView != null) {
                android.graphics.SurfaceTexture surfaceTexture = textureView.getSurfaceTexture();
                if (surfaceTexture != null) {
                    launchGame(new Surface(surfaceTexture));
                }
            }
        }
    }

    private void launchGame(Surface surface) {
        if (gamePath == null) {
            Log.e(TAG, "Game path is null!");
            Toast.makeText(this, "Game path not provided", Toast.LENGTH_LONG).show();
            return;
        }

        Log.i(TAG, "Preparing to launch game: " + gamePath);

        try {
            String launchPath = gamePath;
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

            final String finalLaunchPath = launchPath;

            if (rendererHandler != null) {
                rendererHandler.post(() -> {
                    Log.i(TAG, "Launching game from renderer thread: " + finalLaunchPath);
                    nativeBootGame(finalLaunchPath, surface);
                });
            } else {
                Log.e(TAG, "Renderer handler is null!");
                nativeBootGame(finalLaunchPath, surface);
            }
        } catch (Exception e) {
            Log.e(TAG, "Error preparing game file: " + e.getMessage(), e);
            Toast.makeText(this, "Failed to prepare game file", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        
        if (rendererHandler != null) {
            rendererHandler.post(this::nativeShutdown);
        } else {
            nativeShutdown();
        }

        if (rendererThread != null) {
            rendererThread.quit();
            try {
                rendererThread.join(5000);
            } catch (InterruptedException e) {
                Log.e(TAG, "Interrupted waiting for renderer thread to quit", e);
            }
            rendererThread = null;
        }
        
        rendererHandler = null;
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

    private native void nativeBootGame(String gamePath, Surface surface);
    private native void nativeShutdown();
    private native boolean nativeIsRunning();
}
