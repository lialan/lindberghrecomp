/*
 * host_settings.h - what the player can change: control bindings, the aim
 * calibration, sound and language. Kept in lindbergh.ini in the working
 * directory (the game's own folder) and edited from the host's menus.
 */
#ifndef HOST_SETTINGS_H
#define HOST_SETTINGS_H

typedef enum {
    ACT_COIN1, ACT_START1, ACT_TRIGGER, ACT_RELOAD, ACT_ACTION, ACT_FIREMODE,
    ACT_COIN2, ACT_START2, ACT_TEST, ACT_SERVICE, ACT_MUTE,
    ACT_COUNT
} HostAction;

/* A binding is an SDL keycode (> 0), a mouse button (MOUSE_BIND(n), n = SDL
 * button number) or 0 for none. */
#define MOUSE_BIND(n)   (-(n))
#define IS_MOUSE_BIND(b) ((b) < 0)
#define MOUSE_OF(b)     (-(b))

typedef struct {
    int    bind[ACT_COUNT];
    double aim_sx, aim_ox, aim_sy, aim_oy;   /* sight = s * pointer + o, per axis */
    int    leave_reload;                     /* pointer leaving the window reloads */
    int    muted;
    int    volume;                           /* 0..100 */
    int    country;                          /* 0 Japan (Japanese), 1 English */
} HostSettings;

HostSettings *settings(void);                /* loaded on first use */
void          settings_save(void);
void          settings_reset_controls(void);
void          settings_reset_aim(void);
const char   *settings_action_name(HostAction a);
void          settings_binding_name(int bind, char *out, int n);

/* menu -> host */
void host_begin_capture(HostAction a);       /* the next key or click becomes a's binding */
void host_begin_aim_calibration(void);

#endif
