/*
 * Macro App - Ultra-Fast Linux Macro & Autoclicker for Ubuntu 24.04 (Wayland)
 * 100% Pure C, direct Linux Kernel (/dev/uinput & /dev/input/event*) + GTK 3 UI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <json-c/json.h>
#include <gtk/gtk.h>
#include <dlfcn.h>

#define MAX_EVENTS 2048
#define MAX_MACROS 64
#define MAX_NAME_LEN 128
#define MACRO_FILE "macros.json"
#define MAX_EVENT_FDS 64

/* --- Data Structures --- */

typedef struct {
    int code;             /* Linux kernel evdev code */
    char action[16];      /* "press" or "release" */
    double delay_before;  /* Seconds */
} MacroEvent;

typedef struct {
    int code;             /* Linux evdev code (key or mouse button) */
} Trigger;

typedef struct {
    char name[MAX_NAME_LEN];
    MacroEvent events[MAX_EVENTS];
    int num_events;
    char delay_mode[16];  /* "constant" or "random" */
    int d_min;            /* Milliseconds */
    int d_max;            /* Milliseconds */
    int repeats;          /* 1 to 999999 */
    int no_delay;         /* 1 = remove delay between recorded actions */
    Trigger trigger;
    int has_trigger;
    int enabled;
} Macro;

/* --- Global State --- */

static Macro g_macros[MAX_MACROS];
static int g_num_macros = 0;
static int g_editing_idx = -1;

static MacroEvent g_recorded_events[MAX_EVENTS];
static int g_num_recorded = 0;
static volatile int g_is_recording = 0;
static struct timespec g_last_event_time;

static Trigger g_current_trigger = {0};
static int g_has_current_trigger = 0;
static volatile int g_is_capturing_trigger = 0;

static volatile int g_app_focused = 0;
static volatile double g_ignore_listener_until = 0.0;
static volatile int g_trigger_is_down = 0;
static volatile int g_active_trigger_code = 0;

/* Virtual Kernel Device (/dev/uinput) */
static int g_uinput_fd = -1;

/* Evdev Input Event FDs */
static int g_event_fds[MAX_EVENT_FDS];
static int g_num_event_fds = 0;
static pthread_t g_listener_tid;
static volatile int g_listener_running = 0;

/* Playback Worker Thread State */
typedef struct {
    int macro_idx;
    volatile int stop_requested;
    volatile int is_running;
    pthread_t tid;
} PlayerState;

static PlayerState g_player = {-1, 0, 0, 0};
static pthread_mutex_t g_player_lock = PTHREAD_MUTEX_INITIALIZER;

/* --- UI Widgets --- */

static GtkWidget *g_win_main;
static GtkWidget *g_btn_record;
static GtkWidget *g_lbl_status;
static GtkWidget *g_tv_preview;
static GtkWidget *g_chk_no_delay;
static GtkWidget *g_lbl_trigger;
static GtkWidget *g_btn_trigger;
static GtkWidget *g_btn_clear_trigger;
static GtkWidget *g_entry_repeats;
static GtkWidget *g_radio_const;
static GtkWidget *g_radio_rand;
static GtkWidget *g_entry_min;
static GtkWidget *g_entry_max;
static GtkWidget *g_btn_save;
static GtkWidget *g_btn_new;
static GtkWidget *g_list_macros;

/* Forward Declarations */
static void refresh_macro_list_ui(void);
static void update_preview_text(void);
static void stop_playback(void);
static void load_macros_from_disk(const char *path);
static void save_macros_to_disk(const char *path);

/* --- Timing Utilities --- */

static double get_time_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (ts.tv_nsec / 1e9);
}

static void precise_sleep(double seconds) {
    if (seconds <= 0.0) return;
    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - req.tv_sec) * 1e9);
    nanosleep(&req, NULL);
}

/* --- Key & Button Code Helpers --- */

static void get_key_label(int code, char *buf, size_t sz) {
    if (code <= 0) {
        snprintf(buf, sz, "None");
        return;
    }
    if (code == BTN_LEFT)   { snprintf(buf, sz, "Mouse Left"); return; }
    if (code == BTN_RIGHT)  { snprintf(buf, sz, "Mouse Right"); return; }
    if (code == BTN_MIDDLE) { snprintf(buf, sz, "Mouse Middle"); return; }
    if (code == BTN_SIDE)   { snprintf(buf, sz, "Mouse Side"); return; }
    if (code == BTN_EXTRA)  { snprintf(buf, sz, "Mouse Extra"); return; }

    if (code >= BTN_MISC && code <= BTN_GEAR_UP) {
        snprintf(buf, sz, "BTN_%d", code);
        return;
    }

    if (code >= KEY_A && code <= KEY_Z) {
        snprintf(buf, sz, "Key '%c'", 'A' + (code - KEY_A));
        return;
    }
    if (code >= KEY_1 && code <= KEY_9) {
        snprintf(buf, sz, "Key '%d'", 1 + (code - KEY_1));
        return;
    }
    if (code == KEY_0)     { snprintf(buf, sz, "Key '0'"); return; }
    if (code == KEY_ESC)   { snprintf(buf, sz, "Key ESC"); return; }
    if (code == KEY_SPACE) { snprintf(buf, sz, "Key SPACE"); return; }
    if (code == KEY_ENTER) { snprintf(buf, sz, "Key ENTER"); return; }
    if (code == KEY_TAB)   { snprintf(buf, sz, "Key TAB"); return; }

    if (code >= KEY_F1 && code <= KEY_F12) {
        snprintf(buf, sz, "Key F%d", 1 + (code - KEY_F1));
        return;
    }

    snprintf(buf, sz, "KEY_%d", code);
}

