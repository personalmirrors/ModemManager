#include <arpa/inet.h>

#include <libqmi-glib.h>

#include "mm-log.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-cdma.h"
#include "mm-broadband-modem-qmi.h"
#include "mm-broadband-modem-qmi-private.h"
#include "mm-broadband-modem-qmi-location.h"

static MMIfaceModemLocation *iface_modem_location_parent = NULL;

/*****************************************************************************/
/* Location capabilities loading (Location interface) */

static MMModemLocationSource
location_load_capabilities_finish (MMIfaceModemLocation *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    GError *inner_error = NULL;
    gssize value;

    value = g_task_propagate_int (G_TASK (res), &inner_error);
    if (inner_error) {
        g_propagate_error (error, inner_error);
        return MM_MODEM_LOCATION_SOURCE_NONE;
    }
    return (MMModemLocationSource)value;
}

static void
parent_load_capabilities_ready (MMIfaceModemLocation *self,
                                GAsyncResult *res,
                                GTask *task)
{
    MMModemLocationSource sources;
    GError *error = NULL;
    MMPortQmi *port;

    sources = iface_modem_location_parent->load_capabilities_finish (self, res, &error);
    if (error) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    port = mm_base_modem_peek_port_qmi (MM_BASE_MODEM (self));

    /* Now our own checks */

    /* If we have support for the PDS client, GPS and A-GPS location is supported */
    if (port && mm_port_qmi_peek_client (port,
                                         QMI_SERVICE_PDS,
                                         MM_PORT_QMI_FLAG_DEFAULT))
        sources |= (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                    MM_MODEM_LOCATION_SOURCE_GPS_RAW |
                    MM_MODEM_LOCATION_SOURCE_AGPS);

    /* If the modem is CDMA, we have support for CDMA BS location */
    if (mm_iface_modem_is_cdma (MM_IFACE_MODEM (self)))
        sources |= MM_MODEM_LOCATION_SOURCE_CDMA_BS;

    /* So we're done, complete */
    g_task_return_int (task, sources);
    g_object_unref (task);
}

static void
location_load_capabilities (MMIfaceModemLocation *self,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's setup */
    iface_modem_location_parent->load_capabilities (
        self,
        (GAsyncReadyCallback)parent_load_capabilities_ready,
        task);
}

/*****************************************************************************/
/* Load SUPL server */

