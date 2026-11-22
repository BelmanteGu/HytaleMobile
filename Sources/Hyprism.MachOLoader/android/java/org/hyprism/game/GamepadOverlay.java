// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen gamepad drawn over the game. It feeds an SDL virtual gamepad
 * (gamepad.c), so the client sees an ordinary controller.
 *
 * The PAD button toggles it. While it is off, touches outside that button
 * reach the game as mouse input, which suits the menus.
 */
public class GamepadOverlay extends View {
    // SDL_GamepadButton
    static final int SOUTH = 0, EAST = 1, WEST = 2, NORTH = 3, BACK = 4, START = 6,
        LEFT_STICK = 7, RIGHT_STICK = 8, LEFT_SHOULDER = 9, RIGHT_SHOULDER = 10,
        DPAD_UP = 11, DPAD_DOWN = 12, DPAD_LEFT = 13, DPAD_RIGHT = 14;
    // SDL_GamepadAxis
    static final int LEFT_X = 0, LEFT_Y = 1, RIGHT_X = 2, RIGHT_Y = 3, LEFT_TRIGGER = 4, RIGHT_TRIGGER = 5;
    static final int AXIS_MAX = 32767, AXIS_MIN = -32768;

    static native void nativeSetEnabled(boolean enabled);
    static native void nativeAxis(int axis, int value);
    static native void nativeButton(int button, boolean down);

    /** A round control. button >= 0 is a gamepad button; axis >= 0 is a trigger. */
    static final class Control {
        final String label;
        final float fx, fy, fr; // centre and radius as fractions of the view height
        final int button, axis;
        boolean down;

        Control(String label, float fx, float fy, float fr, int button, int axis) {
            this.label = label; this.fx = fx; this.fy = fy; this.fr = fr; this.button = button; this.axis = axis;
        }
    }

    // fx is a fraction of the width, fy and fr fractions of the height.
    private final Control[] controls = {
        new Control("A", 0.890f, 0.80f, 0.075f, SOUTH, -1),
        new Control("B", 0.955f, 0.64f, 0.075f, EAST, -1),
        new Control("X", 0.825f, 0.64f, 0.075f, WEST, -1),
        new Control("Y", 0.890f, 0.48f, 0.075f, NORTH, -1),
        new Control("RT", 0.950f, 0.30f, 0.070f, -1, RIGHT_TRIGGER),
        new Control("LT", 0.800f, 0.30f, 0.070f, -1, LEFT_TRIGGER),
        new Control("RB", 0.950f, 0.14f, 0.055f, RIGHT_SHOULDER, -1),
        new Control("LB", 0.800f, 0.14f, 0.055f, LEFT_SHOULDER, -1),
        new Control("R3", 0.730f, 0.80f, 0.055f, RIGHT_STICK, -1),
        new Control("L3", 0.270f, 0.80f, 0.055f, LEFT_STICK, -1),
        new Control("☰", 0.600f, 0.07f, 0.045f, START, -1),
        new Control("▣", 0.530f, 0.07f, 0.045f, BACK, -1),
        new Control("▲", 0.080f, 0.22f, 0.045f, DPAD_UP, -1),
        new Control("▼", 0.080f, 0.44f, 0.045f, DPAD_DOWN, -1),
        new Control("◀", 0.040f, 0.33f, 0.045f, DPAD_LEFT, -1),
        new Control("▶", 0.120f, 0.33f, 0.045f, DPAD_RIGHT, -1),
    };

    private static final int ROLE_STICK = -2, ROLE_LOOK = -3, ROLE_TOGGLE = -4;
    private final SparseArray<Integer> roles = new SparseArray<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SharedPreferences prefs;
    private boolean enabled;

    private float stickOriginX, stickOriginY, stickX, stickY;
    private boolean stickActive;
    private float lookLastX, lookLastY;
    private final Runnable lookStop = () -> { nativeAxis(RIGHT_X, 0); nativeAxis(RIGHT_Y, 0); };

