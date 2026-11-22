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


#include "achievements.h"

#include <utility>

#include "environment.h"
#include "log.h"
#include "rc_api_runtime.h"
#include "rc_hash.h"
#include "rcheevos/src/rc_version.h"

namespace libretrodroid {

Achievements& Achievements::getInstance() {
    static Achievements instance;
    return instance;
}

void Achievements::setCallbacks(ServerCall serverCall, EventSink sink) {
    std::lock_guard<std::mutex> lock(queueLock);
    queue.emplace_back([this, serverCall = std::move(serverCall), sink = std::move(sink)]() mutable {
        serverCallSink = std::move(serverCall);
        eventSink = std::move(sink);
    });
}

void Achievements::identify(unsigned newConsoleId, const std::string& romPath) {
    // Hashing reads the ROM file only, so it runs here rather than on the emulation thread.
    char digest[33] = {0};
    bool hashed = rc_hash_generate_from_file(digest, newConsoleId, romPath.c_str()) != 0;
    std::string romHash = hashed ? std::string(digest) : std::string();

    std::lock_guard<std::mutex> lock(queueLock);
    queue.emplace_back([this, newConsoleId, romHash]() {
        consoleId = newConsoleId;
        hash = romHash;
        emit(EVENT_HASH, 0, romHash);
        if (romHash.empty()) return;

        rc_api_resolve_hash_request_t params {};
        params.game_hash = romHash.c_str();
        rc_api_request_t request {};
        int result = rc_api_init_resolve_hash_request(&request, &params);
        if (result != RC_OK) {
            emit(EVENT_IDENTIFIED, 0, rc_error_str(result));
            return;
        }
        Pending resolve;
        resolve.handler = [this, romHash](const rc_api_server_response_t& response) {
            rc_api_resolve_hash_response_t parsed {};
            int status = rc_api_process_resolve_hash_server_response(&parsed, &response);
            if (status == RC_OK) {
                emit(EVENT_IDENTIFIED, parsed.game_id, romHash);
            } else {
                const char* error = parsed.response.error_message;
                emit(EVENT_IDENTIFIED, 0, error != nullptr ? error : rc_error_str(status));
            }
            rc_api_destroy_resolve_hash_response(&parsed);
        };
        send(request, std::move(resolve));
        rc_api_destroy_request(&request);
        loadGameIfReady();
    });
}

void Achievements::login(const std::string& user, const std::string& password, const std::string& token) {
    std::lock_guard<std::mutex> lock(queueLock);
    queue.emplace_back([this, user, password, token]() {
        ensureClient();
        auto callback = [](int result, const char* error, rc_client_t* rc, void* userdata) {
            auto* self = static_cast<Achievements*>(userdata);
            const rc_client_user_t* info = rc_client_get_user_info(rc);
            if (result == RC_OK && info != nullptr) {
                self->emit(EVENT_LOGIN, result, std::string(info->username) + "\n" + info->token);
                self->loadGameIfReady();
            } else {
                self->emit(EVENT_LOGIN, result, error != nullptr ? error : rc_error_str(result));
            }
        };
        if (!token.empty()) {
            rc_client_begin_login_with_token(client, user.c_str(), token.c_str(), callback, this);
        } else {
            rc_client_begin_login_with_password(client, user.c_str(), password.c_str(), callback, this);
        }
    });
}

void Achievements::onServerResponse(int64_t id, std::string body, int httpStatus) {
    std::lock_guard<std::mutex> lock(queueLock);
    queue.emplace_back([this, id, body = std::move(body), httpStatus]() {
        auto it = pending.find(id);
        if (it == pending.end()) return; // the game was unloaded meanwhile
        Pending entry = std::move(it->second);
        pending.erase(it);

        rc_api_server_response_t response {};
        response.body = body.c_str();
        response.body_length = body.size();
        response.http_status_code = httpStatus;
        if (entry.handler) {
            entry.handler(response);
        } else if (entry.callback != nullptr) {
            entry.callback(&response, entry.callbackData);
        }
    });
}

void Achievements::pump(Core* currentCore) {
    core = currentCore;
    std::deque<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock(queueLock);
        work.swap(queue);
    }
    for (auto& task : work) task();

    if (!regionsReady && consoleId != 0 && core != nullptr) {
        const retro_memory_map* map = Environment::getInstance().getRawMemoryMap();
        regionsReady = rc_libretro_memory_init(&regions, map, getCoreMemoryInfo, consoleId) != 0;
        if (!regionsReady) emit(EVENT_LOG, 0, "Achievements: no memory regions for this core");
    }
}

