#include "system_bus.h"
#include <luna-service2/lunaservice.h>
#include <glib.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

static const char *const kRegister = "luna://com.webos.service.tvpower/power/registerScreenSaverRequest";
static const char *const kResponse = "luna://com.webos.service.tvpower/power/responseScreenSaverRequest";
static const char *const kForeground = "luna://com.webos.applicationManager/getForegroundAppInfo";
static const char *const kLaunch = "luna://com.webos.applicationManager/launch";
static const char *const kHome = "com.webos.app.home";

struct SystemBus::Impl {
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    LSHandle *handle = nullptr;
    std::thread thread;
    std::string app_id;
    std::atomic<bool> *block = nullptr;
    std::mutex mutex;
    std::string previous;   // the app in front before UxAir brought itself there
    LSMessageToken foreground_token = 0; // touched only on the bus thread
    bool wants_foreground = false;
};

// The raw JSON value of key in payload (a number or a quoted string), or "".
static std::string json_value(const char *payload, const char *key) {
    const std::string text(payload ? payload : ""), quoted = std::string("\"") + key + "\"";
    size_t p = text.find(quoted);
    if (p == std::string::npos || (p = text.find(':', p + quoted.size())) == std::string::npos) return "";
    p = text.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return "";
    if (text[p] == '"') {
        const size_t end = text.find('"', p + 1);
        return end == std::string::npos ? "" : text.substr(p, end - p + 1);
    }
    const size_t end = text.find_first_of(",}", p);
    return text.substr(p, end == std::string::npos ? std::string::npos : end - p);
}
static std::string unquote(const std::string& v) {
    return v.size() >= 2 && v.front() == '"' ? v.substr(1, v.size() - 2) : v;
}

static void log_error(const char *what, LSError *error) {
    fprintf(stderr, "[bus] %s failed: %s\n", what, error->message ? error->message : "unknown");
    LSErrorFree(error);
}
static void call_once(LSHandle *handle, const char *uri, const std::string& payload, LSFilterFunc callback = nullptr, void *context = nullptr) {
    LSError error;
    LSErrorInit(&error);
    if (!LSCallOneReply(handle, uri, payload.c_str(), callback, context, nullptr, &error)) log_error(uri, &error);
}
static void launch(SystemBus::Impl *impl, const std::string& app_id) {
    fprintf(stderr, "[bus] launch %s\n", app_id.c_str());
    call_once(impl->handle, kLaunch, "{\"id\":\"" + app_id + "\"}");
}

// Work for the bus thread (LSHandle calls stay on the thread that runs its loop).
struct Task { SystemBus::Impl *impl; int kind; std::string target; };
enum { kBringToFront, kReturnToPrevious, kToBackground };

static bool on_screen_saver(LSHandle *handle, LSMessage *message, void *context) {
    auto *impl = static_cast<SystemBus::Impl *>(context);
    const char *payload = LSMessageGetPayload(message);
    const std::string timestamp = json_value(payload, "timestamp");
    if (timestamp.empty()) {   // the subscription's own replies
        fprintf(stderr, "[screensaver] %.200s\n", payload ? payload : "");
        return true;
    }
    const bool block = impl->block->load();
    call_once(handle, kResponse, "{\"clientName\":\"" + impl->app_id + "\",\"ack\":" + (block ? "false" : "true") +
                                 ",\"timestamp\":" + timestamp + "}");
    fprintf(stderr, "[screensaver] request %s -> %s\n", timestamp.c_str(), block ? "postponed" : "allowed");
    return true;
}

static bool on_foreground(LSHandle *, LSMessage *message, void *context) {
    auto *impl = static_cast<SystemBus::Impl *>(context);
    impl->foreground_token=0;
    if(!impl->wants_foreground)return true;
    if(json_value(LSMessageGetPayload(message),"returnValue")=="false")return true;
    const std::string front = unquote(json_value(LSMessageGetPayload(message), "appId"));
    if (front == impl->app_id) return true;   // already in front
    {
        std::lock_guard<std::mutex> l(impl->mutex);
        impl->previous = front;
    }
    fprintf(stderr, "[bus] to front (was %s)\n", front.empty() ? "nothing" : front.c_str());
    launch(impl, impl->app_id);
    return true;
}