static int parse_key_code(const char *name, int code) {
    if (code > 0) return code;
    if (!name || name[0] == '\0') return 0;

    if (strcasecmp(name, "left") == 0) return BTN_LEFT;
    if (strcasecmp(name, "right") == 0) return BTN_RIGHT;
    if (strcasecmp(name, "middle") == 0) return BTN_MIDDLE;
    if (strcasecmp(name, "side") == 0) return BTN_SIDE;
    if (strcasecmp(name, "extra") == 0) return BTN_EXTRA;

    if (strncasecmp(name, "KEY_", 4) == 0 || strncasecmp(name, "BTN_", 4) == 0) {
        return atoi(name + 4);
    }

    int val = atoi(name);
    if (val > 0) return val;

    if (strlen(name) == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
        if (c >= '1' && c <= '9') return KEY_1 + (c - '1');
        if (c == '0') return KEY_0;
    }

    return 0;
}

static inline int is_modifier_key(int code) {
    return (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL ||
            code == KEY_LEFTALT  || code == KEY_RIGHTALT  ||
            code == KEY_LEFTSHIFT|| code == KEY_RIGHTSHIFT||
            code == KEY_LEFTMETA || code == KEY_RIGHTMETA);
}

/* --- Linux Kernel /dev/uinput Virtual Device --- */

static void init_uinput(void) {
    g_uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (g_uinput_fd < 0) {
        printf("[UINPUT] Warning: /dev/uinput inaccessible (permission denied)\n");
        return;
    }

    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(g_uinput_fd, UI_SET_EVBIT, EV_SYN);

    for (int k = 1; k <= 512; k++) {
        ioctl(g_uinput_fd, UI_SET_KEYBIT, k);
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    strcpy(usetup.name, "MacroApp-Virtual-Input");

    if (ioctl(g_uinput_fd, UI_DEV_SETUP, &usetup) < 0 ||
        ioctl(g_uinput_fd, UI_DEV_CREATE) < 0) {
        close(g_uinput_fd);
        g_uinput_fd = -1;
        return;
    }

    printf("[UINPUT] Virtual device ready\n");
}

static void close_uinput(void) {
    if (g_uinput_fd >= 0) {
        ioctl(g_uinput_fd, UI_DEV_DESTROY);
        close(g_uinput_fd);
        g_uinput_fd = -1;
    }
}

static int inject_event(const MacroEvent *ev) {
    if (g_uinput_fd < 0 || ev->code <= 0) return 0;

    int val = (strcmp(ev->action, "press") == 0) ? 1 : 0;
    struct input_event ie[2];
    memset(ie, 0, sizeof(ie));

    ie[0].type = EV_KEY;
    ie[0].code = ev->code;
    ie[0].value = val;

    ie[1].type = EV_SYN;
    ie[1].code = SYN_REPORT;
    ie[1].value = 0;

    return (write(g_uinput_fd, ie, sizeof(ie)) == sizeof(ie));
}

/* --- Playback Worker Thread --- */

static void reset_player_state(void) {
    pthread_mutex_lock(&g_player_lock);
    g_player.is_running = 0;
    g_active_trigger_code = 0;
    g_trigger_is_down = 0;
    pthread_mutex_unlock(&g_player_lock);
}

static void player_interruptible_sleep_ms(int ms) {
    int chunks = ms / 10;
    for (int i = 0; i < chunks; i++) {
        if (g_player.stop_requested || g_app_focused) break;
        usleep(10000);
    }
}

static void* macro_player_thread(void *arg) {
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= g_num_macros) return NULL;
    Macro *m = &g_macros[idx];

    printf("[MACRO] Started: '%s' (repeats: %d)\n", m->name, m->repeats);

    /* Initial pause strictly superior to max delay */
    int max_delay = (m->d_max > m->d_min) ? m->d_max : m->d_min;
    if (strcmp(m->delay_mode, "constant") == 0) {
        max_delay = m->d_min;
    }
    int pre_delay_ms = max_delay + 80;
    if (pre_delay_ms < 250) pre_delay_ms = 250;
    player_interruptible_sleep_ms(pre_delay_ms);

    /* Wait for physical trigger release if user is still holding it */
    int wait_release_count = 0;
    while (g_trigger_is_down && wait_release_count < 100) {
        if (g_player.stop_requested || g_app_focused) break;
        usleep(10000);
        wait_release_count++;
    }
    if (wait_release_count > 0) usleep(20000);

    /* Ensure modifier trigger key is released in virtual device */
    if (is_modifier_key(m->trigger.code)) {
        MacroEvent rel_ev = { .code = m->trigger.code, .action = "release", .delay_before = 0.0 };
        inject_event(&rel_ev);
    }

    if (g_player.stop_requested || g_app_focused) {
        reset_player_state();
        printf("[MACRO] Stopped: '%s'\n", m->name);
        return NULL;
    }

    for (int cycle = 1; cycle <= m->repeats; cycle++) {
        if (g_player.stop_requested || g_app_focused) break;

        for (int i = 0; i < m->num_events; i++) {
            if (g_player.stop_requested || g_app_focused) break;
            MacroEvent *ev = &m->events[i];

            /* Delay between recorded actions (skipped if no_delay is active) */
            if (!m->no_delay && i > 0 && ev->delay_before > 0.0) {
                precise_sleep(ev->delay_before);
            }

            if (g_player.stop_requested || g_app_focused) break;

            g_ignore_listener_until = get_time_monotonic() + 0.02;
            inject_event(ev);
        }

        if (g_player.stop_requested || g_app_focused) break;

        /* Delay between repetition cycles */
        if (cycle < m->repeats) {
            int delay_ms = m->d_min;
            if (strcmp(m->delay_mode, "random") == 0 && m->d_max > m->d_min) {
                delay_ms = m->d_min + (rand() % (m->d_max - m->d_min + 1));
            }
            player_interruptible_sleep_ms(delay_ms);
        }
    }

    reset_player_state();
    printf("[MACRO] %s: '%s'\n", (g_player.stop_requested || g_app_focused) ? "Stopped" : "Finished", m->name);
    return NULL;
}

