/*****************************************************************************
 * chromecast_ctrl.cpp: Chromecast module for vlc
 *****************************************************************************
 * Copyright © 2014-2015 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast.h"

#include <vlc_playlist.h>
#include <vlc_threads.h>

#include <cassert>
#include <cerrno>
#ifdef HAVE_POLL
# include <poll.h>
#endif

#include "../../misc/webservices/json.h"

#define PACKET_MAX_LEN 10 * 1024

// Media player Chromecast app id
#define APP_ID "CC1AD845" // Default media player aka DEFAULT_MEDIA_RECEIVER_APPLICATION_ID

static const int CHROMECAST_CONTROL_PORT = 8009;

/* deadline regarding pings sent from receiver */
#define PING_WAIT_TIME 6000
#define PING_WAIT_RETRIES 0
/* deadline regarding pong we expect after pinging the receiver */
#define PONG_WAIT_TIME 500
#define PONG_WAIT_RETRIES 2

#define CONTROL_CFG_PREFIX "chromecast-"

static const std::string NAMESPACE_DEVICEAUTH       = "urn:x-cast:com.google.cast.tp.deviceauth";
static const std::string NAMESPACE_CONNECTION       = "urn:x-cast:com.google.cast.tp.connection";
static const std::string NAMESPACE_HEARTBEAT        = "urn:x-cast:com.google.cast.tp.heartbeat";
static const std::string NAMESPACE_RECEIVER         = "urn:x-cast:com.google.cast.receiver";

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
static void Clean(intf_thread_t *);

static void *ChromecastThread(void *data);

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define IP_TEXT N_("Chromecast IP address")
#define IP_LONGTEXT N_("This sets the IP adress of the Chromecast receiver.")
#define HTTP_PORT_TEXT N_("HTTP port")
#define HTTP_PORT_LONGTEXT N_("This sets the HTTP port of the server " \
                              "used to stream the media to the Chromecast.")
#define MUXER_TEXT N_("Muxer")
#define MUXER_LONGTEXT N_("This sets the muxer used to stream to the Chromecast.")
#define MIME_TEXT N_("MIME content type")
#define MIME_LONGTEXT N_("This sets the media MIME content type sent to the Chromecast.")

vlc_module_begin ()
    set_shortname( N_("Chromecast") )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
    set_description( N_("Chromecast interface") )
    set_capability( "interface", 0 )
    add_shortcut("chromecast")
    add_string(CONTROL_CFG_PREFIX "addr", "", IP_TEXT, IP_LONGTEXT, false)
    add_integer(CONTROL_CFG_PREFIX "http-port", HTTP_PORT, HTTP_PORT_TEXT, HTTP_PORT_LONGTEXT, false)
    add_string(CONTROL_CFG_PREFIX "mime", "video/x-matroska", MIME_TEXT, MIME_LONGTEXT, false)
    add_string(CONTROL_CFG_PREFIX "mux", "avformat{mux=matroska}", MUXER_TEXT, MUXER_LONGTEXT, false)
    set_callbacks( Open, Close )

vlc_module_end ()

/*****************************************************************************
 * Open: connect to the Chromecast and initialize the sout
 *****************************************************************************/
