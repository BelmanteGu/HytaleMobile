// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

import java.io.IOException;
import java.io.InputStream;

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

    /**
     * A square control: an action icon (assets/icons, Lucide) with the controller label in its corner.
     * button >= 0 is a gamepad button; axis >= 0 is a trigger; a chord holds RIGHT_SHOULDER and then
     * presses its button, which is how Hytale's controller bindings reach the abilities.
     */
    static final class Control {
        final String label, icon;
        final float fx, fy, side; // centre (fx of the width, fy of the height) and side (of the height)
        final int button, axis;
        final boolean chord;
        Bitmap bitmap;
        boolean down;

        Control(String label, String icon, float fx, float fy, float side, int button, int axis) {
            this(label, icon, fx, fy, side, button, axis, false);
        }

        Control(String label, String icon, float fx, float fy, float side, int button, int axis, boolean chord) {
            this.label = label; this.icon = icon; this.fx = fx; this.fy = fy; this.side = side;
            this.button = button; this.axis = axis; this.chord = chord;
        }
    }

    // Hytale's default controller bindings (Settings > Controls, build 34): A jump, R3 crouch/slide/roll,
    // L3 sprint, RT primary action, LT secondary action, X interact, RB + Y/X/B/A abilities 1-4,
    // D-pad left emotes, right utility item, down drop item, B back in menus.
    private final Control[] controls = {
        new Control("A", "a", 0.890f, 0.80f, 0.185f, SOUTH, -1),
        new Control("R3", "r3", 0.955f, 0.62f, 0.150f, RIGHT_STICK, -1),
        new Control("X", "x", 0.825f, 0.62f, 0.150f, WEST, -1),
        new Control("Y", "y", 0.890f, 0.44f, 0.140f, NORTH, -1),
        new Control("RT", "rt", 0.955f, 0.29f, 0.170f, -1, RIGHT_TRIGGER),
        new Control("LT", "lt", 0.810f, 0.29f, 0.150f, -1, LEFT_TRIGGER),
        new Control("RB", "rb", 0.955f, 0.11f, 0.120f, RIGHT_SHOULDER, -1),
        new Control("LB", "lb", 0.825f, 0.11f, 0.120f, LEFT_SHOULDER, -1),
        new Control("RB+Y", "ab1", 0.700f, 0.11f, 0.110f, NORTH, -1, true),
        new Control("RB+X", "ab2", 0.700f, 0.29f, 0.110f, WEST, -1, true),
        new Control("B", "b", 0.730f, 0.84f, 0.120f, EAST, -1),
        new Control("L3", "l3", 0.270f, 0.80f, 0.120f, LEFT_STICK, -1),
        new Control("", "start", 0.600f, 0.07f, 0.090f, START, -1),
        new Control("", "back", 0.530f, 0.07f, 0.090f, BACK, -1),
        new Control("", "up", 0.080f, 0.20f, 0.110f, DPAD_UP, -1),
        new Control("", "down", 0.080f, 0.46f, 0.110f, DPAD_DOWN, -1),
        new Control("", "left", 0.035f, 0.33f, 0.110f, DPAD_LEFT, -1),
        new Control("", "right", 0.125f, 0.33f, 0.110f, DPAD_RIGHT, -1),
    };

    private static final int TILE = 0x52101418, TILE_DOWN = 0x99101418, BORDER = 0xC7ECF0F5,
        GLYPH = 0xEBF2F5F8, LABEL = 0xB3F2F5F8, ACCENT = 0xFFF2B33D, SHADOW = 0x73000000;

    private static final int ROLE_STICK = -2, ROLE_LOOK = -3, ROLE_TOGGLE = -4;
    private final SparseArray<Integer> roles = new SparseArray<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint icon = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final PorterDuffColorFilter glyphTint = new PorterDuffColorFilter(GLYPH, PorterDuff.Mode.SRC_IN);
    private final PorterDuffColorFilter accentTint = new PorterDuffColorFilter(ACCENT, PorterDuff.Mode.SRC_IN);
    private final PorterDuffColorFilter shadowTint = new PorterDuffColorFilter(SHADOW, PorterDuff.Mode.SRC_IN);
    private final RectF box = new RectF();
    private final Path star = new Path();
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
        stroke.setStrokeWidth(2 * getResources().getDisplayMetrics().density);
        stroke.setColor(BORDER);
        text.setColor(LABEL);
        text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        for (Control c : controls) {
            try (InputStream in = context.getAssets().open("icons/" + c.icon + ".png")) {
                c.bitmap = BitmapFactory.decodeStream(in);
            } catch (IOException e) {
                c.bitmap = null; // the controller label is drawn instead
            }
        }
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
    private float half(Control c) { return c.side * getHeight() / 2; }

    // The touch area reaches 10% past the drawn square.
    private Control hit(float x, float y) {
        for (Control c : controls) {
            float r = half(c) * 1.1f;
            if (Math.abs(x - cx(c)) <= r && Math.abs(y - cy(c)) <= r)
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
        if (c.axis >= 0) {
            nativeAxis(c.axis, down ? AXIS_MAX : AXIS_MIN);
        } else if (c.chord) {
            // Hold the modifier first and let the game see it for a couple of frames.
            if (down) {
                nativeButton(RIGHT_SHOULDER, true);
                handler.postDelayed(() -> { if (c.down) nativeButton(c.button, true); }, 40);
            } else {
                nativeButton(c.button, false);
                handler.postDelayed(() -> nativeButton(RIGHT_SHOULDER, false), 40);
            }
        } else {
            nativeButton(c.button, down);
        }
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
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(TILE);
        canvas.drawRoundRect(t, h * 0.015f, h * 0.015f, fill);
        stroke.setColor(enabled ? ACCENT : BORDER);
        canvas.drawRoundRect(t, h * 0.015f, h * 0.015f, stroke);
        text.setColor(enabled ? ACCENT : 0xEBFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTextSize(h * 0.032f);
        canvas.drawText(enabled ? "PAD ON" : "PAD", t.centerX(), t.centerY() + h * 0.011f, text);
        stroke.setColor(BORDER);
        if (!enabled)
            return;

        for (Control c : controls)
            drawControl(canvas, c);
        if (stickActive)
            drawStick(canvas, stickOriginX, stickOriginY, stickX, stickY, 1f);
        else
            drawStick(canvas, getWidth() * 0.15f, h * 0.74f, getWidth() * 0.15f, h * 0.74f, 0.45f);
    }

    private void drawControl(Canvas canvas, Control c) {
        float x = cx(c), y = cy(c), r = half(c) * (c.down ? 0.94f : 1f);
        box.set(x - r, y - r, x + r, y + r);
        float corner = r * 0.32f;
        fill.setColor(c.down ? TILE_DOWN : TILE);
        canvas.drawRoundRect(box, corner, corner, fill);
        canvas.drawRoundRect(box, corner, corner, stroke);

        if (c.bitmap != null) {
            float g = r * 0.58f, lift = r * 0.05f, drop = Math.max(1.5f, r * 0.03f);
            box.set(x - g, y - g - lift, x + g, y + g - lift);
            icon.setColorFilter(shadowTint);
            box.offset(0, drop);
            canvas.drawBitmap(c.bitmap, null, box, icon);
            box.offset(0, -drop);
            icon.setColorFilter(c.down ? accentTint : glyphTint);
            canvas.drawBitmap(c.bitmap, null, box, icon);
        }
        if (!c.label.isEmpty()) {
            text.setColor(c.down ? ACCENT : LABEL);
            text.setTextAlign(c.bitmap != null ? Paint.Align.RIGHT : Paint.Align.CENTER);
            text.setTextSize(c.bitmap != null ? r * 0.40f : r * 0.8f);
            if (c.bitmap != null)
                canvas.drawText(c.label, x + r * 0.86f, y + r * 0.86f, text);
            else
                canvas.drawText(c.label, x, y + r * 0.28f, text);
        }
    }

    // A ring with a four-pointed star as the knob; alpha dims the resting hint.
    private void drawStick(Canvas canvas, float ox, float oy, float kx, float ky, float alpha) {
        float h = getHeight(), ring = h * 0.14f, k = h * 0.075f;
        fill.setColor(TILE);
        fill.setAlpha(Math.round(alpha * (TILE >>> 24)));
        canvas.drawCircle(ox, oy, ring, fill);
        stroke.setAlpha(Math.round(alpha * (BORDER >>> 24)));
        canvas.drawCircle(ox, oy, ring, stroke);
        stroke.setAlpha(Math.round(alpha * 0x55));
        canvas.drawCircle(ox, oy, ring * 0.66f, stroke);
        stroke.setColor(BORDER);

        star.reset();
        float w = k * 0.24f;
        star.moveTo(kx, ky - k);
        star.lineTo(kx + w, ky - w);
        star.lineTo(kx + k, ky);
        star.lineTo(kx + w, ky + w);
        star.lineTo(kx, ky + k);
        star.lineTo(kx - w, ky + w);
        star.lineTo(kx - k, ky);
        star.lineTo(kx - w, ky - w);
        star.close();
        fill.setColor(SHADOW);
        fill.setAlpha(Math.round(alpha * (SHADOW >>> 24)));
        canvas.save();
        canvas.translate(0, Math.max(1.5f, k * 0.04f));
        canvas.drawPath(star, fill);
        canvas.restore();
        fill.setColor(GLYPH);
        fill.setAlpha(Math.round(alpha * (GLYPH >>> 24)));
        canvas.drawPath(star, fill);
        fill.setColor(0xD9101418);
        fill.setAlpha(Math.round(alpha * 0xD9));
        canvas.drawCircle(kx, ky, k * 0.14f, fill);
    }
}
