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

#include "client.h"
#include "common/clipboard.h"
#include "common/cursor.h"
#include "common/display.h"
#include "settings.h"
#include "vnc.h"

#include <cairo/cairo.h>
#include <guacamole/layer.h>
#include <guacamole/client.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>
#include <guacamole/timestamp.h>
#include <rfb/rfbclient.h>
#include <rfb/rfbproto.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

char* GUAC_VNC_CLIENT_KEY = "GUAC_VNC";


/**
 * Callback invoked by libVNCServer when an informational message needs to be
 * logged.
 *
 * @param format
 *     A printf-style format string to log.
 *
 * @param ...
 *     The values to use when filling the conversion specifiers within the
 *     format string.
 */
void guac_vnc_client_log_info(const char* format, ...) {

    char message[2048];

    /* Copy log message into buffer */
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    /* Log to syslog */
    syslog(LOG_INFO, "%s", message);

}

/**
 * Callback invoked by libVNCServer when an error message needs to be logged.
 *
 * @param format
 *     A printf-style format string to log.
 *
 * @param ...
 *     The values to use when filling the conversion specifiers within the
 *     format string.
 */
void guac_vnc_client_log_error(const char* format, ...) {

    char message[2048];

    /* Copy log message into buffer */
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    /* Log to syslog */
    syslog(LOG_ERR, "%s", message);

}

/**
 * Sets the pixel format to request of the VNC server. The request will be made
 * during the connection handshake with the VNC server using the values
 * specified by this function. Note that the VNC server is not required to
 * honor this request.
 *
 * @param client
 *     The VNC client associated with the VNC session whose desired pixel
 *     format should be set.
 *
 * @param color_depth
 *     The desired new color depth, in bits per pixel. Valid values are 8, 16,
 *     24, and 32.
 */
void guac_vnc_set_pixel_format(rfbClient* client, int color_depth) {
    client->format.trueColour = 1;
    switch(color_depth) {
        case 8:
            client->format.depth        = 8;
            client->format.bitsPerPixel = 8;
            client->format.blueShift    = 6;
            client->format.redShift     = 0;
            client->format.greenShift   = 3;
            client->format.blueMax      = 3;
            client->format.redMax       = 7;
            client->format.greenMax     = 7;
            break;

        case 16:
            client->format.depth        = 16;
            client->format.bitsPerPixel = 16;
            client->format.blueShift    = 0;
            client->format.redShift     = 11;
            client->format.greenShift   = 5;
            client->format.blueMax      = 0x1f;
            client->format.redMax       = 0x1f;
            client->format.greenMax     = 0x3f;
            break;

        case 24:
        case 32:
        default:
            client->format.depth        = 24;
            client->format.bitsPerPixel = 32;
            client->format.blueShift    = 0;
            client->format.redShift     = 16;
            client->format.greenShift   = 8;
            client->format.blueMax      = 0xff;
            client->format.redMax       = 0xff;
            client->format.greenMax     = 0xff;
    }
}

/**
 * Callback invoked by libVNCServer when it receives a new binary image data.
 * the VNC server. The image itself will be stored in the designated sub-
 * rectangle of client->framebuffer.
 *
 * @param client
 *     The VNC client associated with the VNC session in which the new image
 *     was received.
 *
 * @param x
 *     The X coordinate of the upper-left corner of the destination rectangle
 *     in which the image should be drawn, in pixels.
 *
 * @param y
 *     The Y coordinate of the upper-left corner of the destination rectangle
 *     in which the image should be drawn, in pixels.
 *
 * @param w
 *     The width of the image, in pixels.
 *
 * @param h
 *     The height of the image, in pixels.
 */