static void toggle_playback(int macro_idx) {
    if (macro_idx < 0 || macro_idx >= g_num_macros) return;
    Macro *m = &g_macros[macro_idx];

    pthread_mutex_lock(&g_player_lock);
    if (g_player.is_running) {
        g_player.stop_requested = 1;
        pthread_mutex_unlock(&g_player_lock);
        pthread_join(g_player.tid, NULL);
        reset_player_state();
        return;
    }

    if (!m->enabled) {
        pthread_mutex_unlock(&g_player_lock);
        return;
    }

    g_active_trigger_code = m->trigger.code;
    g_trigger_is_down = 1;
    g_player.macro_idx = macro_idx;
    g_player.stop_requested = 0;
    g_player.is_running = 1;

    pthread_create(&g_player.tid, NULL, macro_player_thread, (void*)(intptr_t)macro_idx);
    pthread_mutex_unlock(&g_player_lock);
}

static void stop_playback(void) {
    pthread_mutex_lock(&g_player_lock);
    if (g_player.is_running) {
        g_player.stop_requested = 1;
        pthread_mutex_unlock(&g_player_lock);
        pthread_join(g_player.tid, NULL);
    } else {
        pthread_mutex_unlock(&g_player_lock);
    }
    reset_player_state();
}

/* --- Input Event Dispatcher (Main Thread) --- */

typedef struct {
    int code;
    int value;
} InputDispatchData;

static void set_trigger_ui_label(const char *text, const char *color) {
    char markup[128];
    snprintf(markup, sizeof(markup), "<span foreground='%s' weight='bold'>%s</span>", color, text);
    gtk_label_set_markup(GTK_LABEL(g_lbl_trigger), markup);
}

static gboolean on_input_dispatched_in_main(gpointer user_data) {
    InputDispatchData *data = (InputDispatchData*)user_data;
    int code = data->code;
    int value = data->value;
    free(data);

    char label[64];
    get_key_label(code, label, sizeof(label));

    /* 1. Trigger Capture Mode */
    if (g_is_capturing_trigger && value == 1) {
        g_current_trigger.code = code;
        g_has_current_trigger = 1;
        g_is_capturing_trigger = 0;

        set_trigger_ui_label(label, "#2a7ae2");
        gtk_button_set_label(GTK_BUTTON(g_btn_trigger), "Set Trigger");

        printf("[TRIGGER] Set to: %s\n", label);
        return G_SOURCE_REMOVE;
    }

    /* 2. Recording Mode */
    if (g_is_recording) {
        if (code == KEY_ESC && value == 1) {
            g_is_recording = 0;
            gtk_button_set_label(GTK_BUTTON(g_btn_record), "Start Recording");
            gtk_widget_set_name(g_btn_record, "btn_record_idle");
            gtk_widget_hide(g_lbl_status);
            printf("[RECORDER] Stopped (%d events)\n", g_num_recorded);
            return G_SOURCE_REMOVE;
        }

        if (g_num_recorded < MAX_EVENTS) {
            double delay = 0.0;
            gboolean no_delay = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_chk_no_delay));
            if (!no_delay && g_num_recorded > 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                delay = (now.tv_sec - g_last_event_time.tv_sec) +
                        (now.tv_nsec - g_last_event_time.tv_nsec) / 1e9;
            }
            clock_gettime(CLOCK_MONOTONIC, &g_last_event_time);

            MacroEvent *ev = &g_recorded_events[g_num_recorded++];
            ev->code = code;
            snprintf(ev->action, sizeof(ev->action), "%s", (value == 1) ? "press" : "release");
            ev->delay_before = no_delay ? 0.0 : delay;

            update_preview_text();
        }
        return G_SOURCE_REMOVE;
    }

    /* 3. Normal Mode: Trigger Detection (Press Only) */
    if (value == 1) {
        if (g_app_focused) return G_SOURCE_REMOVE;

        for (int i = 0; i < g_num_macros; i++) {
            if (g_macros[i].has_trigger && g_macros[i].trigger.code == code) {
                printf("[MACRO] Triggered: '%s'\n", g_macros[i].name);
                toggle_playback(i);
                break;
            }
        }
    }

    return G_SOURCE_REMOVE;
}

static void dispatch_input(int code, int value) {
    if (g_active_trigger_code > 0 && code == g_active_trigger_code) {
        g_trigger_is_down = (value == 1);
    }

    if (get_time_monotonic() < g_ignore_listener_until) return;

    InputDispatchData *data = malloc(sizeof(InputDispatchData));
    if (!data) return;
    data->code = code;
    data->value = value;
    g_idle_add(on_input_dispatched_in_main, data);
}

/* --- Linux Kernel Evdev Global Listener Thread --- */

