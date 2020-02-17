/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "config.h"

#include "clipboard.h"
#include "common/display.h"
#include "user.h"
#include "vnc.h"

#include <guacamole/client.h>
#include <guacamole/socket.h>
#include <guacamole/user.h>
#include <rfb/rfbclient.h>
#include <rfb/rfbproto.h>

#include <pthread.h>

/**
 * Handler for Guacamole user mouse events.
 */
int guac_vnc_user_mouse_handler(guac_user* user, int x, int y, int mask) {

    guac_client* client = user->client;
    guac_vnc_client* vnc_client = (guac_vnc_client*) client->data;
    rfbClient* rfb_client = vnc_client->rfb_client;

    /* Store current mouse location/state */
    guac_common_cursor_update(vnc_client->display->cursor, user, x, y, mask);

    /* Send VNC event only if finished connecting */
    if (rfb_client != NULL)
        SendPointerEvent(rfb_client, x, y, mask);

    return 0;
}

/**
 * Handler for Guacamole user key events.
 */
int guac_vnc_user_key_handler(guac_user* user, int keysym, int pressed) {

    guac_vnc_client* vnc_client = (guac_vnc_client*) user->client->data;
    rfbClient* rfb_client = vnc_client->rfb_client;

    /* Send VNC event only if finished connecting */
    if (rfb_client != NULL)
        SendKeyEvent(rfb_client, keysym, pressed);

    return 0;
}

int guac_vnc_user_join_handler(guac_user* user, int argc, char** argv) {

    guac_vnc_client* vnc_client = (guac_vnc_client*) user->client->data;

    /* Parse provided arguments */
    guac_vnc_settings* settings = guac_vnc_parse_args(user,
            argc, (const char**) argv);

    /* Fail if settings cannot be parsed */
    if (settings == NULL) {
        guac_user_log(user, GUAC_LOG_INFO,
                "Badly formatted client arguments.");
        return 1;
    }

    /* Store settings at user level */
    user->data = settings;

    /* Connect via VNC if owner */
    if (user->owner) {

        /* Store owner's settings at client level */
        vnc_client->settings = settings;

        /* Start client thread */
        if (pthread_create(&vnc_client->client_thread, NULL, guac_vnc_client_thread, user->client)) {
            guac_user_log(user, GUAC_LOG_ERROR, "Unable to start VNC client thread.");
            return 1;
        }

    }

    /* If not owner, synchronize with current state */
    else {
        /* Synchronize with current display */
        // FIXME: occamy: this is a temporal solution.
        // If two users race on same connection, the client->display is
        // a NULL pointer, which can cause segment fault.
        // see bug report: https://issues.apache.org/jira/browse/GUACAMOLE-898
        if (vnc_client->display != NULL) {
            guac_common_display_dup(vnc_client->display, user, user->socket);
        }
        guac_socket_flush(user->socket);

    }

    /* Only handle events if not read-only */
    if (!settings->read_only) {

        /* General mouse/keyboard/clipboard events */
        user->mouse_handler     = guac_vnc_user_mouse_handler;
        user->key_handler       = guac_vnc_user_key_handler;
        user->clipboard_handler = guac_vnc_clipboard_handler;

    }

    return 0;

}

int guac_vnc_user_leave_handler(guac_user* user) {

    guac_vnc_client* vnc_client = (guac_vnc_client*) user->client->data;

    if (vnc_client->display) {
        /* Update shared cursor state */
        guac_common_cursor_remove_user(vnc_client->display->cursor, user);
    }

    /* Free settings if not owner (owner settings will be freed with client) */
    if (!user->owner) {
        guac_vnc_settings* settings = (guac_vnc_settings*) user->data;
        guac_vnc_settings_free(settings);
    }

    return 0;
}

