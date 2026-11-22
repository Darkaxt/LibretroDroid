/*
 *     Copyright (C) 2026  Darkaxt
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef LIBRETRODROID_ACHIEVEMENTS_H
#define LIBRETRODROID_ACHIEVEMENTS_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "core.h"
#include "rc_client.h"
#include "rcheevos/src/rc_libretro.h"

namespace libretrodroid {

/**
 * RetroAchievements through rcheevos rc_client. Every rc_client call runs on the emulation thread
 * under the core lock: requests from other threads (app commands, HTTP responses) are queued and
 * executed by pump() at the start of the next step. HTTP itself is done by the app (ServerCall).
 */
class Achievements {
public:
    enum Event {
        EVENT_LOG = 0,          // text = message
        EVENT_HASH = 1,         // text = hash ("" if the ROM could not be hashed)
        EVENT_IDENTIFIED = 2,   // value = game id (0 = unknown hash), text = hash or error
        EVENT_LOGIN = 3,        // value = rc result, text = "user\ntoken" or error
        EVENT_GAME_LOADED = 4,  // value = rc result, text = "id\ntitle" or error
        EVENT_ACHIEVEMENT = 5,  // value = achievement id, text = title
        EVENT_SERVER_ERROR = 6, // text = api + message
    };

    using ServerCall = std::function<void(int64_t id, const std::string& url, const std::string& postData,
                                          const std::string& contentType)>;
    using EventSink = std::function<void(int event, int64_t value, const std::string& text)>;

    static Achievements& getInstance();

    /** Installs the app callbacks; they are invoked on the emulation thread. */
    void setCallbacks(ServerCall serverCall, EventSink eventSink);

    /** App thread: hash the ROM now and queue its identification by hash (no login needed). */
    void identify(unsigned consoleId, const std::string& romPath);
    /** App thread: queue a login; a non-empty token takes precedence over the password. */
    void login(const std::string& user, const std::string& password, const std::string& token);
    /** Any thread: queue the answer to a ServerCall. */
    void onServerResponse(int64_t id, std::string body, int httpStatus);

    /** Emulation thread, core lock held. */
    void pump(Core* core);
    void doFrame();
    void onReset();
    void unload();

    std::string userAgentClause();

private:
    struct Pending {
        rc_client_server_callback_t callback = nullptr;
        void* callbackData = nullptr;
        std::function<void(const rc_api_server_response_t&)> handler;
    };

    void ensureClient();
    void send(const rc_api_request_t& request, Pending pending);
    void loadGameIfReady();
    void emit(int event, int64_t value, const std::string& text);

    static uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t size, rc_client_t* client);
    static void serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                           void* callbackData, rc_client_t* client);
    static void onEvent(const rc_client_event_t* event, rc_client_t* client);
    static void onLog(const char* message, const rc_client_t* client);
    static void getCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info);

    std::mutex queueLock;
    std::deque<std::function<void()>> queue;

    ServerCall serverCallSink;
    EventSink eventSink;

    rc_client_t* client = nullptr;
    rc_libretro_memory_regions_t regions {};
    bool regionsReady = false;
    Core* core = nullptr;

    unsigned consoleId = 0;
    std::string hash;
    bool gameLoadRequested = false;
    int64_t nextRequestId = 1;
    std::map<int64_t, Pending> pending;
};

}

#endif