static void* evdev_listener_thread(void *arg) {
    (void)arg;
    struct pollfd pfds[MAX_EVENT_FDS];

    while (g_listener_running) {
        int n_fds = g_num_event_fds;
        if (n_fds <= 0) {
            sleep(1);
            continue;
        }
        if (n_fds > MAX_EVENT_FDS) n_fds = MAX_EVENT_FDS;

        for (int i = 0; i < n_fds; i++) {
            pfds[i].fd = g_event_fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        int ret = poll(pfds, (nfds_t)n_fds, 50);
        if (ret > 0) {
            for (int i = 0; i < n_fds; i++) {
                if (pfds[i].revents & POLLIN) {
                    struct input_event ie;
                    while (read(pfds[i].fd, &ie, sizeof(ie)) == sizeof(ie)) {
                        if (ie.type == EV_KEY && (ie.value == 0 || ie.value == 1)) {
                            dispatch_input(ie.code, ie.value);
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

static void init_evdev_listener(void) {
    g_num_event_fds = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                unsigned long evbits[(EV_MAX + 7) / 8] = {0};
                if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0) {
                    if (evbits[EV_KEY / 8] & (1 << (EV_KEY % 8))) {
                        g_event_fds[g_num_event_fds++] = fd;
                        if (g_num_event_fds >= MAX_EVENT_FDS) break;
                    } else {
                        close(fd);
                    }
                } else {
                    close(fd);
                }
            }
        }
    }
    closedir(dir);

    printf("[EVDEV] Listening (%d devices)\n", g_num_event_fds);

    g_listener_running = 1;
    pthread_create(&g_listener_tid, NULL, evdev_listener_thread, NULL);
}

static void close_evdev_listener(void) {
    g_listener_running = 0;
    pthread_join(g_listener_tid, NULL);
    for (int i = 0; i < g_num_event_fds; i++) {
        if (g_event_fds[i] >= 0) close(g_event_fds[i]);
    }
    g_num_event_fds = 0;
}

/* --- JSON Helpers & Persistence (macros.json) --- */

static const char* json_get_str(struct json_object *obj, const char *key, const char *def) {
    struct json_object *v;
    return (json_object_object_get_ex(obj, key, &v) && v) ? json_object_get_string(v) : def;
}

static int json_get_int(struct json_object *obj, const char *key, int def) {
    struct json_object *v;
    return (json_object_object_get_ex(obj, key, &v) && v) ? json_object_get_int(v) : def;
}

static double json_get_double(struct json_object *obj, const char *key, double def) {
    struct json_object *v;
    return (json_object_object_get_ex(obj, key, &v) && v) ? json_object_get_double(v) : def;
}

static int json_get_bool(struct json_object *obj, const char *key, int def) {
    struct json_object *v;
    return (json_object_object_get_ex(obj, key, &v) && v) ? json_object_get_boolean(v) : def;
}

static void load_macros_from_disk(const char *path) {
    g_num_macros = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return; }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, len, f);
    buf[rd] = '\0';
    fclose(f);

    struct json_object *root = json_tokener_parse(buf);
    free(buf);
    if (!root || !json_object_is_type(root, json_type_array)) {
        if (root) json_object_put(root);
        return;
    }

    int n = json_object_array_length(root);
    for (int i = 0; i < n && g_num_macros < MAX_MACROS; i++) {
        struct json_object *jm = json_object_array_get_idx(root, i);
        Macro *m = &g_macros[g_num_macros];
        memset(m, 0, sizeof(Macro));

        const char *name = json_get_str(jm, "name", "");
        if (name[0] != '\0') {
            snprintf(m->name, MAX_NAME_LEN, "%s", name);
        } else {
            snprintf(m->name, MAX_NAME_LEN, "Macro %d", g_num_macros + 1);
        }

        snprintf(m->delay_mode, sizeof(m->delay_mode), "%s", json_get_str(jm, "delay_mode", "constant"));
        m->d_min = json_get_int(jm, "d_min", 100);
        m->d_max = json_get_int(jm, "d_max", 180);
        m->repeats = json_get_int(jm, "repeats", 1);
        if (m->repeats < 1) m->repeats = 1;
        m->no_delay = json_get_bool(jm, "no_delay", 0);
        m->enabled = json_get_bool(jm, "enabled", 1);

        /* Load Trigger */
        struct json_object *jtrig;
        if (json_object_object_get_ex(jm, "trigger", &jtrig) && jtrig) {
            int code = json_get_int(jtrig, "code", 0);
            const char *tname = json_get_str(jtrig, "name", NULL);
            m->trigger.code = parse_key_code(tname, code);
            m->has_trigger = (m->trigger.code > 0);
        }

        /* Load Events */
        struct json_object *jevents;
        if (json_object_object_get_ex(jm, "events", &jevents) && json_object_is_type(jevents, json_type_array)) {
            int n_ev = json_object_array_length(jevents);
            for (int e = 0; e < n_ev && m->num_events < MAX_EVENTS; e++) {
                struct json_object *je = json_object_array_get_idx(jevents, e);
                MacroEvent *ev = &m->events[m->num_events];

                int code = json_get_int(je, "code", 0);
                snprintf(ev->action, sizeof(ev->action), "%s", json_get_str(je, "action", "press"));
                ev->delay_before = json_get_double(je, "delay", 0.0);

                const char *tgt_name = NULL;
                struct json_object *etgt;
                if (json_object_object_get_ex(je, "target", &etgt) && etgt) {
                    tgt_name = json_get_str(etgt, "name", NULL);
                }

                ev->code = parse_key_code(tgt_name, code);
                m->num_events++;
            }
        }

        g_num_macros++;
    }

    json_object_put(root);
    printf("[DISK] Loaded %d macros\n", g_num_macros);
}

static void save_macros_to_disk(const char *path) {
    struct json_object *root = json_object_new_array();

    for (int i = 0; i < g_num_macros; i++) {
        Macro *m = &g_macros[i];
        struct json_object *jm = json_object_new_object();

        json_object_object_add(jm, "name", json_object_new_string(m->name));
        json_object_object_add(jm, "delay_mode", json_object_new_string(m->delay_mode));
        json_object_object_add(jm, "d_min", json_object_new_int(m->d_min));
        json_object_object_add(jm, "d_max", json_object_new_int(m->d_max));
        json_object_object_add(jm, "repeats", json_object_new_int(m->repeats));
        json_object_object_add(jm, "no_delay", json_object_new_boolean(m->no_delay));
        json_object_object_add(jm, "enabled", json_object_new_boolean(m->enabled));

        if (m->has_trigger) {
            char lbl[64];
            get_key_label(m->trigger.code, lbl, sizeof(lbl));
            struct json_object *jtrig = json_object_new_object();
            json_object_object_add(jtrig, "code", json_object_new_int(m->trigger.code));
            json_object_object_add(jtrig, "name", json_object_new_string(lbl));
            json_object_object_add(jm, "trigger", jtrig);
        } else {
            json_object_object_add(jm, "trigger", NULL);
        }

        struct json_object *jevents = json_object_new_array();
        for (int e = 0; e < m->num_events; e++) {
            MacroEvent *ev = &m->events[e];
            char lbl[64];
            get_key_label(ev->code, lbl, sizeof(lbl));

            struct json_object *je = json_object_new_object();
            json_object_object_add(je, "code", json_object_new_int(ev->code));
            json_object_object_add(je, "action", json_object_new_string(ev->action));
            json_object_object_add(je, "delay", json_object_new_double(ev->delay_before));

            struct json_object *jtgt = json_object_new_object();
            json_object_object_add(jtgt, "name", json_object_new_string(lbl));
            json_object_object_add(je, "target", jtgt);

            json_object_array_add(jevents, je);
        }
        json_object_object_add(jm, "events", jevents);
        json_object_array_add(root, jm);
    }

    FILE *f = fopen(path, "w");
    if (f) {
        const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
        fputs(json_str, f);
        fputc('\n', f);
        fclose(f);
        printf("[DISK] Saved %d macros\n", g_num_macros);
    }
    json_object_put(root);
}

/* --- UI Helpers & Callbacks --- */

static void update_preview_text(void) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_tv_preview));
    gtk_text_buffer_set_text(buf, "", -1);

    char line[256];
    for (int i = 0; i < g_num_recorded; i++) {
        MacroEvent *ev = &g_recorded_events[i];
        char lbl[64];
        get_key_label(ev->code, lbl, sizeof(lbl));
        snprintf(line, sizeof(line), "[%d] %s -> %s (+%.3fs)\n",
                 i + 1, lbl, ev->action, ev->delay_before);
        gtk_text_buffer_insert_at_cursor(buf, line, -1);
    }
}

