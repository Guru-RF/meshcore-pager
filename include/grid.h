// ============================================================
// RF.Guru MeshCore Pager - Maidenhead locator
//
// The pager has no GPS. It learns where it is from the repeater adverts it
// hears, and then deliberately throws precision away: it converts the
// repeater's position to a Maidenhead square and advertises the CENTRE of
// that square. What it actually knows is "somewhere within earshot of that
// repeater", and a grid centre says exactly that, where copying the
// repeater's own coordinates would claim to be standing on its mast.
//
// A 6-character square is about 5 x 4.5 km at 50 degrees north; a
// 4-character one about 150 x 110 km.
// ============================================================

#pragma once

#include <Arduino.h>

// Fill `out` (>= 7 bytes) with the locator containing lat/lon. `precision`
// is 4 or 6 characters.
static inline void gridFromLatLon(double lat, double lon, char *out, int precision) {
    double alon = lon + 180.0;
    double alat = lat + 90.0;
    if (alon < 0) alon = 0; if (alon >= 360.0) alon = 359.999999;
    if (alat < 0) alat = 0; if (alat >= 180.0) alat = 179.999999;

    int fieldLon = (int)(alon / 20.0);
    int fieldLat = (int)(alat / 10.0);
    alon -= fieldLon * 20.0;
    alat -= fieldLat * 10.0;

    int sqLon = (int)(alon / 2.0);
    int sqLat = (int)(alat / 1.0);
    alon -= sqLon * 2.0;
    alat -= sqLat * 1.0;

    out[0] = (char)('A' + fieldLon);
    out[1] = (char)('A' + fieldLat);
    out[2] = (char)('0' + sqLon);
    out[3] = (char)('0' + sqLat);
    if (precision >= 6) {
        out[4] = (char)('A' + (int)(alon / (2.0 / 24.0)));
        out[5] = (char)('A' + (int)(alat / (1.0 / 24.0)));
        out[6] = '\0';
    } else {
        out[4] = '\0';
    }
}

// Centre of the square named by `grid` (4 or 6 characters, case-insensitive).
// Returns false - and leaves lat/lon untouched - if it is not a locator.
static inline bool gridCentre(const char *grid, double *lat, double *lon) {
    if (!grid) return false;
    size_t n = strlen(grid);
    if (n != 4 && n != 6) return false;

    char g[7];
    for (size_t i = 0; i < n; i++) g[i] = (char)toupper((unsigned char)grid[i]);
    g[n] = '\0';

    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return false;
    if (!isdigit((unsigned char)g[2]) || !isdigit((unsigned char)g[3])) return false;

    double alon = (g[0] - 'A') * 20.0 + (g[2] - '0') * 2.0;
    double alat = (g[1] - 'A') * 10.0 + (g[3] - '0') * 1.0;

    if (n == 6) {
        if (g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X') return false;
        alon += (g[4] - 'A') * (2.0 / 24.0) + (2.0 / 24.0) / 2.0;   // centre of the subsquare
        alat += (g[5] - 'A') * (1.0 / 24.0) + (1.0 / 24.0) / 2.0;
    } else {
        alon += 1.0;                                                 // centre of the square
        alat += 0.5;
    }

    *lon = alon - 180.0;
    *lat = alat - 90.0;
    return true;
}

// The two together: snap a heard position to its grid centre, and report the
// locator that was used.
static inline bool gridSnap(double inLat, double inLon, int precision,
                            double *outLat, double *outLon, char *locator) {
    gridFromLatLon(inLat, inLon, locator, precision);
    return gridCentre(locator, outLat, outLon);
}
