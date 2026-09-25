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

// Draw::image for the movie frame, which is not in the image table.
constexpr int kMovieImage = -2;

HudFitMode g_fit = HudFitMode::Fit;

uint32_t GreyFromShade(uint8_t shade) {
    return 0xFF000000u | (static_cast<uint32_t>(shade) << 16) |
           (static_cast<uint32_t>(shade) << 8) | shade;
}

}  // namespace

void SetHudFit(HudFitMode mode) { g_fit = mode; }
HudFitMode HudFit() { return g_fit; }

void Overlay::SetGameScreenSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == gameWidth_ && height == gameHeight_) return;
    gameWidth_ = width;
    gameHeight_ = height;
    Log("overlay: game screen is %dx%d (from the camera struct)", gameWidth_, gameHeight_);
}

// The whole menu is scaled by targetSize/gameSize at flush time, so a wrong
// gameSize puts the front end in a corner and a gameSize that *changes* makes it
// jitter - most visibly at the right and bottom edges, where the scale error
// accumulates. Both were the camera struct being read after the level that owned
// it had gone.
void Overlay::RevertToWindowSize() {
    if (windowWidth_ <= 0 || windowHeight_ <= 0) return;
    if (windowWidth_ == gameWidth_ && windowHeight_ == gameHeight_) return;
    Log("overlay: back to %dx%d from gbh_SetWindow (was %dx%d)", windowWidth_, windowHeight_,
        gameWidth_, gameHeight_);
    gameWidth_ = windowWidth_;
    gameHeight_ = windowHeight_;
}

// gbh_SetWindow hands over the clip rectangle the game draws inside, which is
// the most reliable statement of its own screen size while the menus are up:
// the camera struct is not populated until a world loads.
//
// The four arguments are a rectangle, but the original stores them as
// left/right/top/bottom rather than the usual order, so rather than depend on
// which it is, take the two largest - for any real screen rectangle those are
// the right and bottom edges under either convention.
void Overlay::NoteWindow(float a, float b, float c, float d) {
    int values[4] = {static_cast<int>(a), static_cast<int>(b), static_cast<int>(c),
                     static_cast<int>(d)};
    std::sort(values, values + 4);
    const int right = values[3];
    const int bottom = values[2];
    if (right <= 1 || bottom <= 1 || right > 4096 || bottom > 4096) return;
    const int width = right + 1;
    const int height = bottom + 1;
    // Recorded whether or not it is applied, so RevertToWindowSize has an answer.
    windowWidth_ = width;
    windowHeight_ = height;
    if (sizeFromWindow_ && width == gameWidth_ && height == gameHeight_) return;
    sizeFromWindow_ = true;
    gameWidth_ = width;
    gameHeight_ = height;
    Log("overlay: game screen is %dx%d (from gbh_SetWindow)", gameWidth_, gameHeight_);
}

void Overlay::TraceNextFrames(int frames) {
    if (frames > 0) traceFrames_ = frames;
}