int Open(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = reinterpret_cast<intf_thread_t*>(p_this);
    intf_sys_t *p_sys = new(std::nothrow) intf_sys_t(p_intf);
    if (unlikely(p_sys == NULL))
        return VLC_ENOMEM;

    char *psz_ipChromecast = var_InheritString(p_intf, CONTROL_CFG_PREFIX "addr");
    if (psz_ipChromecast == NULL)
    {
        msg_Err(p_intf, "No Chromecast receiver IP provided");
        Clean(p_intf);
        return VLC_EGENERIC;
    }

    p_sys->i_sock_fd = p_sys->connectChromecast(psz_ipChromecast);
    free(psz_ipChromecast);
    if (p_sys->i_sock_fd < 0)
    {
        msg_Err(p_intf, "Could not connect the Chromecast");
        Clean(p_intf);
        return VLC_EGENERIC;
    }
    p_sys->setConnectionStatus(CHROMECAST_TLS_CONNECTED);

    char psz_localIP[NI_MAXNUMERICHOST];
    if (net_GetSockAddress(p_sys->i_sock_fd, psz_localIP, NULL))
    {
        msg_Err(p_this, "Cannot get local IP address");
        Clean(p_intf);
        return VLC_EGENERIC;
    }
    p_sys->serverIP = psz_localIP;

    char *psz_mux = var_InheritString(p_intf, CONTROL_CFG_PREFIX "mux");
    if (psz_mux == NULL)
    {
        Clean(p_intf);
        return VLC_EGENERIC;
    }

    // Start the Chromecast event thread.
    if (vlc_clone(&p_sys->chromecastThread, ChromecastThread, p_intf,
                  VLC_THREAD_PRIORITY_LOW))
    {
        msg_Err(p_intf, "Could not start the Chromecast talking thread");
        Clean(p_intf);
        return VLC_EGENERIC;
    }

    /* Ugly part:
     * We want to be sure that the Chromecast receives the first data packet sent by
     * the HTTP server. */

    // Lock the sout thread until we have sent the media loading command to the Chromecast.
    int i_ret = 0;
    const mtime_t deadline = mdate() + 6 * CLOCK_FREQ;
    vlc_mutex_lock(&p_sys->lock);
    while (p_sys->getConnectionStatus() != CHROMECAST_MEDIA_LOAD_SENT)
    {
        i_ret = vlc_cond_timedwait(&p_sys->loadCommandCond, &p_sys->lock, deadline);
        if (i_ret == ETIMEDOUT)
        {
            msg_Err(p_intf, "Timeout reached before sending the media loading command");
            vlc_mutex_unlock(&p_sys->lock);
            vlc_cancel(p_sys->chromecastThread);
            Clean(p_intf);
            return VLC_EGENERIC;
        }
    }
    vlc_mutex_unlock(&p_sys->lock);

    /* Even uglier: sleep more to let to the Chromecast initiate the connection
     * to the http server. */
    msleep(2 * CLOCK_FREQ);

    p_intf->p_sys = p_sys;

    return VLC_SUCCESS;
}


/*****************************************************************************
 * Close: destroy interface
 *****************************************************************************/
void Close(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = reinterpret_cast<intf_thread_t*>(p_this);
    intf_sys_t *p_sys = p_intf->p_sys;

    vlc_cancel(p_sys->chromecastThread);
    vlc_join(p_sys->chromecastThread, NULL);

    switch (p_sys->getConnectionStatus())
    {
    case CHROMECAST_MEDIA_LOAD_SENT:
    case CHROMECAST_APP_STARTED:
        // Generate the close messages.
        p_sys->msgReceiverClose(p_sys->appTransportId);
        // ft
    case CHROMECAST_AUTHENTICATED:
        p_sys->msgReceiverClose(DEFAULT_CHOMECAST_RECEIVER);
        // ft
    default:
        break;
    }

    Clean(p_intf);
}

/**
 * @brief Clean and release the variables in a sout_stream_sys_t structure
 */
void Clean(intf_thread_t *p_stream)
{
    intf_sys_t *p_sys = p_stream->p_sys;

    p_sys->disconnectChromecast();

    delete p_sys;
}

/**
 * @brief Build a CastMessage to send to the Chromecast
 * @param namespace_ the message namespace
 * @param payloadType the payload type (CastMessage_PayloadType_STRING or
 * CastMessage_PayloadType_BINARY
 * @param payload the payload
 * @param destinationId the destination idenifier
 * @return the generated CastMessage
 */
void intf_sys_t::buildMessage(const std::string & namespace_,
                              const std::string & payload,
                              const std::string & destinationId,
                              castchannel::CastMessage_PayloadType payloadType)
{
    castchannel::CastMessage msg;

    msg.set_protocol_version(castchannel::CastMessage_ProtocolVersion_CASTV2_1_0);
    msg.set_namespace_(namespace_);
    msg.set_payload_type(payloadType);
    msg.set_source_id("sender-vlc");
    msg.set_destination_id(destinationId);
    if (payloadType == castchannel::CastMessage_PayloadType_STRING)
        msg.set_payload_utf8(payload);
    else // CastMessage_PayloadType_BINARY
        msg.set_payload_binary(payload);

    sendMessage(msg);
}

