/*
 * Copyright © 2014 Kosma Moczek <kosma@cloudyourcar.com>
 * This program is free software. It comes without any warranty, to the extent
 * permitted by applicable law. You can redistribute it and/or modify it under
 * the terms of the Do What The Fuck You Want To Public License, Version 2, as
 * published by Sam Hocevar. See the COPYING file for more details.
 */

#include <attentive/modem/common.h>
#include <attentive/cellular.h>
#include <attentive/at-timegm.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>


#define TELIT2_WAITACK_TIMEOUT 60
#define TELIT2_FTP_TIMEOUT 60
#define TELIT2_LOCATE_TIMEOUT 150

static const char *const telit2_urc_responses[] = {
    "SRING: ",
    "#AGPSRING: ",
    NULL
};

struct cellular_telit2 {
    struct cellular dev;

    int locate_status;
    float latitude, longitude, altitude;
};

static enum at_response_type scan_line(const char *line, size_t len, void *arg)
{
    (void) line;
    (void) len;

    struct cellular_telit2 *priv = arg;
    (void) priv;

    if (at_prefix_in_table(line, telit2_urc_responses))
        return AT_RESPONSE_URC;

    return AT_RESPONSE_UNKNOWN;
}

static void handle_urc(const char *line, size_t len, void *arg)
{
    struct cellular_telit2 *priv = arg;

    int status;
    if (sscanf(line, "#AGPSRING: %d", &status) == 1) {
        priv->locate_status = status;
        sscanf(line, "#AGPSRING: %*d,%f,%f,%f", &priv->latitude, &priv->longitude, &priv->altitude);
        return;
    }

    printf("[telit2@%p] urc: %.*s\n", priv, (int) len, line);
}

static const struct at_callbacks telit2_callbacks = {
    .scan_line = scan_line,
    .handle_urc = handle_urc,
};

static int telit2_attach(struct cellular *modem)
{
    at_set_callbacks(modem->at, &telit2_callbacks, (void *) modem);

    at_set_timeout(modem->at, 1);
    at_command(modem->at, "AT");        /* Aid autobauding. Always a good idea. */
    at_command(modem->at, "ATE0");      /* Disable local echo. */

    /* Initialize modem. */
    static const char *const init_strings[] = {
        "AT&K0",                        /* Disable hardware flow control. */
        "AT#SELINT=2",                  /* Set Telit module compatibility level. */
        "AT+CMEE=2",                    /* Enable extended error reporting. */
        NULL
    };
    for (const char *const *command=init_strings; *command; command++)
        at_command_simple(modem->at, "%s", *command);

    return 0;
}

static int telit2_detach(struct cellular *modem)
{
    at_set_callbacks(modem->at, NULL, NULL);
    return 0;
}

static int telit2_pdp_open(struct cellular *modem, const char *apn)
{
    at_set_timeout(modem->at, 5);
    at_command_simple(modem->at, "AT+CGDCONT=1,IP,\"%s\"", apn);

    at_set_timeout(modem->at, 150);
    const char *response = at_command(modem->at, "AT#SGACT=1,1");

    if (response == NULL)
        return -1;

    if (!strcmp(response, "+CME ERROR: context already activated"))
        return 0;

    int ip[4];
    at_simple_scanf(response, "#SGACT: %d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);

    return 0;
}

static int telit2_pdp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, 150);
    at_command_simple(modem->at, "AT#SGACT=1,0");

    return 0;
}

static int telit2_op_iccid(struct cellular *modem, char *buf, size_t len)
{
    char fmt[24];
    if (snprintf(fmt, sizeof(fmt), "#CCID: %%[0-9]%ds", (int) len) >= (int) sizeof(fmt)) {
        errno = ENOSPC;
        return -1;
    }

    at_set_timeout(modem->at, 5);
    const char *response = at_command(modem->at, "AT#CCID");
    at_simple_scanf(response, fmt, buf);
    buf[len-1] = '\0';

    return 0;
}

static int telit2_op_clock_gettime(struct cellular *modem, struct timespec *ts)
{
    struct tm tm;
    int offset;

    at_set_timeout(modem->at, 1);
    const char *response = at_command(modem->at, "AT+CCLK?");
    memset(&tm, 0, sizeof(struct tm));
    at_simple_scanf(response, "+CCLK: \"%d/%d/%d,%d:%d:%d%d\"",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec,
            &offset);

    /* Most modems report some starting date way in the past when they have
     * no date/time estimation. */
    if (tm.tm_year < 14) {
        errno = EINVAL;
        return 1;
    }

    /* Adjust values and perform conversion. */
    tm.tm_year += 2000 - 1900;
    tm.tm_mon -= 1;
    time_t unix_time = at_timegm(&tm);
    if (unix_time == -1) {
        errno = EINVAL;
        return -1;
    }

    /* Telit modems return local date/time instead of UTC (as defined in 3GPP
     * 27.007). Remove the timezone shift. */
    unix_time -= 15*60*offset;

    /* All good. Return the result. */
    ts->tv_sec = unix_time;
    ts->tv_nsec = 0;
    return 0;
}