void Overlay::BeginFrame() {
    vertices_.clear();
    draws_.clear();
    missingArtwork_ = 0;
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

// The colour is whatever gbh_ConvertColour handed the game, and that returns
// 5:6:5 - which is what a renderer the game asked for 16-bit colour is supposed
// to return. It comes straight back here, so it has to be *expanded* rather than
// reinterpreted: taken as ARGB8888, red lands in the byte 5:6:5 never fills and
// every filled panel in the UI comes out the wrong colour.
uint32_t Overlay::PanelColour(uint32_t colour) {
    // Anything in the high half is already a full colour and is left alone.
    if (colour & 0xFFFF0000u) return colour | 0xFF000000u;
    const uint32_t r = (colour >> 11) & 0x1F;
    const uint32_t g = (colour >> 5) & 0x3F;
    const uint32_t b = colour & 0x1F;
    return 0xFF000000u | (((r * 255 + 15) / 31) << 16) | (((g * 255 + 31) / 63) << 8) |
           ((b * 255 + 15) / 31);
}

void Overlay::FlatRect(const float* v, uint32_t colour) {
    if (!v) return;
    Vertex corners[4] = {};
    for (int i = 0; i < 4; ++i) {
        const float* src = v + i * kGameVertexFloats;
        corners[i].x = src[0];
        corners[i].y = src[1];
        corners[i].colour = PanelColour(colour);
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

void Overlay::MovieFrame(const void* pixels, int width, int height, int pitch) {
    if (!pixels || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        pitch < width * 4) {
        return;
    }
    if (width != movie_.width || height != movie_.height) {
        if (movie_.texture) movie_.texture->Release();
        movie_.texture = nullptr;
        movie_.width = width;
        movie_.height = height;
    }
    // X8R8G8B8 as Bink writes it, but the fourth byte is whatever was there and
    // this pass blends on alpha, so it is made opaque on the way in.
    movie_.pixels.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(pixels) + static_cast<size_t>(y) * pitch);
        uint32_t* out = &movie_.pixels[static_cast<size_t>(y) * width];
        for (int x = 0; x < width; ++x) out[x] = row[x] | 0xFF000000u;
    }
    movieChanged_ = true;

    // The whole of the game's screen, which is 4:3 like the movie; the fit at
    // flush time then puts it in the middle of the display.
    const float w = static_cast<float>(gameWidth_);
    const float h = static_cast<float>(gameHeight_);
    const float u = static_cast<float>(width);
    const float v = static_cast<float>(height);
    const float xy[4][2] = {{0.0f, 0.0f}, {w, 0.0f}, {w, h}, {0.0f, h}};
    const float uv[4][2] = {{0.0f, 0.0f}, {u, 0.0f}, {u, v}, {0.0f, v}};
    Vertex corners[4] = {};
    for (int i = 0; i < 4; ++i) {
        corners[i].x = xy[i][0];
        corners[i].y = xy[i][1];
        corners[i].u = uv[i][0];
        corners[i].v = uv[i][1];
        corners[i].colour = 0xFFFFFFFFu;
    }
    PushTriangleFan(corners, 4, nullptr, kMovieImage, false);
}

void Overlay::EndMovie() {
    if (movie_.texture) movie_.texture->Release();
    movie_ = Image{};
    movieChanged_ = false;
}

// One texture, rewritten whenever a new frame has arrived.
IDirect3DTexture9* Overlay::ResolveMovie(IDirect3DDevice9* device) {
    if (!movie_.width) return nullptr;
    if (!movie_.texture) {
        if (FAILED(device->CreateTexture(movie_.width, movie_.height, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_MANAGED, &movie_.texture, nullptr))) {
            movie_.texture = nullptr;
            return nullptr;
        }
        movieChanged_ = true;
    }
    if (movieChanged_) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(movie_.texture->LockRect(0, &locked, nullptr, 0))) {
            for (int y = 0; y < movie_.height; ++y) {
                memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
                       &movie_.pixels[static_cast<size_t>(y) * movie_.width],
                       static_cast<size_t>(movie_.width) * 4);
            }
            movie_.texture->UnlockRect(0);
            movieChanged_ = false;
        }
    }
    return movie_.texture;
}