intf_sys_t::intf_sys_t(intf_thread_t * const p_this)
 : p_stream(p_this)
 , p_tls(NULL)
 , conn_status(CHROMECAST_DISCONNECTED)
 , i_receiver_requestId(0)
 , i_requestId(0)
{
    vlc_mutex_init(&lock);
    vlc_cond_init(&loadCommandCond);
}

intf_sys_t::~intf_sys_t()
{
    vlc_cond_destroy(&loadCommandCond);
    vlc_mutex_destroy(&lock);
}

/**
 * @brief Connect to the Chromecast
 * @param p_stream the sout_stream_t structure
 * @return the opened socket file descriptor or -1 on error
 */
int intf_sys_t::connectChromecast(char *psz_ipChromecast)
{
    int fd = net_ConnectTCP(p_stream, psz_ipChromecast, CHROMECAST_CONTROL_PORT);
    if (fd < 0)
        return -1;

    p_creds = vlc_tls_ClientCreate(VLC_OBJECT(p_stream));
    if (p_creds == NULL)
    {
        net_Close(fd);
        return -1;
    }

    p_tls = vlc_tls_ClientSessionCreateFD(p_creds, fd, psz_ipChromecast,
                                               "tcps", NULL, NULL);

    if (p_tls == NULL)
    {
        net_Close(fd);
        vlc_tls_Delete(p_creds);
        return -1;
    }

    return fd;
}


/**
 * @brief Disconnect from the Chromecast
 */
void intf_sys_t::disconnectChromecast()
{
    if (p_tls)
    {
        vlc_tls_Close(p_tls);
        vlc_tls_Delete(p_creds);
        p_tls = NULL;
        setConnectionStatus(CHROMECAST_DISCONNECTED);
    }
}


/**
 * @brief Receive a data packet from the Chromecast
 * @param p_stream the sout_stream_t structure
 * @param b_msgReceived returns true if a message has been entirely received else false
 * @param i_payloadSize returns the payload size of the message received
 * @return the number of bytes received of -1 on error
 */
// Use here only C linkage and POD types as this function is a cancelation point.
extern "C" int recvPacket(vlc_object_t *p_stream, bool &b_msgReceived,
                          uint32_t &i_payloadSize, int i_sock_fd, vlc_tls_t *p_tls,
                          unsigned *pi_received, uint8_t *p_data, bool *pb_pingTimeout,
                          int *pi_wait_delay, int *pi_wait_retries)
{
    struct pollfd ufd[1];
    ufd[0].fd = i_sock_fd;
    ufd[0].events = POLLIN;

    /* The Chromecast normally sends a PING command every 5 seconds or so.
     * If we do not receive one after 6 seconds, we send a PING.
     * If after this PING, we do not receive a PONG, then we consider the
     * connection as dead. */
    if (poll(ufd, 1, *pi_wait_delay) == 0)
    {
        if (*pb_pingTimeout)
        {
            if (!*pi_wait_retries)
            {
                msg_Err(p_stream, "No PONG answer received from the Chromecast");
                return 0; // Connection died
            }
            (*pi_wait_retries)--;
        }
        else
        {
            /* now expect a pong */
            *pi_wait_delay = PONG_WAIT_TIME;
            *pi_wait_retries = PONG_WAIT_RETRIES;
            msg_Warn(p_stream, "No PING received from the Chromecast, sending a PING");
        }
        *pb_pingTimeout = true;
    }
    else
    {
        *pb_pingTimeout = false;
        /* reset to default ping waiting */
        *pi_wait_delay = PING_WAIT_TIME;
        *pi_wait_retries = PING_WAIT_RETRIES;
    }

    int i_ret;

    /* Packet structure:
     * +------------------------------------+------------------------------+
     * | Payload size (uint32_t big endian) |         Payload data         |
     * +------------------------------------+------------------------------+ */
    while (*pi_received < PACKET_HEADER_LEN)
    {
        // We receive the header.
        i_ret = tls_Recv(p_tls, p_data + *pi_received, PACKET_HEADER_LEN - *pi_received);
        if (i_ret <= 0)
            return i_ret;
        *pi_received += i_ret;
    }

    // We receive the payload.

    // Get the size of the payload
    i_payloadSize = U32_AT( p_data );
    const uint32_t i_maxPayloadSize = PACKET_MAX_LEN - PACKET_HEADER_LEN;

    if (i_payloadSize > i_maxPayloadSize)
    {
        // Error case: the packet sent by the Chromecast is too long: we drop it.
        msg_Err(p_stream, "Packet too long: droping its data");

        uint32_t i_size = i_payloadSize - (*pi_received - PACKET_HEADER_LEN);
        if (i_size > i_maxPayloadSize)
            i_size = i_maxPayloadSize;

        i_ret = tls_Recv(p_tls, p_data + PACKET_HEADER_LEN, i_size);
        if (i_ret <= 0)
            return i_ret;
        *pi_received += i_ret;

        if (*pi_received < i_payloadSize + PACKET_HEADER_LEN)
            return i_ret;

        *pi_received = 0;
        return -1;
    }

    // Normal case
    i_ret = tls_Recv(p_tls, p_data + *pi_received,
                     i_payloadSize - (*pi_received - PACKET_HEADER_LEN));
    if (i_ret <= 0)
        return i_ret;
    *pi_received += i_ret;

    if (*pi_received < i_payloadSize + PACKET_HEADER_LEN)
        return i_ret;

    assert(*pi_received == i_payloadSize + PACKET_HEADER_LEN);
    *pi_received = 0;
    b_msgReceived = true;
    return i_ret;
}