static gboolean on_btn_record_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget; (void)user_data;
    if (event->button == 1 && g_is_recording) {
        g_ignore_listener_until = get_time_monotonic() + 0.35;
    }
    return FALSE;
}

static void strip_trailing_click(void) {
    if (g_num_recorded > 0 && g_recorded_events[g_num_recorded - 1].code == BTN_LEFT) {
        if (strcmp(g_recorded_events[g_num_recorded - 1].action, "release") == 0) {
            g_num_recorded--;
            if (g_num_recorded > 0 &&
                g_recorded_events[g_num_recorded - 1].code == BTN_LEFT &&
                strcmp(g_recorded_events[g_num_recorded - 1].action, "press") == 0) {
                g_num_recorded--;
            }
        } else if (strcmp(g_recorded_events[g_num_recorded - 1].action, "press") == 0) {
            g_num_recorded--;
        }
    }
}

static void on_btn_record_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;

    if (!g_is_recording) {
        g_is_recording = 1;
        g_num_recorded = 0;
        g_ignore_listener_until = get_time_monotonic() + 0.25;
        update_preview_text();
        gtk_button_set_label(GTK_BUTTON(g_btn_record), "Stop Recording");
        gtk_widget_set_name(g_btn_record, "btn_record_active");
        gtk_widget_show(g_lbl_status);
        printf("[RECORDER] Started\n");
    } else {
        g_is_recording = 0;
        g_ignore_listener_until = get_time_monotonic() + 0.35;

        /* Strip the trailing click used to stop recording */
        strip_trailing_click();

        update_preview_text();
        gtk_button_set_label(GTK_BUTTON(g_btn_record), "Start Recording");
        gtk_widget_set_name(g_btn_record, "btn_record_idle");
        gtk_widget_hide(g_lbl_status);
        printf("[RECORDER] Stopped (%d events)\n", g_num_recorded);
    }
}

static void on_btn_trigger_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;

    if (!g_is_capturing_trigger) {
        g_is_capturing_trigger = 1;
        gtk_button_set_label(GTK_BUTTON(g_btn_trigger), "Listening...");
        set_trigger_ui_label("Press any key/button...", "#e01b24");
    } else {
        g_is_capturing_trigger = 0;
        gtk_button_set_label(GTK_BUTTON(g_btn_trigger), "Set Trigger");
        char lbl[64];
        get_key_label(g_current_trigger.code, lbl, sizeof(lbl));
        set_trigger_ui_label(lbl, "#2a7ae2");
    }
}

static void on_btn_clear_trigger_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    g_current_trigger.code = 0;
    g_has_current_trigger = 0;
    g_is_capturing_trigger = 0;
    gtk_button_set_label(GTK_BUTTON(g_btn_trigger), "Set Trigger");
    set_trigger_ui_label("None", "#888888");
    printf("[TRIGGER] Cleared\n");
}

static void on_delay_mode_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    gboolean is_rand = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_radio_rand));
    gtk_widget_set_sensitive(g_entry_max, is_rand);
}

static void on_btn_new_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    g_editing_idx = -1;
    g_num_recorded = 0;
    g_current_trigger.code = 0;
    g_has_current_trigger = 0;

    update_preview_text();
    set_trigger_ui_label("None", "#888888");
    gtk_entry_set_text(GTK_ENTRY(g_entry_repeats), "1");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_chk_no_delay), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_radio_const), TRUE);
    gtk_entry_set_text(GTK_ENTRY(g_entry_min), "100");
    gtk_entry_set_text(GTK_ENTRY(g_entry_max), "180");
    gtk_button_set_label(GTK_BUTTON(g_btn_save), "Save Macro");
}