void Overlay::Flush(IDirect3DDevice9* device, int targetWidth, int targetHeight) {
    if (!device || draws_.empty()) return;

    // The game draws at its own resolution - 640x480 by default - while our back
    // buffer is the window's real size, so everything is scaled on the way out.
    float scaleX = static_cast<float>(targetWidth) / static_cast<float>(gameWidth_);
    float scaleY = static_cast<float>(targetHeight) / static_cast<float>(gameHeight_);
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    if (g_fit == HudFitMode::Fit && scaleX != scaleY) {
        // The smaller scale, so all of the game's screen is on screen, and the
        // slack split evenly - the front end is a full-screen image and belongs
        // in the middle of the display rather than against one edge.
        const float uniform = scaleX < scaleY ? scaleX : scaleY;
        offsetX = (static_cast<float>(targetWidth) - gameWidth_ * uniform) * 0.5f;
        offsetY = (static_cast<float>(targetHeight) - gameHeight_ * uniform) * 0.5f;
        scaleX = uniform;
        scaleY = uniform;
    }

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
        // Whether this draw asked for artwork at all. FlatRect and Line do not;
        // everything the game blits does.
        const bool movie = draw.image == kMovieImage;
        const bool wantsArtwork = draw.image >= 0 || movie || draw.texture != nullptr;
        if (movie) {
            texture = ResolveMovie(device);
            invU = movie_.width ? 1.0f / movie_.width : 1.0f;
            invV = movie_.height ? 1.0f / movie_.height : 1.0f;
        } else if (draw.image >= 0 && static_cast<size_t>(draw.image) < images_.size()) {
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

        // A draw that wanted artwork and did not get it is dropped, not painted.
        //
        // Falling through to the untextured path below fills the quad with its
        // vertex colour instead, and for anything the game blits that colour is
        // the shade byte splatted to grey - so a menu image that failed to
        // resolve came out as a solid grey panel covering exactly the area the
        // artwork should have filled, blinking as the resolve succeeded and
        // failed from one frame to the next.
        //
        // It fails for ordinary reasons: InitImageTable frees the whole table
        // before the game reloads it, so every blit in between has nothing
        // behind it, and a sprite texture is empty until the game has registered
        // its palette. Missing artwork for a frame is a gap. Painting grey over
        // the frame is a wall.
        if (wantsArtwork && !texture) {
            ++missingArtwork_;
            // Everything needed to say *why*, once per dropped draw. The counters
            // in texture_store say how a build failed but not which draw it cost,
            // and the flicker is one glyph for one frame - so the record itself
            // has to be printed at the moment it could not be resolved.
            if (missingLogged_ < 120) {
                ++missingLogged_;
                if (movie) {
                    Log("overlay drop: movie frame %dx%d, no texture", movie_.width,
                        movie_.height);
                } else if (draw.image >= 0) {
                    Log("overlay drop: blit image=%d, table holds %zu", draw.image, images_.size());
                } else {
                    const TextureRecord* r = static_cast<const TextureRecord*>(draw.texture);
                    Log("overlay drop: tex=%p %dx%d palette=%u rev=%u flags=%02X pixels=%p",
                        draw.texture, static_cast<int>(r->width), static_cast<int>(r->height),
                        static_cast<unsigned>(r->palette), static_cast<unsigned>(r->revision),
                        static_cast<unsigned>(r->flags), r->pixels);
                }
            }
            continue;
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
        // The movie is a photograph scaled up to the display, which point sampling
        // turns into blocks; the UI artwork keeps its hard pixels.
        const D3DTEXTUREFILTERTYPE filter = movie ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, filter);

        // UVs arrive in texels, exactly as the original received them; it
        // scaled them by 1/size before handing them to Direct3D.
        std::vector<Vertex> batch(vertices_.begin() + draw.first,
                                  vertices_.begin() + draw.first + draw.count);
        for (Vertex& vertex : batch) {
            // The half pixel is the usual Direct3D 9 texel-to-pixel offset, and
            // it is subtracted after the fit so that centring cannot shift it.
            vertex.x = vertex.x * scaleX + offsetX - 0.5f;
            vertex.y = vertex.y * scaleY + offsetY - 0.5f;
            vertex.z = 0.0f;
            vertex.rhw = 1.0f;
            vertex.u *= invU;
            vertex.v *= invV;
        }
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, draw.count / 3, batch.data(), sizeof(Vertex));
    }

    // One line per frame whose draw list is not identical to the last, which is
    // exactly the frames the menu changes on - and therefore the frames the
    // flicker happens on.
    size_t signature = draws_.size() * 1000003u;
    for (const Draw& draw : draws_) {
        signature = signature * 31u + reinterpret_cast<uintptr_t>(draw.texture) +
                    static_cast<size_t>(draw.image) * 7u + draw.count;
    }
    if (signature != lastSignature_) {
        lastSignature_ = signature;
        if (changeLogged_ < 60) {
            ++changeLogged_;
            Log("overlay: draw list changed - %zu draw(s), %d dropped", draws_.size(),
                missingArtwork_);
        }
    }

    // Which draw came or went, by name. The count alternating by one says a
    // single draw is blinking; this says which, and where it is, so the game
    // side can be found rather than guessed at.
    {
        std::vector<const void*> keys;
        keys.reserve(draws_.size());
        for (const Draw& draw : draws_) {
            keys.push_back(draw.image >= 0
                               ? reinterpret_cast<const void*>(0x10000u + draw.image)
                               : draw.texture);
        }
        if (diffLogged_ < 40 && keys != lastKeys_) {
            int said = 0;
            for (size_t i = 0; i < keys.size() && said < 6; ++i) {
                if (std::find(lastKeys_.begin(), lastKeys_.end(), keys[i]) != lastKeys_.end()) {
                    continue;
                }
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (uint32_t v = 0; v < draws_[i].count; ++v) {
                    const Vertex& vertex = vertices_[draws_[i].first + v];
                    x0 = (std::min)(x0, vertex.x);
                    y0 = (std::min)(y0, vertex.y);
                    x1 = (std::max)(x1, vertex.x);
                    y1 = (std::max)(y1, vertex.y);
                }
                Log("  + %p at (%.0f,%.0f)-(%.0f,%.0f)", keys[i], x0, y0, x1, y1);
                ++said;
            }
            for (size_t i = 0; i < lastKeys_.size() && said < 12; ++i) {
                if (std::find(keys.begin(), keys.end(), lastKeys_[i]) != keys.end()) continue;
                Log("  - %p gone", lastKeys_[i]);
                ++said;
            }
            if (said) ++diffLogged_;
        }
        lastKeys_.swap(keys);
    }

    if (traceFrames_ > 0) {
        --traceFrames_;
        Log("overlay trace: %zu draw(s), game screen %dx%d, target %dx%d", draws_.size(),
            gameWidth_, gameHeight_, targetWidth, targetHeight);
        int shown = 0;
        for (const Draw& draw : draws_) {
            if (++shown > 40) {
                Log("  ... %zu more", draws_.size() - 40);
                break;
            }
            // The quad's extent in the game's own coordinates, which is what
            // says whether a draw covers the region that comes out grey.
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (uint32_t i = 0; i < draw.count; ++i) {
                const Vertex& vertex = vertices_[draw.first + i];
                x0 = (std::min)(x0, vertex.x);
                y0 = (std::min)(y0, vertex.y);
                x1 = (std::max)(x1, vertex.x);
                y1 = (std::max)(y1, vertex.y);
            }
            const char* kind = draw.image == kMovieImage
                                   ? "movie"
                                   : draw.image >= 0 ? "blit" : (draw.texture ? "quad" : "flatrect");
            Log("  %-8s image=%-4d tex=%p colour=%08X  (%.0f,%.0f)-(%.0f,%.0f)", kind, draw.image,
                draw.texture, vertices_[draw.first].colour, x0, y0, x1, y1);
        }
    }

    if (missingArtwork_ > 0 && missingLogged_ < 8) {
        ++missingLogged_;
        Log("overlay: %d draw(s) dropped, artwork not ready (logged %d/8)", missingArtwork_,
            missingLogged_);
    }

    device->SetTexture(0, nullptr);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

void Overlay::ReleaseResources() {
    FreeImageTable();
    EndMovie();
    vertices_.clear();
    draws_.clear();
}

}  // namespace gta2dx9
