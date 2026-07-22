#pragma once

namespace ui::geo {

constexpr float kKmPerDeg = 111.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km);

void latLonToScreen(float lat, float lon, int* out_x, int* out_y);

int distSqFromCenter(int x, int y);

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1);

}  // namespace ui::geo
