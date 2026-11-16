/* Funciones puras del driver: NMEA, UBX, geodesia y tiempo. Sin hardware. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "neo6m.h"

#define MAX_FIELDS 24

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool neo6m_nmea_checksum_ok(const char *line)
{
    if (!line || line[0] != '$') return false;
    uint8_t x = 0;
    const char *p = line + 1;
    while (*p && *p != '*') x ^= (uint8_t)*p++;
    if (*p != '*') return false;
    int hi = hexval(p[1]), lo = hexval(p[2]);
    return hi >= 0 && lo >= 0 && x == (uint8_t)((hi << 4) | lo);
}

/* Parte la frase en campos (los campos vacíos quedan como ""). */
static int split(char *s, char *f[], int max)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == ',') {
            *p = '\0';
            f[n++] = p + 1;
        } else if (*p == '*') {
            *p = '\0';
            break;
        }
    }
    return n;
}

/* "ddmm.mmmm" + 'N'/'S' -> grados decimales. */
static bool coord(const char *v, const char *hemi, double *out)
{
    if (!v[0] || !hemi[0]) return false;
    double raw = strtod(v, NULL);
    int deg = (int)(raw / 100.0);
    double r = deg + (raw - deg * 100.0) / 60.0;
    *out = (hemi[0] == 'S' || hemi[0] == 'W') ? -r : r;
    return true;
}

static void parse_time(neo6m_data_t *d, const char *t)
{
    if (strlen(t) < 6) {
        d->time_valid = false;
        return;
    }
    d->hour = (t[0] - '0') * 10 + (t[1] - '0');
    d->minute = (t[2] - '0') * 10 + (t[3] - '0');
    d->second = (t[4] - '0') * 10 + (t[5] - '0');
    d->millis = (t[6] == '.') ? (uint16_t)lroundf(strtof(t + 6, NULL) * 1000.0f) : 0;
    d->time_valid = true;
}

static float fnum(const char *s, float def)
{
    return s[0] ? strtof(s, NULL) : def;
}

static void parse_rmc(neo6m_data_t *d, char *f[], int n)
{
    if (n < 10) return;
    parse_time(d, f[1]);
    d->valid = f[2][0] == 'A';
    if (d->valid) {
        coord(f[3], f[4], &d->lat);
        coord(f[5], f[6], &d->lon);
    }
    d->speed_kmh = fnum(f[7], 0) * 1.852f;  // nudos -> km/h
    d->course_deg = fnum(f[8], 0);
    if (strlen(f[9]) == 6) {
        d->day = (f[9][0] - '0') * 10 + (f[9][1] - '0');
        d->month = (f[9][2] - '0') * 10 + (f[9][3] - '0');
        d->year = 2000 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
        d->date_valid = true;
    } else {
        d->date_valid = false;
    }
}

static void parse_gga(neo6m_data_t *d, char *f[], int n)
{
    if (n < 12) return;
    parse_time(d, f[1]);
    d->quality = (uint8_t)atoi(f[6]);
    if (d->quality) {
        coord(f[2], f[3], &d->lat);
        coord(f[4], f[5], &d->lon);
        d->alt_m = fnum(f[9], 0);
        d->geoid_sep_m = fnum(f[11], 0);
    }
    d->sats_used = (uint8_t)atoi(f[7]);
    d->hdop = fnum(f[8], 99.99f);
}

static void parse_gsa(neo6m_data_t *d, char *f[], int n)
{
    if (n < 18) return;
    d->fix_type = (uint8_t)atoi(f[2]);
    d->pdop = fnum(f[15], 99.99f);
    d->hdop = fnum(f[16], 99.99f);
    d->vdop = fnum(f[17], 99.99f);
}

static void parse_gsv(neo6m_data_t *d, char *f[], int n)
{
    if (n < 4) return;
    int msg = atoi(f[2]);
    if (msg == 1) d->n_sats = 0;  // primera frase del grupo: lista nueva
    d->sats_view = (uint8_t)atoi(f[3]);
    // Cada satélite ocupa 4 campos: PRN, elevación, azimut, SNR.
    for (int i = 4; i + 3 < n; i += 4) {
        if (!f[i][0] || d->n_sats >= NEO6M_MAX_SATS) continue;
        neo6m_sat_t *s = &d->sats[d->n_sats++];
        s->prn = (uint8_t)atoi(f[i]);
        s->elev = (int8_t)atoi(f[i + 1]);
        s->azim = (uint16_t)atoi(f[i + 2]);
        s->snr = (uint8_t)atoi(f[i + 3]);
    }
}

esp_err_t neo6m_nmea_parse(neo6m_data_t *d, const char *line)
{
    if (!d || !line) return ESP_ERR_INVALID_ARG;
    if (!neo6m_nmea_checksum_ok(line)) return ESP_ERR_INVALID_CRC;
    size_t len = strlen(line);
    if (len < 7 || len > 120) return ESP_ERR_INVALID_ARG;

    char buf[128];
    memcpy(buf, line, len + 1);
    char *f[MAX_FIELDS];
    int n = split(buf, f, MAX_FIELDS);
    const char *type = f[0] + 3;  // "$GPRMC" -> "RMC" (vale también "$GN...")

    if (!strcmp(type, "RMC")) parse_rmc(d, f, n);
    else if (!strcmp(type, "GGA")) parse_gga(d, f, n);
    else if (!strcmp(type, "GSA")) parse_gsa(d, f, n);
    else if (!strcmp(type, "GSV")) parse_gsv(d, f, n);
    else return ESP_ERR_NOT_SUPPORTED;  // VTG, GLL, TXT...: válidas pero no usadas
    return ESP_OK;
}

size_t neo6m_ubx_frame(uint8_t cls, uint8_t id, const void *payload, uint16_t len, uint8_t *out,
                       size_t out_size)
{
    size_t total = 8u + len;
    if (!out || out_size < total) return 0;
    out[0] = 0xB5;
    out[1] = 0x62;
    out[2] = cls;
    out[3] = id;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);
    if (len) memcpy(out + 6, payload, len);
    // Checksum Fletcher-8 sobre clase, id, longitud y payload.
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 6u + len; i++) {
        a += out[i];
        b += a;
    }
    out[6 + len] = a;
    out[7 + len] = b;
    return total;
}

#define RAD(x) ((x) * M_PI / 180.0)

double neo6m_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    // Haversine sobre una esfera de radio medio terrestre.
    double dlat = RAD(lat2 - lat1), dlon = RAD(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) + cos(RAD(lat1)) * cos(RAD(lat2)) * sin(dlon / 2) * sin(dlon / 2);
    return 2.0 * 6371008.8 * atan2(sqrt(a), sqrt(1.0 - a));
}

double neo6m_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double dlon = RAD(lon2 - lon1);
    double y = sin(dlon) * cos(RAD(lat2));
    double x = cos(RAD(lat1)) * sin(RAD(lat2)) - sin(RAD(lat1)) * cos(RAD(lat2)) * cos(dlon);
    double b = atan2(y, x) * 180.0 / M_PI;
    return b < 0 ? b + 360.0 : b;
}

time_t neo6m_unix_time(const neo6m_data_t *d)
{
    if (!d || !d->time_valid || !d->date_valid) return (time_t)-1;
    // Días desde 1970-01-01 (algoritmo "days from civil" de H. Hinnant).
    int y = d->year - (d->month <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int mp = (d->month + 9) % 12;
    int doy = (153 * mp + 2) / 5 + d->day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + d->hour * 3600 + d->minute * 60 + d->second);
}
