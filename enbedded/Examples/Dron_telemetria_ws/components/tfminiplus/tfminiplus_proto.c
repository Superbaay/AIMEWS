/* Protocolo del TF Mini Plus: funciones puras, sin UART ni FreeRTOS. */
#include <string.h>
#include "tfminiplus.h"

#define HDR_DATA 0x59
#define HDR_CMD 0x5A

uint8_t tfminiplus_checksum(const uint8_t *buf, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) s += buf[i];  // el desbordamiento de uint8_t ya es "8 bits bajos"
    return s;
}

size_t tfminiplus_cmd_frame(uint8_t id, const void *payload, uint8_t len, uint8_t *out, size_t out_size)
{
    size_t total = 4u + len;  // 5A + longitud + ID + datos + checksum
    if (total > TFMINIPLUS_MAX_CMD || total > out_size || (len && !payload)) return 0;
    out[0] = HDR_CMD;
    out[1] = (uint8_t)total;
    out[2] = id;
    if (len) memcpy(&out[3], payload, len);
    out[total - 1] = tfminiplus_checksum(out, total - 1);
    return total;
}

esp_err_t tfminiplus_decode(const uint8_t f[TFMINIPLUS_FRAME_LEN], bool unit_mm, uint16_t min_strength,
                            tfminiplus_data_t *out)
{
    if (!f || !out || f[0] != HDR_DATA || f[1] != HDR_DATA) return ESP_ERR_INVALID_ARG;
    if (tfminiplus_checksum(f, 8) != f[8]) return ESP_ERR_INVALID_CRC;

    uint16_t dist = (uint16_t)(f[2] | (f[3] << 8));
    uint16_t strength = (uint16_t)(f[4] | (f[5] << 8));
    uint16_t temp = (uint16_t)(f[6] | (f[7] << 8));

    out->raw_dist = dist;
    out->strength = strength;
    out->temp_c = temp / 8.0f - 256.0f;  // fórmula del datasheet

    // Con señal débil (<100) o saturada (65535) el sensor avisa de que la
    // distancia no es fiable; 0 y 0xFFFF son los valores que usa para "sin medida".
    bool ok = strength >= min_strength && strength != 0xFFFF && dist != 0 && dist != 0xFFFF;
    uint32_t mm = unit_mm ? dist : dist * 10u;
    if (mm > 12000u + 500u) ok = false;  // fuera del rango de 12 m (con margen)

    out->valid = ok;
    out->dist_mm = ok ? (uint16_t)mm : 0;
    out->dist_cm = ok ? (uint16_t)((mm + 5u) / 10u) : 0;
    return ESP_OK;
}

void tfminiplus_parser_reset(tfminiplus_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

static void drop(tfminiplus_parser_t *p, size_t k)
{
    memmove(p->buf, p->buf + k, p->n - k);
    p->n -= k;
}

void tfminiplus_parser_feed(tfminiplus_parser_t *p, const uint8_t *data, size_t len, tfminiplus_frame_cb_t cb,
                            void *arg)
{
    for (size_t i = 0; i < len; i++) {
        if (p->n == sizeof(p->buf)) {  // no debería pasar: ninguna trama es tan larga
            drop(p, 1);
            p->junk++;
        }
        p->buf[p->n++] = data[i];

        // Extraer todas las tramas completas que haya en el búfer. Ante un
        // checksum malo se descarta SOLO el primer byte y se vuelve a buscar:
        // la cabecera verdadera puede estar dentro de lo que ya llegó.
        while (p->n > 0) {
            size_t need;
            if (p->buf[0] == HDR_DATA) {
                if (p->n < 2) break;
                if (p->buf[1] != HDR_DATA) {
                    drop(p, 1);
                    p->junk++;
                    continue;
                }
                need = TFMINIPLUS_FRAME_LEN;
            } else if (p->buf[0] == HDR_CMD) {
                if (p->n < 2) break;
                need = p->buf[1];
                if (need < 4 || need > TFMINIPLUS_MAX_CMD) {
                    drop(p, 1);
                    p->junk++;
                    continue;
                }
            } else {
                drop(p, 1);
                p->junk++;
                continue;
            }
            if (p->n < need) break;  // falta por llegar
            if (tfminiplus_checksum(p->buf, need - 1) == p->buf[need - 1]) {
                if (cb) cb(p->buf, need, arg);
                drop(p, need);
            } else {
                p->bad++;
                drop(p, 1);
            }
        }
    }
}