void guac_vnc_update(rfbClient* client, int x, int y, int w, int h) {

    guac_client* gc = rfbClientGetClientData(client, GUAC_VNC_CLIENT_KEY);
    guac_vnc_client* vnc_client = (guac_vnc_client*) gc->data;

    int dx, dy;

    /* Cairo image buffer */
    int stride;
    unsigned char* buffer;
    unsigned char* buffer_row_current;
    cairo_surface_t* surface;

    /* VNC framebuffer */
    unsigned int bpp;
    unsigned int fb_stride;
    unsigned char* fb_row_current;

    /* Ignore extra update if already handled by copyrect */
    if (vnc_client->copy_rect_used) {
        vnc_client->copy_rect_used = 0;
        return;
    }

    /* Init Cairo buffer */
    stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
    buffer = malloc(h*stride);
    buffer_row_current = buffer;

    bpp = client->format.bitsPerPixel/8;
    fb_stride = bpp * client->width;
    fb_row_current = client->frameBuffer + (y * fb_stride) + (x * bpp);

    /* Copy image data from VNC client to PNG */
    for (dy = y; dy<y+h; dy++) {

        unsigned int*  buffer_current;
        unsigned char* fb_current;
        
        /* Get current buffer row, advance to next */
        buffer_current      = (unsigned int*) buffer_row_current;
        buffer_row_current += stride;

        /* Get current framebuffer row, advance to next */
        fb_current      = fb_row_current;
        fb_row_current += fb_stride;

        for (dx = x; dx<x+w; dx++) {

            unsigned char red, green, blue;
            unsigned int v;

            switch (bpp) {
                case 4:
                    v = *((uint32_t*)  fb_current);
                    break;

                case 2:
                    v = *((uint16_t*) fb_current);
                    break;

                default:
                    v = *((uint8_t*)  fb_current);
            }

            /* Translate value to RGB */
            red   = (v >> client->format.redShift)   * 0x100 / (client->format.redMax  + 1);
            green = (v >> client->format.greenShift) * 0x100 / (client->format.greenMax+ 1);
            blue  = (v >> client->format.blueShift)  * 0x100 / (client->format.blueMax + 1);

            /* Output RGB */
            if (vnc_client->settings->swap_red_blue)
                *(buffer_current++) = (blue << 16) | (green << 8) | red;
            else
                *(buffer_current++) = (red  << 16) | (green << 8) | blue;

            fb_current += bpp;

        }
    }

    /* Create surface from decoded buffer */
    surface = cairo_image_surface_create_for_data(buffer, CAIRO_FORMAT_RGB24,
            w, h, stride);

    /* Draw directly to default layer */
    guac_common_surface_draw(vnc_client->display->default_surface,
            x, y, surface);

    /* Free surface */
    cairo_surface_destroy(surface);
    free(buffer);

}

/**
 * Callback invoked by libVNCServer when it receives a CopyRect message.
 * CopyRect specified a rectangle of source data within the display and a
 * set of X/Y coordinates to which that rectangle should be copied.
 *
 * @param client
 *     The VNC client associated with the VNC session in which the CopyRect
 *     was received.
 *
 * @param src_x
 *     The X coordinate of the upper-left corner of the source rectangle
 *     from which the image data should be copied, in pixels.
 *
 * @param src_y
 *     The Y coordinate of the upper-left corner of the source rectangle
 *     from which the image data should be copied, in pixels.
 *
 * @param w
 *     The width of the source and destination rectangles, in pixels.
 *
 * @param h
 *     The height of the source and destination rectangles, in pixels.
 *
 * @param dest_x
 *     The X coordinate of the upper-left corner of the destination rectangle
 *     in which the copied image data should be drawn, in pixels.
 *
 * @param dest_y
 *     The Y coordinate of the upper-left corner of the destination rectangle
 *     in which the copied image data should be drawn, in pixels.
 */
void guac_vnc_copyrect(rfbClient* client, int src_x, int src_y, int w, int h, int dest_x, int dest_y) {

    guac_client* gc = rfbClientGetClientData(client, GUAC_VNC_CLIENT_KEY);
    guac_vnc_client* vnc_client = (guac_vnc_client*) gc->data;

    /* Copy specified rectangle within default layer */
    guac_common_surface_copy(vnc_client->display->default_surface,
            src_x, src_y, w, h,
            vnc_client->display->default_surface, dest_x, dest_y);

    vnc_client->copy_rect_used = 1;

}

/* Define cairo_format_stride_for_width() if missing */
#ifndef HAVE_CAIRO_FORMAT_STRIDE_FOR_WIDTH
#define cairo_format_stride_for_width(format, width) (width*4)
#endif