static void on_btn_save_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;

    if (g_num_recorded == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_win_main),
            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Record some actions first!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    int idx = g_editing_idx;
    if (idx < 0) {
        if (g_num_macros >= MAX_MACROS) {
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(g_win_main),
                GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Maximum number of macros reached (%d).", MAX_MACROS);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return;
        }
        idx = g_num_macros++;
        snprintf(g_macros[idx].name, MAX_NAME_LEN, "Macro %d", idx + 1);
    }

    Macro *m = &g_macros[idx];
    m->repeats = atoi(gtk_entry_get_text(GTK_ENTRY(g_entry_repeats)));
    if (m->repeats < 1) m->repeats = 1;

    m->no_delay = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_chk_no_delay));

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_radio_rand))) {
        strcpy(m->delay_mode, "random");
    } else {
        strcpy(m->delay_mode, "constant");
    }
    m->d_min = atoi(gtk_entry_get_text(GTK_ENTRY(g_entry_min)));
    if (m->d_min < 0) m->d_min = 0;
    m->d_max = atoi(gtk_entry_get_text(GTK_ENTRY(g_entry_max)));
    if (m->d_max < 0) m->d_max = 0;

    m->trigger = g_current_trigger;
    m->has_trigger = g_has_current_trigger;
    m->enabled = 1;

    m->num_events = g_num_recorded;
    memcpy(m->events, g_recorded_events, sizeof(MacroEvent) * g_num_recorded);

    save_macros_to_disk(MACRO_FILE);
    refresh_macro_list_ui();

    printf("[MACRO] Saved: '%s'\n", m->name);
    on_btn_new_clicked(NULL, NULL);
}

static void on_macro_check_toggled(GtkToggleButton *chk, gpointer user_data) {
    int idx = (int)(intptr_t)user_data;
    if (idx < 0 || idx >= g_num_macros) return;
    g_macros[idx].enabled = gtk_toggle_button_get_active(chk);
    save_macros_to_disk(MACRO_FILE);
}

static void on_macro_delete_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int idx = (int)(intptr_t)user_data;
    if (idx < 0 || idx >= g_num_macros) return;

    printf("[MACRO] Deleted: '%s'\n", g_macros[idx].name);
    for (int i = idx; i < g_num_macros - 1; i++) {
        g_macros[i] = g_macros[i + 1];
    }
    g_num_macros--;
    save_macros_to_disk(MACRO_FILE);
    refresh_macro_list_ui();
}

static void on_macro_rename_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int idx = (int)(intptr_t)user_data;
    if (idx < 0 || idx >= g_num_macros) return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Rename Macro",
        GTK_WINDOW(g_win_main), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), g_macros[idx].name);
    gtk_container_add(GTK_CONTAINER(content), entry);
    gtk_widget_show_all(content);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const char *txt = gtk_entry_get_text(GTK_ENTRY(entry));
        if (txt && txt[0] != '\0') {
            snprintf(g_macros[idx].name, MAX_NAME_LEN, "%s", txt);
            save_macros_to_disk(MACRO_FILE);
            refresh_macro_list_ui();
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_macro_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    int idx = gtk_list_box_row_get_index(row);
    if (idx < 0 || idx >= g_num_macros) return;

    Macro *m = &g_macros[idx];
    g_editing_idx = idx;

    g_num_recorded = m->num_events;
    memcpy(g_recorded_events, m->events, sizeof(MacroEvent) * m->num_events);
    update_preview_text();

    g_current_trigger = m->trigger;
    g_has_current_trigger = m->has_trigger;

    char lbl[64];
    get_key_label(m->trigger.code, lbl, sizeof(lbl));
    set_trigger_ui_label(lbl, "#2a7ae2");

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", m->repeats);
    gtk_entry_set_text(GTK_ENTRY(g_entry_repeats), buf);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_chk_no_delay), m->no_delay);

    if (strcmp(m->delay_mode, "random") == 0) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_radio_rand), TRUE);
    } else {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_radio_const), TRUE);
    }

    snprintf(buf, sizeof(buf), "%d", m->d_min);
    gtk_entry_set_text(GTK_ENTRY(g_entry_min), buf);

    snprintf(buf, sizeof(buf), "%d", m->d_max);
    gtk_entry_set_text(GTK_ENTRY(g_entry_max), buf);

    gtk_button_set_label(GTK_BUTTON(g_btn_save), "Update Macro");
}

static GtkWidget* create_icon_btn(const char *icon_name, const char *tooltip, GCallback cb, gpointer data) {
    GtkWidget *btn = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect(btn, "clicked", cb, data);
    return btn;
}

static void refresh_macro_list_ui(void) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_list_macros));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    for (int i = 0; i < g_num_macros; i++) {
        Macro *m = &g_macros[i];

        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row_box, 6);
        gtk_widget_set_margin_end(row_box, 6);
        gtk_widget_set_margin_top(row_box, 2);
        gtk_widget_set_margin_bottom(row_box, 2);

        /* Title Label */
        char lbl[64];
        get_key_label(m->trigger.code, lbl, sizeof(lbl));
        char title[512];
        if (m->has_trigger) {
            snprintf(title, sizeof(title), "%.128s  [%.128s]", m->name, lbl);
        } else {
            snprintf(title, sizeof(title), "%.256s", m->name);
        }
        GtkWidget *lbl_title = gtk_label_new(title);
        gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
        gtk_box_pack_start(GTK_BOX(row_box), lbl_title, TRUE, TRUE, 0);

        /* Enable Checkbox */
        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), m->enabled);
        gtk_widget_set_tooltip_text(chk, "Enable / Disable Macro Trigger");
        g_signal_connect(chk, "toggled", G_CALLBACK(on_macro_check_toggled), (gpointer)(intptr_t)i);
        gtk_box_pack_start(GTK_BOX(row_box), chk, FALSE, FALSE, 0);

        /* Rename Button */
        GtkWidget *btn_ren = create_icon_btn("document-edit", "Rename", G_CALLBACK(on_macro_rename_clicked), (gpointer)(intptr_t)i);
        gtk_box_pack_start(GTK_BOX(row_box), btn_ren, FALSE, FALSE, 0);

        /* Delete Button */
        GtkWidget *btn_del = create_icon_btn("user-trash", "Delete", G_CALLBACK(on_macro_delete_clicked), (gpointer)(intptr_t)i);
        gtk_box_pack_start(GTK_BOX(row_box), btn_del, FALSE, FALSE, 0);

        gtk_list_box_insert(GTK_LIST_BOX(g_list_macros), row_box, -1);
    }

    gtk_widget_show_all(g_list_macros);
}