    public GamepadOverlay(Context context) {
        super(context);
        prefs = context.getSharedPreferences("gamepad", Context.MODE_PRIVATE);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3 * getResources().getDisplayMetrics().density);
        stroke.setColor(0x99FFFFFF);
        text.setColor(0xDDFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    /** Restores the last state once the game has initialised SDL. */
    void restore() {
        setGamepadEnabled(prefs.getBoolean("enabled", false));
    }

    private void setGamepadEnabled(boolean on) {
        enabled = on;
        prefs.edit().putBoolean("enabled", on).apply();
        nativeSetEnabled(on);
        invalidate();
    }

    private RectF toggleRect() {
        float h = getHeight(), w = getWidth();
        return new RectF(w * 0.40f, h * 0.02f, w * 0.40f + h * 0.16f, h * 0.10f);
    }

    private float cx(Control c) { return c.fx * getWidth(); }
    private float cy(Control c) { return c.fy * getHeight(); }
    private float radius(Control c) { return c.fr * getHeight(); }

    private Control hit(float x, float y) {
        for (Control c : controls) {
            float dx = x - cx(c), dy = y - cy(c), r = radius(c) * 1.25f;
            if (dx * dx + dy * dy <= r * r)
                return c;
        }
        return null;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        float x = e.getX(index), y = e.getY(index);

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            if (toggleRect().contains(x, y)) {
                roles.put(id, ROLE_TOGGLE);
                return true;
            }
            if (!enabled)
                return false; // let the game treat it as a mouse
            Control c = hit(x, y);
            if (c != null) {
                roles.put(id, indexOf(c));
                press(c, true);
            } else if (x < getWidth() * 0.45f && !stickActive) {
                roles.put(id, ROLE_STICK);
                stickActive = true;
                stickOriginX = stickX = x;
                stickOriginY = stickY = y;
            } else {
                roles.put(id, ROLE_LOOK);
                lookLastX = x;
                lookLastY = y;
            }
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); i++) {
                Integer role = roles.get(e.getPointerId(i));
                if (role == null)
                    continue;
                float px = e.getX(i), py = e.getY(i);
                if (role == ROLE_STICK)
                    moveStick(px, py);
                else if (role == ROLE_LOOK)
                    look(px, py);
            }
            invalidate();
            return true;
        }

        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_CANCEL) {
            Integer role = roles.get(id);
            roles.remove(id);
            if (role != null)
                release(role, action == MotionEvent.ACTION_CANCEL ? false : toggleRect().contains(x, y));
            if (action == MotionEvent.ACTION_CANCEL)
                releaseAll();
            invalidate();
            return role != null;
        }
        return enabled;
    }

    private int indexOf(Control c) {
        for (int i = 0; i < controls.length; i++)
            if (controls[i] == c)
                return i;
        return -1;
    }

    private void press(Control c, boolean down) {
        c.down = down;
        if (c.axis >= 0)
            nativeAxis(c.axis, down ? AXIS_MAX : AXIS_MIN);
        else
            nativeButton(c.button, down);
    }

    private void release(int role, boolean onToggle) {
        if (role == ROLE_TOGGLE) {
            if (onToggle)
                setGamepadEnabled(!enabled);
        } else if (role == ROLE_STICK) {
            stickActive = false;
            nativeAxis(LEFT_X, 0);
            nativeAxis(LEFT_Y, 0);
        } else if (role == ROLE_LOOK) {
            handler.removeCallbacks(lookStop);
            lookStop.run();
        } else if (role >= 0) {
            press(controls[role], false);
        }
    }

    private void releaseAll() {
        for (Control c : controls)
            if (c.down)
                press(c, false);
        stickActive = false;
        nativeAxis(LEFT_X, 0);
        nativeAxis(LEFT_Y, 0);
        lookStop.run();
        roles.clear();
    }

    private void moveStick(float x, float y) {
        float max = getHeight() * 0.14f;
        float dx = x - stickOriginX, dy = y - stickOriginY;
        float len = (float) Math.hypot(dx, dy);
        if (len > max) {
            dx = dx / len * max;
            dy = dy / len * max;
        }
        stickX = stickOriginX + dx;
        stickY = stickOriginY + dy;
        nativeAxis(LEFT_X, clamp(dx / max));
        nativeAxis(LEFT_Y, clamp(dy / max));
    }

    // A swipe turns the camera: the finger speed becomes a right stick deflection
    // that falls back to zero shortly after the finger stops.
    private void look(float x, float y) {
        float scale = getHeight() * 0.02f;
        nativeAxis(RIGHT_X, clamp((x - lookLastX) / scale));
        nativeAxis(RIGHT_Y, clamp((y - lookLastY) / scale));
        lookLastX = x;
        lookLastY = y;
        handler.removeCallbacks(lookStop);
        handler.postDelayed(lookStop, 50);
    }

    private static int clamp(float v) {
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        return (int) (v * AXIS_MAX);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float h = getHeight();
        RectF t = toggleRect();
        fill.setColor(enabled ? 0x9933AA55 : 0x66000000);
        canvas.drawRoundRect(t, h * 0.02f, h * 0.02f, fill);
        canvas.drawRoundRect(t, h * 0.02f, h * 0.02f, stroke);
        text.setTextSize(h * 0.035f);
        canvas.drawText(enabled ? "PAD ON" : "PAD", t.centerX(), t.centerY() + h * 0.012f, text);
        if (!enabled)
            return;

        for (Control c : controls) {
            fill.setColor(c.down ? 0x99FFFFFF : 0x44000000);
            canvas.drawCircle(cx(c), cy(c), radius(c), fill);
            canvas.drawCircle(cx(c), cy(c), radius(c), stroke);
            text.setTextSize(radius(c) * 0.8f);
            canvas.drawText(c.label, cx(c), cy(c) + radius(c) * 0.28f, text);
        }
        if (stickActive) {
            fill.setColor(0x33FFFFFF);
            canvas.drawCircle(stickOriginX, stickOriginY, h * 0.14f, fill);
            canvas.drawCircle(stickOriginX, stickOriginY, h * 0.14f, stroke);
            fill.setColor(0x88FFFFFF);
            canvas.drawCircle(stickX, stickY, h * 0.06f, fill);
        }
    }
}