/**
 * Callback invoked by libVNCServer when it receives a new cursor image from
 * the VNC server. The cursor image itself will be split across
 * client->rcSource and client->rcMask, where rcSource is an image buffer of
 * the format natively used by the current VNC connection, and rcMask is an
 * array if bitmasks. Each bit within rcMask corresponds to a pixel within
 * rcSource, where a 0 denotes full transparency and a 1 denotes full opacity.
 *
 * @param client
 *     The VNC client associated with the VNC session in which the new cursor
 *     image was received.
 *
 * @param x
 *     The X coordinate of the new cursor image's hotspot, in pixels.
 *
 * @param y
 *     The Y coordinate of the new cursor image's hotspot, in pixels.
 *
 * @param w
 *     The width of the cursor image, in pixels.
 *
 * @param h
 *     The height of the cursor image, in pixels.
 *
 * @param bpp
 *     The number of bytes in each pixel, which must be either 4, 2, or 1.
 */
void guac_vnc_cursor(rfbClient* client, int x, int y, int w, int h, int bpp) {

    guac_client* gc = rfbClientGetClientData(client, GUAC_VNC_CLIENT_KEY);
    guac_vnc_client* vnc_client = (guac_vnc_client*) gc->data;

    /* Cairo image buffer */
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    unsigned char* buffer = malloc(h*stride);
    unsigned char* buffer_row_current = buffer;

    /* VNC image buffer */
    unsigned int fb_stride = bpp * w;
    unsigned char* fb_row_current = client->rcSource;
    unsigned char* fb_mask = client->rcMask;

    int dx, dy;

    /* Copy image data from VNC client to RGBA buffer */
    for (dy = 0; dy<h; dy++) {

        unsigned int*  buffer_current;
        unsigned char* fb_current;
        
        /* Get current buffer row, advance to next */
        buffer_current      = (unsigned int*) buffer_row_current;
        buffer_row_current += stride;

        /* Get current framebuffer row, advance to next */
        fb_current      = fb_row_current;
        fb_row_current += fb_stride;

        for (dx = 0; dx<w; dx++) {

            unsigned char alpha, red, green, blue;
            unsigned int v;

            /* Read current pixel value */
            switch (bpp) {
                case 4:
                    v = *((uint32_t*) fb_current);
                    break;

                case 2:
                    v = *((uint16_t*) fb_current);
                    break;

                default:
                    v = *((uint8_t*)  fb_current);
            }

            /* Translate mask to alpha */
            if (*(fb_mask++)) alpha = 0xFF;
            else              alpha = 0x00;

            /* Translate value to RGB */
            red   = (v >> client->format.redShift)   * 0x100 / (client->format.redMax  + 1);
            green = (v >> client->format.greenShift) * 0x100 / (client->format.greenMax+ 1);
            blue  = (v >> client->format.blueShift)  * 0x100 / (client->format.blueMax + 1);

            /* Output ARGB */
            if (vnc_client->settings->swap_red_blue)
                *(buffer_current++) = (alpha << 24) | (blue << 16) | (green << 8) | red;
            else
                *(buffer_current++) = (alpha << 24) | (red  << 16) | (green << 8) | blue;

            /* Next VNC pixel */
            fb_current += bpp;

        }
    }

    /* Update stored cursor information */
    guac_common_cursor_set_argb(vnc_client->display->cursor, x, y,
            buffer, w, h, stride);

    /* Free surface */
    free(buffer);

    /* libvncclient does not free rcMask as it does rcSource */
    free(client->rcMask);
}

/**
 * Overridden implementation of the rfb_MallocFrameBuffer function invoked by
 * libVNCServer when the display is being resized (or initially allocated).
 *
 * @param client
 *     The VNC client associated with the VNC session whose display needs to be
 *     allocated or reallocated.
 *
 * @return
 *     The original value returned by rfb_MallocFrameBuffer().
 */
rfbBool guac_vnc_malloc_framebuffer(rfbClient* rfb_client) {

    guac_client* gc = rfbClientGetClientData(rfb_client, GUAC_VNC_CLIENT_KEY);
    guac_vnc_client* vnc_client = (guac_vnc_client*) gc->data;

    /* Resize surface */
    if (vnc_client->display != NULL)
        guac_common_surface_resize(vnc_client->display->default_surface,
                rfb_client->width, rfb_client->height);

    /* Use original, wrapped proc */
    return vnc_client->rfb_MallocFrameBuffer(rfb_client);
}

