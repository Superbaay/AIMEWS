/**
 * @file esp32s3cam_modes.c
 * @brief Modos afinados del OV3660: ventana + temporización para más fps que los de serie.
 *
 * Cómo se calcula (driver ov3660.c):  fps = SYSCLK / (HTS * VTS)
 *   - SYSCLK de serie con XCLK de 20 MHz: 50 MHz (PLL x30, /3, /4). Todos los modos
 *     usan ese PLL: en la AI-Thinker subirlo (59-74 MHz) daba JPEG con bandas.
 *   - De serie: todo lo que usa binning (hasta XGA) va a 2300 x 783 -> 27,8 fps;
 *     HD/FHD leen 1920x1080 y lo ESCALAN (2172 x 1322) -> 17,4 fps.
 *
 * Coordenadas del sensor (ratio_table del driver): matriz útil x=0..2079, y=0..1547.
 *   - Recorte 1:1: se lee salida + 2*offset (16, 6) -> (1280+32) x (720+12) para 720p.
 *   - Binning 2x2: se lee el doble de la salida + márgenes; offsets del ISP 8 y 2.
 *
 * Límites MEDIDOS en esta placa (tools/barrido.py, results/barrido.json):
 *   - Recorte 1:1: HTS puede bajar casi hasta el ancho leído (+40..90 px). El VTS
 *     necesita ~28 líneas de margen sobre las leídas: HD con 740 (732 leídas) da frames
 *     rotos y FHD con 1100 (1092) da colores verde/magenta. Con binning bastan ~9.
 *   - Binning: HTS >= ~1,3x el ancho leído. Por debajo el sensor se salta uno de cada
 *     dos frames (fps reales = la mitad) o rompe la imagen.
 *   - Menos HTS = línea más corta = exposición máxima más corta: con poca luz los
 *     modos rápidos salen más oscuros (el AEC no puede alargar más).
 *
 * Verificación de cada modo (tools/validar_modos.py -> results/modos.json):
 *   - 240 frames con el patrón de barras: ffmpeg + check_colorbar (transporte: ISP,
 *     JPEG, DVP, DMA).
 *   - Frames con escena real: check_chroma (lectura de la matriz: el patrón de barras
 *     se inyecta después y no ve una fase Bayer o un VTS mal puestos).
 *   - fps reales con la escena.
 */
#include "esp32s3cam_modes.h"

const esp32s3cam_mode_t esp32s3cam_mode_table[] = {
    {
        .name = "VGA60",
        .description = "640x480 a 60 fps, binning de la zona central 1280x960 (zoom 1,6x)",
        .frame_size = FRAMESIZE_VGA,
        .window = {
            .start_x = 384, .start_y = 288, .end_x = 1695, .end_y = 1259,
            .offset_x = 8, .offset_y = 2,
            .total_x = 1680, .total_y = 495,   // 1600 ya salta frames
            .output_x = 640, .output_y = 480,
            .scale = false, .binning = true,
        },
        .has_pll = false,
        .fps = 60,
    },
    {
        .name = "XGA30",
        .description = "1024x768 a 30 fps, campo completo con binning",
        .frame_size = FRAMESIZE_XGA,
        .window = {
            .start_x = 0, .start_y = 0, .end_x = 2079, .end_y = 1547,
            .offset_x = 8, .offset_y = 2,
            .total_x = 2100, .total_y = 783,   // de serie 2300; 1800 ya salta frames
            .output_x = 1024, .output_y = 768,
            .scale = false, .binning = true,
        },
        .has_pll = false,
        .fps = 30,
    },
    {
        .name = "HD30",
        .description = "1280x720 a 36 fps, recorte central 1:1 (campo 62 %), exposición larga",
        .frame_size = FRAMESIZE_HD,
        .window = {
            .start_x = 384, .start_y = 408, .end_x = 1695, .end_y = 1139,
            .offset_x = 16, .offset_y = 6,
            .total_x = 1800, .total_y = 760,
            .output_x = 1280, .output_y = 720,
            .scale = false, .binning = false,
        },
        .has_pll = false,
        .fps = 36,
    },
    {
        .name = "HD45",
        .description = "1280x720 a 47 fps, recorte central 1:1 (campo 62 %), necesita buena luz",
        .frame_size = FRAMESIZE_HD,
        .window = {
            .start_x = 384, .start_y = 408, .end_x = 1695, .end_y = 1139,
            .offset_x = 16, .offset_y = 6,
            .total_x = 1400, .total_y = 760,   // 1350 también pasa: se deja margen
            .output_x = 1280, .output_y = 720,
            .scale = false, .binning = false,
        },
        .has_pll = false,
        .fps = 47,
    },
    {
        .name = "FHD22",
        .description = "1920x1080 a 22 fps, recorte central 1:1 (campo 94 %)",
        .frame_size = FRAMESIZE_FHD,
        .window = {
            .start_x = 64, .start_y = 228, .end_x = 2015, .end_y = 1319,
            .offset_x = 16, .offset_y = 6,
            // VTS 1100 (8 líneas de margen) daba imagen VERDE/MAGENTA y duplicada con
            // escena real, pero el patrón de barras salía perfecto: el fallo está en la
            // lectura de la matriz, antes de donde el sensor inyecta las barras.
            .total_x = 2000, .total_y = 1120,
            .output_x = 1920, .output_y = 1080,
            .scale = false, .binning = false,
        },
        .has_pll = false,
        .fps = 22,
    },
};

const size_t esp32s3cam_mode_count = sizeof(esp32s3cam_mode_table) / sizeof(esp32s3cam_mode_table[0]);