static gboolean run_task(gpointer data) {
    Task *task = static_cast<Task *>(data);
    SystemBus::Impl *impl = task->impl;
    if(!impl->handle)return G_SOURCE_REMOVE;
    impl->wants_foreground=task->kind==kBringToFront;
    // A reply to an older foreground query must not relaunch UxAir after Back
    // or after the sender has disconnected.
    if(impl->foreground_token) {
        LSError error;LSErrorInit(&error);
        if(!LSCallCancel(impl->handle,impl->foreground_token,&error))log_error("cancel foreground",&error);
        impl->foreground_token=0;
    }
    std::string previous;
    {
        std::lock_guard<std::mutex> l(impl->mutex);
        previous = impl->previous;
        if (task->kind != kBringToFront) impl->previous.clear();
    }
    switch (task->kind) {
    case kBringToFront: {
        LSError error;LSErrorInit(&error);
        if(!LSCallOneReply(impl->handle,kForeground,"{}",on_foreground,impl,&impl->foreground_token,&error))
            log_error("foreground",&error);
        break;
    }
    case kReturnToPrevious: if (!previous.empty()) launch(impl, previous); break;
    case kToBackground: launch(impl, !task->target.empty() ? task->target : previous.empty() ? kHome : previous); break;
    }
    return G_SOURCE_REMOVE;
}
static void post(SystemBus::Impl *impl, int kind, const char *target = nullptr) {
    if (!impl || !impl->thread.joinable())return;
    GSource *source=g_idle_source_new();
    g_source_set_callback(source,run_task,new Task{impl,kind,target?target:""},
                          [](gpointer data){delete static_cast<Task*>(data);});
    g_source_attach(source,impl->context);g_source_unref(source);
}

bool SystemBus::start(const char *app_id) {
    if (impl_) return impl_->thread.joinable();
    impl_ = new Impl;
    impl_->app_id = app_id;
    impl_->block = &block_;
    impl_->context = g_main_context_new();
    impl_->loop = g_main_loop_new(impl_->context, FALSE);
    LSError error;
    LSErrorInit(&error);
    if (!LSRegister(nullptr, &impl_->handle, &error)) { log_error("register", &error); impl_->handle = nullptr; return false; }
    // LSGmainAttach, not LSGmainContextAttach: also in the luna-service2 of webOS 3
    if (!LSGmainAttach(impl_->handle, impl_->loop, &error)) { log_error("attach", &error); return false; }
    const std::string payload = "{\"subscribe\":true,\"clientName\":\"" + impl_->app_id + "\"}";
    LSMessageToken token = 0;
    if (!LSCall(impl_->handle, kRegister, payload.c_str(), on_screen_saver, impl_, &token, &error))
        log_error("registerScreenSaverRequest", &error);
    impl_->thread = std::thread([this] { g_main_loop_run(impl_->loop); });
    fprintf(stderr, "[bus] connected as %s\n", app_id);
    return true;
}

void SystemBus::bring_to_front() { post(impl_, kBringToFront); }
void SystemBus::return_to_previous() { post(impl_, kReturnToPrevious); }
void SystemBus::to_background(const char *app_id) { post(impl_, kToBackground, app_id); }

SystemBus::~SystemBus() {
    if (!impl_) return;
    if (impl_->thread.joinable()) {
        // Queue quit: a direct quit before the new thread enters run() can be
        // lost and leave join() waiting forever during immediate shutdown.
        GSource *source=g_idle_source_new();
        g_source_set_priority(source,G_PRIORITY_HIGH);
        g_source_set_callback(source,[](gpointer data)->gboolean {
            auto *impl=static_cast<Impl*>(data);
            LSError error;LSErrorInit(&error);
            if(impl->handle && !LSUnregister(impl->handle,&error))log_error("unregister",&error);
            impl->handle=nullptr;g_main_loop_quit(impl->loop);return G_SOURCE_REMOVE;
        },impl_,nullptr);
        g_source_attach(source,impl_->context);g_source_unref(source);
        impl_->thread.join();
    }
    if (impl_->handle) {
        LSError error;
        LSErrorInit(&error);
        if (!LSUnregister(impl_->handle, &error)) LSErrorFree(&error);
    }
    if (impl_->loop) g_main_loop_unref(impl_->loop);
    if (impl_->context) g_main_context_unref(impl_->context);
    delete impl_;
}
