// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * The phone options, inside the game: a native panel drawn over it, opened from the gear in the touch
 * controls' top bar. The game's own settings screen is the client's and cannot take new entries, so
 * the options that only exist on the phone live here instead of in the launcher.
 */
final class GameSettingsPanel extends FrameLayout {
    private static final int BG = 0xFF16181D, TEXT = 0xFFE9ECF2, HINT = 0xFF9AA0AB, ACCENT = 0xFFF2B33D,
        CHIP = 0xFF262A32, LINE = 0xFF2C3038;

    private final HytaleActivity activity;
    private final GamepadOverlay pad;
    private final LinearLayout card;
    private final TextView restartNote;
    private final Button restartButton;
    private final TextView[] chips = new TextView[GameSettings.RESOLUTIONS.length];

    GameSettingsPanel(HytaleActivity activity, GamepadOverlay pad) {
        super(activity);
        this.activity = activity;
        this.pad = pad;
        setVisibility(GONE);
        setClickable(true); // swallows every touch, so nothing reaches the game or the controls
        setOnClickListener(v -> hide()); // a tap outside the card closes it
        setBackgroundColor(0xB3000000);

        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(false);
        card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        int pad16 = dp(20);
        card.setPadding(pad16, pad16, pad16, pad16);
        card.setBackground(round(BG, dp(16)));
        scroll.addView(card, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(560), ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.CENTER);
        lp.topMargin = lp.bottomMargin = dp(12);
        addView(scroll, lp);

        LinearLayout header = new LinearLayout(activity);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("Configurações do celular", 20, TEXT, true);
        header.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button close = button("Fechar", false);
        close.setOnClickListener(v -> hide());
        header.addView(close);
        card.addView(header);

        // 3D resolution
        LinearLayout chipRow = new LinearLayout(activity);
        chipRow.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < chips.length; i++) {
            int height = GameSettings.RESOLUTIONS[i];
            TextView chip = text(height == 0 ? "Jogo" : height + "p", 14, TEXT, true);
            chip.setGravity(Gravity.CENTER);
            chip.setPadding(dp(10), dp(8), dp(10), dp(8));
            chip.setOnClickListener(v -> {
                GameSettings.setRenderHeight(activity, height);
                paintChips();
                refreshRestart();
            });
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
            cp.leftMargin = i == 0 ? 0 : dp(6);
            chipRow.addView(chip, cp);
            chips[i] = chip;
        }
        row("Resolução do 3D",
            "Desenha o mundo nessa altura e estica para a tela; a interface continua nítida. Menor é mais leve, bom para celulares fracos. \"Jogo\" usa a escala do próprio jogo.",
            chipRow, true);

        row("Modo bateria",
            "Joga a uns 45 FPS (30 se a tela não tiver esse modo) em vez de 60: esquenta e gasta menos bateria.",
            toggle(GameSettings.batterySaver(activity), on -> {
                GameSettings.setBatterySaver(activity, on);
                refreshRestart();
            }), false);

        row("Texturas mais leves",
            "Carrega as texturas grandes pela metade: menos memória de vídeo, um pouco menos nítido de perto.",
            toggle(GameSettings.halfTextures(activity), on -> {
                GameSettings.setHalfTextures(activity, on);
                refreshRestart();
            }), false);

        row("Pré-gerar mundos novos",
            "Antes de jogar um mundo novo, gera 1 km em volta de você para explorar depois ficar mais leve. Leva alguns minutos e dá para pular.",
            toggle(GameSettings.pregenerate(activity), on -> GameSettings.setPregenerate(activity, on)), false);

        row("Tocar para mirar (estilo Bedrock)",
            "Tocar num ponto da tela vira a câmera até ele e usa ou coloca; segurar quebra ou bate; arrastar mexe a câmera. O analógico fica no canto inferior esquerdo.",
            toggle(GameSettings.touchAim(activity), on -> {
                GameSettings.setTouchAim(activity, on);
                pad.reloadSettings();
            }), false);

