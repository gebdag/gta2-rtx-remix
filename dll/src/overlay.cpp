#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"
#include "texture_store.h"

namespace gta2dx9 {
namespace {

// The game's vertex is 8 floats: x, y, 1/w, rhw, diffuse, specular, u, v.
// Only the fields we need are named here; the stride is what matters.
constexpr int kGameVertexFloats = 8;
constexpr int kUvU = 6;
constexpr int kUvV = 7;
constexpr int kDiffuse = 4;

// Sprites and tiles alike are pointers into a 256-pixel-wide page, so a row
// step is a page, not the bitmap's own width.
constexpr int kPageStride = 256;

constexpr DWORD kOverlayFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

uint32_t GreyFromShade(uint8_t shade) {
    return 0xFF000000u | (static_cast<uint32_t>(shade) << 16) |
           (static_cast<uint32_t>(shade) << 8) | shade;
}

}  // namespace

void Overlay::SetGameScreenSize(int width, int height) {
    if (width > 0 && height > 0) {
        gameWidth_ = width;
        gameHeight_ = height;
    }
}

// gbh_SetWindow hands over the clip rectangle the game draws inside, which is
// the most reliable statement of its own screen size while the menus are up:
// the camera struct is not populated until a world loads.
//
// The four arguments are a rectangle, but the original stores them as
// left/right/top/bottom rather than the usual order, so rather than depend on
// which it is, take the two largest - for any real screen rectangle those are
// the right and bottom edges under either convention.
void Overlay::NoteWindow(int a, int b, int c, int d) {
    int values[4] = {a, b, c, d};
    std::sort(values, values + 4);
    const int right = values[3];
    const int bottom = values[2];
    if (right <= 1 || bottom <= 1 || right > 4096 || bottom > 4096) return;
    const int width = right + 1;
    const int height = bottom + 1;
    if (sizeFromWindow_ && width == gameWidth_ && height == gameHeight_) return;
    sizeFromWindow_ = true;
    gameWidth_ = width;
    gameHeight_ = height;
    Log("overlay: game screen is %dx%d (from gbh_SetWindow)", gameWidth_, gameHeight_);
}

void Overlay::BeginFrame() {
    vertices_.clear();
    draws_.clear();
}

void Overlay::PushTriangleFan(const Vertex* corners, int count, const void* texture, int image,
                              bool alphaTest) {
    if (count < 3) return;
    Draw draw;
    draw.texture = texture;
    draw.image = image;
    draw.alphaTest = alphaTest;
    draw.first = static_cast<uint32_t>(vertices_.size());
    // A fan of `count` corners is count-2 triangles; the renderer only ever
    // gets 3 or 4, matching the original's DrawPrimitive(D3DPT_TRIANGLEFAN, 4).
    for (int i = 1; i + 1 < count; ++i) {
        vertices_.push_back(corners[0]);
        vertices_.push_back(corners[i]);
        vertices_.push_back(corners[i + 1]);
    }
    draw.count = static_cast<uint32_t>(vertices_.size()) - draw.first;
    draws_.push_back(draw);
}

void Overlay::Quad(unsigned flags, const void* texture, const float* v, uint8_t shade) {
    if (!v) return;
    // The original rejects the whole primitive when the first vertex is behind
    // the eye; without this, off-screen sprites smear across the view.
    if (v[2] <= 0.0f) return;

    const bool vertexColour = (flags & quad_flags::kVertexColour) != 0;
    const uint32_t flat = GreyFromShade(shade);

    Vertex corners[4] = {};
    if (flags & quad_flags::kExpandFromTexture) {
        // Only vertex 0 is filled in: the renderer is expected to lay the
        // texture out from there at its native size (d3ddll.dll!FUN_00e02cc0).
        const TextureRecord* record = static_cast<const TextureRecord*>(texture);
        if (!record) return;
        const float w = static_cast<float>(record->width);
        const float h = static_cast<float>(record->height);
        const float x = v[0];
        const float y = v[1];
        const float uv[4][2] = {{0.0f, 0.0f}, {w, 0.0f}, {w, h}, {0.0f, h}};
        const float xy[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        for (int i = 0; i < 4; ++i) {
            corners[i].x = xy[i][0];
            corners[i].y = xy[i][1];
            corners[i].u = uv[i][0];
            corners[i].v = uv[i][1];
            corners[i].colour = vertexColour ? *reinterpret_cast<const uint32_t*>(&v[kDiffuse])
                                             : flat;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            const float* src = v + i * kGameVertexFloats;
            corners[i].x = src[0];
            corners[i].y = src[1];
            corners[i].u = src[kUvU];
            corners[i].v = src[kUvV];
            corners[i].colour =
                vertexColour ? *reinterpret_cast<const uint32_t*>(&src[kDiffuse]) : flat;
        }
    }
    PushTriangleFan(corners, 4, texture, -1, (flags & quad_flags::kOpaque) == 0);
}

void Overlay::Triangle(unsigned flags, const void* texture, const float* v, uint8_t shade) {
    if (!v || v[2] <= 0.0f) return;
    const bool vertexColour = (flags & quad_flags::kVertexColour) != 0;
    const uint32_t flat = GreyFromShade(shade);

    Vertex corners[3] = {};
    for (int i = 0; i < 3; ++i) {
        const float* src = v + i * kGameVertexFloats;
        corners[i].x = src[0];
        corners[i].y = src[1];
        corners[i].u = src[kUvU];
        corners[i].v = src[kUvV];
        corners[i].colour = vertexColour ? *reinterpret_cast<const uint32_t*>(&src[kDiffuse]) : flat;
    }
    PushTriangleFan(corners, 3, texture, -1, (flags & quad_flags::kOpaque) == 0);
}

void Overlay::FlatRect(const float* v, uint32_t colour) {
    if (!v) return;
    Vertex corners[4] = {};
    for (int i = 0; i < 4; ++i) {
        const float* src = v + i * kGameVertexFloats;
        corners[i].x = src[0];
        corners[i].y = src[1];
        corners[i].colour = colour | 0xFF000000u;
    }
    PushTriangleFan(corners, 4, nullptr, -1, false);
}

void Overlay::Line(int x1, int y1, int x2, int y2, uint32_t colour) {
    // Drawn as a thin quad: a line list would need its own primitive type for
    // one or two debug overlays a frame.
    const float dx = static_cast<float>(x2 - x1);
    const float dy = static_cast<float>(y2 - y1);
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.001f) return;
    const float nx = -dy / length * 0.5f;
    const float ny = dx / length * 0.5f;

    Vertex corners[4] = {};
    const float px[4] = {x1 + nx, x2 + nx, x2 - nx, x1 - nx};
    const float py[4] = {y1 + ny, y2 + ny, y2 - ny, y1 - ny};
    for (int i = 0; i < 4; ++i) {
        corners[i].x = px[i];
        corners[i].y = py[i];
        corners[i].colour = colour | 0xFF000000u;
    }
    PushTriangleFan(corners, 4, nullptr, -1, false);
}

void Overlay::InitImageTable(int count) {
    FreeImageTable();
    if (count > 0 && count < 4096) images_.resize(count);
    Log("overlay: image table sized %d", count);
}

void Overlay::FreeImageTable() {
    for (Image& image : images_) {
        if (image.texture) image.texture->Release();
    }
    images_.clear();
}

// The header check mirrors the original's: uncompressed (type 2), no colour
// map, 16 bits per pixel. Anything else it rejected outright.
int Overlay::LoadImage(const void* data) {
    const uint8_t* tga = static_cast<const uint8_t*>(data);
    if (!tga || tga[1] != 0 || tga[2] != 2 || tga[16] != 0x10) {
        Log("overlay: gbh_LoadImage rejected a non 16-bit uncompressed TGA");
        return -1;
    }
    size_t slot = images_.size();
    for (size_t i = 0; i < images_.size(); ++i) {
        if (!images_[i].width) {
            slot = i;
            break;
        }
    }
    if (slot == images_.size()) {
        if (images_.size() >= 4096) return -1;
        images_.emplace_back();
    }

    const int width = tga[12] | (tga[13] << 8);
    const int height = tga[14] | (tga[15] << 8);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return -1;

    Image& image = images_[slot];
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * height, 0);

