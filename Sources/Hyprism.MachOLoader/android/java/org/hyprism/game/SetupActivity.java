// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/**
 * The launcher's Play button lands here, in the game's process: it prepares the world server (see
 * {@link ServerSetup}), which takes a moment only after the app or the game was updated, and then
 * opens {@link HytaleActivity} with the launcher's arguments.
 */
public class SetupActivity extends Activity {
    private TextView status;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(Color.rgb(16, 18, 24));
        ProgressBar spinner = new ProgressBar(this);
        layout.addView(spinner);
        status = new TextView(this);
        status.setTextColor(Color.rgb(220, 224, 232));
        status.setTextSize(16);
        status.setGravity(Gravity.CENTER);
        status.setPadding(48, 32, 48, 0);
        status.setText("Preparando o jogo…");
        layout.addView(status);
        setContentView(layout);
        hideSystemBars();

        ServerSetup setup = new ServerSetup(this, message -> runOnUiThread(() -> status.setText(message)));
        if (!setup.client().isFile()) {
            spinner.setVisibility(ProgressBar.GONE);
            status.setText("O Hytale ainda não está instalado.\nAbra o launcher e toque em Instalar.");
            return;
        }
        Thread worker = new Thread(() -> {
            try {
                setup.run();
                runOnUiThread(this::startGame);
            } catch (Exception e) {
                Log.e("HytaleSetup", "setup failed", e);
                runOnUiThread(() -> {
                    spinner.setVisibility(ProgressBar.GONE);
                    status.setText("Não foi possível preparar o servidor do jogo:\n" + e.getMessage());
                });
            }
        }, "HytaleSetup");
        worker.start();
    }

    private void startGame() {
        Intent game = new Intent(this, HytaleActivity.class);
        if (getIntent() != null && getIntent().getExtras() != null)
            game.putExtras(getIntent().getExtras());
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        startActivity(game);
        finish();
    }

    private void hideSystemBars() {
        getWindow().setDecorFitsSystemWindows(false);
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller == null)
            return;
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }
}
