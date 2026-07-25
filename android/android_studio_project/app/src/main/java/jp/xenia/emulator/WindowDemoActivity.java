package jp.xenia.emulator;

import android.os.Bundle;
import android.view.SurfaceView;
import android.view.SurfaceHolder;
import androidx.appcompat.app.AppCompatActivity;

public class WindowDemoActivity extends AppCompatActivity implements SurfaceHolder.Callback {
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
            surfaceView.post(() -> nativeBootGame(gamePath, holder.getSurface()));
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

    private native void nativeBootGame(String gamePath, android.view.Surface surface);
    private native void nativeShutdown();
    private native boolean nativeIsRunning();
}