    // TGA rows run bottom-up, which is why the original filled its surface from
    // the last row backwards. Pixels are A1R5G5B5 little-endian.
    const uint8_t* pixels = tga + 18 + tga[0];
    for (int y = 0; y < height; ++y) {
        uint32_t* out = &image.pixels[static_cast<size_t>(height - 1 - y) * width];
        for (int x = 0; x < width; ++x) {
            const uint8_t low = pixels[0];
            const uint8_t high = pixels[1];
            pixels += 2;
            const uint32_t r = (high >> 2) & 0x1F;
            const uint32_t g = ((low >> 5) | ((high & 3) << 3)) & 0x1F;
            const uint32_t b = low & 0x1F;
            out[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) |
                     (b << 3 | b >> 2);
        }
    }
    return static_cast<int>(slot);
}

void Overlay::BlitImage(int image, int srcX1, int srcY1, int srcX2, int srcY2, int dstX, int dstY) {
    if (image < 0 || image >= static_cast<int>(images_.size())) return;
    const Image& source = images_[image];
    if (!source.width) return;
    if (srcX2 <= srcX1 || srcY2 <= srcY1) return;

    const float u0 = static_cast<float>(srcX1);
    const float v0 = static_cast<float>(srcY1);
    const float u1 = static_cast<float>(srcX2);
    const float v1 = static_cast<float>(srcY2);
    const float x0 = static_cast<float>(dstX);
    const float y0 = static_cast<float>(dstY);
    const float x1 = x0 + (u1 - u0);
    const float y1 = y0 + (v1 - v0);

    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    Vertex corners[4] = {};
    for (int i = 0; i < 4; ++i) {
        corners[i].x = xy[i][0];
        corners[i].y = xy[i][1];
        corners[i].u = uv[i][0];
        corners[i].v = uv[i][1];
        corners[i].colour = 0xFFFFFFFFu;
    }
    // A blit is a straight surface copy in the original: fully opaque.
    PushTriangleFan(corners, 4, nullptr, image, false);
}