/**
 * Handler for clipboard data received via VNC, invoked by libVNCServer
 * whenever text has been copied or cut within the VNC session.
 *
 * @param client
 *     The VNC client associated with the session in which the user cut or
 *     copied text.
 *
 * @param text
 *     The string of cut/copied text.
 *
 * @param textlen
 *     The number of bytes in the string of cut/copied text.
 */
void guac_vnc_cut_text(rfbClient* client, const char* text, int textlen) {

    guac_client* gc = rfbClientGetClientData(client, GUAC_VNC_CLIENT_KEY);
    guac_vnc_client* vnc_client = (guac_vnc_client*) gc->data;

    char received_data[GUAC_VNC_CLIPBOARD_MAX_LENGTH];

    const char* input = text;
    char* output = received_data;
    guac_iconv_read* reader = vnc_client->clipboard_reader;

    /* Convert clipboard contents */
    guac_iconv(reader, &input, textlen,
               GUAC_WRITE_UTF8, &output, sizeof(received_data));

    /* Send converted data */
    guac_common_clipboard_reset(vnc_client->clipboard, "text/plain");
    guac_common_clipboard_append(vnc_client->clipboard, received_data, output - received_data);
    guac_common_clipboard_send(vnc_client->clipboard, gc);

}

/**
 * Callback which is invoked by libVNCServer when it needs to read the user's
 * VNC password. As ths user's password, if any, will be stored in the
 * connection settings, this function does nothing more than return that value.
 *
 * @param client
 *     The rfbClient associated with the VNC connection requiring the password.
 *
 * @return
 *     The password to provide to the VNC server.
 */
char* guac_vnc_get_password(rfbClient* client) {
    guac_client* gc = rfbClientGetClientData(client, GUAC_VNC_CLIENT_KEY);
    return ((guac_vnc_client*) gc->data)->settings->password;
}

rfbClient* guac_vnc_get_client(guac_client* client) {

    rfbClient* rfb_client = rfbGetClient(8, 3, 4); /* 32-bpp client */
    guac_vnc_client* vnc_client = (guac_vnc_client*) client->data;
    guac_vnc_settings* vnc_settings = vnc_client->settings;

    /* Store Guac client in rfb client */
    rfbClientSetClientData(rfb_client, GUAC_VNC_CLIENT_KEY, client);

    /* Framebuffer update handler */
    rfb_client->GotFrameBufferUpdate = guac_vnc_update;
    rfb_client->GotCopyRect = guac_vnc_copyrect;

    /* Do not handle clipboard and local cursor if read-only */
    if (vnc_settings->read_only == 0) {

        /* Clipboard */
        rfb_client->GotXCutText = guac_vnc_cut_text;

        /* Set remote cursor */
        if (vnc_settings->remote_cursor) {
            rfb_client->appData.useRemoteCursor = FALSE;
        }

        else {
            /* Enable client-side cursor */
            rfb_client->appData.useRemoteCursor = TRUE;
            rfb_client->GotCursorShape = guac_vnc_cursor;
        }

    }

    /* Password */
    rfb_client->GetPassword = guac_vnc_get_password;

    /* Depth */
    guac_vnc_set_pixel_format(rfb_client, vnc_settings->color_depth);

    /* Hook into allocation so we can handle resize. */
    vnc_client->rfb_MallocFrameBuffer = rfb_client->MallocFrameBuffer;
    rfb_client->MallocFrameBuffer = guac_vnc_malloc_framebuffer;
    rfb_client->canHandleNewFBSize = 1;

    /* Set hostname and port */
    rfb_client->serverHost = strdup(vnc_settings->hostname);
    rfb_client->serverPort = vnc_settings->port;

#ifdef ENABLE_VNC_REPEATER
    /* Set repeater parameters if specified */
    if (vnc_settings->dest_host) {
        rfb_client->destHost = strdup(vnc_settings->dest_host);
        rfb_client->destPort = vnc_settings->dest_port;
    }
#endif

#ifdef ENABLE_VNC_LISTEN
    /* If reverse connection enabled, start listening */
    if (vnc_settings->reverse_connect) {

        guac_client_log(client, GUAC_LOG_INFO, "Listening for connections on port %i", vnc_settings->port);

        /* Listen for connection from server */
        rfb_client->listenPort = vnc_settings->port;
        if (listenForIncomingConnectionsNoFork(rfb_client, vnc_settings->listen_timeout*1000) <= 0)
            return NULL;

    }
#endif

    /* Set encodings if provided */
    if (vnc_settings->encodings)
        rfb_client->appData.encodingsString = strdup(vnc_settings->encodings);

    /* Connect */
    if (rfbInitClient(rfb_client, NULL, NULL))
        return rfb_client;

    /* If connection fails, return NULL */
    return NULL;

}