/**
 * @brief Process a message received from the Chromecast
 * @param msg the CastMessage to process
 * @return 0 if the message has been successfuly processed else -1
 */
void intf_sys_t::processMessage(const castchannel::CastMessage &msg)
{
    const std::string & namespace_ = msg.namespace_();

#ifndef NDEBUG
    msg_Dbg(p_stream,"processMessage: %s->%s %s", namespace_.c_str(), msg.destination_id().c_str(), msg.payload_utf8().c_str());
#endif

    if (namespace_ == NAMESPACE_DEVICEAUTH)
    {
        castchannel::DeviceAuthMessage authMessage;
        authMessage.ParseFromString(msg.payload_binary());

        if (authMessage.has_error())
        {
            msg_Err(p_stream, "Authentification error: %d", authMessage.error().error_type());
        }
        else if (!authMessage.has_response())
        {
            msg_Err(p_stream, "Authentification message has no response field");
        }
        else
        {
            vlc_mutex_locker locker(&lock);
            setConnectionStatus(CHROMECAST_AUTHENTICATED);
            msgConnect(DEFAULT_CHOMECAST_RECEIVER);
            msgReceiverLaunchApp();
        }
    }
    else if (namespace_ == NAMESPACE_HEARTBEAT)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);

        if (type == "PING")
        {
            msg_Dbg(p_stream, "PING received from the Chromecast");
            msgPong();
        }
        else if (type == "PONG")
        {
            msg_Dbg(p_stream, "PONG received from the Chromecast");
        }
        else
        {
            msg_Warn(p_stream, "Heartbeat command not supported: %s", type.c_str());
        }

        json_value_free(p_data);
    }
    else if (namespace_ == NAMESPACE_RECEIVER)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);

        if (type == "RECEIVER_STATUS")
        {
            json_value applications = (*p_data)["status"]["applications"];
            const json_value *p_app = NULL;

            vlc_mutex_locker locker(&lock);
            for (unsigned i = 0; i < applications.u.array.length; ++i)
            {
                std::string appId(applications[i]["appId"]);
                if (appId == APP_ID)
                {
                    const char *pz_transportId = applications[i]["transportId"];
                    if (pz_transportId != NULL)
                    {
                        appTransportId = std::string(pz_transportId);
                        p_app = &applications[i];
                    }
                    break;
                }
            }

            if ( p_app )
            {
                if (!appTransportId.empty()
                        && getConnectionStatus() == CHROMECAST_AUTHENTICATED)
                {
                    msgConnect(appTransportId);
                    setConnectionStatus(CHROMECAST_APP_STARTED);
                    msgPlayerLoad();
                    setConnectionStatus(CHROMECAST_MEDIA_LOAD_SENT);
                    vlc_cond_signal(&loadCommandCond);
                }
            }
            else
            {
                switch(getConnectionStatus())
                {
                /* If the app is no longer present */
                case CHROMECAST_APP_STARTED:
                case CHROMECAST_MEDIA_LOAD_SENT:
                    msg_Warn(p_stream, "app is no longer present. closing");
                    msgReceiverClose(appTransportId);
                    setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
                    break;

                case CHROMECAST_AUTHENTICATED:
                    msg_Dbg(p_stream, "Chromecast was running no app, launch media_app");
                    appTransportId = "";
                    msgReceiverLaunchApp();
                    break;

                default:
                    break;
                }

            }
        }
        else if (type == "LAUNCH_ERROR")
        {
            json_value reason = (*p_data)["reason"];
            msg_Err(p_stream, "Failed to start the MediaPlayer: %s",
                    (const char *)reason);
        }
        else
        {
            msg_Warn(p_stream, "Receiver command not supported: %s",
                    msg.payload_utf8().c_str());
        }

        json_value_free(p_data);
    }
    else if (namespace_ == NAMESPACE_MEDIA)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);

        if (type == "MEDIA_STATUS")
        {
            json_value status = (*p_data)["status"];
            msg_Dbg(p_stream, "Player state: %s sessionId:%d",
                    status[0]["playerState"].operator const char *(),
                    (int)(json_int_t) status[0]["mediaSessionId"]);
        }
        else if (type == "LOAD_FAILED")
        {
            msg_Err(p_stream, "Media load failed");
            msgReceiverClose(appTransportId);
            vlc_mutex_locker locker(&lock);
            setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
        }
        else if (type == "INVALID_REQUEST")
        {
            msg_Dbg(p_stream, "We sent an invalid request reason:%s", (*p_data)["reason"].operator const char *());
        }
        else
        {
            msg_Warn(p_stream, "Media command not supported: %s",
                    msg.payload_utf8().c_str());
        }

        json_value_free(p_data);
    }
    else if (namespace_ == NAMESPACE_CONNECTION)
    {
        json_value *p_data = json_parse(msg.payload_utf8().c_str());
        std::string type((*p_data)["type"]);
        json_value_free(p_data);

        if (type == "CLOSE")
        {
            msg_Warn(p_stream, "received close message");
            vlc_mutex_locker locker(&lock);
            setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
        }
        else
        {
            msg_Warn(p_stream, "Connection command not supported: %s",
                    type.c_str());
        }
    }
    else
    {
        msg_Err(p_stream, "Unknown namespace: %s", msg.namespace_().c_str());
    }
}

