package jp.xenia.emulator;

import android.content.Intent;
import android.os.Bundle;

public class GameActivity extends WindowedAppActivity {
    @Override
    protected String getWindowedAppIdentifier() {
        return "xenia";
    }

    @Override
    protected void onCreate(final Bundle savedInstanceState) {
        final Intent intent = getIntent();
        final Bundle launchArguments = intent.getBundleExtra(WindowedAppActivity.EXTRA_CVARS);
        if (launchArguments != null) {
            final Bundle mergedArguments = new Bundle(launchArguments);
            if (!mergedArguments.containsKey("target") && intent.getData() != null) {
                mergedArguments.putString("target", intent.getData().getPath());
            }
            intent.putExtra(WindowedAppActivity.EXTRA_CVARS, mergedArguments);
        }
        super.onCreate(savedInstanceState);

        setContentView(R.layout.activity_game);
        setWindowSurfaceView(findViewById(R.id.game_surface_view));
    }
}
