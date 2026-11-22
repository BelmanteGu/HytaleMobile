// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only
//
// On-screen gamepad: GamepadOverlay.java draws the controls and reports them
// here, and this file feeds them to an SDL virtual gamepad. The client uses the
// same SDL library, so it sees an ordinary controller.
//
// Buttons and axes follow SDL_GamepadButton and SDL_GamepadAxis order, which
// SDL maps automatically for a virtual joystick of gamepad type.

#include <jni.h>
#include <SDL3/SDL.h>

#include "macho.h"

static SDL_JoystickID g_id;
static SDL_Joystick *g_joystick;

static void attach(void)
{
    if (g_joystick)
        return;
    if (!SDL_WasInit(SDL_INIT_JOYSTICK) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        macho_log("gamepad", "SDL_InitSubSystem failed: %s", SDL_GetError());
        return;
    }
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
    desc.name = "Hyprism Touch Gamepad";
    g_id = SDL_AttachVirtualJoystick(&desc);
    if (!g_id) {
        macho_log("gamepad", "SDL_AttachVirtualJoystick failed: %s", SDL_GetError());
        return;
    }
    g_joystick = SDL_OpenJoystick(g_id);
    if (!g_joystick) {
        macho_log("gamepad", "SDL_OpenJoystick failed: %s", SDL_GetError());
        SDL_DetachVirtualJoystick(g_id);
        g_id = 0;
        return;
    }
    macho_log("gamepad", "touch gamepad attached as joystick %u", (unsigned)g_id);
}

static void detach(void)
{
    if (!g_joystick)
        return;
    SDL_CloseJoystick(g_joystick);
    SDL_DetachVirtualJoystick(g_id);
    g_joystick = NULL;
    g_id = 0;
    macho_log("gamepad", "touch gamepad detached");
}

JNIEXPORT void JNICALL Java_org_hyprism_game_GamepadOverlay_nativeSetEnabled(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env; (void)cls;
    if (on)
        attach();
    else
        detach();
}

JNIEXPORT void JNICALL Java_org_hyprism_game_GamepadOverlay_nativeAxis(JNIEnv *env, jclass cls, jint axis, jint value)
{
    (void)env; (void)cls;
    if (g_joystick)
        SDL_SetJoystickVirtualAxis(g_joystick, axis, (Sint16)value);
}

JNIEXPORT void JNICALL Java_org_hyprism_game_GamepadOverlay_nativeButton(JNIEnv *env, jclass cls, jint button, jboolean down)
{
    (void)env; (void)cls;
    if (g_joystick)
        SDL_SetJoystickVirtualButton(g_joystick, button, down);
}
