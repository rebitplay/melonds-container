#define _GNU_SOURCE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIRE_MAGIC 0x324e414cU
#define MSG_PACKET 1
#define MSG_STATS 2
#define MSG_READY 3
#define MSG_PROBE_SEND 4
#define MAX_PLAYERS 4
#define MAX_OPTIONS 256
#define MAX_PACKET_SIZE 65536
#define DEFAULT_FRAMES 900

struct wire_header {
    uint32_t magic;
    uint16_t type;
    uint16_t src;
    uint16_t dst;
    uint32_t flags;
    uint32_t len;
};

struct runner_config {
    char core_path[PATH_MAX];
    char rom_path[PATH_MAX];
    char runtime_dir[PATH_MAX];
    unsigned players;
    unsigned frames;
};

struct player_stats {
    uint64_t sent_packets;
    uint64_t sent_bytes;
    uint64_t received_packets;
    uint64_t received_bytes;
    uint64_t video_frames;
    uint64_t audio_frames;
};

struct core_api {
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    unsigned (*retro_api_version)(void);
    void (*retro_get_system_info)(struct retro_system_info*);
    void (*retro_get_system_av_info)(struct retro_system_av_info*);
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    void (*retro_run)(void);
    bool (*retro_load_game)(const struct retro_game_info*);
    void (*retro_unload_game)(void);
};

struct option_pair {
    char key[96];
    char value[160];
};

struct child_context {
    struct runner_config config;
    unsigned slot;
    int fd;
    void* dl;
    struct core_api api;
    struct retro_netpacket_callback netpacket;
    struct retro_frame_time_callback frame_time;
    struct option_pair defaults[MAX_OPTIONS];
    size_t defaults_len;
    char username[32];
    char system_dir[PATH_MAX];
    char save_dir[PATH_MAX];
    bool shutdown_requested;
    bool netpacket_ready;
    enum retro_pixel_format pixel_format;
    uint16_t joypad_mask;
    struct player_stats stats;
};

struct parent_player {
    unsigned slot;
    pid_t pid;
    int fd;
    bool alive;
    bool ready;
    struct player_stats child_stats;
    uint64_t routed_packets;
    uint64_t routed_bytes;
};

struct static_option {
    const char* key;
    const char* value;
};

static const struct static_option option_overrides[] = {
    {"melonds_console_mode", "ds"},
    {"melonds_sysfile_mode", "builtin"},
    {"melonds_boot_mode", "direct"},
    {"melonds_render_mode", "software"},
    {"melonds_network_mode", "disabled"},
    {"melonds_mac_address_mode", "from-username"},
    {"melonds_firmware_username", "guess_username"},
    {"melonds_touch_mode", "touch"},
    {"melonds_show_cursor", "disabled"},
    {"melonds_show_bios_warnings", "disabled"},
    {"melonds_homebrew_sdcard", "disabled"},
    {"melonds_dsi_sdcard", "disabled"},
    {"melonds_dsi_sdcard_sync_sdcard_to_host", "disabled"},
    {"melonds_homebrew_sync_sdcard_to_host", "disabled"},
    {NULL, NULL},
};

static struct child_context* g_child;

static void die_usage(const char* argv0, const char* message) {
    fprintf(stderr,
        "error: %s\nusage: %s --core /path/melondsds_libretro.so --rom /path/game.nds --runtime /tmp/rebit-room [--players 2] [--frames 900]\n",
        message,
        argv0);
    exit(2);
}

static void copy_arg(char* dst, size_t dst_len, const char* value, const char* label) {
    if (!value || strlen(value) >= dst_len) {
        fprintf(stderr, "error: %s path is too long\n", label);
        exit(2);
    }
    strcpy(dst, value);
}

static const char* need_value(int argc, char** argv, int* index, const char* name) {
    if (*index + 1 >= argc) {
        die_usage(argv[0], "missing argument value");
    }
    (void)name;
    (*index)++;
    return argv[*index];
}