IDirect3DTexture9* Overlay::ResolveImage(IDirect3DDevice9* device, int index) {
    if (index < 0 || index >= static_cast<int>(images_.size())) return nullptr;
    Image& image = images_[index];
    if (!image.width) return nullptr;
    if (image.texture) return image.texture;

    if (FAILED(device->CreateTexture(image.width, image.height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_MANAGED, &image.texture, nullptr))) {
        image.texture = nullptr;
        return nullptr;
    }
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(image.texture->LockRect(0, &locked, nullptr, 0))) {
        for (int y = 0; y < image.height; ++y) {
            memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
                   &image.pixels[static_cast<size_t>(y) * image.width],
                   static_cast<size_t>(image.width) * 4);
        }
        image.texture->UnlockRect(0);
    }
    return image.texture;
}

void Overlay::Flush(IDirect3DDevice9* device, int targetWidth, int targetHeight) {
    if (!device || draws_.empty()) return;

    // The game draws at its own resolution - 640x480 by default - while our back
    // buffer is the window's real size, so everything is scaled on the way out.
    const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(gameWidth_);
    const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(gameHeight_);

    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetRenderState(D3DRS_ALPHAREF, 1);

    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

    device->SetFVF(kOverlayFvf);

    for (const Draw& draw : draws_) {
        IDirect3DTexture9* texture = nullptr;
        float invU = 1.0f;
        float invV = 1.0f;
        if (draw.image >= 0) {
            texture = ResolveImage(device, draw.image);
            const Image& image = images_[draw.image];
            invU = image.width ? 1.0f / image.width : 1.0f;
            invV = image.height ? 1.0f / image.height : 1.0f;
        } else if (draw.texture) {
            texture = DeviceTextureFor(device, draw.texture);
            const TextureRecord* record = static_cast<const TextureRecord*>(draw.texture);
            invU = record->width ? 1.0f / record->width : 1.0f;
            invV = record->height ? 1.0f / record->height : 1.0f;
        }

        if (texture) {
            device->SetTexture(0, texture);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        } else {
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, draw.alphaTest && texture ? TRUE : FALSE);

        // UVs arrive in texels, exactly as the original received them; it
        // scaled them by 1/size before handing them to Direct3D.
        std::vector<Vertex> batch(vertices_.begin() + draw.first,
                                  vertices_.begin() + draw.first + draw.count);
        for (Vertex& vertex : batch) {
            vertex.x = vertex.x * scaleX - 0.5f;
            vertex.y = vertex.y * scaleY - 0.5f;
            vertex.z = 0.0f;
            vertex.rhw = 1.0f;
            vertex.u *= invU;
            vertex.v *= invV;
        }
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, draw.count / 3, batch.data(), sizeof(Vertex));
    }

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

void Overlay::ReleaseResources() {
    FreeImageTable();
    vertices_.clear();
    draws_.clear();
}

}  // namespace gta2dx9
