// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Android entry point. SDLActivity loads libmain.so and calls SDL_main on its
// own thread; this runs the Mach-O client from the app's external files folder:
//
//   <files>/Client/Hytale.app/Contents/MacOS/HytaleClient
//   <files>/Assets.zip
//   <files>/UserData/
//   <files>/args.txt   optional extra arguments, one per line
//   <files>/env.txt    optional NAME=value environment entries, one per line

#include <android/log.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int macho_run(int argc, char **argv);
void security_init_java(void *env);
extern const char *SDL_GetAndroidExternalStoragePath(void);
extern void *SDL_GetAndroidJNIEnv(void);

static void *pump_to_logcat(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char line[1024];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, line + used, sizeof line - 1 - used);
        if (n <= 0)
            break;
        used += (size_t)n;
        char *start = line, *nl;
        while ((nl = memchr(start, '\n', used - (size_t)(start - line)))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "Hytale", start);
            start = nl + 1;
        }
        used -= (size_t)(start - line);
        memmove(line, start, used);
        if (used == sizeof line - 1) {
            line[used] = 0;
            __android_log_write(ANDROID_LOG_INFO, "Hytale", line);
            used = 0;
        }
    }
    return NULL;
}

static void redirect_output_to_logcat(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    pthread_t t;
    pthread_create(&t, NULL, pump_to_logcat, (void *)(intptr_t)fds[0]);
    pthread_detach(t);
}

int SDL_main(int argc, char *argv[])
{
    redirect_output_to_logcat();

    const char *files = SDL_GetAndroidExternalStoragePath();
    if (!files) {
        fprintf(stderr, "[host] no external storage path\n");
        return 1;
    }
    static char exe[PATH_MAX], user[PATH_MAX], extra_path[PATH_MAX];
    snprintf(exe, sizeof exe, "%s/Client/Hytale.app/Contents/MacOS/HytaleClient", files);
    snprintf(user, sizeof user, "%s/UserData", files);
    snprintf(extra_path, sizeof extra_path, "%s/args.txt", files);
    chdir(files);
    char env_path[PATH_MAX];
    snprintf(env_path, sizeof env_path, "%s/env.txt", files);
    FILE *env = fopen(env_path, "r");
    if (env) {
        char line[1024];
        while (fgets(line, sizeof line, env)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (line[0] && line[0] != '#' && eq) {
                *eq = 0;
                setenv(line, eq + 1, 1);
            }
        }
        fclose(env);
    }
    // Desktop OpenGL 4.x on the device's OpenGL ES driver through MobileGlues.
    char mg_dir[PATH_MAX];
    snprintf(mg_dir, sizeof mg_dir, "%s/MG", files);
    mkdir(mg_dir, 0775);
    setenv("MG_DIR_PATH", mg_dir, 0);
    char mg_config[PATH_MAX];
    snprintf(mg_config, sizeof mg_config, "%s/config.json", mg_dir);
    if (access(mg_config, F_OK) != 0) {
        // Hytale requires an OpenGL 4.1 core context; cache translated shaders.
        FILE *config = fopen(mg_config, "w");
        if (config) {
            fputs("{\n  \"customGLVersion\": 41,\n  \"maxGlslCacheSize\": 128\n}\n", config);
            fclose(config);
        }
    }
    setenv("SDL_OPENGL_LIBRARY", "libmobileglues.so", 0);
    setenv("SDL_EGL_LIBRARY", "libmobileglues.so", 0);
    setenv("HOME", files, 1);
    setenv("TMPDIR", files, 1);

    static char *args[64];
    int n = 0;
    args[n++] = "hyprism-macho";
    args[n++] = exe;
    args[n++] = "--app-dir";
    args[n++] = (char *)files;
    args[n++] = "--user-dir";
    args[n++] = user;

    // Arguments from the Hyprism launcher (argv[0] is SDL's process name).
    for (int i = 1; i < argc && n < 63; i++)
        args[n++] = argv[i];
    int launcher_arguments = argc > 1;

    FILE *extra = fopen(extra_path, "r");
    if (extra) {
        char line[1024];
        while (n < 63 && fgets(line, sizeof line, extra)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0])
                args[n++] = strdup(line);
        }
        fclose(extra);
    }
    args[n] = NULL;

    fprintf(stderr, "[host] starting %s (%s)\n", exe,
            launcher_arguments ? "launched by Hyprism" : "no launcher arguments");
    // SDLThread sees the app classes; the client's network threads do not.
    security_init_java(SDL_GetAndroidJNIEnv());
    int rc = macho_run(n, args);
    fprintf(stderr, "[host] client exited with %d\n", rc);
    return rc;
}