static struct runner_config parse_args(int argc, char** argv) {
    struct runner_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.players = 2;
    cfg.frames = DEFAULT_FRAMES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--core") == 0) {
            copy_arg(cfg.core_path, sizeof(cfg.core_path), need_value(argc, argv, &i, "--core"), "core");
        } else if (strcmp(argv[i], "--rom") == 0) {
            copy_arg(cfg.rom_path, sizeof(cfg.rom_path), need_value(argc, argv, &i, "--rom"), "rom");
        } else if (strcmp(argv[i], "--runtime") == 0) {
            copy_arg(cfg.runtime_dir, sizeof(cfg.runtime_dir), need_value(argc, argv, &i, "--runtime"), "runtime");
        } else if (strcmp(argv[i], "--players") == 0) {
            cfg.players = (unsigned)strtoul(need_value(argc, argv, &i, "--players"), NULL, 10);
        } else if (strcmp(argv[i], "--frames") == 0) {
            cfg.frames = (unsigned)strtoul(need_value(argc, argv, &i, "--frames"), NULL, 10);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            die_usage(argv[0], "help requested");
        } else {
            die_usage(argv[0], "unknown argument");
        }
    }

    if (!cfg.core_path[0]) die_usage(argv[0], "--core is required");
    if (!cfg.rom_path[0]) die_usage(argv[0], "--rom is required");
    if (!cfg.runtime_dir[0]) die_usage(argv[0], "--runtime is required");
    if (cfg.players < 2 || cfg.players > MAX_PLAYERS) die_usage(argv[0], "--players must be between 2 and 4");
    if (cfg.frames == 0) die_usage(argv[0], "--frames must be greater than 0");
    return cfg;
}

static int mkdir_p(const char* path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int ensure_regular_file(const char* path, const char* label) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "[room] %s not found: %s\n", label, path);
        return -1;
    }
    return 0;
}

static unsigned char* read_file(const char* path, size_t* out_size) {
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    unsigned char* data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)size;
    return data;
}