/* --- Window Focus Handling (Macro Safety) --- */

static gboolean on_window_focus_in(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget; (void)event; (void)user_data;
    g_app_focused = 1;
    stop_playback();
    return FALSE;
}

static gboolean on_window_focus_out(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)widget; (void)event; (void)user_data;
    g_app_focused = 0;
    return FALSE;
}

/* --- System Tray Indicator (AppIndicator) --- */

typedef enum {
    APP_INDICATOR_CATEGORY_APPLICATION_STATUS,
    APP_INDICATOR_CATEGORY_COMMUNICATIONS,
    APP_INDICATOR_CATEGORY_SYSTEM_SERVICES,
    APP_INDICATOR_CATEGORY_HARDWARE,
    APP_INDICATOR_CATEGORY_OTHER
} AppIndicatorCategory;

typedef enum {
    APP_INDICATOR_STATUS_PASSIVE,
    APP_INDICATOR_STATUS_ACTIVE,
    APP_INDICATOR_STATUS_ATTENTION
} AppIndicatorStatus;

typedef void* (*fn_app_indicator_new)(const char *id, const char *icon_name, AppIndicatorCategory category);
typedef void (*fn_app_indicator_set_status)(void *indicator, AppIndicatorStatus status);
typedef void (*fn_app_indicator_set_menu)(void *indicator, GtkMenu *menu);

static void *g_tray_indicator = NULL;

static void on_tray_open_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    gtk_widget_show_all(g_win_main);
    gtk_window_present(GTK_WINDOW(g_win_main));
}

static void on_tray_quit_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item; (void)user_data;
    gtk_main_quit();
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    (void)event; (void)user_data;
    if (g_tray_indicator) {
        gtk_widget_hide(widget);
        return TRUE; /* Keep running in tray */
    }
    return FALSE; /* Exit if no tray */
}

static void init_tray_indicator(void) {
    void *lib = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY);
    if (!lib) lib = dlopen("libappindicator3.so.1", RTLD_LAZY);
    if (!lib) return;

    fn_app_indicator_new app_ind_new = (fn_app_indicator_new)dlsym(lib, "app_indicator_new");
    fn_app_indicator_set_status app_ind_set_status = (fn_app_indicator_set_status)dlsym(lib, "app_indicator_set_status");
    fn_app_indicator_set_menu app_ind_set_menu = (fn_app_indicator_set_menu)dlsym(lib, "app_indicator_set_menu");

    if (!app_ind_new || !app_ind_set_status || !app_ind_set_menu) return;

    g_tray_indicator = app_ind_new("macro-app-tray", "preferences-desktop-keyboard", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!g_tray_indicator) return;

    /* Tray Menu with only Open and Quit */
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *item_open = gtk_menu_item_new_with_label("Open");
    g_signal_connect(item_open, "activate", G_CALLBACK(on_tray_open_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_open);

    GtkWidget *item_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(item_quit, "activate", G_CALLBACK(on_tray_quit_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_quit);

    gtk_widget_show_all(menu);
    app_ind_set_status(g_tray_indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_ind_set_menu(g_tray_indicator, GTK_MENU(menu));
}

/* Local key press for fallback when focused */
static gboolean on_window_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    (void)widget; (void)user_data;
    if (g_is_capturing_trigger) {
        int code = 0;
        if (event->keyval == GDK_KEY_Escape) code = KEY_ESC;
        else if (event->keyval == GDK_KEY_Return) code = KEY_ENTER;
        else if (event->keyval == GDK_KEY_space) code = KEY_SPACE;
        else if (event->keyval == GDK_KEY_Tab) code = KEY_TAB;
        else if (event->keyval >= GDK_KEY_F1 && event->keyval <= GDK_KEY_F12) code = KEY_F1 + (event->keyval - GDK_KEY_F1);
        else if (event->string && event->string[0] != '\0') {
            code = parse_key_code(event->string, 0);
        }

        if (code > 0) {
            dispatch_input(code, 1);
            return TRUE;
        }
    }
    return FALSE;
}

/* --- UI Helper for Digit Entries --- */

static GtkWidget* create_num_entry(const char *initial_val, int width) {
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial_val);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), width);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_DIGITS);
    return entry;
}