void Achievements::doFrame() {
    if (client == nullptr || !regionsReady) return;
    if (rc_client_is_game_loaded(client)) {
        rc_client_do_frame(client);
    } else {
        rc_client_idle(client);
    }
}

void Achievements::onReset() {
    if (client != nullptr && rc_client_is_game_loaded(client)) rc_client_reset(client);
}

void Achievements::unload() {
    if (client != nullptr) {
        rc_client_destroy(client);
        client = nullptr;
    }
    if (regionsReady) {
        rc_libretro_memory_destroy(&regions);
        regionsReady = false;
    }
    pending.clear();
    consoleId = 0;
    hash.clear();
    gameLoadRequested = false;
    core = nullptr;
    std::lock_guard<std::mutex> lock(queueLock);
    // Keep the app callbacks registered for the next game; drop everything else.
    queue.clear();
}

std::string Achievements::userAgentClause() {
    return std::string("rcheevos/") + RCHEEVOS_VERSION_STRING;
}

void Achievements::ensureClient() {
    if (client != nullptr) return;
    client = rc_client_create(readMemory, serverCall);
    rc_client_set_userdata(client, this);
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, onLog);
    rc_client_set_event_handler(client, onEvent);
    rc_client_set_hardcore_enabled(client, 0); // rewind, run-ahead and state loads stay allowed
    rc_client_set_allow_background_memory_reads(client, 0);
}

void Achievements::send(const rc_api_request_t& request, Pending entry) {
    int64_t id = nextRequestId++;
    pending.emplace(id, std::move(entry));
    if (!serverCallSink) {
        pending.erase(id);
        emit(EVENT_LOG, 0, "Achievements: no server bridge installed");
        return;
    }
    serverCallSink(
        id,
        request.url != nullptr ? request.url : "",
        request.post_data != nullptr ? request.post_data : "",
        request.content_type != nullptr ? request.content_type : ""
    );
}

void Achievements::loadGameIfReady() {
    if (client == nullptr || gameLoadRequested || hash.empty()) return;
    if (rc_client_get_user_info(client) == nullptr) return;
    gameLoadRequested = true;
    rc_client_begin_load_game(client, hash.c_str(), [](int result, const char* error, rc_client_t* rc, void* userdata) {
        auto* self = static_cast<Achievements*>(userdata);
        const rc_client_game_t* game = rc_client_get_game_info(rc);
        if (result == RC_OK && game != nullptr) {
            self->emit(EVENT_GAME_LOADED, result, std::to_string(game->id) + "\n" + (game->title ? game->title : ""));
        } else {
            self->emit(EVENT_GAME_LOADED, result, error != nullptr ? error : rc_error_str(result));
        }
    }, this);
}

void Achievements::emit(int event, int64_t value, const std::string& text) {
    if (eventSink) eventSink(event, value, text);
}

uint32_t Achievements::readMemory(uint32_t address, uint8_t* buffer, uint32_t size, rc_client_t* client) {
    auto* self = static_cast<Achievements*>(rc_client_get_userdata(client));
    if (self == nullptr || !self->regionsReady) return 0;
    return rc_libretro_memory_read(&self->regions, address, buffer, size);
}

void Achievements::serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback,
                              void* callbackData, rc_client_t* client) {
    auto* self = static_cast<Achievements*>(rc_client_get_userdata(client));
    Pending entry;
    entry.callback = callback;
    entry.callbackData = callbackData;
    self->send(*request, std::move(entry));
}

void Achievements::onEvent(const rc_client_event_t* event, rc_client_t* client) {
    auto* self = static_cast<Achievements*>(rc_client_get_userdata(client));
    switch (event->type) {
        case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
            self->emit(EVENT_ACHIEVEMENT, event->achievement->id,
                       event->achievement->title != nullptr ? event->achievement->title : "");
            break;
        case RC_CLIENT_EVENT_SERVER_ERROR:
            self->emit(EVENT_SERVER_ERROR, event->server_error->result,
                       std::string(event->server_error->api ? event->server_error->api : "") + ": " +
                       (event->server_error->error_message ? event->server_error->error_message : ""));
            break;
        default:
            break;
    }
}

void Achievements::onLog(const char* message, const rc_client_t* client) {
    LOGI("rcheevos: %s", message);
}

void Achievements::getCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
    Core* active = getInstance().core;
    info->data = active != nullptr ? static_cast<uint8_t*>(active->retro_get_memory_data(id)) : nullptr;
    info->size = active != nullptr ? active->retro_get_memory_size(id) : 0;
}

}