static int telit2_socket_connect(struct cellular *modem, int connid, const char *host, uint16_t port)
{
    /* Reset socket configuration to default. */
    at_set_timeout(modem->at, 5);
    at_command_simple(modem->at, "AT#SCFGEXT=%d,0,0,0,0,0", connid);
    at_command_simple(modem->at, "AT#SCFGEXT2=%d,0,0,0,0,0", connid);

    /* Open connection. */
    cellular_command_simple_pdp(modem, "AT#SD=%d,0,%d,%s,0,0,1", connid, port, host);

    return 0;
}

static ssize_t telit2_socket_send(struct cellular *modem, int connid, const void *buffer, size_t amount, int flags)
{
    (void) flags;

    /* Request transmission. */
    at_set_timeout(modem->at, 150);
    at_expect_dataprompt(modem->at);
    at_command_simple(modem->at, "AT#SSENDEXT=%d,%zu", connid, amount);

    /* Send raw data. */
    at_command_raw_simple(modem->at, buffer, amount);

    return amount;
}

static enum at_response_type scanner_srecv(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int chunk;
    if (sscanf(line, "#SRECV: %*d,%d", &chunk) == 1)
        return AT_RESPONSE_RAWDATA_FOLLOWS(chunk);

    return AT_RESPONSE_UNKNOWN;
}

static ssize_t telit2_socket_recv(struct cellular *modem, int connid, void *buffer, size_t length, int flags)
{
    (void) flags;

    int cnt = 0;
    while (cnt < (int) length) {
        int chunk = (int) length - cnt;
        /* Limit read size to avoid overflowing AT response buffer. */
        if (chunk > 128)
            chunk = 128;

        /* Perform the read. */
        at_set_timeout(modem->at, 150);
        at_set_command_scanner(modem->at, scanner_srecv);
        const char *response = at_command(modem->at, "AT#SRECV=%d,%d", connid, chunk);
        if (response == NULL)
            return -1;

        /* Find the header line. */
        int bytes;
        at_simple_scanf(response, "#SRECV: %*d,%d", &bytes);

        /* Bail out if we're out of data. Message is misleading. */
        /* FIXME: We should maybe block until we receive something? */
        if (!strcmp(response, "+CME ERROR: activation failed"))
            break;

        /* Locate the payload. */
        const char *data = strchr(response, '\n');
        if (data == NULL) {
            errno = EPROTO;
            return -1;
        }
        data += 1;

        /* Copy payload to result buffer. */
        memcpy((char *)buffer + cnt, data, bytes);
        cnt += bytes;
    }

    return cnt;
}

static int telit2_socket_waitack(struct cellular *modem, int connid)
{
    const char *response;

    at_set_timeout(modem->at, 5);
    for (int i=0; i<TELIT2_WAITACK_TIMEOUT; i++) {
        /* Read number of bytes waiting. */
        int ack_waiting;
        response = at_command(modem->at, "AT#SI=%d", connid);
        at_simple_scanf(response, "#SI: %*d,%*d,%*d,%*d,%d", &ack_waiting);

        /* ack_waiting is meaningless if socket is not connected. Check this. */
        int socket_status;
        response = at_command(modem->at, "AT#SS=%d", connid);
        at_simple_scanf(response, "#SS: %*d,%d", &socket_status);
        if (socket_status == 0) {
            errno = ECONNRESET;
            return -1;
        }

        /* Return if all bytes were acknowledged. */
        if (ack_waiting == 0)
            return 0;

        sleep(1);
    }

    errno = ETIMEDOUT;
    return -1;
}

static int telit2_socket_close(struct cellular *modem, int connid)
{
    at_set_timeout(modem->at, 150);
    at_command_simple(modem->at, "AT#SH=%d", connid);

    return 0;
}

static int telit2_ftp_open(struct cellular *modem, const char *host, uint16_t port, const char *username, const char *password, bool passive)
{
    cellular_command_simple_pdp(modem, "AT#FTPOPEN=%s:%d,%s,%s,%d", host, port, username, password, passive);

    return 0;
}

static int telit2_ftp_get(struct cellular *modem, const char *filename)
{
    at_set_timeout(modem->at, 90);
    at_command_simple(modem->at, "AT#FTPGETPKT=\"%s\",0", filename);

    return 0;
}

