// C ABI for the `ui` Rust crate (src/crates/ui): the GAME UI feature-lib —
// flexbox layout (`panes`) + glyph rasterization/text layout (`fontdue`).
// Linked into the badlands C++ app via Corrosion.
//
// This is the *game* UI surface, deliberately separate from the Dear ImGui
// *debug* UI (see CLAUDE.md). C++ owns the GPU; this crate owns layout + text
// and knows nothing about heroes, gold, or buildings — only panels, labels and
// buttons.
//
// The seam is COARSE ON PURPOSE: exactly one call per frame. C++ hands over a
// flat batch of high-level elements; Rust returns (a) draw commands as quads in
// physical pixels and (b) hit rects tagged with the caller's element ids. Quads
// rather than vertices keeps this crate renderer-agnostic (no coupling to
// shaders/ui/ui.wesl's vertex layout) and the whole seam testable in pure Rust.
//
// Because the rects Rust just solved ARE the hit targets, C++ never re-derives
// geometry: draw and hit-test cannot disagree. (The legacy Rust UI enforced the
// same invariant by convention — src/ui/panel.rs's "rects() triad"; here it is
// enforced across the ABI.)
//
// Buffers are caller-owned throughout, and counts follow the count-then-fill
// truncation idiom: the output count is the TOTAL required; a value > the cap
// means truncated, so call again with a bigger buffer. Nothing is written past
// a cap.
#ifndef BADLANDS_UI_H
#define BADLANDS_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Error codes (returned by the int32_t entry points; 0 = success)
// ---------------------------------------------------------------------------
#define UI_OK                   0
#define UI_ERR_NULL            -1  // a required pointer argument was NULL
#define UI_ERR_BAD_TREE        -2  // parent index out of range / not before its child
#define UI_ERR_BAD_TEXT        -3  // text_off/text_len outside text_blob, or not UTF-8
#define UI_ERR_LAYOUT          -4  // the layout engine rejected the tree/constraints
#define UI_ERR_CAPACITY        -5  // an output buffer was too small (see the counts)
#define UI_ERR_PANIC           -6  // a Rust panic was caught at the boundary

// ---------------------------------------------------------------------------
// Element batch (input)
// ---------------------------------------------------------------------------

typedef enum UiElementKind {
    UI_ELEM_ROW = 0,   // flex container, children laid left -> right
    UI_ELEM_COL,       // flex container, children laid top -> bottom
    UI_ELEM_PANEL,     // container + background (a COL that paints bg_rgba)
    UI_ELEM_LABEL,     // leaf: one text run
    UI_ELEM_BUTTON,    // leaf: background + centred text; emits a hit rect
    UI_ELEM_SPACER,    // leaf: occupies space, draws nothing
    UI_ELEM_KIND_COUNT
} UiElementKind;

// Element flags (bitwise OR into UiElement.flags).
#define UI_FLAG_NONE         0u
#define UI_FLAG_DISABLED     (1u << 0)  // greyed; still emits a hit rect so the
                                        // click is consumed rather than falling
                                        // through to the world behind it
#define UI_FLAG_ALIGN_RIGHT  (1u << 1)  // right-align this element's text
#define UI_FLAG_ALIGN_CENTER (1u << 2)  // centre this element's text

// One UI element. The array forms a tree via `parent` indices.
//
// ORDERING REQUIREMENT: an element's `parent` must be a LOWER index than the
// element itself (i.e. parents precede children). Element 0 is the root and
// must have parent == -1; it is laid out against the full viewport. Violations
// return UI_ERR_BAD_TREE rather than being silently reinterpreted.
//
// Sizes are in LOGICAL pixels and are multiplied by UiBuildInput.scale_factor
// internally, so callers author one set of numbers for every display density.
typedef struct UiElement {
    uint32_t id;        // caller-assigned; echoed back in UiHitRect. 0 = non-interactive
    uint8_t  kind;      // UiElementKind
    int32_t  parent;    // index into the element array; -1 for the root (element 0)
    float    fixed;     // main-axis size, logical px (0 => use `grow`)
    float    grow;      // flex-grow weight (used when `fixed` is 0)
    float    pad;       // inner padding, logical px
    float    gap;       // spacing between children, logical px (containers only)
    uint32_t bg_rgba;   // 0xRRGGBBAA background; alpha 0 => no background quad
    uint32_t fg_rgba;   // 0xRRGGBBAA text colour
    uint32_t text_off;  // byte offset into UiBuildInput.text_blob
    uint32_t text_len;  // byte length of the text run (0 => no text)
    uint32_t flags;     // UI_FLAG_*
} UiElement;