/*****************************************************************************
 * Message preparation
 *****************************************************************************/
void intf_sys_t::msgAuth()
{
    castchannel::DeviceAuthMessage authMessage;
    authMessage.mutable_challenge();

    buildMessage(NAMESPACE_DEVICEAUTH, authMessage.SerializeAsString(),
                 DEFAULT_CHOMECAST_RECEIVER, castchannel::CastMessage_PayloadType_BINARY);
}


void intf_sys_t::msgPing()
{
    std::string s("{\"type\":\"PING\"}");
    buildMessage(NAMESPACE_HEARTBEAT, s);
}


void intf_sys_t::msgPong()
{
    std::string s("{\"type\":\"PONG\"}");
    buildMessage(NAMESPACE_HEARTBEAT, s);
}

void intf_sys_t::msgConnect(const std::string & destinationId)
{
    std::string s("{\"type\":\"CONNECT\"}");
    buildMessage(NAMESPACE_CONNECTION, s, destinationId);
}


void intf_sys_t::msgReceiverClose(std::string destinationId)
{
    std::string s("{\"type\":\"CLOSE\"}");
    buildMessage(NAMESPACE_CONNECTION, s, destinationId);
}

void intf_sys_t::msgReceiverGetStatus()
{
    std::stringstream ss;
    ss << "{\"type\":\"GET_STATUS\","
       <<  "\"requestId\":" << i_receiver_requestId++ << "}";

    buildMessage(NAMESPACE_RECEIVER, ss.str());
}

void intf_sys_t::msgReceiverLaunchApp()
{
    std::stringstream ss;
    ss << "{\"type\":\"LAUNCH\","
       <<  "\"appId\":\"" << APP_ID << "\","
       <<  "\"requestId\":" << i_receiver_requestId++ << "}";

    buildMessage(NAMESPACE_RECEIVER, ss.str());
}