/* --- Main Entry Point --- */

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Ensure working directory is always the executable's directory */
    char exe_dir[1024];
    ssize_t elen = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (elen > 0) {
        exe_dir[elen] = '\0';
        char *last_slash = strrchr(exe_dir, '/');
        if (last_slash) {
            *last_slash = '\0';
            if (chdir(exe_dir) != 0) {
                /* ignore */
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("Macro App v1.0\n");
            return 0;
        }
        if (strcmp(argv[i], "--test") == 0) {
            load_macros_from_disk(MACRO_FILE);
            printf("[TEST] Loaded %d macros\n", g_num_macros);
            init_uinput();
            if (g_uinput_fd >= 0) {
                printf("[TEST] /dev/uinput OK\n");
                close_uinput();
            }
            return 0;
        }
    }

    g_set_prgname("macro-app");
    g_set_application_name("Macro App");

    gtk_init(&argc, &argv);
    gtk_window_set_default_icon_name("preferences-desktop-keyboard");

    printf("Macro App (Ubuntu 24.04 Wayland)\n");

    init_uinput();

    /* CSS Styling */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#btn_record_active { color: #e01b24; font-weight: bold; }\n"
        "#btn_record_idle { font-weight: bold; }\n",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* Main Window */
    g_win_main = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win_main), "Macro App");
    gtk_window_set_icon_name(GTK_WINDOW(g_win_main), "preferences-desktop-keyboard");
    gtk_window_set_default_size(GTK_WINDOW(g_win_main), 360, 520);
    g_signal_connect(g_win_main, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(g_win_main, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(g_win_main, "focus-in-event", G_CALLBACK(on_window_focus_in), NULL);
    g_signal_connect(g_win_main, "focus-out-event", G_CALLBACK(on_window_focus_out), NULL);
    g_signal_connect(g_win_main, "key-press-event", G_CALLBACK(on_window_key_press), NULL);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(main_vbox), 8);
    gtk_container_add(GTK_CONTAINER(g_win_main), main_vbox);

    /* --- Record Section --- */
    g_btn_record = gtk_button_new_with_label("Start Recording");
    gtk_widget_set_name(g_btn_record, "btn_record_idle");
    g_signal_connect(g_btn_record, "button-press-event", G_CALLBACK(on_btn_record_button_press), NULL);
    g_signal_connect(g_btn_record, "clicked", G_CALLBACK(on_btn_record_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(main_vbox), g_btn_record, FALSE, FALSE, 0);

    g_lbl_status = gtk_label_new("Press 'Esc' or click Stop Recording.");
    gtk_widget_set_no_show_all(g_lbl_status, TRUE);
    gtk_box_pack_start(GTK_BOX(main_vbox), g_lbl_status, FALSE, FALSE, 0);

    GtkWidget *scrolled_prev = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_prev), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scrolled_prev, -1, 90);
    g_tv_preview = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_tv_preview), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g_tv_preview), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled_prev), g_tv_preview);
    gtk_box_pack_start(GTK_BOX(main_vbox), scrolled_prev, TRUE, TRUE, 0);

    /* No-delay between actions checkbox */
    g_chk_no_delay = gtk_check_button_new_with_label("No delay between recorded actions");
    gtk_box_pack_start(GTK_BOX(main_vbox), g_chk_no_delay, FALSE, FALSE, 0);

    /* --- Settings Section --- */
    GtkWidget *settings_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(main_vbox), settings_vbox, FALSE, FALSE, 0);

    /* Trigger row */
    GtkWidget *trig_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl_t_title = gtk_label_new("Trigger Key:");
    g_lbl_trigger = gtk_label_new(NULL);
    set_trigger_ui_label("None", "#888888");
    g_btn_trigger = gtk_button_new_with_label("Set Trigger");
    g_signal_connect(g_btn_trigger, "clicked", G_CALLBACK(on_btn_trigger_clicked), NULL);
    g_btn_clear_trigger = gtk_button_new_with_label("Clear");
    g_signal_connect(g_btn_clear_trigger, "clicked", G_CALLBACK(on_btn_clear_trigger_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(trig_hbox), lbl_t_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(trig_hbox), g_lbl_trigger, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(trig_hbox), g_btn_trigger, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(trig_hbox), g_btn_clear_trigger, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings_vbox), trig_hbox, FALSE, FALSE, 0);

    /* Repeats row */
    GtkWidget *rep_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl_reps = gtk_label_new("Repetitions:");
    g_entry_repeats = create_num_entry("1", 6);
    gtk_box_pack_start(GTK_BOX(rep_hbox), lbl_reps, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rep_hbox), g_entry_repeats, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(settings_vbox), rep_hbox, FALSE, FALSE, 0);

    /* Delay Mode row */
    GtkWidget *delay_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl_mode = gtk_label_new("Delay:");
    g_radio_const = gtk_radio_button_new_with_label(NULL, "Constant");
    g_radio_rand = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(g_radio_const), "Random");
    g_signal_connect(g_radio_rand, "toggled", G_CALLBACK(on_delay_mode_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(delay_hbox), lbl_mode, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delay_hbox), g_radio_const, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(delay_hbox), g_radio_rand, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings_vbox), delay_hbox, FALSE, FALSE, 0);

    /* Min / Max Inputs */
    GtkWidget *spin_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl_min = gtk_label_new("Min (ms):");
    g_entry_min = create_num_entry("100", 6);

    GtkWidget *lbl_max = gtk_label_new("Max (ms):");
    g_entry_max = create_num_entry("180", 6);
    gtk_widget_set_sensitive(g_entry_max, FALSE);

    gtk_box_pack_start(GTK_BOX(spin_hbox), lbl_min, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_hbox), g_entry_min, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(spin_hbox), lbl_max, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(spin_hbox), g_entry_max, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(settings_vbox), spin_hbox, FALSE, FALSE, 0);

    /* Action buttons: Save & New */
    GtkWidget *act_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_btn_save = gtk_button_new_with_label("Save Macro");
    g_signal_connect(g_btn_save, "clicked", G_CALLBACK(on_btn_save_clicked), NULL);
    g_btn_new = gtk_button_new_with_label("New Macro");
    g_signal_connect(g_btn_new, "clicked", G_CALLBACK(on_btn_new_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(act_hbox), g_btn_save, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(act_hbox), g_btn_new, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), act_hbox, FALSE, FALSE, 0);

    /* --- Saved Macros List Section --- */
    GtkWidget *lbl_list_header = gtk_label_new("Configured Macros:");
    gtk_label_set_xalign(GTK_LABEL(lbl_list_header), 0.0);
    gtk_box_pack_start(GTK_BOX(main_vbox), lbl_list_header, FALSE, FALSE, 0);

    GtkWidget *scrolled_list = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_list), GTK_SHADOW_IN);
    g_list_macros = gtk_list_box_new();
    g_signal_connect(g_list_macros, "row-selected", G_CALLBACK(on_macro_row_selected), NULL);
    gtk_container_add(GTK_CONTAINER(scrolled_list), g_list_macros);
    gtk_box_pack_start(GTK_BOX(main_vbox), scrolled_list, TRUE, TRUE, 0);

    /* Load Macros from disk */
    load_macros_from_disk(MACRO_FILE);
    refresh_macro_list_ui();

    /* Start global evdev listener */
    init_evdev_listener();

    gtk_widget_show_all(g_win_main);

    /* Initialize System Tray Indicator */
    init_tray_indicator();

    /* Run GTK Main Loop */
    gtk_main();

    /* Clean up on exit */
    close_evdev_listener();
    close_uinput();

    return 0;
}
