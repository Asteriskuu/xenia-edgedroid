package jp.xenia.emulator;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.OpenableColumns;
import android.view.Choreographer;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.TextView;
import android.widget.Toast;
import androidx.appcompat.app.AppCompatActivity;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.io.File;
import java.io.IOException;
import android.util.Log;
import android.opengl.GLES20;

public class WindowDemoActivity extends AppCompatActivity implements SurfaceHolder.Callback {
    private static final String TAG = "WindowDemoActivity";
    private SurfaceView surfaceView;
    private HandlerThread rendererThread;
    private Handler rendererHandler;
    private String gamePath = null;
    private boolean surfaceReady = false;
    private boolean layoutReady = false;
    private int surfaceWidth = 0;
    private int surfaceHeight = 0;
    private Surface mActiveSurface;

    private TextView debugOverlay;
    private final double[] frameTimes = new double[60];
    private int frameTimesIndex = 0;
    private boolean frameTimesFilled = false;
    private long lastFrameTimeNanos = 0;
    private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            if (lastFrameTimeNanos != 0) {
                double delta = (frameTimeNanos - lastFrameTimeNanos) / 1_000_000_000.0; // seconds
                frameTimes[frameTimesIndex] = delta;
                frameTimesIndex = (frameTimesIndex + 1) % frameTimes.length;
                if (frameTimesIndex == 0) frameTimesFilled = true;
            }
            lastFrameTimeNanos = frameTimeNanos;
            updateDebugOverlay();
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

    static {
        System.loadLibrary("xenia-app");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_window_demo);

        gamePath = getIntent().getStringExtra("game_path");

        surfaceView = findViewById(R.id.window_demo_surface_view);
        if (surfaceView == null) {
            Log.e(TAG, "SurfaceView not found in layout!");
            finish();
            return;
        }

        surfaceView.getHolder().addCallback(this);

        debugOverlay = new TextView(this);
        debugOverlay.setId(View.generateViewId());
        debugOverlay.setTextColor(0xFFFFFFFF);
        debugOverlay.setBackgroundColor(0x7F000000);
        debugOverlay.setTextSize(12);
        debugOverlay.setPadding(8, 8, 8, 8);
        debugOverlay.setTypeface(android.graphics.Typeface.MONOSPACE);

        View root = findViewById(android.R.id.content);
        if (root instanceof android.view.ViewGroup) {
            android.view.ViewGroup vg = (android.view.ViewGroup) root;
            android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(
                    android.widget.FrameLayout.LayoutParams.WRAP_CONTENT,
                    android.widget.FrameLayout.LayoutParams.WRAP_CONTENT
            );
            lp.leftMargin = 8;
            lp.topMargin = 8;
            lp.gravity = android.view.Gravity.TOP | android.view.Gravity.LEFT;
            vg.addView(debugOverlay, lp);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        Choreographer.getInstance().postFrameCallback(frameCallback);
    }

    @Override
    protected void onPause() {
        super.onPause();
        Choreographer.getInstance().removeFrameCallback(frameCallback);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated");
        surfaceReady = true;
        mActiveSurface = holder.getSurface();

        if (rendererThread == null) {
            rendererThread = new HandlerThread("XeniaRenderer");
            rendererThread.start();
            rendererHandler = new Handler(rendererThread.getLooper());
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        surfaceWidth = width;
        surfaceHeight = height;

        if (width > 0 && height > 0 && !layoutReady) {
            layoutReady = true;
            surfaceView.postDelayed(() -> {
                if (surfaceReady && gamePath != null && mActiveSurface != null) {
                    launchGame(mActiveSurface);
                }
            }, 500);
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed");
        surfaceReady = false;
        layoutReady = false;
        mActiveSurface = null;
        nativeShutdown();
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

            Log.i(TAG, "BEFORE nativeBootGame");
            nativeBootGame(finalLaunchPath, surface);
            Log.i(TAG, "AFTER nativeBootGame");

        } catch (Exception e) {
            Log.e(TAG, "Error preparing game file: " + e.getMessage(), e);
            Toast.makeText(this, "Failed to prepare game file", Toast.LENGTH_LONG).show();
        }
    }

    private void updateDebugOverlay() {
        double sum = 0.0;
        int count = frameTimesFilled ? frameTimes.length : frameTimesIndex;
        for (int i = 0; i < count; ++i) sum += frameTimes[i];
        double avg = (count > 0) ? (sum / count) : (1.0 / 60.0);
        double fps = (avg > 0.0) ? (1.0 / avg) : 0.0;
        double last_ms = (count > 0) ? (frameTimes[(frameTimesIndex - 1 + frameTimes.length) % frameTimes.length] * 1000.0) : (1000.0 / 60.0);

        String vulkan = "Vulkan: ???";
        String surface = "Surface: " + surfaceWidth + "x" + surfaceHeight;
        String swapchain = nativeIsRunning() ? "Swapchain: OK" : "Swapchain: Missing";

        String cpu = "CPU backend: " + (Build.SUPPORTED_ABIS.length > 0 && Build.SUPPORTED_ABIS[0].contains("arm64") ? "A64" : (Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "Unknown"));

        String gpu = "GPU: Unknown";
        try {
            String renderer = GLES20.glGetString(GLES20.GL_RENDERER);
            if (renderer != null && !renderer.isEmpty()) gpu = "GPU: " + renderer;
        } catch (Exception e) {
            // ignore
        }

        String game = "Game: Unknown";
        if (gamePath != null) {
            File f = new File(gamePath);
            game = "Game: " + f.getName();
        }
        String stage = "Stage: Booting...";

        String text = String.format("FPS:  %.0f\nFrame time: %.1f ms\n\n%s\n%s\n%s\n\n%s\n%s\n\n%s\n%s",
                fps, last_ms, vulkan, surface, swapchain, cpu, gpu, game, stage);

        runOnUiThread(() -> debugOverlay.setText(text));
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