void intf_sys_t::msgPlayerLoad()
{
    char *psz_mime = var_InheritString(p_stream, CONTROL_CFG_PREFIX "mime");
    if (psz_mime == NULL)
        return;

    std::stringstream ss;
    ss << "{\"type\":\"LOAD\","
       <<  "\"media\":{\"contentId\":\"http://" << serverIP << ":"
           << var_InheritInteger(p_stream, CONTROL_CFG_PREFIX"http-port")
           << "/stream\","
       <<             "\"streamType\":\"LIVE\","
       <<             "\"contentType\":\"" << std::string(psz_mime) << "\"},"
       <<  "\"requestId\":" << i_requestId++ << "}";

    free(psz_mime);

    buildMessage(NAMESPACE_MEDIA, ss.str(), appTransportId);
}

/**
 * @brief Send a message to the Chromecast
 * @param msg the CastMessage to send
 * @return vlc error code
 */
int intf_sys_t::sendMessage(const castchannel::CastMessage &msg)
{
    int i_size = msg.ByteSize();
    uint8_t *p_data = new(std::nothrow) uint8_t[PACKET_HEADER_LEN + i_size];
    if (p_data == NULL)
        return VLC_ENOMEM;

#ifndef NDEBUG
    msg_Dbg(p_stream, "sendMessage: %s->%s %s", msg.namespace_().c_str(), msg.destination_id().c_str(), msg.payload_utf8().c_str());
#endif

    SetDWBE(p_data, i_size);
    msg.SerializeWithCachedSizesToArray(p_data + PACKET_HEADER_LEN);

    vlc_mutex_locker locker(&lock);
    int i_ret = tls_Send(p_tls, p_data, PACKET_HEADER_LEN + i_size);
    delete[] p_data;
    if (i_ret == PACKET_HEADER_LEN + i_size)
        return VLC_SUCCESS;

    return VLC_EGENERIC;
}

/*****************************************************************************
 * Chromecast thread
 *****************************************************************************/
static void* ChromecastThread(void* p_data)
{
    int canc = vlc_savecancel();
    // Not cancellation-safe part.
    intf_thread_t *p_stream = reinterpret_cast<intf_thread_t*>(p_data);
    intf_sys_t *p_sys = p_stream->p_sys;

    p_sys->msgAuth();
    vlc_restorecancel(canc);

    while (1)
    {
        p_sys->handleMessages();

        vlc_mutex_locker locker(&p_sys->lock);
        if ( p_sys->getConnectionStatus() == CHROMECAST_CONNECTION_DEAD )
            break;
    }

    return NULL;
}

void intf_sys_t::handleMessages()
{
    unsigned i_received = 0;
    uint8_t p_packet[PACKET_MAX_LEN];
    bool b_pingTimeout = false;

    int i_waitdelay = PING_WAIT_TIME;
    int i_retries = PING_WAIT_RETRIES;

    bool b_msgReceived = false;
    uint32_t i_payloadSize = 0;
    int i_ret = recvPacket(VLC_OBJECT(p_stream), b_msgReceived, i_payloadSize, i_sock_fd,
                           p_tls, &i_received, p_packet, &b_pingTimeout,
                           &i_waitdelay, &i_retries);

    int canc = vlc_savecancel();
    // Not cancellation-safe part.

#if defined(_WIN32)
    if ((i_ret < 0 && WSAGetLastError() != WSAEWOULDBLOCK) || (i_ret == 0))
#else
    if ((i_ret < 0 && errno != EAGAIN) || i_ret == 0)
#endif
    {
        msg_Err(p_stream, "The connection to the Chromecast died (receiving).");
        vlc_mutex_locker locker(&lock);
        setConnectionStatus(CHROMECAST_CONNECTION_DEAD);
        vlc_restorecancel(canc);
        return;
    }

    if (b_pingTimeout)
    {
        msgPing();
        msgReceiverGetStatus();
    }

    if (b_msgReceived)
    {
        castchannel::CastMessage msg;
        msg.ParseFromArray(p_packet + PACKET_HEADER_LEN, i_payloadSize);
        processMessage(msg);
    }

    vlc_restorecancel(canc);
}