static gchar *
location_load_supl_server_finish (MMIfaceModemLocation *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
get_agps_config_ready (QmiClientPds *client,
                       GAsyncResult *res,
                       GTask *task)
{
    QmiMessagePdsGetAgpsConfigOutput *output = NULL;
    GError *error = NULL;
    guint32 ip;
    guint32 port;
    GArray *url;
    gchar *str;

    output = qmi_client_pds_get_agps_config_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_get_agps_config_output_get_result (output, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    str = NULL;

    /* Prefer IP/PORT to URL */
    if (qmi_message_pds_get_agps_config_output_get_location_server_address (
            output,
            &ip,
            &port,
            NULL) &&
        ip != 0 &&
        port != 0) {
        struct in_addr a = { .s_addr = ip };
        gchar buf[INET_ADDRSTRLEN + 1];

        memset (buf, 0, sizeof (buf));

        if (!inet_ntop (AF_INET, &a, buf, sizeof (buf) - 1)) {
            g_task_return_new_error (task,
                                     MM_CORE_ERROR,
                                     MM_CORE_ERROR_FAILED,
                                     "Cannot convert numeric IP address to string");
            g_object_unref (task);
            return;
        }

        str = g_strdup_printf ("%s:%u", buf, port);
    }

    if (!str &&
        qmi_message_pds_get_agps_config_output_get_location_server_url (
            output,
            &url,
            NULL) &&
        url->len > 0) {
        str = g_convert (url->data, url->len, "UTF-8", "UTF-16BE", NULL, NULL, NULL);
    }

    if (!str)
        str = g_strdup ("");

    qmi_message_pds_get_agps_config_output_unref (output);

    g_task_return_pointer (task, str, g_free);
    g_object_unref (task);
}

static void
location_load_supl_server (MMIfaceModemLocation *self,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    QmiClient *client = NULL;
    QmiMessagePdsGetAgpsConfigInput *input;

    if (!mm_broadband_modem_qmi_ensure_client (MM_BROADBAND_MODEM_QMI (self),
                                               QMI_SERVICE_PDS, &client,
                                               callback, user_data)) {
        return;
    }

    input = qmi_message_pds_get_agps_config_input_new ();

    /* For multimode devices, prefer UMTS by default */
    if (mm_iface_modem_is_3gpp (MM_IFACE_MODEM (self)))
        qmi_message_pds_get_agps_config_input_set_network_mode (input, QMI_PDS_NETWORK_MODE_UMTS, NULL);
    else if (mm_iface_modem_is_cdma (MM_IFACE_MODEM (self)))
        qmi_message_pds_get_agps_config_input_set_network_mode (input, QMI_PDS_NETWORK_MODE_CDMA, NULL);

    qmi_client_pds_get_agps_config (
        QMI_CLIENT_PDS (client),
        input,
        10,
        NULL, /* cancellable */
        (GAsyncReadyCallback)get_agps_config_ready,
        g_task_new (self, NULL, callback, user_data));
    qmi_message_pds_get_agps_config_input_unref (input);
}

/*****************************************************************************/
/* Set SUPL server */

static gboolean
location_set_supl_server_finish (MMIfaceModemLocation *self,
                                 GAsyncResult *res,
                                 GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
set_agps_config_ready (QmiClientPds *client,
                       GAsyncResult *res,
                       GTask *task)
{
    QmiMessagePdsSetAgpsConfigOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_agps_config_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_agps_config_output_get_result (output, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    qmi_message_pds_set_agps_config_output_unref (output);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static gboolean
parse_as_ip_port (const gchar *supl,
                  guint32 *out_ip,
                  guint32 *out_port)
{
    gboolean valid = FALSE;
    gchar **split;
    guint port;
    guint32 ip;

    split = g_strsplit (supl, ":", -1);
    if (g_strv_length (split) != 2)
        goto out;

    if (!mm_get_uint_from_str (split[1], &port))
        goto out;
    if (port == 0 || port > G_MAXUINT16)
        goto out;
    if (inet_pton (AF_INET, split[0], &ip) <= 0)
        goto out;

    *out_ip = ip;
    *out_port = port;
    valid = TRUE;

out:
    g_strfreev (split);
    return valid;
}

static gboolean
parse_as_url (const gchar *supl,
              GArray **out_url)
{
    gchar *utf16;
    gsize utf16_len;

    utf16 = g_convert (supl, -1, "UTF-16BE", "UTF-8", NULL, &utf16_len, NULL);
    *out_url = g_array_append_vals (g_array_sized_new (FALSE, FALSE, sizeof (guint8), utf16_len),
                                    utf16,
                                    utf16_len);
    g_free (utf16);
    return TRUE;
}

static void
location_set_supl_server (MMIfaceModemLocation *self,
                          const gchar *supl,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    QmiClient *client = NULL;
    QmiMessagePdsSetAgpsConfigInput *input;
    guint32 ip;
    guint32 port;
    GArray *url;

    if (!mm_broadband_modem_qmi_ensure_client (MM_BROADBAND_MODEM_QMI (self),
                                               QMI_SERVICE_PDS, &client,
                                               callback, user_data)) {
        return;
    }

    input = qmi_message_pds_set_agps_config_input_new ();

    /* For multimode devices, prefer UMTS by default */
    if (mm_iface_modem_is_3gpp (MM_IFACE_MODEM (self)))
        qmi_message_pds_set_agps_config_input_set_network_mode (input, QMI_PDS_NETWORK_MODE_UMTS, NULL);
    else if (mm_iface_modem_is_cdma (MM_IFACE_MODEM (self)))
        qmi_message_pds_set_agps_config_input_set_network_mode (input, QMI_PDS_NETWORK_MODE_CDMA, NULL);

    if (parse_as_ip_port (supl, &ip, &port))
        qmi_message_pds_set_agps_config_input_set_location_server_address (input, ip, port, NULL);
    else if (parse_as_url (supl, &url)) {
        qmi_message_pds_set_agps_config_input_set_location_server_url (input, url, NULL);
        g_array_unref (url);
    } else
        g_assert_not_reached ();

    qmi_client_pds_set_agps_config (
        QMI_CLIENT_PDS (client),
        input,
        10,
        NULL, /* cancellable */
        (GAsyncReadyCallback)set_agps_config_ready,
        g_task_new (self, NULL, callback, user_data));
    qmi_message_pds_set_agps_config_input_unref (input);
}

/*****************************************************************************/
/* Disable location gathering (Location interface) */

typedef struct {
    QmiClientPds *client;
    MMModemLocationSource source;
    /* Default tracking session (for A-GPS disabling) */
    QmiPdsOperatingMode session_operation;
    guint8 data_timeout;
    guint32 interval;
    guint32 accuracy_threshold;
} DisableLocationGatheringContext;

static void
disable_location_gathering_context_free (DisableLocationGatheringContext *ctx)
{
    if (ctx->client)
        g_object_unref (ctx->client);
    g_slice_free (DisableLocationGatheringContext, ctx);
}

static gboolean
disable_location_gathering_finish (MMIfaceModemLocation *self,
                                   GAsyncResult *res,
                                   GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
gps_service_state_stop_ready (QmiClientPds *client,
                              GAsyncResult *res,
                              GTask *task)
{
    MMBroadbandModemQmi *self;
    DisableLocationGatheringContext *ctx;
    QmiMessagePdsSetGpsServiceStateOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_gps_service_state_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_gps_service_state_output_get_result (output, &error)) {
        if (!g_error_matches (error,
                              QMI_PROTOCOL_ERROR,
                              QMI_PROTOCOL_ERROR_NO_EFFECT)) {
            g_prefix_error (&error, "Couldn't set GPS service state: ");
            g_task_return_error (task, error);
            g_object_unref (task);
            qmi_message_pds_set_gps_service_state_output_unref (output);
            return;
        }

        g_error_free (error);
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    qmi_message_pds_set_gps_service_state_output_unref (output);

    g_assert (self->priv->location_event_report_indication_id != 0);
    g_signal_handler_disconnect (client, self->priv->location_event_report_indication_id);
    self->priv->location_event_report_indication_id = 0;

    mm_dbg ("GPS stopped");
    self->priv->enabled_sources &= ~ctx->source;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_default_tracking_session_stop_ready (QmiClientPds *client,
                                         GAsyncResult *res,
                                         GTask *task)
{
    MMBroadbandModemQmi *self;
    DisableLocationGatheringContext *ctx;
    QmiMessagePdsSetDefaultTrackingSessionOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_default_tracking_session_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_default_tracking_session_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't set default tracking session: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        qmi_message_pds_set_default_tracking_session_output_unref (output);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    qmi_message_pds_set_default_tracking_session_output_unref (output);

    /* Done */
    mm_dbg ("A-GPS disabled");
    self->priv->enabled_sources &= ~ctx->source;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
get_default_tracking_session_stop_ready (QmiClientPds *client,
                                         GAsyncResult *res,
                                         GTask *task)
{
    MMBroadbandModemQmi *self;
    DisableLocationGatheringContext *ctx;
    QmiMessagePdsSetDefaultTrackingSessionInput *input;
    QmiMessagePdsGetDefaultTrackingSessionOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_get_default_tracking_session_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_get_default_tracking_session_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get default tracking session: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        qmi_message_pds_get_default_tracking_session_output_unref (output);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    qmi_message_pds_get_default_tracking_session_output_get_info (
        output,
        &ctx->session_operation,
        &ctx->data_timeout,
        &ctx->interval,
        &ctx->accuracy_threshold,
        NULL);

    qmi_message_pds_get_default_tracking_session_output_unref (output);

    if (ctx->session_operation == QMI_PDS_OPERATING_MODE_STANDALONE) {
        /* Done */
        mm_dbg ("A-GPS already disabled");
        self->priv->enabled_sources &= ~ctx->source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    input = qmi_message_pds_set_default_tracking_session_input_new ();
    qmi_message_pds_set_default_tracking_session_input_set_info (
        input,
        QMI_PDS_OPERATING_MODE_STANDALONE,
        ctx->data_timeout,
        ctx->interval,
        ctx->accuracy_threshold,
        NULL);
    qmi_client_pds_set_default_tracking_session (
        ctx->client,
        input,
        10,
        NULL, /* cancellable */
        (GAsyncReadyCallback)set_default_tracking_session_stop_ready,
        task);
    qmi_message_pds_set_default_tracking_session_input_unref (input);
}

static void
disable_location_gathering (MMIfaceModemLocation *_self,
                            MMModemLocationSource source,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);
    DisableLocationGatheringContext *ctx;
    GTask *task;
    QmiClient *client = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    /* Nothing to be done to disable 3GPP or CDMA locations */
    if (source == MM_MODEM_LOCATION_SOURCE_3GPP_LAC_CI ||
        source == MM_MODEM_LOCATION_SOURCE_CDMA_BS) {
        /* Just mark it as disabled */
        self->priv->enabled_sources &= ~source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* Setup context and client */
    if (!mm_broadband_modem_qmi_ensure_client (self,
                                               QMI_SERVICE_PDS, &client,
                                               callback, user_data)) {
        g_object_unref (task);
        return;
    }
    ctx = g_slice_new0 (DisableLocationGatheringContext);
    ctx->client = g_object_ref (client);
    ctx->source = source;

    g_task_set_task_data (task, ctx, (GDestroyNotify)disable_location_gathering_context_free);

    /* Disable A-GPS? */
    if (source == MM_MODEM_LOCATION_SOURCE_AGPS) {
        qmi_client_pds_get_default_tracking_session (
            ctx->client,
            NULL,
            10,
            NULL, /* cancellable */
            (GAsyncReadyCallback)get_default_tracking_session_stop_ready,
            task);
        return;
    }

    /* Only stop GPS engine if no GPS-related sources enabled */
    if (source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        MMModemLocationSource tmp;

        /* If no more GPS sources enabled, stop GPS */
        tmp = self->priv->enabled_sources;
        tmp &= ~source;
        if (!(tmp & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW))) {
            QmiMessagePdsSetGpsServiceStateInput *input;

            input = qmi_message_pds_set_gps_service_state_input_new ();
            qmi_message_pds_set_gps_service_state_input_set_state (input, FALSE, NULL);
            qmi_client_pds_set_gps_service_state (
                ctx->client,
                input,
                10,
                NULL, /* cancellable */
                (GAsyncReadyCallback)gps_service_state_stop_ready,
                task);
            qmi_message_pds_set_gps_service_state_input_unref (input);
            return;
        }

        /* Otherwise, we have more GPS sources enabled, we shouldn't stop GPS, just
         * return */
        self->priv->enabled_sources &= ~source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* The QMI implementation has a fixed set of capabilities supported. Arriving
     * here means we tried to disable one which wasn't set as supported, which should
     * not happen */
    g_assert_not_reached ();
}

/*****************************************************************************/
/* Enable location gathering (Location interface) */

static void
location_event_report_indication_cb (QmiClientPds *client,
                                     QmiIndicationPdsEventReportOutput *output,
                                     MMBroadbandModemQmi *self)
{
    QmiPdsPositionSessionStatus session_status;
    const gchar *nmea;

    if (qmi_indication_pds_event_report_output_get_position_session_status (
            output,
            &session_status,
            NULL)) {
        mm_dbg ("[GPS] session status changed: '%s'",
                qmi_pds_position_session_status_get_string (session_status));
    }

    if (qmi_indication_pds_event_report_output_get_nmea_position (
            output,
            &nmea,
            NULL)) {
        mm_dbg ("[NMEA] %s", nmea);
        mm_iface_modem_location_gps_update (MM_IFACE_MODEM_LOCATION (self), nmea);
    }
}

typedef struct {
    QmiClientPds *client;
    MMModemLocationSource source;
    /* Default tracking session (for A-GPS enabling) */
    QmiPdsOperatingMode session_operation;
    guint8 data_timeout;
    guint32 interval;
    guint32 accuracy_threshold;
} EnableLocationGatheringContext;

static void
enable_location_gathering_context_free (EnableLocationGatheringContext *ctx)
{
    if (ctx->client)
        g_object_unref (ctx->client);
    g_slice_free (EnableLocationGatheringContext, ctx);
}

static gboolean
enable_location_gathering_finish (MMIfaceModemLocation *self,
                                  GAsyncResult *res,
                                  GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
ser_location_ready (QmiClientPds *client,
                    GAsyncResult *res,
                    GTask *task)
{
    MMBroadbandModemQmi *self;
    EnableLocationGatheringContext *ctx;
    QmiMessagePdsSetEventReportOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_event_report_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_event_report_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't set event report: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        qmi_message_pds_set_event_report_output_unref (output);
        return;
    }

    qmi_message_pds_set_event_report_output_unref (output);

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    mm_dbg ("Adding location event report indication handling");
    g_assert (self->priv->location_event_report_indication_id == 0);
    self->priv->location_event_report_indication_id =
        g_signal_connect (client,
                          "event-report",
                          G_CALLBACK (location_event_report_indication_cb),
                          self);

    /* Done */
    mm_dbg ("GPS started");
    self->priv->enabled_sources |= ctx->source;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
auto_tracking_state_start_ready (QmiClientPds *client,
                                 GAsyncResult *res,
                                 GTask *task)
{
    EnableLocationGatheringContext *ctx;
    QmiMessagePdsSetEventReportInput *input;
    QmiMessagePdsSetAutoTrackingStateOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_auto_tracking_state_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_auto_tracking_state_output_get_result (output, &error)) {
        if (!g_error_matches (error,
                              QMI_PROTOCOL_ERROR,
                              QMI_PROTOCOL_ERROR_NO_EFFECT)) {
            g_prefix_error (&error, "Couldn't set auto-tracking state: ");
            g_task_return_error (task, error);
            g_object_unref (task);
            qmi_message_pds_set_auto_tracking_state_output_unref (output);
            return;
        }
        g_error_free (error);
    }

    qmi_message_pds_set_auto_tracking_state_output_unref (output);

    ctx = g_task_get_task_data (task);

    /* Only gather standard NMEA traces */
    input = qmi_message_pds_set_event_report_input_new ();
    qmi_message_pds_set_event_report_input_set_nmea_position_reporting (input, TRUE, NULL);
    qmi_client_pds_set_event_report (
        ctx->client,
        input,
        5,
        NULL,
        (GAsyncReadyCallback)ser_location_ready,
        task);
    qmi_message_pds_set_event_report_input_unref (input);
}

static void
gps_service_state_start_ready (QmiClientPds *client,
                               GAsyncResult *res,
                               GTask *task)
{
    EnableLocationGatheringContext *ctx;
    QmiMessagePdsSetAutoTrackingStateInput *input;
    QmiMessagePdsSetGpsServiceStateOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_gps_service_state_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_gps_service_state_output_get_result (output, &error)) {
        if (!g_error_matches (error,
                              QMI_PROTOCOL_ERROR,
                              QMI_PROTOCOL_ERROR_NO_EFFECT)) {
            g_prefix_error (&error, "Couldn't set GPS service state: ");
            g_task_return_error (task, error);
            g_object_unref (task);
            qmi_message_pds_set_gps_service_state_output_unref (output);
            return;
        }
        g_error_free (error);
    }

    qmi_message_pds_set_gps_service_state_output_unref (output);

    ctx = g_task_get_task_data (task);

    /* Enable auto-tracking for a continuous fix */
    input = qmi_message_pds_set_auto_tracking_state_input_new ();
    qmi_message_pds_set_auto_tracking_state_input_set_state (input, TRUE, NULL);
    qmi_client_pds_set_auto_tracking_state (
        ctx->client,
        input,
        10,
        NULL, /* cancellable */
        (GAsyncReadyCallback)auto_tracking_state_start_ready,
        task);
    qmi_message_pds_set_auto_tracking_state_input_unref (input);
}

static void
set_default_tracking_session_start_ready (QmiClientPds *client,
                                          GAsyncResult *res,
                                          GTask *task)
{
    MMBroadbandModemQmi *self;
    EnableLocationGatheringContext *ctx;
    QmiMessagePdsSetDefaultTrackingSessionOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_set_default_tracking_session_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_set_default_tracking_session_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't set default tracking session: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        qmi_message_pds_set_default_tracking_session_output_unref (output);
        return;
    }

    qmi_message_pds_set_default_tracking_session_output_unref (output);

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    /* Done */
    mm_dbg ("A-GPS enabled");
    self->priv->enabled_sources |= ctx->source;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
get_default_tracking_session_start_ready (QmiClientPds *client,
                                          GAsyncResult *res,
                                          GTask *task)
{
    MMBroadbandModemQmi *self;
    EnableLocationGatheringContext *ctx;
    QmiMessagePdsSetDefaultTrackingSessionInput *input;
    QmiMessagePdsGetDefaultTrackingSessionOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_pds_get_default_tracking_session_finish (client, res, &error);
    if (!output) {
        g_prefix_error (&error, "QMI operation failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!qmi_message_pds_get_default_tracking_session_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't get default tracking session: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        qmi_message_pds_get_default_tracking_session_output_unref (output);
        return;
    }

    self = g_task_get_source_object (task);
    ctx = g_task_get_task_data (task);

    qmi_message_pds_get_default_tracking_session_output_get_info (
        output,
        &ctx->session_operation,
        &ctx->data_timeout,
        &ctx->interval,
        &ctx->accuracy_threshold,
        NULL);

    qmi_message_pds_get_default_tracking_session_output_unref (output);

    if (ctx->session_operation == QMI_PDS_OPERATING_MODE_MS_ASSISTED) {
        /* Done */
        mm_dbg ("A-GPS already enabled");
        self->priv->enabled_sources |= ctx->source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    input = qmi_message_pds_set_default_tracking_session_input_new ();
    qmi_message_pds_set_default_tracking_session_input_set_info (
        input,
        QMI_PDS_OPERATING_MODE_MS_ASSISTED,
        ctx->data_timeout,
        ctx->interval,
        ctx->accuracy_threshold,
        NULL);
    qmi_client_pds_set_default_tracking_session (
        ctx->client,
        input,
        10,
        NULL, /* cancellable */
        (GAsyncReadyCallback)set_default_tracking_session_start_ready,
        task);
    qmi_message_pds_set_default_tracking_session_input_unref (input);
}

static void
parent_enable_location_gathering_ready (MMIfaceModemLocation *_self,
                                        GAsyncResult *res,
                                        GTask *task)
{
    MMBroadbandModemQmi *self = MM_BROADBAND_MODEM_QMI (_self);
    EnableLocationGatheringContext *ctx;
    GError *error = NULL;
    QmiClient *client;

    if (!iface_modem_location_parent->enable_location_gathering_finish (_self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx = g_task_get_task_data (task);

    /* Nothing else needed in the QMI side for LAC/CI */
    if (ctx->source == MM_MODEM_LOCATION_SOURCE_3GPP_LAC_CI) {
        self->priv->enabled_sources |= ctx->source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* CDMA modems need to re-run registration checks when enabling the CDMA BS
     * location source, so that we get up to date BS location information.
     * Note that we don't care for when the registration checks get finished.
     */
    if (ctx->source == MM_MODEM_LOCATION_SOURCE_CDMA_BS &&
        mm_iface_modem_is_cdma (MM_IFACE_MODEM (self))) {
        /* Reload registration to get LAC/CI */
        mm_iface_modem_cdma_run_registration_checks (MM_IFACE_MODEM_CDMA (self), NULL, NULL);
        /* Just mark it as enabled */
        self->priv->enabled_sources |= ctx->source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* Setup context and client */
    client = mm_broadband_modem_qmi_peek_client (self, QMI_SERVICE_PDS, &error);
    if (!client) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx->client = g_object_ref (client);

    /* Enabling A-GPS? */
    if (ctx->source == MM_MODEM_LOCATION_SOURCE_AGPS) {
        qmi_client_pds_get_default_tracking_session (
            ctx->client,
            NULL,
            10,
            NULL, /* cancellable */
            (GAsyncReadyCallback)get_default_tracking_session_start_ready,
            task);
        return;
    }

    /* NMEA and RAW are both enabled in the same way */
    if (ctx->source & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA | MM_MODEM_LOCATION_SOURCE_GPS_RAW)) {
        /* Only start GPS engine if not done already */
        if (!(self->priv->enabled_sources & (MM_MODEM_LOCATION_SOURCE_GPS_NMEA |
                                             MM_MODEM_LOCATION_SOURCE_GPS_RAW))) {
            QmiMessagePdsSetGpsServiceStateInput *input;

            input = qmi_message_pds_set_gps_service_state_input_new ();
            qmi_message_pds_set_gps_service_state_input_set_state (input, TRUE, NULL);
            qmi_client_pds_set_gps_service_state (
                ctx->client,
                input,
                10,
                NULL, /* cancellable */
                (GAsyncReadyCallback)gps_service_state_start_ready,
                task);
            qmi_message_pds_set_gps_service_state_input_unref (input);
            return;
        }

        /* GPS already started, we're done */
        self->priv->enabled_sources |= ctx->source;
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    /* The QMI implementation has a fixed set of capabilities supported. Arriving
     * here means we tried to enable one which wasn't set as supported, which should
     * not happen */
    g_assert_not_reached ();
}

static void
enable_location_gathering (MMIfaceModemLocation *self,
                           MMModemLocationSource source,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
    EnableLocationGatheringContext *ctx;
    GTask *task;

    ctx = g_slice_new0 (EnableLocationGatheringContext);
    /* Store source to enable, there will be only one! */
    ctx->source = source;

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify)enable_location_gathering_context_free);

    /* Chain up parent's gathering enable */
    iface_modem_location_parent->enable_location_gathering (
        self,
        ctx->source,
        (GAsyncReadyCallback)parent_enable_location_gathering_ready,
        task);
}

static MMIfaceModemLocation location_interface = {
    .load_capabilities = location_load_capabilities,
    .load_capabilities_finish = location_load_capabilities_finish,
    .load_supl_server = location_load_supl_server,
    .load_supl_server_finish = location_load_supl_server_finish,
    .set_supl_server = location_set_supl_server,
    .set_supl_server_finish = location_set_supl_server_finish,
    .enable_location_gathering = enable_location_gathering,
    .enable_location_gathering_finish = enable_location_gathering_finish,
    .disable_location_gathering = disable_location_gathering,
    .disable_location_gathering_finish = disable_location_gathering_finish,
};

MMIfaceModemLocation *mm_broadband_modem_qmi_location_pds(MMIfaceModemLocation *parent) {
    iface_modem_location_parent = parent;
    return &location_interface;
}