/**
 * Waits until data is available to be read from the given rfbClient, and thus
 * a call to HandleRFBServerMessages() should not block. If the timeout elapses
 * before data is available, zero is returned.
 *
 * @param rfb_client
 *     The rfbClient to wait for.
 *
 * @param timeout
 *     The maximum amount of time to wait, in microseconds.
 *
 * @returns
 *     A positive value if data is available, zero if the timeout elapses
 *     before data becomes available, or a negative value on error.
 */
static int guac_vnc_wait_for_messages(rfbClient* rfb_client, int timeout) {

    /* Do not explicitly wait while data is on the buffer */
    if (rfb_client->buffered)
        return 1;

    /* If no data on buffer, wait for data on socket */
    return WaitForMessage(rfb_client, timeout);

}

/**
 * Sets the encoding of clipboard data exchanged with the VNC server to the
 * encoding having the given name. If the name is NULL, or is invalid, the
 * standard ISO8859-1 encoding will be used.
 *
 * @param client
 *     The client to set the clipboard encoding of.
 *
 * @param name
 *     The name of the encoding to use for all clipboard data. Valid values
 *     are: "ISO8859-1", "UTF-8", "UTF-16", "CP1252", or NULL.
 *
 * @return
 *     Zero if the chosen encoding is standard for VNC, or non-zero if the VNC
 *     standard is being violated.
 */
int guac_vnc_set_clipboard_encoding(guac_client* client,
        const char* name) {

    guac_vnc_client* vnc_client = (guac_vnc_client*) client->data;

    /* Use ISO8859-1 if explicitly selected or NULL */
    if (name == NULL || strcmp(name, "ISO8859-1") == 0) {
        vnc_client->clipboard_reader = GUAC_READ_ISO8859_1;
        vnc_client->clipboard_writer = GUAC_WRITE_ISO8859_1;
        return 0;
    }

    /* UTF-8 */
    if (strcmp(name, "UTF-8") == 0) {
        vnc_client->clipboard_reader = GUAC_READ_UTF8;
        vnc_client->clipboard_writer = GUAC_WRITE_UTF8;
        return 1;
    }

    /* UTF-16 */
    if (strcmp(name, "UTF-16") == 0) {
        vnc_client->clipboard_reader = GUAC_READ_UTF16;
        vnc_client->clipboard_writer = GUAC_WRITE_UTF16;
        return 1;
    }

    /* CP1252 */
    if (strcmp(name, "CP1252") == 0) {
        vnc_client->clipboard_reader = GUAC_READ_CP1252;
        vnc_client->clipboard_writer = GUAC_WRITE_CP1252;
        return 1;
    }

    /* If encoding unrecognized, warn and default to ISO8859-1 */
    guac_client_log(client, GUAC_LOG_WARNING,
            "Encoding '%s' is invalid. Defaulting to ISO8859-1.", name);

    vnc_client->clipboard_reader = GUAC_READ_ISO8859_1;
    vnc_client->clipboard_writer = GUAC_WRITE_ISO8859_1;
    return 0;

}