// ---------------------------------------------------------------------------
// Draw commands + hit rects (output)
// ---------------------------------------------------------------------------

// One textured quad in PHYSICAL pixels, origin top-left, to be expanded by the
// caller into two triangles. Solid rects use the atlas's reserved white texel
// (UiFontInfo.white_u/white_v for all four corners), so rects and glyphs share
// one pipeline and one draw call.
typedef struct UiQuad {
    float x, y, w, h;
    float u0, v0, u1, v1;
    uint32_t rgba;  // 0xRRGGBBAA, straight alpha, sRGB
} UiQuad;

// The screen rect of one interactive element (kind BUTTON, or any element with
// a non-zero id), in PHYSICAL pixels. Emitted innermost-first, so a caller that
// takes the FIRST containing rect gets the topmost/most specific hit.
typedef struct UiHitRect {
    uint32_t id;  // the UiElement.id the caller assigned
    float x, y, w, h;
    uint32_t flags;  // the element's UI_FLAG_* (so callers can ignore DISABLED
                     // for actions while still consuming the click)
} UiHitRect;

// Baked-font metrics, filled by ui_atlas.
typedef struct UiFontInfo {
    uint32_t atlas_size;     // atlas is square: atlas_size * atlas_size R8 bytes
    float ascent_px;
    float descent_px;
    float line_height_px;
    float white_u, white_v;  // UV of the reserved solid-white texel
} UiFontInfo;

// ---------------------------------------------------------------------------
// Build call
// ---------------------------------------------------------------------------

typedef struct UiBuildInput {
    const UiElement* elements;
    uint32_t element_count;
    const char* text_blob;      // UTF-8; may be NULL when text_blob_len == 0
    uint32_t text_blob_len;
    float viewport_w_px;        // PHYSICAL pixels
    float viewport_h_px;        // PHYSICAL pixels
    float scale_factor;         // logical -> physical (1.0 on non-HiDPI)
} UiBuildInput;

typedef struct UiBuildOutput {
    UiQuad* quads;          // caller-owned, may be NULL when quad_cap == 0
    uint32_t quad_cap;
    uint32_t quad_count;    // OUT: total required (> quad_cap => truncated)
    UiHitRect* hits;        // caller-owned, may be NULL when hit_cap == 0
    uint32_t hit_cap;
    uint32_t hit_count;     // OUT: total required (> hit_cap => truncated)
} UiBuildOutput;

// Opaque handle: owns the baked glyph atlas + font metrics.
typedef struct UiContext UiContext;

// Bake the glyph atlas from the TTF bytes in `ttf_bytes` (`ttf_len` bytes) at
// `px_size` (already scaled for the display density). The bytes are only read
// during this call; the caller may free them immediately after.
//
// Bytes rather than a path keeps this seam data-only (per CLAUDE.md): file I/O
// and cwd assumptions stay on the C++ side, and the crate stays trivially
// mockable. Returns NULL if the font is unparseable or the glyphs don't fit the
// atlas at that size. Free with ui_destroy.
UiContext* ui_create(const uint8_t* ttf_bytes, uint32_t ttf_len, float px_size);

// Free a UiContext. Safe to call with NULL.
void ui_destroy(UiContext* ctx);

// Copy the baked R8 coverage atlas into `out_r8` and fill `out_info`. Call once
// after ui_create to upload the texture. Returns UI_ERR_CAPACITY (and still
// fills out_info, so the caller learns the required size) when
// cap < atlas_size * atlas_size.
int32_t ui_atlas(const UiContext* ctx, uint8_t* out_r8, uint32_t cap, UiFontInfo* out_info);

// Solve layout + text for one frame's element batch and emit draw quads and hit
// rects. THE per-frame call. Pure: no interior mutability, no allocation
// visible to the caller, safe to call every frame.
int32_t ui_build(const UiContext* ctx, const UiBuildInput* in, UiBuildOutput* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BADLANDS_UI_H