static enum at_response_type scanner_ftprecv(const char *line, size_t len, void *arg)
{
    (void) len;
    (void) arg;

    int bytes;
    if (sscanf(line, "#FTPRECV: %d", &bytes) == 1)
        return AT_RESPONSE_RAWDATA_FOLLOWS(bytes);
    return AT_RESPONSE_UNKNOWN;
}

static int telit2_ftp_getdata(struct cellular *modem, char *buffer, size_t length)
{
    /* FIXME: This function's flow is really ugly. */
    int retries = 0;
retry:
    at_set_timeout(modem->at, 150);
    at_set_command_scanner(modem->at, scanner_ftprecv);
    const char *response = at_command(modem->at, "AT#FTPRECV=%zu", length);

    if (response == NULL)
        return -1;

    int bytes;
    if (sscanf(response, "#FTPRECV: %d", &bytes) == 1) {
        /* Zero means no data is available. Wait for it. */
        if (bytes == 0) {
            /* Bail out on timeout. */
            if (++retries >= TELIT2_FTP_TIMEOUT) {
                errno = ETIMEDOUT;
                return -1;
            }
            sleep(1);
            goto retry;
        }

        /* Locate the payload. */
        const char *data = strchr(response, '\n');
        if (data == NULL) {
            errno = EPROTO;
            return -1;
        }
        data += 1;

        /* Copy payload to result buffer. */
        memcpy(buffer, data, bytes);
        return bytes;
    }

    /* Error or EOF? */
    int eof;
    response = at_command(modem->at, "AT#FTPGETPKT?");
    /* Expected response: #FTPGETPKT: <remotefile>,<viewMode>,<eof> */
#if 0
    /* The %[] specifier is not supported on some embedded systems. */
    at_simple_scanf(response, "#FTPGETPKT: %*[^,],%*d,%d", &eof);
#else
    /* Parse manually. */
    if (response == NULL)
        return -1;
    errno = EPROTO;
    /* Check the initial part of the response. */
    if (strncmp(response, "#FTPGETPKT: ", 12))
        return -1;
    /* Skip the filename. */
    response = strchr(response, ',');
    if (response == NULL)
        return -1;
    response++;
    at_simple_scanf(response, "%*d,%d", &eof);
#endif

    if (eof == 1)
        return 0;

    return -1;
}

static int telit2_locate(struct cellular *modem, float *latitude, float *longitude, float *altitude)
{
    struct cellular_telit2 *priv = (struct cellular_telit2 *) modem;

    priv->locate_status = -1;
    at_set_timeout(modem->at, 150);
    cellular_command_simple_pdp(modem, "AT#AGPSSND");

    for (int i=0; i<TELIT2_LOCATE_TIMEOUT; i++) {
        sleep(1);
        if (priv->locate_status == 200) {
            *latitude = priv->latitude;
            *longitude = priv->longitude;
            *altitude = priv->altitude;
            return 0;
        }
        if (priv->locate_status != -1) {
            errno = ECONNABORTED;
            return -1;
        }
    }

    errno = ETIMEDOUT;
    return -1;
}

static int telit2_ftp_close(struct cellular *modem)
{
    at_set_timeout(modem->at, 90);
    at_command_simple(modem->at, "AT#FTPCLOSE");

    return 0;
}

static const struct cellular_ops telit2_ops = {
    .attach = telit2_attach,
    .detach = telit2_detach,

    .pdp_open = telit2_pdp_open,
    .pdp_close = telit2_pdp_close,

    .imei = cellular_op_imei,
    .iccid = telit2_op_iccid,
    .creg = cellular_op_creg,
    .rssi = cellular_op_rssi,
    .clock_gettime = telit2_op_clock_gettime,
    .clock_settime = cellular_op_clock_settime,
    .socket_connect = telit2_socket_connect,
    .socket_send = telit2_socket_send,
    .socket_recv = telit2_socket_recv,
    .socket_waitack = telit2_socket_waitack,
    .socket_close = telit2_socket_close,
    .ftp_open = telit2_ftp_open,
    .ftp_get = telit2_ftp_get,
    .ftp_getdata = telit2_ftp_getdata,
    .ftp_close = telit2_ftp_close,
    .locate = telit2_locate,
};

struct cellular *cellular_telit2_alloc(void)
{
    struct cellular_telit2 *modem = malloc(sizeof(struct cellular_telit2));
    if (modem == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memset(modem, 0, sizeof(*modem));

    modem->dev.ops = &telit2_ops;

    return (struct cellular *) modem;
}

void cellular_telit2_free(struct cellular *modem)
{
    free(modem);
}

/* vim: set ts=4 sw=4 et: */