static bool write_all(int fd, const void* data, size_t len) {
    const unsigned char* ptr = data;
    while (len > 0) {
        ssize_t n = write(fd, ptr, len);
        if (n > 0) {
            ptr += n;
            len -= (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            poll(&pfd, 1, 100);
        } else {
            return false;
        }
    }
    return true;
}

static bool send_message(int fd, uint16_t type, uint16_t src, uint16_t dst, uint32_t flags, const void* data, uint32_t len) {
    struct wire_header header;
    header.magic = WIRE_MAGIC;
    header.type = type;
    header.src = src;
    header.dst = dst;
    header.flags = flags;
    header.len = len;
    if (!write_all(fd, &header, sizeof(header))) return false;
    if (len == 0) return true;
    return write_all(fd, data, len);
}

static bool recv_exact(int fd, void* data, size_t len) {
    unsigned char* ptr = data;
    while (len > 0) {
        ssize_t n = read(fd, ptr, len);
        if (n > 0) {
            ptr += n;
            len -= (size_t)n;
        } else if (n == 0) {
            return false;
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool recv_message(int fd, struct wire_header* header, unsigned char** payload) {
    *payload = NULL;
    if (!recv_exact(fd, header, sizeof(*header))) return false;
    if (header->magic != WIRE_MAGIC || header->len > MAX_PACKET_SIZE) return false;
    if (header->len == 0) return true;
    *payload = malloc(header->len);
    if (!*payload) return false;
    if (!recv_exact(fd, *payload, header->len)) {
        free(*payload);
        *payload = NULL;
        return false;
    }
    return true;
}

static void* load_symbol(void* dl, const char* name) {
    void* sym = dlsym(dl, name);
    if (!sym) fprintf(stderr, "[slot %u] missing symbol %s: %s\n", g_child ? g_child->slot : 0, name, dlerror());
    return sym;
}

static bool load_core_api(struct child_context* ctx) {
#define LOAD(name) do { ctx->api.name = load_symbol(ctx->dl, #name); if (!ctx->api.name) return false; } while (0)
    LOAD(retro_set_environment);
    LOAD(retro_set_video_refresh);
    LOAD(retro_set_audio_sample);
    LOAD(retro_set_audio_sample_batch);
    LOAD(retro_set_input_poll);
    LOAD(retro_set_input_state);
    LOAD(retro_init);
    LOAD(retro_deinit);
    LOAD(retro_api_version);
    LOAD(retro_get_system_info);
    LOAD(retro_get_system_av_info);
    LOAD(retro_set_controller_port_device);
    LOAD(retro_run);
    LOAD(retro_load_game);
    LOAD(retro_unload_game);
#undef LOAD
    return true;
}

static const char* log_level_name(enum retro_log_level level) {
    switch (level) {
        case RETRO_LOG_DEBUG: return "debug";
        case RETRO_LOG_INFO: return "info";
        case RETRO_LOG_WARN: return "warn";
        case RETRO_LOG_ERROR: return "error";
        default: return "log";
    }
}

static void core_log(enum retro_log_level level, const char* fmt, ...) {
    if (level == RETRO_LOG_DEBUG && !getenv("MELONDS_RUNNER_DEBUG")) return;

    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
    fprintf(stderr, "[slot %u] core %s: %s\n", g_child ? g_child->slot : 0, log_level_name(level), text);
}

static void store_default_option(struct child_context* ctx, const char* key, const char* value) {
    if (!key || !value || !*value || ctx->defaults_len >= MAX_OPTIONS) return;
    for (size_t i = 0; i < ctx->defaults_len; i++) {
        if (strcmp(ctx->defaults[i].key, key) == 0) return;
    }
    snprintf(ctx->defaults[ctx->defaults_len].key, sizeof(ctx->defaults[ctx->defaults_len].key), "%s", key);
    snprintf(ctx->defaults[ctx->defaults_len].value, sizeof(ctx->defaults[ctx->defaults_len].value), "%s", value);
    ctx->defaults_len++;
}

static void store_v2_options(struct child_context* ctx, const struct retro_core_options_v2* options) {
    if (!options || !options->definitions) return;
    for (const struct retro_core_option_v2_definition* def = options->definitions; def->key; def++) {
        const char* value = def->default_value ? def->default_value : def->values[0].value;
        store_default_option(ctx, def->key, value);
    }
}

static void store_v1_options(struct child_context* ctx, const struct retro_core_option_definition* defs) {
    if (!defs) return;
    for (const struct retro_core_option_definition* def = defs; def->key; def++) {
        const char* value = def->default_value ? def->default_value : def->values[0].value;
        store_default_option(ctx, def->key, value);
    }
}

static void store_legacy_variables(struct child_context* ctx, const struct retro_variable* vars) {
    if (!vars) return;
    for (const struct retro_variable* var = vars; var->key; var++) {
        if (!var->value) continue;
        const char* start = strstr(var->value, "; ");
        start = start ? start + 2 : var->value;
        char value[160];
        snprintf(value, sizeof(value), "%s", start);
        char* end = strchr(value, '|');
        if (end) *end = '\0';
        store_default_option(ctx, var->key, value);
    }
}

static const char* find_option(struct child_context* ctx, const char* key) {
    if (!key) return NULL;
    for (const struct static_option* it = option_overrides; it->key; it++) {
        if (strcmp(it->key, key) == 0) return it->value;
    }
    for (size_t i = 0; i < ctx->defaults_len; i++) {
        if (strcmp(ctx->defaults[i].key, key) == 0) return ctx->defaults[i].value;
    }
    return NULL;
}

static bool environment_cb(unsigned cmd, void* data) {
    struct child_context* ctx = g_child;
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback* log = data;
            log->log = core_log;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
            if (!data) return false;
            ctx->netpacket = *(const struct retro_netpacket_callback*)data;
            ctx->netpacket_ready = ctx->netpacket.start && ctx->netpacket.receive;
            fprintf(stderr, "[slot %u] captured libretro netpacket interface\n", ctx->slot);
            return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *(unsigned*)data = 2;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            store_v2_options(ctx, data);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
            store_v1_options(ctx, data);
            return true;
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            store_legacy_variables(ctx, data);
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable* var = data;
            const char* value = find_option(ctx, var ? var->key : NULL);
            if (!var || !value) return false;
            var->value = value;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = false;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            ctx->pixel_format = *(enum retro_pixel_format*)data;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data = ctx->system_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = ctx->save_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_USERNAME:
            *(const char**)data = ctx->username;
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            *(unsigned*)data = 1;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const struct retro_message* msg = data;
            if (msg && msg->msg) core_log(RETRO_LOG_INFO, "message: %s", msg->msg);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            const struct retro_message_ext* msg = data;
            if (msg && msg->msg) core_log(msg->level, "message: %s", msg->msg);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
            ctx->frame_time = *(const struct retro_frame_time_callback*)data;
            return true;
        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
            return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *(bool*)data = false;
            return true;
        case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
            struct retro_throttle_state* state = data;
            state->mode = RETRO_THROTTLE_NONE;
            state->rate = 60.0f;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *(int*)data = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
            return true;
        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            *(float*)data = 60.0f;
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
            *(unsigned*)data = 1;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
            *(int*)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
            return true;
        case RETRO_ENVIRONMENT_SHUTDOWN:
            ctx->shutdown_requested = true;
            return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_ROTATION:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE:
        case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE:
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        case RETRO_ENVIRONMENT_GET_DEVICE_POWER:
            return false;
        default:
            return false;
    }
}

static void video_cb(const void* data, unsigned width, unsigned height, size_t pitch) {
    (void)data;
    (void)pitch;
    g_child->stats.video_frames++;
    if (g_child->stats.video_frames == 1) {
        fprintf(stderr, "[slot %u] first video frame %ux%u\n", g_child->slot, width, height);
    }
}

static void audio_cb(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

static size_t audio_batch_cb(const int16_t* data, size_t frames) {
    (void)data;
    g_child->stats.audio_frames += frames;
    return frames;
}

static void input_poll_cb(void) {
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port;
    (void)index;
    if (device == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)g_child->joypad_mask;
    if (device == RETRO_DEVICE_JOYPAD && id <= RETRO_DEVICE_ID_JOYPAD_R3) {
        return (g_child->joypad_mask & (1u << id)) ? 1 : 0;
    }
    if (device == RETRO_DEVICE_POINTER) return 0;
    return 0;
}

static uint16_t scripted_buttons(unsigned frame) {
    if (getenv("MELONDS_RUNNER_NO_INPUT")) return 0;

    unsigned phase = frame % 180;
    if (frame < 90) return 0;
    if (phase < 18) return (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_A);
    if (phase >= 45 && phase < 63) return (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_START);
    if (phase >= 90 && phase < 108) return (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_A);
    return 0;
}

static void child_send_cb(int flags, const void* buf, size_t len, uint16_t client_id) {
    struct child_context* ctx = g_child;
    if (!buf || len == 0) return;
    if (len > MAX_PACKET_SIZE) {
        core_log(RETRO_LOG_WARN, "dropping oversized netpacket: %zu bytes", len);
        return;
    }
    if (send_message(ctx->fd, MSG_PACKET, (uint16_t)ctx->slot, client_id, (uint32_t)flags, buf, (uint32_t)len)) {
        ctx->stats.sent_packets++;
        ctx->stats.sent_bytes += len;
    }
}

static void child_poll_receive_cb(void) {
    struct child_context* ctx = g_child;
    for (;;) {
        struct pollfd pfd = {ctx->fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0 || !(pfd.revents & POLLIN)) return;

        struct wire_header header;
        unsigned char* payload = NULL;
        if (!recv_message(ctx->fd, &header, &payload)) {
            ctx->shutdown_requested = true;
            return;
        }
        if (header.type == MSG_PROBE_SEND && payload && header.len > 0) {
            child_send_cb((int)header.flags, payload, header.len, header.dst);
        } else if (header.type == MSG_PACKET && payload && header.len > 0 && ctx->netpacket.receive) {
            ctx->netpacket.receive(payload, header.len, header.src);
            ctx->stats.received_packets++;
            ctx->stats.received_bytes += header.len;
        }
        free(payload);
    }
}

static void sleep_frame(double fps) {
    if (fps <= 0.0) fps = 59.8261;
    long ns = (long)(1000000000.0 / fps);
    struct timespec ts;
    ts.tv_sec = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    nanosleep(&ts, NULL);
}

static void send_child_stats(struct child_context* ctx) {
    send_message(ctx->fd, MSG_STATS, (uint16_t)ctx->slot, 0, 0, &ctx->stats, sizeof(ctx->stats));
}

static int run_child(struct runner_config cfg, unsigned slot, int fd) {
    struct child_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.config = cfg;
    ctx.slot = slot;
    ctx.fd = fd;
    ctx.pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    snprintf(ctx.username, sizeof(ctx.username), "rebit-p%u", slot);
    snprintf(ctx.system_dir, sizeof(ctx.system_dir), "%s/player-%u/system", cfg.runtime_dir, slot);
    snprintf(ctx.save_dir, sizeof(ctx.save_dir), "%s/player-%u/save", cfg.runtime_dir, slot);
    g_child = &ctx;

    unsigned char* rom = NULL;
    size_t rom_size = 0;
    int rc = 1;

    if (mkdir_p(ctx.system_dir) != 0 || mkdir_p(ctx.save_dir) != 0) {
        fprintf(stderr, "[slot %u] failed to create runtime dirs: %s\n", slot, strerror(errno));
        goto done;
    }

    rom = read_file(cfg.rom_path, &rom_size);
    if (!rom) {
        fprintf(stderr, "[slot %u] failed to read ROM %s\n", slot, cfg.rom_path);
        goto done;
    }

    ctx.dl = dlopen(cfg.core_path, RTLD_NOW | RTLD_LOCAL);
    if (!ctx.dl) {
        fprintf(stderr, "[slot %u] dlopen failed: %s\n", slot, dlerror());
        goto done;
    }
    if (!load_core_api(&ctx)) goto done;

    ctx.api.retro_set_environment(environment_cb);
    ctx.api.retro_set_video_refresh(video_cb);
    ctx.api.retro_set_audio_sample(audio_cb);
    ctx.api.retro_set_audio_sample_batch(audio_batch_cb);
    ctx.api.retro_set_input_poll(input_poll_cb);
    ctx.api.retro_set_input_state(input_state_cb);

    struct retro_system_info sys;
    memset(&sys, 0, sizeof(sys));
    ctx.api.retro_get_system_info(&sys);
    fprintf(stderr,
        "[slot %u] loaded core '%s' version '%s' api=%u need_fullpath=%s\n",
        slot,
        sys.library_name ? sys.library_name : "?",
        sys.library_version ? sys.library_version : "?",
        ctx.api.retro_api_version(),
        sys.need_fullpath ? "true" : "false");

    ctx.api.retro_init();
    ctx.api.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    struct retro_game_info game;
    memset(&game, 0, sizeof(game));
    game.path = cfg.rom_path;
    game.data = rom;
    game.size = rom_size;
    if (!ctx.api.retro_load_game(&game)) {
        fprintf(stderr, "[slot %u] retro_load_game failed\n", slot);
        goto done;
    }
    if (!ctx.netpacket_ready) {
        fprintf(stderr, "[slot %u] core did not expose netpacket callbacks\n", slot);
        goto done;
    }

    struct retro_system_av_info av;
    memset(&av, 0, sizeof(av));
    ctx.api.retro_get_system_av_info(&av);
    double fps = av.timing.fps > 0.0 ? av.timing.fps : 59.8261;
    fprintf(stderr,
        "[slot %u] game loaded, av=%ux%u max=%ux%u fps=%.4f sample_rate=%.0f\n",
        slot,
        av.geometry.base_width,
        av.geometry.base_height,
        av.geometry.max_width,
        av.geometry.max_height,
        fps,
        av.timing.sample_rate);

    uint16_t client_id = slot == 1 ? 0 : (uint16_t)slot;
    ctx.netpacket.start(client_id, child_send_cb, child_poll_receive_cb);
    send_message(ctx.fd, MSG_READY, (uint16_t)slot, 0, 0, NULL, 0);

    for (unsigned frame = 0; frame < cfg.frames && !ctx.shutdown_requested; frame++) {
        ctx.joypad_mask = scripted_buttons(frame);
        if (ctx.frame_time.callback) {
            retro_usec_t usec = ctx.frame_time.reference > 0 ? ctx.frame_time.reference : (retro_usec_t)(1000000.0 / fps);
            ctx.frame_time.callback(usec);
        }
        if (ctx.netpacket.poll) ctx.netpacket.poll();
        child_poll_receive_cb();
        ctx.api.retro_run();
        child_poll_receive_cb();
        sleep_frame(fps);
    }

    if (ctx.netpacket.stop) ctx.netpacket.stop();
    send_child_stats(&ctx);
    fprintf(stderr,
        "[slot %u] done: sent=%llu/%lluB received=%llu/%lluB video=%llu audio_frames=%llu\n",
        slot,
        (unsigned long long)ctx.stats.sent_packets,
        (unsigned long long)ctx.stats.sent_bytes,
        (unsigned long long)ctx.stats.received_packets,
        (unsigned long long)ctx.stats.received_bytes,
        (unsigned long long)ctx.stats.video_frames,
        (unsigned long long)ctx.stats.audio_frames);
    rc = 0;

done:
    if (rc != 0) send_child_stats(&ctx);
    if (ctx.dl && ctx.api.retro_unload_game) ctx.api.retro_unload_game();
    if (ctx.dl && ctx.api.retro_deinit) ctx.api.retro_deinit();
    if (ctx.dl) dlclose(ctx.dl);
    free(rom);
    return rc;
}

static struct parent_player* find_player(struct parent_player* players, unsigned count, unsigned slot) {
    for (unsigned i = 0; i < count; i++) {
        if (players[i].slot == slot) return &players[i];
    }
    return NULL;
}

static void deliver_packet(struct parent_player* target, const struct parent_player* source, const struct wire_header* header, const unsigned char* payload) {
    if (!target->alive || target->fd < 0 || target->slot == source->slot || header->len == 0) return;
    if (send_message(target->fd, MSG_PACKET, (uint16_t)source->slot, (uint16_t)target->slot, header->flags, payload, header->len)) {
        target->routed_packets++;
        target->routed_bytes += header->len;
    }
}

static void route_packet(struct parent_player* players, unsigned count, const struct parent_player* source, const struct wire_header* header, const unsigned char* payload) {
    if (header->dst == RETRO_NETPACKET_BROADCAST) {
        for (unsigned i = 0; i < count; i++) deliver_packet(&players[i], source, header, payload);
        return;
    }
    struct parent_player* target = find_player(players, count, header->dst);
    if (target) deliver_packet(target, source, header, payload);
}

static void maybe_send_probe(struct parent_player* players, unsigned count, bool* probe_sent) {
    if (*probe_sent || getenv("MELONDS_RUNNER_NO_PROBE")) return;

    for (unsigned i = 0; i < count; i++) {
        if (!players[i].ready) return;
    }

    static const unsigned char probe_packet[10] = {
        0, 0, 0, 0, 0, 0, 0, 1, // timestamp
        0, // aid
        0, // packet type: other
    };
    uint32_t flags = RETRO_NETPACKET_UNSEQUENCED | RETRO_NETPACKET_FLUSH_HINT;
    fprintf(stderr, "[room] injecting one netpacket probe through each ready child\n");
    for (unsigned i = 0; i < count; i++) {
        send_message(players[i].fd, MSG_PROBE_SEND, 0, RETRO_NETPACKET_BROADCAST, flags, probe_packet, sizeof(probe_packet));
    }
    *probe_sent = true;
}

static int run_parent(struct runner_config cfg) {
    if (ensure_regular_file(cfg.core_path, "core") != 0) return 1;
    if (ensure_regular_file(cfg.rom_path, "rom") != 0) return 1;
    if (mkdir_p(cfg.runtime_dir) != 0) {
        fprintf(stderr, "[room] failed to create runtime dir: %s\n", strerror(errno));
        return 1;
    }

    fprintf(stderr, "[room] starting %u melonDS instances\n", cfg.players);
    fprintf(stderr, "[room] core: %s\n", cfg.core_path);
    fprintf(stderr, "[room] rom: %s\n", cfg.rom_path);
    fprintf(stderr, "[room] runtime: %s\n", cfg.runtime_dir);

    struct parent_player players[MAX_PLAYERS];
    memset(players, 0, sizeof(players));

    for (unsigned slot = 1; slot <= cfg.players; slot++) {
        int sv[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            fprintf(stderr, "[room] socketpair failed: %s\n", strerror(errno));
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[room] fork failed: %s\n", strerror(errno));
            return 1;
        }
        if (pid == 0) {
            close(sv[0]);
            int code = run_child(cfg, slot, sv[1]);
            close(sv[1]);
            _exit(code);
        }
        close(sv[1]);
        players[slot - 1].slot = slot;
        players[slot - 1].pid = pid;
        players[slot - 1].fd = sv[0];
        players[slot - 1].alive = true;
    }

    unsigned alive_count = cfg.players;
    bool probe_sent = false;
    while (alive_count > 0) {
        struct pollfd pfds[MAX_PLAYERS];
        unsigned map[MAX_PLAYERS];
        nfds_t nfds = 0;
        for (unsigned i = 0; i < cfg.players; i++) {
            if (players[i].alive && players[i].fd >= 0) {
                pfds[nfds].fd = players[i].fd;
                pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
                pfds[nfds].revents = 0;
                map[nfds] = i;
                nfds++;
            }
        }

        int pr = poll(pfds, nfds, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[room] poll failed: %s\n", strerror(errno));
            return 1;
        }

        for (nfds_t n = 0; n < nfds; n++) {
            struct parent_player* player = &players[map[n]];
            if (!(pfds[n].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (pfds[n].revents & POLLIN) {
                struct wire_header header;
                unsigned char* payload = NULL;
                if (!recv_message(player->fd, &header, &payload)) {
                    pfds[n].revents |= POLLHUP;
                } else if (header.type == MSG_READY) {
                    player->ready = true;
                    fprintf(stderr, "[room] slot %u ready for netpacket routing\n", player->slot);
                    maybe_send_probe(players, cfg.players, &probe_sent);
                } else if (header.type == MSG_STATS) {
                    if (payload && header.len == sizeof(struct player_stats)) {
                        memcpy(&player->child_stats, payload, sizeof(struct player_stats));
                    }
                } else if (header.type == MSG_PACKET) {
                    route_packet(players, cfg.players, player, &header, payload);
                }
                free(payload);
            }
            if (pfds[n].revents & (POLLHUP | POLLERR)) {
                close(player->fd);
                player->fd = -1;
                player->alive = false;
                alive_count--;
            }
        }

        for (unsigned i = 0; i < cfg.players; i++) {
            if (!players[i].alive) continue;
            int status = 0;
            pid_t result = waitpid(players[i].pid, &status, WNOHANG);
            if (result == players[i].pid) {
                if (players[i].fd >= 0) close(players[i].fd);
                players[i].fd = -1;
                players[i].alive = false;
                alive_count--;
            }
        }
    }

    int failures = 0;
    for (unsigned i = 0; i < cfg.players; i++) {
        int status = 0;
        if (waitpid(players[i].pid, &status, 0) == players[i].pid) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failures++;
        }
    }

    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    fprintf(stderr, "[room] summary\n");
    for (unsigned i = 0; i < cfg.players; i++) {
        total_sent += players[i].child_stats.sent_packets;
        total_received += players[i].child_stats.received_packets;
        fprintf(stderr,
            "[room] slot %u ready=%s core_sent=%llu core_recv=%llu routed_to_slot=%llu video=%llu audio_frames=%llu\n",
            players[i].slot,
            players[i].ready ? "yes" : "no",
            (unsigned long long)players[i].child_stats.sent_packets,
            (unsigned long long)players[i].child_stats.received_packets,
            (unsigned long long)players[i].routed_packets,
            (unsigned long long)players[i].child_stats.video_frames,
            (unsigned long long)players[i].child_stats.audio_frames);
    }
    fprintf(stderr, "[room] netpacket totals: sent=%llu received=%llu\n",
        (unsigned long long)total_sent,
        (unsigned long long)total_received);

    if (failures > 0) {
        fprintf(stderr, "[room] failed: %d child process(es) exited with errors\n", failures);
        return 1;
    }
    if (total_sent == 0) {
        fprintf(stderr, "[room] warning: cores booted but no multiplayer packets were emitted by this ROM/session\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    struct runner_config cfg = parse_args(argc, argv);
    return run_parent(cfg);
}
