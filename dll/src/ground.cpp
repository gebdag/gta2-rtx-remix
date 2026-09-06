#include "ground.h"

#include <cmath>

namespace gta2dx9 {
namespace {

// A lid is a bilinear patch over the cell's four corner heights, which for every
// ramp GTA2 has is simply a plane. Sampling it rather than taking the block's
// top is what makes the height continuous inside a cell; it is continuous
// *across* cells too, because a ramp block at step k spans (steps-k-1)/steps to
// (steps-k)/steps, so its neighbour shares the edge height exactly and the last
// step meets flat ground at 0.
//
// fx runs west to east, fy north to south, matching the map's own row order and
// the NW, NE, SE, SW corner order of gta2::LidHeights.
float BilinearLid(const gta2::CornerHeights& h, float fx, float fy) {
    const float north = h[0] + (h[1] - h[0]) * fx;
    const float south = h[3] + (h[2] - h[3]) * fx;
    return north + (south - north) * fy;
}

}  // namespace

void GroundSampler::Reset(const gta2::Map* map, const gta2::SlopeInfo* resolvedSlopes) {
    map_ = map;
    for (int type = 0; type < gta2::kSlopeTypeCount; ++type) {
        slopes_[type] = resolvedSlopes ? resolvedSlopes[type] : gta2::SlopeInfo{};
    }
}

bool GroundSampler::SurfaceAt(float x, float z, float ceiling, float* outY) const {
    if (!Ready()) return false;

    // Renderer z is north-positive and the map's rows run north to south, so the
    // row index is the mirror the mesh builder uses in reverse.
    const float mapY = static_cast<float>(gta2::kMapHeight) - z;
    const float bxf = std::floor(x);
    const float byf = std::floor(mapY);
    const int bx = static_cast<int>(bxf);
    const int by = static_cast<int>(byf);
    if (bx < 0 || bx >= gta2::kMapWidth || by < 0 || by >= gta2::kMapHeight) return false;

    const float fx = x - bxf;
    const float fy = mapY - byf;

    const gta2::Column& column = map_->ColumnAt(bx, by);
    float best = 0.0f;
    bool found = false;
    for (size_t i = 0; i < column.blocks.size(); ++i) {
        const uint32_t index = column.blocks[i];
        if (index >= map_->BlockCount()) continue;
        const gta2::Block& block = map_->BlockAt(index);
        if (!block.lid) continue;  // no lid is nothing to stand on

        const int level = column.offset + static_cast<int>(i);
        const gta2::SlopeInfo& slope =
            slopes_[block.SlopeType() & (gta2::kSlopeTypeCount - 1)];
        // Diagonals, corner ramps and partial blocks all come back flat here,
        // because their slope descriptor carries no direction. Their lids do sit
        // at the block's top over the part of the cell they cover, so the height
        // is right where there is floor and optimistic where there is not - and
        // the ceiling test below is what keeps a sprite on the storey underneath
        // from being lifted onto one of them.
        const float surface =
            static_cast<float>(level) + BilinearLid(gta2::LidHeights(slope), fx, fy);
        if (surface > ceiling) continue;
        if (!found || surface > best) {
            best = surface;
            found = true;
        }
    }
    if (found && outY) *outY = best;
    return found;
}

GroundPlane GroundSampler::Fit(const float* xs, const float* zs, int count, float ceiling,
                               float centreX, float centreZ) const {
    GroundPlane plane;
    if (!Ready() || count < 3 || count > 8) return plane;

    float px[8], pz[8], py[8];
    int n = 0;
    for (int i = 0; i < count; ++i) {
        float y = 0.0f;
        if (!SurfaceAt(xs[i], zs[i], ceiling, &y)) continue;
        px[n] = xs[i];
        pz[n] = zs[i];
        py[n] = y;
        ++n;
    }
    plane.samples = n;
    // Two points describe a line, not a plane, and a sprite half over a hole has
    // no floor worth conforming to. The caller keeps its old behaviour there.
    if (n < 3) return plane;

    float mx = 0.0f, mz = 0.0f, my = 0.0f;
    for (int i = 0; i < n; ++i) {
        mx += px[i];
        mz += pz[i];
        my += py[i];
    }
    const float inv = 1.0f / static_cast<float>(n);
    mx *= inv;
    mz *= inv;
    my *= inv;

    // Least squares y = a*x + b*z + c, solved about the centroid so the normal
    // equations stay conditioned at map coordinates in the hundreds.
    float sxx = 0.0f, sxz = 0.0f, szz = 0.0f, sxy = 0.0f, szy = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float dx = px[i] - mx, dz = pz[i] - mz, dy = py[i] - my;
        sxx += dx * dx;
        sxz += dx * dz;
        szz += dz * dz;
        sxy += dx * dy;
        szy += dz * dy;
    }
    const float det = sxx * szz - sxz * sxz;

    float a = 0.0f, b = 0.0f, c = my;
    if (std::fabs(det) > 1e-9f) {
        a = (szz * sxy - sxz * szy) / det;
        b = (sxx * szy - sxz * sxy) / det;
        c = my - a * mx - b * mz;
    } else {
        // Samples in a line, or all in one spot: no gradient to recover, so keep
        // the plane level and put it at the highest of them rather than the mean.
        // Standing a sprite on the mean of a step would bury half of it.
        c = py[0];
        for (int i = 1; i < n; ++i) {
            if (py[i] > c) c = py[i];
        }
    }

    auto height = [&](float x, float z) { return a * x + b * z + c; };

    plane.residual = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float over = py[i] - height(px[i], pz[i]);
        if (over > plane.residual) plane.residual = over;
    }

    // y - a*x - b*z - c = 0, so the gradient is (-a, 1, -b) and already points up.
    const float length = std::sqrt(a * a + 1.0f + b * b);
    plane.nx = -a / length;
    plane.ny = 1.0f / length;
    plane.nz = -b / length;
    plane.y = height(centreX, centreZ);
    plane.valid = true;
    return plane;
}

}  // namespace gta2dx9