void* guac_vnc_client_thread(void* data) {

    guac_client* client = (guac_client*) data;
    guac_vnc_client* vnc_client = (guac_vnc_client*) client->data;
    guac_vnc_settings* settings = vnc_client->settings;

    /* Configure clipboard encoding */
    if (guac_vnc_set_clipboard_encoding(client, settings->clipboard_encoding)) {
        guac_client_log(client, GUAC_LOG_INFO, "Using non-standard VNC "
                "clipboard encoding: '%s'.", settings->clipboard_encoding);
    }

    /* Set up libvncclient logging */
    rfbClientLog = guac_vnc_client_log_info;
    rfbClientErr = guac_vnc_client_log_error;

    /* Attempt connection */
    rfbClient* rfb_client = guac_vnc_get_client(client);
    int retries_remaining = settings->retries;

    /* If unsuccessful, retry as many times as specified */
    while (!rfb_client && retries_remaining > 0) {

        guac_client_log(client, GUAC_LOG_INFO,
                "Connect failed. Waiting %ims before retrying...",
                GUAC_VNC_CONNECT_INTERVAL);

        /* Wait for given interval then retry */
        guac_timestamp_msleep(GUAC_VNC_CONNECT_INTERVAL);
        rfb_client = guac_vnc_get_client(client);
        retries_remaining--;

    }

    /* If the final connect attempt fails, return error */
    if (!rfb_client) {
        guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_NOT_FOUND,
                "Unable to connect to VNC server.");
        return NULL;
    }

    /* Set remaining client data */
    vnc_client->rfb_client = rfb_client;

    /* Create display */
    vnc_client->display = guac_common_display_alloc(client,
            rfb_client->width, rfb_client->height);

    /* If not read-only, set an appropriate cursor */
    if (settings->read_only == 0) {
        if (settings->remote_cursor)
            guac_common_cursor_set_dot(vnc_client->display->cursor);
        else
            guac_common_cursor_set_pointer(vnc_client->display->cursor);

    }

    guac_socket_flush(client->socket);

    guac_timestamp last_frame_end = guac_timestamp_current();

    /* Handle messages from VNC server while client is running */
    while (client->state == GUAC_CLIENT_RUNNING) {

        /* Wait for start of frame */
        int wait_result = guac_vnc_wait_for_messages(rfb_client,
                GUAC_VNC_FRAME_START_TIMEOUT);
        if (wait_result > 0) {

            int processing_lag = guac_client_get_processing_lag(client);
            guac_timestamp frame_start = guac_timestamp_current();

            /* Read server messages until frame is built */
            do {

                guac_timestamp frame_end;
                int frame_remaining;

                /* Handle any message received */
                if (!HandleRFBServerMessage(rfb_client)) {
                    guac_client_abort(client,
                            GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR,
                            "Error handling message from VNC server.");
                    break;
                }

                /* Calculate time remaining in frame */
                frame_end = guac_timestamp_current();
                frame_remaining = frame_start + GUAC_VNC_FRAME_DURATION
                                - frame_end;

                /* Calculate time that client needs to catch up */
                int time_elapsed = frame_end - last_frame_end;
                int required_wait = processing_lag - time_elapsed;

                /* Increase the duration of this frame if client is lagging */
                if (required_wait > GUAC_VNC_FRAME_TIMEOUT)
                    wait_result = guac_vnc_wait_for_messages(rfb_client,
                            required_wait*1000);

                /* Wait again if frame remaining */
                else if (frame_remaining > 0)
                    wait_result = guac_vnc_wait_for_messages(rfb_client,
                            GUAC_VNC_FRAME_TIMEOUT*1000);
                else
                    break;

            } while (wait_result > 0);

            /* Record end of frame, excluding server-side rendering time (we
             * assume server-side rendering time will be consistent between any
             * two subsequent frames, and that this time should thus be
             * excluded from the required wait period of the next frame). */
            last_frame_end = frame_start;

        }

        /* If an error occurs, log it and fail */
        if (wait_result < 0)
            guac_client_abort(client, GUAC_PROTOCOL_STATUS_UPSTREAM_ERROR, "Connection closed.");

        /* Flush frame */
        guac_common_surface_flush(vnc_client->display->default_surface);
        guac_client_end_frame(client);
        guac_socket_flush(client->socket);

    }

    /* Kill client and finish connection */
    guac_client_stop(client);
    guac_client_log(client, GUAC_LOG_INFO, "Internal VNC client disconnected");
    return NULL;

}