        SeekBar aim = new SeekBar(activity);
        aim.setMax(150); // 50% to 200%
        aim.setProgress(Math.round((GameSettings.aimScale(activity) - 0.5f) * 100));
        aim.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (!fromUser)
                    return;
                GameSettings.setAimScale(activity, 0.5f + progress / 100f);
                pad.reloadSettings();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) { }
            @Override public void onStopTrackingTouch(SeekBar bar) { }
        });
        row("Ajuste da mira por toque",
            "Se a câmera gira de menos ao tocar, aumente; se passa do ponto, diminua.", aim, true);

        SeekBar size = new SeekBar(activity);
        size.setMax(100); // 60% to 160%
        size.setProgress(Math.round((GameSettings.controlSize(activity) - 0.6f) * 100));
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (!fromUser)
                    return;
                GameSettings.setControlSize(activity, 0.6f + progress / 100f);
                pad.reloadSettings();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) { }
            @Override public void onStopTrackingTouch(SeekBar bar) { }
        });
        row("Tamanho dos controles", "Aumenta ou diminui todos os botões da tela de uma vez.", size, true);

        Button edit = button("Posição e opacidade dos controles…", true);
        edit.setOnClickListener(v -> {
            hide();
            pad.startEditor();
        });
        LinearLayout.LayoutParams ep = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        ep.topMargin = dp(14);
        card.addView(edit, ep);

        restartNote = text("Resolução, modo bateria e texturas valem depois de reiniciar o jogo.", 13, ACCENT, false);
        restartNote.setPadding(0, dp(14), 0, dp(6));
        card.addView(restartNote);
        restartButton = button("Reiniciar o jogo agora", true);
        restartButton.setOnClickListener(v -> activity.restartGame());
        card.addView(restartButton, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT));
        paintChips();
        refreshRestart();
    }

    void show() {
        paintChips();
        refreshRestart();
        pad.panelOpened();
        setVisibility(VISIBLE);
    }

    void hide() {
        setVisibility(GONE);
        pad.panelClosed();
    }

    boolean isOpen() {
        return getVisibility() == VISIBLE;
    }

    private void refreshRestart() {
        boolean needed = GameSettings.restartNeeded(activity);
        restartNote.setText(needed
            ? "Mudou algo que só vale depois de reiniciar o jogo."
            : "Resolução, modo bateria e texturas valem depois de reiniciar o jogo.");
        restartNote.setTextColor(needed ? ACCENT : HINT);
        restartButton.setVisibility(needed ? VISIBLE : GONE);
    }

    private void paintChips() {
        int selected = GameSettings.renderHeight(activity);
        for (int i = 0; i < chips.length; i++) {
            boolean on = GameSettings.RESOLUTIONS[i] == selected;
            chips[i].setBackground(round(on ? ACCENT : CHIP, dp(8)));
            chips[i].setTextColor(on ? 0xFF141409 : TEXT);
        }
    }

    // ---- small view helpers ---------------------------------------------------------------------

    private interface OnChange {
        void changed(boolean on);
    }

    private Switch toggle(boolean on, OnChange change) {
        Switch s = new Switch(activity);
        s.setChecked(on);
        s.setOnCheckedChangeListener((b, checked) -> change.changed(checked));
        return s;
    }

    /** A label and hint with the control below (wide controls) or beside (switches). */
    private void row(String label, String hint, View control, boolean below) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(below ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        row.setGravity(below ? Gravity.START : Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(14), 0, dp(14));
        LinearLayout words = new LinearLayout(activity);
        words.setOrientation(LinearLayout.VERTICAL);
        words.addView(text(label, 16, TEXT, true));
        TextView h = text(hint, 12, HINT, false);
        h.setPadding(0, dp(2), 0, below ? dp(8) : 0);
        words.addView(h);
        if (below) {
            row.addView(words);
            row.addView(control, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        } else {
            row.addView(words, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
            cp.leftMargin = dp(16);
            row.addView(control, cp);
        }
        View line = new View(activity);
        line.setBackgroundColor(LINE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1);
        lp.topMargin = dp(8);
        card.addView(line, lp);
        card.addView(row);
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(activity);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold)
            t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Button button(String s, boolean accent) {
        Button b = new Button(activity);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(accent ? 0xFF141409 : TEXT);
        b.setBackground(round(accent ? ACCENT : CHIP, dp(10)));
        return b;
    }

    private static GradientDrawable round(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
