// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"
#include "../screen_filter.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    if (g_vkc.api < VK_API_VERSION_1_3) h ^= (uint64_t)g_vkc.api << 32;  // (SPIR-V 1.5 for Vulkan 1.2: other files)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = ENV("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan,  // (SPIR-V 1.6 / 1.5: compat.c)
                                           g_vkc.api >= VK_API_VERSION_1_3 ? shaderc_env_version_vulkan_1_3 : shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_optimization_level(o, ENV("LARECOMP_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    extern uint32_t g_vk_spirv_salt;  // (device.c vk_bad_code_note: a shader the driver's own disk cache doesn't know)
    uint32_t *salted = NULL;
    if (g_vk_spirv_salt && nwords > 5) {
        salted = malloc(nwords * 4);
        memcpy(salted, spv, nwords * 4);
        salted[2] ^= g_vk_spirv_salt << 16;  // (the generator's tool id: read by no one)
    }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4,
                                   .pCode = salted ? salted : spv};
    VkShaderModule m;
    VkResult r = vkCreateShaderModule(g_vk.dev, &ci, NULL, &m);
    free(salted);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    // Reduzida's copies (exec.c za_red_copy): a fullscreen draw fetches our half-size cascade and writes it into the
    // game's layer 1 - a render pass instead of vkCmdBlitImage, whose format feature this driver need not report.
    // pc.a.xy: source texel per destination texel (0.5, 0.5); pc.a.zw: the source's size, for the fetch clamp.
    if (!strcmp(name, "shadow_far_depth.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "void main() {\n"
               "  ivec2 sc = ivec2(vec2(gl_FragCoord.xy) * pc.a.xy);\n"
               "  gl_FragDepth = texelFetch(t0, clamp(sc, ivec2(0), ivec2(pc.a.zw) - 1), 0).r;\n"
               "}\n";
    }
    if (!strcmp(name, "shadow_far_id.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 sc = ivec2(vec2(gl_FragCoord.xy) * pc.a.xy);\n"
               "  o = texelFetch(t0, clamp(sc, ivec2(0), ivec2(pc.a.zw) - 1), 0);\n"
               "}\n";
    }
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform F { layout(offset = 32) vec4 pf; };\n"  // (the screen filter)
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 c = texture(t, uv).rgb;\n"
               SCREEN_FILTER_SRC("vec3")
               "  o = vec4(c, 1.0);\n"
               "}\n";
    }
    // ---- decoded video, NV12 -> RGBA (exec.c vk_convert_nv12): limited range BT.601, as swscale did (the movies
    // declare no matrix). Fullscreen triangle; Vulkan NDC y points down, so uv = p.
    if (!strcmp(name, "nv12.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  uv = p;\n"
               "}\n";
    }
    if (!strcmp(name, "nv12.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ty;\n"
               "layout(set = 0, binding = 1) uniform sampler2D tuv;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  float y = (texture(ty, uv).r - 16.0 / 255.0) * (255.0 / 219.0);\n"
               "  vec2 c = (texture(tuv, uv).rg - 128.0 / 255.0) * (255.0 / 224.0);\n"
               "  o = vec4(clamp(vec3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x), 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "texcopy.frag")) {  // (texture dumps: alpha kept - foliage, fences, decals are cut out by it)
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = texture(t, uv); }\n";
    }
    // ---- Android pre-rotation (exec.c vk_rotate_encode): the landscape window image (W x H: r.y, r.z) -> the display's
    // native one, the content turned r.x degrees clockwise (VK_SURFACE_TRANSFORM_ROTATE_*: "rotated clockwise").
    // Inverted per destination texel d: 90 takes source (d.y, H-1-d.x) - source top-left lands top-right -, 270
    // (W-1-d.y, d.x), 180 both axes. texelFetch: the sizes only swap, so it is an exact copy.
    if (!strcmp(name, "rotate.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D src;\n"
               "layout(push_constant) uniform P { ivec4 r; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 d = ivec2(gl_FragCoord.xy), s;\n"
               "  int W = pc.r.y, H = pc.r.z;\n"
               "  if (pc.r.x == 90) s = ivec2(d.y, H - 1 - d.x);\n"
               "  else if (pc.r.x == 270) s = ivec2(W - 1 - d.y, d.x);\n"
               "  else s = ivec2(W - 1 - d.x, H - 1 - d.y);\n"
               "  o = texelFetch(src, s, 0);\n"
               "}\n";
    }
    // ---- HDR output (F1 > Video > HDR; exec.c vk_hdr_encode): the window's SDR image -> its HDR10 / scRGB image.
    // Everything is paper white bright (cfg.x, in scRGB units of 80 nits) except the game's highlights: above the knee
    // (cfg.z, linear, on the brightest channel) the scene brightens smoothly up to the peak (cfg.y x paper white) - an
    // inverse tone map of the game's SDR picture, as Windows' Auto HDR does. Kept at paper white: what the overlay drew
    // (the window image against `base`, its copy from before the overlay; flags.y) and the game's HUD (the game's shown
    // image against its scene from before the HUD, at the game image's place vp / crop; flags.z, without them nothing is
    // brightened). flags.x: 1 scRGB (linear BT.709), 0 HDR10 (BT.2020, PQ); flags.w: the game image is upside down.
    if (!strcmp(name, "hdr.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D sdr;\n"
               "layout(set = 0, binding = 1) uniform sampler2D base;\n"
               "layout(set = 0, binding = 2) uniform sampler2D shown;\n"
               "layout(set = 0, binding = 3) uniform sampler2D scene;\n"
               "layout(push_constant) uniform P { vec4 vp; vec4 crop; vec4 cfg; vec4 flags; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"
               "float mx(vec3 v) { return max(v.r, max(v.g, v.b)); }\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec3 c = texelFetch(sdr, p, 0).rgb, l = lin(c);\n"
               "  float ui = pc.flags.y > 0.5 && mx(abs(c - texelFetch(base, p, 0).rgb)) > 1.5 / 255.0 ? 1.0 : 0.0, g = 1.0;\n"
               "  vec2 u = (gl_FragCoord.xy - pc.vp.xy) * pc.vp.zw;\n"
               "  if (pc.flags.z > 0.5 && all(greaterThanEqual(u, vec2(0.0))) && all(lessThanEqual(u, vec2(1.0)))) {\n"
               "    if (pc.flags.w > 0.5) u.y = 1.0 - u.y;\n"
               "    vec2 t = pc.crop.xy + u * pc.crop.zw;\n"
               "    float d = mx(abs(textureLod(shown, t, 0.0).rgb - textureLod(scene, t, 0.0).rgb));\n"
               "    ui = max(ui, smoothstep(2.0 / 255.0, 10.0 / 255.0, d));\n"
               "    float s = clamp((mx(l) - pc.cfg.z) / (1.0 - pc.cfg.z), 0.0, 1.0);\n"
               "    g = mix(1.0 + (pc.cfg.y - 1.0) * s * s, 1.0, ui);\n"
               "  }\n"
               "  l *= g * pc.cfg.x;\n"
               "  if (pc.flags.x > 0.5) { o = vec4(l, 1.0); return; }\n"
               "  const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
               "  vec3 n = pow(max(to2020 * l * (80.0 / 10000.0), vec3(0.0)), vec3(0.1593017578125));\n"
               "  o = vec4(pow((0.8359375 + 18.8515625 * n) / (1.0 + 18.6875 * n), vec3(78.84375)), 1.0);\n"
               "}\n";
    }
    // ---- FSR 2's inputs (exec.c fsr2_frame): camera motion vectors and the reactive mask, at the scene's size. The
    // depth (reversed, infinite far plane) at each pixel, its point unjittered (jit.xy: the frame's jitter in pixels,
    // jit.zw: 1 / size), back through this frame's view-projection (inv_cur) and forward through the last frame's
    // (prev): where it was, in pixels from here (FSR 2: current -> previous). The game's clip space has y up (the
    // negative-height viewport). Only the camera moves things: characters (stencil 0x30-0x3F) and foliage (2) also
    // move on their own, so FSR 2 leans on the current frame there (react.x, react.y).
    if (!strcmp(name, "fsr2_mv.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 1) uniform usampler2D sten;\n"
               "layout(set = 0, binding = 2) uniform U { mat4 inv_cur; mat4 prev; vec4 jit; vec4 react; } u;\n"
               "layout(location = 0) out vec2 mv;\n"
               "layout(location = 1) out float rx;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  float d = max(texelFetch(dep, p, 0).r, 1e-7);\n"
               "  vec2 uv = (gl_FragCoord.xy - u.jit.xy) * u.jit.zw;\n"
               "  vec4 c = u.prev * (u.inv_cur * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0));\n"
               "  vec2 puv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);\n"
               "  mv = c.w > 0.0 ? (puv - uv) / u.jit.zw : vec2(0.0);\n"
               "  uint s = texelFetch(sten, p, 0).r;\n"
               "  rx = (s & 0xF0u) == 0x30u ? u.react.x : s == 2u ? u.react.y : 0.0;\n"
               "}\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // Snapdragon Game Super Resolution 1 (Copyright (c) 2025, Qualcomm Innovation Center, Inc., BSD-3-Clause:
    // third_party/sgsr/LICENSE), from sgsr/v1/include/glsl/sgsr1_shader_mobile.frag: upscale and sharpen in one pass,
    // 12-tap Lanczos-like weights only where the green channel shows an edge (else the plain bilinear tap). Made for
    // Adreno: mediump math (RelaxedPrecision for the driver), textureGather. Taken as it is apart from the inputs:
    // a = ViewportInfo (1 / texture size, texture size), b.xy = texture uv per output pixel (the contents' part of
    // the texture under Dynamic resolution); RGBA mode (1), the edge threshold and sharpness its defaults.
    if (!strcmp(name, "sgsr1.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "precision mediump float;\n"
               "precision highp int;\n"
               "layout(set = 0, binding = 0) uniform mediump sampler2D ps0;\n"
               "layout(push_constant) uniform P { highp vec4 a; highp vec4 b; } pc;\n"
               "layout(location = 0) out vec4 out_Target0;\n"
               "const int mode = 1;\n"
               "const float edgeThreshold = 8.0 / 255.0, edgeSharpness = 2.0;\n"
               "float fastLanczos2(float x) { float wA = x - 4.0; float wB = x * wA - wA; wA *= wA; return wB * wA; }\n"
               "vec2 weightY(float dx, float dy, float c, float std) {\n"
               "  float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);\n"
               "  float w = fastLanczos2(x);\n"
               "  return vec2(w, w * c);\n"
               "}\n"
               "void main() {\n"
               "  highp vec4 ViewportInfo = pc.a;\n"
               "  highp vec2 uv = gl_FragCoord.xy * pc.b.xy;\n"
               "  vec4 color;\n"
               "  color.xyz = textureLod(ps0, uv, 0.0).xyz;\n"
               "  highp vec2 imgCoord = ((uv * ViewportInfo.zw) + vec2(-0.5, 0.5));\n"
               "  highp vec2 imgCoordPixel = floor(imgCoord);\n"
               "  highp vec2 coord = (imgCoordPixel * ViewportInfo.xy);\n"
               "  vec2 pl = (imgCoord + (-imgCoordPixel));\n"
               "  vec4 left = textureGather(ps0, coord, mode);\n"
               "  float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y) + abs(color[mode] - left.z);\n"
               "  if (edgeVote > edgeThreshold) {\n"
               "    coord.x += ViewportInfo.x;\n"
               "    vec4 right = textureGather(ps0, coord + vec2(ViewportInfo.x, 0.0), mode);\n"
               "    vec4 upDown;\n"
               "    upDown.xy = textureGather(ps0, coord + vec2(0.0, -ViewportInfo.y), mode).wz;\n"
               "    upDown.zw = textureGather(ps0, coord + vec2(0.0, ViewportInfo.y), mode).yx;\n"
               "    float mean = (left.y + left.z + right.x + right.w) * 0.25;\n"
               "    left = left - vec4(mean);\n"
               "    right = right - vec4(mean);\n"
               "    upDown = upDown - vec4(mean);\n"
               "    color.w = color[mode] - mean;\n"
               "    float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) + (((abs(right.x) + abs(right.y)) + "
               "abs(right.z)) + abs(right.w))) + (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));\n"
               "    float std = 2.181818 / sum;\n"
               "    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);\n"
               "    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);\n"
               "    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);\n"
               "    aWY += weightY(pl.x, pl.y, left.z, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);\n"
               "    float finalY = aWY.y / aWY.x;\n"
               "    float maxY = max(max(left.y, left.z), max(right.x, right.w));\n"
               "    float minY = min(min(left.y, left.z), min(right.x, right.w));\n"
               "    finalY = clamp(edgeSharpness * finalY, minY, maxY);\n"
               "    float deltaY = finalY - color.w;\n"
               "    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);\n"  // (smooth high contrast input)
               "    color.x = clamp((color.x + deltaY), 0.0, 1.0);\n"
               "    color.y = clamp((color.y + deltaY), 0.0, 1.0);\n"
               "    color.z = clamp((color.z + deltaY), 0.0, 1.0);\n"
               "  }\n"
               "  color.w = 1.0;\n"
               "  out_Target0 = color;\n"
               "}\n";
    }
    // ---- Frame generation (exec.c fg_*, F1 > Video > Frame generation): the game at 30 fps shown at 60, an image made
    // between each two real ones by optical flow. Adapted from letsgo-recomp's flow method (github.com/raulzrrs/
    // letsgo-recomp, MIT licence, Copyright (c) 2026 raulzito12345rrs: third_party/letsgo-fg/LICENSE): the "analytic"
    // vector is the camera's motion (fg_cam, from Pokémon Z-A's scene depth and view-projections; no motion without
    // them), and the block search must beat it clearly. Vectors are N's uv - N-1's.
    // fg_luma.frag: the luminance of N's (r) and N-1's (g) images at the flow's size (~640 wide), 4 bilinear taps a
    // pixel. a.xy = 1 / that size.
    if (!strcmp(name, "fg_luma.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t1;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t0;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy, d = pc.a.xy * 0.25;\n"
               "  vec3 a = vec3(0.0), b = vec3(0.0);\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    vec2 q = uv + vec2((i & 1) != 0 ? d.x : -d.x, (i & 2) != 0 ? d.y : -d.y);\n"
               "    a += texture(t1, q).rgb; b += texture(t0, q).rgb;\n"
               "  }\n"
               "  o = vec4(lum(a * 0.25), lum(b * 0.25), 0.0, 0.0);\n"
               "}\n";
    }
    // fg_match.frag: one 8 x 8 block every a.w luminance pixels, its 16 samples against N-1 moved by each candidate:
    // the analytic vector va (the camera's at the block's centre and corners, fg_cam, b.y = 1; else no motion), a
    // search of +-a.z pixels in steps of 2 and the 3 x 3 around zero (the HUD, still particles). The search wins only
    // clearly (under 0.8 x va's difference, and by 0.4 % of white): flat or relit areas keep va. fg_refine.frag: again
    // from the best of the block's and its 8 neighbours' results (a good vector spreads), then +-1 and +-0.5 pixel
    // around it. Out: xy = the vector (uv), z = the difference left (fg_cut), w = 1: the block's own (not va). a.xy =
    // 1 / the luminance's size. fg_match's b.x = 1 (ours): F is a coarse level's result (the same match at a quarter
    // of the size), its vector a candidate and the search's centre - +-4 pixels alone missed the ground near the camera
    // in a turn. M (binding 2): fg_cam's vectors, any size.
    if (!strcmp(name, "fg_match.frag") || !strcmp(name, "fg_refine.frag")) {
        *stage = STAGE_FRAGMENT;
        static const char *common =
            "#version 450\n"
            "layout(set = 0, binding = 0) uniform sampler2D L;\n"
            "layout(set = 0, binding = 1) uniform sampler2D F;\n"
            "layout(set = 0, binding = 2) uniform sampler2D M;\n"
            "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
            "layout(location = 0) out vec4 o;\n"
            "vec2 lsz;\n"
            "float na[16];\n"
            "void prep(vec2 c) {\n"
            "  for (int y = 0; y < 4; y++)\n"
            "    for (int x = 0; x < 4; x++) na[y * 4 + x] = texture(L, (c + vec2(float(x * 2 - 3), float(y * 2 - 3))) / lsz).r;\n"
            "}\n"
            "float sad(vec2 c, vec2 v) {\n"
            "  float s = 0.0;\n"
            "  for (int y = 0; y < 4; y++)\n"
            "    for (int x = 0; x < 4; x++) {\n"
            "      vec2 q = c + vec2(float(x * 2 - 3), float(y * 2 - 3));\n"
            "      s += abs(na[y * 4 + x] - texture(L, (q - v) / lsz).g);\n"
            "    }\n"
            "  return s / 16.0;\n"
            "}\n"
            "vec2 va_at(vec2 c) {\n"  // (luminance pixels)
            "  if (pc.b.y < 0.5) return vec2(0.0);\n"
            "  ivec2 ms = textureSize(M, 0);\n"
            "  return texelFetch(M, clamp(ivec2(c / lsz * vec2(ms)), ivec2(0), ms - 1), 0).xy * lsz;\n"
            "}\n";
        static char src[2][6144];
        int k = !strcmp(name, "fg_refine.frag");
        if (!src[k][0] && !k)
            snprintf(src[k], sizeof src[k], "%s%s", common,
                     "void main() {\n"
                     "  lsz = 1.0 / pc.a.xy;\n"
                     "  vec2 c = (floor(gl_FragCoord.xy) + 0.5) * pc.a.w;\n"
                     "  prep(c);\n"
                     "  vec2 va = va_at(c), best = va;\n"
                     "  float sa = sad(c, va), sb = sa, s;\n"
                     "  int R = int(pc.a.z);\n"
                     "  vec2 v0 = floor(va + 0.5);\n"
                     "  if (pc.b.x > 0.5) {\n"  // (the coarse level's vector at this block: the search around it)
                     "    ivec2 fz = textureSize(F, 0);\n"
                     "    v0 = floor(texelFetch(F, clamp(ivec2(c / lsz * vec2(fz)), ivec2(0), fz - 1), 0).xy * lsz + 0.5);\n"
                     "    s = sad(c, v0); if (s < sb) { sb = s; best = v0; }\n"
                     "  }\n"
                     "  for (int y = -R; y <= R; y += 2)\n"
                     "    for (int x = -R; x <= R; x += 2) { vec2 v = v0 + vec2(x, y); s = sad(c, v); if (s < sb) { sb = s; best = v; } }\n"
                     "  for (int y = -1; y <= 1; y++)\n"
                     "    for (int x = -1; x <= 1; x++) { s = sad(c, vec2(x, y)); if (s < sb) { sb = s; best = vec2(x, y); } }\n"
                     "  for (int i = 0; i < 4; i++) {\n"  // (the camera's vector at the block's corners: an edge between depths)
                     "    vec2 v = va_at(c + vec2((i & 1) != 0 ? 3.0 : -3.0, (i & 2) != 0 ? 3.0 : -3.0));\n"
                     "    s = sad(c, v); if (s < sb) { sb = s; best = v; }\n"
                     "  }\n"
                     "  bool rep = sb < sa * 0.8 - 0.004;\n"
                     "  o = vec4((rep ? best : va) / lsz, rep ? sb : sa, rep ? 1.0 : 0.0);\n"
                     "}\n");
        else if (!src[k][0])
            snprintf(src[k], sizeof src[k], "%s%s", common,
                     "void main() {\n"
                     "  lsz = 1.0 / pc.a.xy;\n"
                     "  ivec2 g = ivec2(gl_FragCoord.xy), gs = textureSize(F, 0);\n"
                     "  vec2 c = (vec2(g) + 0.5) * pc.a.w;\n"
                     "  prep(c);\n"
                     "  vec2 va = va_at(c), best = va;\n"
                     "  float sa = sad(c, va), sb = sa, s;\n"
                     "  for (int y = -1; y <= 1; y++)\n"
                     "    for (int x = -1; x <= 1; x++) {\n"
                     "      vec2 v = texelFetch(F, clamp(g + ivec2(x, y), ivec2(0), gs - 1), 0).xy * lsz;\n"
                     "      s = sad(c, v); if (s < sb) { sb = s; best = v; }\n"
                     "    }\n"
                     "  for (int k = 0; k < 2; k++) {\n"
                     "    float h = k == 0 ? 1.0 : 0.5;\n"
                     "    vec2 b0 = best;\n"
                     "    for (int i = 0; i < 4; i++) {\n"
                     "      vec2 w = b0 + h * (i == 0 ? vec2(1.0, 0.0) : i == 1 ? vec2(-1.0, 0.0) : i == 2 ? vec2(0.0, 1.0) : vec2(0.0, -1.0));\n"
                     "      s = sad(c, w); if (s < sb) { sb = s; best = w; }\n"
                     "    }\n"
                     "  }\n"
                     "  bool rep = sb < sa * 0.8 - 0.004;\n"
                     "  o = vec4((rep ? best : va) / lsz, rep ? sb : sa, rep ? 1.0 : 0.0);\n"
                     "}\n");
        return src[k];
    }
    // fg_pick.frag: per luminance pixel, the analytic vector (the camera's there, M; else no motion) against the four
    // nearest blocks' own vectors on the pixel's 3 x 3 (a block's only clearly better, as above): block edges follow
    // the picture instead of the 4-pixel grid. Out: xy = the vector (uv). a.xy = 1 / the luminance's size, a.w = the
    // blocks' stride, b.y = 1: M bound.
    if (!strcmp(name, "fg_pick.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D L;\n"
               "layout(set = 0, binding = 1) uniform sampler2D F;\n"
               "layout(set = 0, binding = 2) uniform sampler2D M;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec2 lsz;\n"
               "float err(vec2 p, vec2 v) {\n"
               "  float s = 0.0;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec2 q = p + vec2(x, y);\n"
               "      s += abs(texture(L, q / lsz).r - texture(L, (q - v) / lsz).g);\n"
               "    }\n"
               "  return s / 9.0;\n"
               "}\n"
               "float err_wide(vec2 p, vec2 v) {\n"  // (5 x 5 luminance pixels, every other one)
               "  float s = 0.0;\n"
               "  for (int y = -2; y <= 2; y += 2)\n"
               "    for (int x = -2; x <= 2; x += 2) {\n"
               "      vec2 q = p + vec2(x, y);\n"
               "      s += abs(texture(L, q / lsz).r - texture(L, (q - v) / lsz).g);\n"
               "    }\n"
               "  return s / 9.0;\n"
               "}\n"
               "void main() {\n"
               "  lsz = 1.0 / pc.a.xy;\n"
               "  vec2 p = gl_FragCoord.xy, va = vec2(0.0);\n"
               "  bool chr = false;\n"
               "  if (pc.b.y > 0.5) {\n"
               "    ivec2 ms = textureSize(M, 0);\n"
               "    vec4 m4 = texelFetch(M, clamp(ivec2(p / lsz * vec2(ms)), ivec2(0), ms - 1), 0);\n"
               "    va = m4.xy * lsz, chr = m4.z > 0.5;\n"
               "  }\n"
               "  ivec2 gs = textureSize(F, 0), g0 = ivec2(floor(p / pc.a.w - 0.5));\n"
               "  vec4 f[4];\n"
               "  bool any_rep = false;\n"
               "  for (int i = 0; i < 4; i++) {\n"
               "    f[i] = texelFetch(F, clamp(g0 + ivec2(i & 1, i >> 1), ivec2(0), gs - 1), 0);\n"
               "    any_rep = any_rep || f[i].w > 0.5;\n"
               "  }\n"
               // (a character: the best fit over a wider patch of the camera's vector (someone standing in the world),
               // none (the player's, which the camera follows) and the blocks' - only a start: chosen alone, the blocks'
               // vectors tore hair, arms and bags into patches; fg_lk refines it into a smooth field)
               "  if (chr) {\n"
               "    vec2 best = va; float eb = err_wide(p, va), e = err_wide(p, vec2(0.0));\n"
               "    if (e < eb) eb = e, best = vec2(0.0);\n"
               "    for (int i = 0; i < 4; i++) { vec2 v = f[i].xy * lsz; e = err_wide(p, v); if (e < eb) eb = e, best = v; }\n"
               "    o = vec4(best / lsz, 0.0, 0.0);\n"
               "    return;\n"
               "  }\n"
               "  float ea = err(p, va), eb = 1e9;\n"
               "  vec2 best = va;\n"
               "  for (int i = 0; any_rep && i < 4; i++) {\n"
               "    if (f[i].w < 0.5) continue;\n"
               "    vec2 v = f[i].xy * lsz; float e = err(p, v); if (e < eb) { eb = e; best = v; }\n"
               "  }\n"
               "  if (!(eb < ea * 0.8 - 0.004)) best = va, eb = ea;\n"
               // (and no motion, where it fits clearly better: the camera's vector is the scene camera's, and what
               // another camera draws - the character creator's figure - went off with it: the body vanished)
               "  if (va != vec2(0.0)) { float e0 = err(p, vec2(0.0)); if (e0 < eb * 0.8 - 0.004) best = vec2(0.0); }\n"
               "  o = vec4(best / lsz, 0.0, 0.0);\n"
               "}\n";
    }
    // fg_cam.frag (ours, after fsr2_mv.frag): the camera's motion at the flow's size, from the scene depth (reversed,
    // infinite far plane) and m = the last frame's view-projection x the inverse of this one's (mv_note_draw's, made on
    // the CPU: one matrix leaves room for the rest). Out: xy = this frame's uv - the last one's (the flow's vectors).
    // a.xy = 1 / the output's size, a.zw = the scene's size in the depth's texels. The analytic vector of fg_match /
    // fg_refine / fg_pick (b.y = 1): what the still world does as the camera turns, which the block search alone missed
    // in fast turns - the in-between images blurred there. z = 1: a character (stencil 0x30 - 0x3F, as fsr2_mv.frag
    // reads it; b.x = 1: the stencil bound) - they move on their own, the camera's vector is wrong for them (the
    // player's character follows the camera: still on screen), so fg_pick and fg_warp treat them apart.
    if (!strcmp(name, "fg_cam.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform usampler2D sten;\n"
               "layout(set = 0, binding = 1) uniform sampler2D dep;\n"
               "layout(set = 0, binding = 2) uniform sampler2D prv;\n"  // (b.y = 1: the frame before's - its z into w)
               "layout(push_constant) uniform P { mat4 m; vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 uv = gl_FragCoord.xy * pc.a.xy;\n"
               "  ivec2 dz = textureSize(dep, 0), q = clamp(ivec2(uv * pc.a.zw), ivec2(0), dz - 1);\n"
               "  float d = max(texelFetch(dep, q, 0).r, 1e-7);\n"
               "  vec4 c = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
               "  vec2 puv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);\n"
               "  float ch = pc.b.x > 0.5 && (texelFetch(sten, q, 0).r & 0xF0u) == 0x30u ? 1.0 : 0.0;\n"
               "  float chp = pc.b.y > 0.5 ? texelFetch(prv, ivec2(gl_FragCoord.xy), 0).z : 0.0;\n"
               // (a point barely in front of the last frame's camera - w near 0 - gave a vector of thousands of pixels:
               // infinite in half floats, and the warp read black there - figures of people in black. A quarter of the
               // picture at most, more is no motion this flow can follow)
               "  vec2 mv = c.w > 1e-4 ? uv - puv : vec2(0.0);\n"
               "  if (any(isnan(mv)) || any(greaterThan(abs(mv), vec2(0.25)))) mv = vec2(0.0);\n"
               "  o = vec4(mv, ch, chp);\n"
               "}\n";
    }
    // fg_lk.frag (ours): the characters' own motion - they move on their own, and no motion vectors come with them -
    // refined per pixel by Lucas-Kanade on the luminance (N's r against N-1's g): from fg_pick's vector, four steps of
    // the 2 x 2 least-squares fit over the pixel's 5 x 5, kept only where they fit better than the start. A window's
    // fit moves neighbouring pixels alike: the blocks' own vectors alone tore hair and clothes into patches, this
    // stretches at worst. Other pixels as fg_pick left them. F = fg_pick's, M = fg_cam's (z: a character); a.xy = 1 /
    // the luminance's size.
    if (!strcmp(name, "fg_lk.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D L;\n"
               "layout(set = 0, binding = 1) uniform sampler2D F;\n"
               "layout(set = 0, binding = 2) uniform sampler2D M;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec2 lsz;\n"
               "float err(vec2 p, vec2 v) {\n"
               "  float s = 0.0;\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      vec2 q = p + vec2(x, y);\n"
               "      s += abs(texture(L, q / lsz).r - texture(L, (q - v) / lsz).g);\n"
               "    }\n"
               "  return s;\n"
               "}\n"
               "void main() {\n"
               "  lsz = 1.0 / pc.a.xy;\n"
               "  vec2 p = gl_FragCoord.xy;\n"
               "  vec4 f = texelFetch(F, ivec2(p), 0);\n"
               "  ivec2 ms = textureSize(M, 0);\n"
               "  if (texelFetch(M, clamp(ivec2(p / lsz * vec2(ms)), ivec2(0), ms - 1), 0).z < 0.5) { o = f; return; }\n"
               "  vec2 v0 = f.xy * lsz, v = v0, dx = vec2(1.0, 0.0) / lsz, dy = vec2(0.0, 1.0) / lsz;\n"
               "  for (int it = 0; it < 4; it++) {\n"
               "    float a11 = 0.0, a12 = 0.0, a22 = 0.0, b1 = 0.0, b2 = 0.0;\n"
               "    for (int y = -2; y <= 2; y++)\n"
               "      for (int x = -2; x <= 2; x++) {\n"
               "        vec2 q = p + vec2(x, y), s = (q - v) / lsz;\n"
               "        float gx = (texture(L, s + dx).g - texture(L, s - dx).g) * 0.5;\n"
               "        float gy = (texture(L, s + dy).g - texture(L, s - dy).g) * 0.5;\n"
               "        float e = texture(L, q / lsz).r - texture(L, s).g;\n"
               "        a11 += gx * gx, a12 += gx * gy, a22 += gy * gy, b1 += gx * e, b2 += gy * e;\n"
               "      }\n"
               "    float det = a11 * a22 - a12 * a12;\n"
               "    if (det < 1e-7) break;\n"
               // (N-1 at q - v - dv ~ N-1 at q - v - grad . dv = N at q: grad . dv = -e, dv = -A^-1 b)
               "    vec2 dv = -vec2(a22 * b1 - a12 * b2, a11 * b2 - a12 * b1) / det;\n"
               "    v += clamp(dv, vec2(-2.0), vec2(2.0));\n"
               "  }\n"
               "  if (any(isnan(v)) || any(greaterThan(abs(v - v0), vec2(12.0)))) v = v0;\n"  // (a fit that ran off: the start)
               "  o = vec4((err(p, v) < err(p, v0) ? v : v0) / lsz, 0.0, 0.0);\n"
               "}\n";
    }
    // fg_cut.frag (ours): one pixel, the mean of what the coarse level's best vectors leave unexplained (fg_match's z,
    // the luminance's mean difference) over all its blocks. A camera cut or a fade - the intro's cutscenes, the jump to
    // the bag - leaves most of the picture unexplained: fg_warp then shows N as it is (an in-between image of two
    // unrelated pictures flashed).
    if (!strcmp(name, "fg_cut.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D B;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 z = textureSize(B, 0);\n"
               "  float s = 0.0;\n"
               "  for (int y = 0; y < z.y; y++)\n"
               "    for (int x = 0; x < z.x; x++) s += texelFetch(B, ivec2(x, y), 0).z;\n"
               "  o = vec4(s / float(z.x * z.y), 0.0, 0.0, 0.0);\n"
               "}\n";
    }
    // fg_median.frag (ours, not letsgo-recomp's: they start from the renderer's motion vectors, a smooth field): the
    // vector median of each pixel's 3 x 3 at a spacing of a.z pixels (the one of the nine nearest the other eight).
    // The pick alone left flat ground and edges speckled - a pixel without a clear match stays still between moving
    // neighbours, and the warp then tears there; run at spacing 1 and 2.
    if (!strcmp(name, "fg_median.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D F;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy), z = textureSize(F, 0) - 1, d = ivec2(int(pc.a.z));\n"
               "  vec2 v[9];\n"
               "  for (int i = 0; i < 9; i++) v[i] = texelFetch(F, clamp(p + (ivec2(i % 3, i / 3) - 1) * d, ivec2(0), z), 0).xy;\n"
               "  float bs = 1e30; vec2 best = v[4];\n"
               "  for (int i = 0; i < 9; i++) {\n"
               "    float s = 0.0;\n"
               "    for (int j = 0; j < 9; j++) { vec2 e = abs(v[i] - v[j]); s += e.x + e.y; }\n"
               "    if (s < bs) { bs = s; best = v[i]; }\n"
               "  }\n"
               "  o = vec4(best, 0.0, 0.0);\n"
               "}\n";
    }
    // fg_warp.frag: the in-between image at t (a.x, 0..1 from N-1 to N) at the game image's size: c0 = N-1, c1 = N,
    // fn = the vectors. Each pixel q: the point p of N that is at q at t (q + (1 - t) v(p) = p, three fixed-point
    // steps), checked with its own vector (within 1.5 of the field's pixels: found) - then N there mixed by t with N-1
    // where the point was (p - v). Not found (uncovered since t): N-1 along q's own vector. Both images Catmull-Rom (5
    // bilinear taps): bilinear, the moved image was softer than the real ones. b.xy = 1 / the output's size, b.z = 1:
    // tinted (LARECOMP_FG_DEBUG: red where moved, blue not found).
    if (!strcmp(name, "fg_warp.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D c0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D c1;\n"
               "layout(set = 0, binding = 2) uniform sampler2D fn;\n"
               "layout(set = 0, binding = 3) uniform sampler2D hud;\n"
               "layout(set = 0, binding = 4) uniform sampler2D cut;\n"  // (fg_cut's: above a.z, N as it is)
               "layout(set = 0, binding = 5) uniform sampler2D cm;\n"   // (fg_cam's: z the characters)
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               // (a.y = 1: c0 / c1 are the pictures before the HUD and hud is N as shown - where they differ, the HUD:
               // N's shown pixel as it is, alpha 1 so fg_sharp leaves it. The pixel alone: its 3 x 3 was 18 reads a
               // pixel, too dear for a phone's GPU already busy at 30 fps)
               "bool is_hud(ivec2 p) {\n"
               "  vec3 d = abs(texelFetch(hud, p, 0).rgb - texelFetch(c1, p, 0).rgb);\n"
               "  return max(d.r, max(d.g, d.b)) > 0.012;\n"
               "}\n"
               "vec3 cr(sampler2D h, vec2 uv) {\n"
               "  vec2 sz = vec2(textureSize(h, 0)), p = uv * sz, c1 = floor(p - 0.5) + 0.5, f = p - c1;\n"
               "  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
               "  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
               "  vec2 w12 = w1 + w2, t0 = (c1 - 1.0) / sz, t3 = (c1 + 2.0) / sz, t12 = (c1 + w2 / w12) / sz;\n"
               "  vec3 r = texture(h, vec2(t12.x, t0.y)).rgb * w12.x * w0.y + texture(h, vec2(t0.x, t12.y)).rgb * w0.x * w12.y\n"
               "         + texture(h, t12).rgb * w12.x * w12.y + texture(h, vec2(t3.x, t12.y)).rgb * w3.x * w12.y\n"
               "         + texture(h, vec2(t12.x, t3.y)).rgb * w12.x * w3.y;\n"
               "  float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;\n"
               "  return clamp(r / ws, 0.0, 1.0);\n"
               "}\n"
               // (a vector that is not one - not a number, infinite, past a quarter of the picture - none: read past the
               // images, it showed black)
               "vec2 at(vec2 uv) {\n"
               "  ivec2 z = textureSize(fn, 0);\n"
               "  vec2 v = texelFetch(fn, clamp(ivec2(uv * vec2(z)), ivec2(0), z - 1), 0).xy;\n"
               "  return any(isnan(v)) || any(greaterThan(abs(v), vec2(0.25))) ? vec2(0.0) : v;\n"
               "}\n"
               "bool inside(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))); }\n"
               "void main() {\n"
               "  vec2 q = gl_FragCoord.xy * pc.b.xy, fs = vec2(textureSize(fn, 0));\n"
               "  float t = pc.a.x;\n"
               "  if (pc.a.z > 0.0 && texelFetch(cut, ivec2(0), 0).r > pc.a.z) {\n"
               "    o = vec4(texelFetch(hud, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);\n"
               "    return;\n"
               "  }\n"
               "  if (pc.a.y > 0.5 && is_hud(ivec2(gl_FragCoord.xy))) {\n"
               "    vec3 hc = texelFetch(hud, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "    o = vec4(pc.b.z > 0.5 ? mix(hc, vec3(0.0, 1.0, 0.0), 0.5) : hc, 1.0);\n"
               "    return;\n"
               "  }\n"
               // (a.w = 2, LARECOMP_FG_CHARS=0: characters not moved - a character in N here, or next to one, fg_cam's z
               // on its 3 x 3: N's pixel as it is, like the HUD; sharp, at the game's own 30 fps)
               "  if (pc.a.w > 1.5) {\n"
               "    ivec2 cz = textureSize(cm, 0), cp = ivec2(q * vec2(cz));\n"
               "    bool near_chr = false;\n"
               "    for (int y = -1; y <= 1; y++)\n"
               "      for (int x = -1; x <= 1; x++) near_chr = near_chr || texelFetch(cm, clamp(cp + ivec2(x, y), ivec2(0), cz - 1), 0).z > 0.5;\n"
               "    if (near_chr) {\n"
               "      vec3 nc = texelFetch(c1, ivec2(gl_FragCoord.xy), 0).rgb;\n"
               "      o = vec4(pc.b.z > 0.5 ? mix(nc, vec3(1.0, 1.0, 0.0), 0.5) : nc, 1.0);\n"
               "      return;\n"
               "    }\n"
               "  }\n"
               "  vec2 v0 = at(q);\n"
               "  vec2 c = q + (1.0 - t) * v0;\n"
               "  for (int i = 0; i < 3; i++) c = q + (1.0 - t) * at(c);\n"
               "  vec2 g = at(c), e = (q + (1.0 - t) * g - c) * fs;\n"
               "  vec3 col, dbg = vec3(0.0);\n"
               "  if (dot(e, e) < 2.25 && inside(c)) {\n"
               "    vec2 r = c - g;\n"
               "    vec3 n1 = cr(c1, c);\n"
               // (mixed only where N-1 there agrees: where the vector is wrong the mix was a double image - the
               // in-between images looked blurred - and N's pixel moved alone stays sharp)
               // (the point of N is a character - fg_cam's z, a.w >= 1: bound. Moved (fg_lk's field): N's pixel moved
               // alone, no N-1 mixed in - a double image on hair and clothes. Not moved (a.w = 2): they stay where N has
               // them (above), so not a second one here - N's own pixel at q; a person in a camera turn showed twice)
               "    bool chr = pc.a.w > 0.5 && texelFetch(cm, clamp(ivec2(c * vec2(textureSize(cm, 0))), ivec2(0), textureSize(cm, 0) - 1), 0).z > 0.5;\n"
               "    if (chr) col = pc.a.w > 1.5 ? cr(c1, q) : n1;\n"
               "    else if (inside(r)) {\n"
               "      vec3 p0 = cr(c0, r), d = abs(p0 - n1);\n"
               "      col = mix(mix(p0, n1, t), n1, smoothstep(0.04, 0.12, max(d.r, max(d.g, d.b))));\n"
               "    } else col = n1;\n"
               "    if (g != vec2(0.0)) dbg.r = 1.0;\n"
               "    if (chr) dbg = vec3(1.0, 1.0, 0.0);\n"  // (debug: a character, yellow)
               "  } else {\n"
               // (not found. On a character in N: N's pixel there - the player's, which the camera follows, broke up in
               // patches of N-1 along its own vector. Elsewhere it was uncovered since t - the ground behind someone
               // walking: N-1 along the still world's motion (the camera's, fg_cam), not the character's vector next
               // to it that spread there (a halo round the player))
               "    vec4 cq = pc.a.w > 0.5 ? texelFetch(cm, clamp(ivec2(q * vec2(textureSize(cm, 0))), ivec2(0), textureSize(cm, 0) - 1), 0) : vec4(v0, 0.0, 0.0);\n"
               // (a character in N at q and the fixed point not found: N moved by q's own vector - fg_lk's field is
               // smooth there. N's pixel unmoved instead, next to moved ones, left seams across the player's body)
               "    if (cq.z > 0.5) col = pc.a.w > 1.5 ? cr(c1, q) : cr(c1, q + (1.0 - t) * v0);\n"
               "    else {\n"
               "      vec2 vb = pc.a.w > 0.5 ? cq.xy : v0, r = q - t * vb;\n"
               // (N-1 there a character - the camera image's w, b.w = 1: kept from the frame before - N's pixel instead:
               // that was the person where they stood in N-1, a ghost of their legs and shoes)
               "      bool chp = pc.b.w > 0.5 && inside(r) && texelFetch(cm, clamp(ivec2(r * vec2(textureSize(cm, 0))), ivec2(0), textureSize(cm, 0) - 1), 0).w > 0.5;\n"
               "      col = chp ? cr(c1, q) : inside(r) ? cr(c0, r) : cr(c1, q + (1.0 - t) * vb);\n"
               "    }\n"
               "    dbg.b = 1.0;\n"
               "  }\n"
               "  if (pc.b.z > 0.5) col = mix(col, dbg, 0.5 * max(dbg.r, max(dbg.g, dbg.b)));\n"
               "  o = vec4(col, 0.0);\n"
               "}\n";
    }
    // fg_sharp.frag: the in-between image sharpened back (a.x: the strength; mixing N-1 and N a fraction of a pixel
    // apart softens what both show): unsharp on the 4 neighbours, clamped to the 3 x 3's range (no halos).
    if (!strcmp(name, "fg_sharp.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy), z = textureSize(t, 0) - 1;\n"
               "  vec4 c4 = texelFetch(t, p, 0);\n"
               "  if (c4.a > 0.5) { o = vec4(c4.rgb, 1.0); return; }\n"  // (the HUD, fg_warp's: as it is)
               "  vec3 c = c4.rgb, mn = c, mx = c, s = vec3(0.0);\n"
               "  for (int y = -1; y <= 1; y++)\n"
               "    for (int x = -1; x <= 1; x++) {\n"
               "      if (x == 0 && y == 0) continue;\n"
               "      vec3 n = texelFetch(t, clamp(p + ivec2(x, y), ivec2(0), z), 0).rgb;\n"
               "      mn = min(mn, n); mx = max(mx, n);\n"
               "      if (x == 0 || y == 0) s += n;\n"
               "    }\n"
               "  o = vec4(clamp(c + pc.a.x * (c - s * 0.25), mn, mx), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    // Box downscale (exec.c post_process): the image is larger than its place in the window (internal resolution
    // above the window's). a = source texels per output pixel (x, y), source size. Every source texel under the
    // output pixel counts, weighted by how much of it is covered (non-integer scales); at most 9 x 9 of them.
    if (!strcmp(name, "boxdown.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 lo = floor(gl_FragCoord.xy) * pc.a.xy, hi = lo + pc.a.xy;\n"
               "  ivec2 i0 = ivec2(floor(lo)), i1 = min(ivec2(ceil(hi)) - 1, i0 + 8), sz = ivec2(pc.a.zw) - 1;\n"
               "  vec3 s = vec3(0.0);\n"
               "  float n = 0.0;\n"
               "  for (int y = i0.y; y <= i1.y; y++) {\n"
               "    float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));\n"
               "    for (int x = i0.x; x <= i1.x; x++) {\n"
               "      float w = wy * (min(hi.x, float(x + 1)) - max(lo.x, float(x)));\n"
               "      s += texelFetch(t, clamp(ivec2(x, y), ivec2(0), sz), 0).rgb * w;\n"
               "      n += w;\n"
               "    }\n"
               "  }\n"
               "  o = vec4(s / max(n, 1e-6), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    // GTAO (exec.c gtao_*): ambient occlusion from Pokémon Z-A's scene depth, after XeGTAO (Intel, MIT licence). The
    // push constants (all three passes): m0..m3 the game's own reconstruction - world position (homogeneous) =
    // ndc.x * m0 + ndc.y * m1 + depth * m2 + m3 -, cam the camera's world position, prm (radius in metres, intensity,
    // ndc y sign, frame), sz (AO image size, depth image size), prj (tan of the half field of view x / y).
    static const char gtao_common[] =
        "#version 450\n"
        "layout(push_constant) uniform P { vec4 m0, m1, m2, m3, cam, prm, sz, prj; } pc;\n"
        "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
        "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
        "layout(location = 0) out vec4 o;\n"
        "const float PI = 3.14159265, HALF_PI = 1.57079633;\n"
        // depth at a uv of the full-resolution depth image (nearest texel: depth isn't filtered)
        "float depth_at(sampler2D d, vec2 uv) {\n"
        "  ivec2 s = ivec2(pc.sz.zw);\n"
        "  return texelFetch(d, clamp(ivec2(uv * pc.sz.zw), ivec2(0), s - 1), 0).r;\n"
        "}\n"
        "vec4 wpos4(vec2 uv, float d) {\n"
        "  vec2 n = vec2(uv.x * 2.0 - 1.0, (uv.y * 2.0 - 1.0) * pc.prm.z);\n"
        "  return n.x * pc.m0 + n.y * pc.m1 + d * pc.m2 + pc.m3;\n"
        "}\n"
        "vec3 wpos(vec2 uv, float d) { vec4 p = wpos4(uv, d); return p.xyz / p.w; }\n";
    if (!strcmp(name, "gtao.frag")) {  // t0 = the scene depth; out .r = visibility (1 = open)
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "vec3 pos_at(vec2 uv) { return wpos(uv, depth_at(t0, uv)); }\n"
                "float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
                "float facos(float x) { float r = -0.156583 * abs(x) + HALF_PI; r *= sqrt(1.0 - abs(x)); return x >= 0.0 ? r : PI - r; }\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  float d = depth_at(t0, uv);\n"
                "  vec4 P4 = wpos4(uv, d);\n"
                "  if (abs(P4.w) < 1e-7) { o = vec4(1.0); return; }\n"
                "  vec3 P = P4.xyz / P4.w;\n"
                "  float dist = length(pc.cam.xyz - P);\n"
                "  if (!(dist > 0.05 && dist < 400.0)) { o = vec4(1.0); return; }\n"  // (sky, far: open)
                "  vec3 V = (pc.cam.xyz - P) / dist;\n"
                // the normal from the depth: on each axis the neighbour closer in depth (no smearing across edges)
                "  vec2 px = 1.0 / pc.sz.zw;\n"
                "  vec3 Pl = pos_at(uv - vec2(px.x * 2.0, 0.0)), Pr = pos_at(uv + vec2(px.x * 2.0, 0.0));\n"
                "  vec3 Pu = pos_at(uv - vec2(0.0, px.y * 2.0)), Pd = pos_at(uv + vec2(0.0, px.y * 2.0));\n"
                "  vec3 dx = length(Pr - P) < length(P - Pl) ? Pr - P : P - Pl;\n"
                "  vec3 dy = length(Pd - P) < length(P - Pu) ? Pd - P : P - Pu;\n"
                "  vec3 N = normalize(cross(dy, dx));\n"
                "  if (dot(N, V) < 0.0) N = -N;\n"
                "  if (pc.prj.z == 2.0) { o = vec4(N * 0.5 + 0.5, 1.0); return; }\n"  // (debug: the normals)
                "  if (pc.prj.z == 3.0) { o = vec4(vec3(fract(dist / 5.0)), 1.0); return; }\n"  // (debug: 5 m bands)
                "  float radius = pc.prm.x;\n"
                "  vec2 rUV = 0.5 * radius / (dist * pc.prj.xy);\n"  // (the radius in uv at that distance)
                "  if (rUV.x * pc.sz.x < 1.0) { o = vec4(1.0); return; }\n"
                "  float falloffRange = 0.615 * radius, falloffFrom = radius * (1.0 - 0.615);\n"
                "  float falloffMul = -1.0 / falloffRange, falloffAdd = falloffFrom / falloffRange + 1.0;\n"
                "  float n0 = ign(gl_FragCoord.xy + pc.prm.w * 5.588238), n1 = ign(gl_FragCoord.yx * 1.37 + 17.0 + pc.prm.w * 3.1);\n"
                "  int SLICES = int(pc.prj.w), STEPS = SLICES * 2;\n"  // (quality: 2 / 3 / 4 slices)
                "  float vis = 0.0;\n"
                "  for (int s = 0; s < SLICES; s++) {\n"
                "    float phi = (float(s) + n0) * PI / float(SLICES);\n"
                "    vec2 dir = vec2(cos(phi), sin(phi));\n"
                // the slice's plane holds the view ray and the screen direction: its 3D direction from a point at the
                // same depth along that screen direction
                "    vec3 D3 = wpos(uv + dir * rUV * 0.25, d) - P;\n"
                "    vec3 orthoDir = D3 - V * dot(D3, V);\n"
                "    if (dot(orthoDir, orthoDir) < 1e-12) continue;\n"
                "    orthoDir = normalize(orthoDir);\n"
                "    vec3 axis = normalize(cross(orthoDir, V));\n"
                "    vec3 projN = N - axis * dot(N, axis);\n"
                "    float projNLen = length(projN);\n"
                "    float signN = sign(dot(orthoDir, projN));\n"
                "    float cosN = clamp(dot(projN, V) / max(projNLen, 1e-6), 0.0, 1.0);\n"
                "    float n = signN * facos(cosN);\n"
                "    float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);\n"
                "    float hc0 = low0, hc1 = low1;\n"
                "    for (int j = 0; j < STEPS; j++) {\n"
                "      float t = (float(j) + fract(n1 + float(j) * 0.618034)) / float(STEPS);\n"
                "      t = t * t;\n"
                "      vec2 off = dir * (rUV * t + 1.0 / pc.sz.xy);\n"
                "      vec3 d0 = pos_at(uv + off) - P, d1 = pos_at(uv - off) - P;\n"
                // (XeGTAO's thin occluder compensation: a sample far in front of the point - a grass blade, a railing -
                // counts as farther away along the view axis, so it occludes less: thin things aren't solid walls)
                "      float l0 = length(d0 + V * dot(d0, V) * 2.0), l1 = length(d1 + V * dot(d1, V) * 2.0);\n"
                "      float sh0 = dot(normalize(d0 + 1e-9), V), sh1 = dot(normalize(d1 + 1e-9), V);\n"  // (the true direction)
                "      float w0 = clamp(l0 * falloffMul + falloffAdd, 0.0, 1.0), w1 = clamp(l1 * falloffMul + falloffAdd, 0.0, 1.0);\n"
                "      hc0 = max(hc0, mix(low0, sh0 - 0.1, w0));\n"
                "      hc1 = max(hc1, mix(low1, sh1 - 0.1, w1));\n"
                "    }\n"
                "    float h0 = -facos(hc1), h1 = facos(hc0);\n"
                "    h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);\n"
                "    h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);\n"
                "    float sinN = sin(n);\n"
                "    float a0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;\n"
                "    float a1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;\n"
                "    vis += projNLen * (a0 + a1);\n"
                "  }\n"
                "  vis = clamp(vis / float(SLICES), 0.0, 1.0);\n"
                "  vis = pow(vis, 1.6);\n"  // (XeGTAO's final power, softened: the game's lighting is soft already)
                "  o = vec4(vis, 0.0, 0.0, 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "gtao_blur.frag")) {  // t0 = raw AO, t1 = the scene depth: a 5x5 depth-aware average
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  vec4 c4 = wpos4(uv, depth_at(t1, uv));\n"
                "  if (abs(c4.w) < 1e-7) { o = vec4(1.0); return; }\n"
                "  float cd = length(c4.xyz / c4.w - pc.cam.xyz);\n"
                "  float sum = 0.0, wsum = 0.0;\n"
                "  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
                "    vec2 u = uv + vec2(x, y) / pc.sz.xy;\n"
                "    vec4 p4 = wpos4(u, depth_at(t1, u));\n"
                "    float sd = abs(p4.w) < 1e-7 ? 1e9 : length(p4.xyz / p4.w - pc.cam.xyz);\n"
                "    float w = exp(-abs(sd - cd) / (0.03 * cd + 0.01)) * (1.0 / (1.0 + float(x * x + y * y) * 0.25));\n"
                "    sum += texelFetch(t0, clamp(ivec2(gl_FragCoord.xy) + ivec2(x, y), ivec2(0), ivec2(pc.sz.xy) - 1), 0).r * w;\n"
                "    wsum += w;\n"
                "  }\n"
                "  o = vec4(sum / max(wsum, 1e-6), 0.0, 0.0, 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "gtao_apply.frag")) {  // t0 = the filtered AO (half size, bilinear): drawn with a multiply blend
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.zw;\n"  // (sz.zw: the lit image's size here)
                // depth-aware upsampling: of the 4 half-size AO texels around, those at this pixel's distance count
                // (a background texel's AO doesn't spill onto a character's arm in front of it)
                "  vec4 c4 = wpos4(uv, depth_at(t1, uv));\n"
                "  float cd = abs(c4.w) < 1e-7 ? 1e9 : length(c4.xyz / c4.w - pc.cam.xyz);\n"
                "  vec2 hp = uv * pc.sz.xy - 0.5, b = floor(hp), fr = hp - b;\n"
                "  float v = 0.0, ws = 0.0;\n"
                "  for (int k = 0; k < 4; k++) {\n"
                "    vec2 o2 = vec2(k & 1, k >> 1), tc = b + o2;\n"
                "    vec2 tuv = (tc + 0.5) / pc.sz.xy;\n"
                "    vec4 t4 = wpos4(tuv, depth_at(t1, tuv));\n"
                "    float td = abs(t4.w) < 1e-7 ? 1e9 : length(t4.xyz / t4.w - pc.cam.xyz);\n"
                "    float bw = (o2.x > 0.5 ? fr.x : 1.0 - fr.x) * (o2.y > 0.5 ? fr.y : 1.0 - fr.y) + 1e-3;\n"
                "    float w = bw * exp(-abs(td - cd) / (0.02 * cd + 0.02));\n"
                "    v += w * texelFetch(t0, clamp(ivec2(tc), ivec2(0), ivec2(pc.sz.xy) - 1), 0).r; ws += w;\n"
                "  }\n"
                "  v = ws > 1e-5 ? v / ws : texture(t0, uv).r;\n"
                "  if (pc.prj.z >= 2.0) { o = vec4(texture(t0, uv).rgb, 1.0); return; }\n"
                "  o = pc.prj.z != 0.0 ? vec4(vec3(v), 1.0) : vec4(vec3(max(mix(1.0, v, pc.prm.y), 0.0)), 1.0);\n"  // (strength above 1: a stronger darkening, never below black)
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    // ---- screen-space indirect light (exec.c ssgi_apply), with GTAO's camera and push constants (gtao_common):
    // prm.x the reach in metres, prm.y the strength, prj.z debug (1: the bounce light alone), prj.w the slices.
    // ssgi.frag (half size; t0 = the scene depth, t1 = the lit image): per slice and side, marching away from the pixel,
    // each surface seen above the horizon so far lights it with its own lit colour over the angle it newly covers
    // (cos-weighted: the band between the old and the new horizon, cos a_old - cos a_new) - one bounce of the game's
    // light from what is on screen. Sky (no depth) and things past the reach give nothing: the game's own ambient
    // light covers them. Out .rgb: the cos-weighted mean incoming light (x the albedo: the light it reflects).
    if (!strcmp(name, "ssgi.frag")) {
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "vec3 pos_at(vec2 uv) { return wpos(uv, depth_at(t0, uv)); }\n"
                "float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  float d = depth_at(t0, uv);\n"
                "  vec4 P4 = wpos4(uv, d);\n"
                "  if (abs(P4.w) < 1e-7) { o = vec4(0.0); return; }\n"
                "  vec3 P = P4.xyz / P4.w;\n"
                "  float dist = length(pc.cam.xyz - P);\n"
                "  if (!(dist > 0.05 && dist < 300.0)) { o = vec4(0.0); return; }\n"
                "  vec3 V = (pc.cam.xyz - P) / dist;\n"
                "  vec2 px = 1.0 / pc.sz.zw;\n"
                "  vec3 Pl = pos_at(uv - vec2(px.x * 2.0, 0.0)), Pr = pos_at(uv + vec2(px.x * 2.0, 0.0));\n"
                "  vec3 Pu = pos_at(uv - vec2(0.0, px.y * 2.0)), Pd = pos_at(uv + vec2(0.0, px.y * 2.0));\n"
                "  vec3 dx = length(Pr - P) < length(P - Pl) ? Pr - P : P - Pl;\n"
                "  vec3 dy = length(Pd - P) < length(P - Pu) ? Pd - P : P - Pu;\n"
                "  vec3 N = normalize(cross(dy, dx));\n"
                "  if (dot(N, V) < 0.0) N = -N;\n"
                "  float radius = pc.prm.x;\n"
                "  vec2 rUV = 0.5 * radius / (dist * pc.prj.xy);\n"
                "  if (rUV.x * pc.sz.x < 2.0) { o = vec4(0.0); return; }\n"
                "  rUV = min(rUV, vec2(0.25));\n"  // (close up: at most a quarter of the screen)
                "  float n0 = ign(gl_FragCoord.xy), n1 = ign(gl_FragCoord.yx * 1.37 + 17.0);\n"
                "  int SLICES = int(pc.prj.w), STEPS = SLICES * 3;\n"
                "  vec3 sum = vec3(0.0);\n"
                "  for (int s = 0; s < SLICES; s++) {\n"
                "    float phi = (float(s) + n0) * PI / float(SLICES);\n"
                "    vec2 dir = vec2(cos(phi), sin(phi));\n"
                "    for (int side = -1; side <= 1; side += 2) {\n"
                "      float hs = 0.0;\n"  // (the horizon so far: the sine of its elevation above the surface)
                "      for (int j = 0; j < STEPS; j++) {\n"
                "        float t = (float(j) + fract(n1 + float(j) * 0.618034)) / float(STEPS);\n"
                "        t = t * t;\n"
                "        vec2 su = uv + float(side) * dir * (rUV * t + 2.0 / pc.sz.xy);\n"
                "        if (any(lessThan(su, vec2(0.0))) || any(greaterThan(su, vec2(1.0)))) break;\n"
                "        float sd = depth_at(t0, su);\n"
                "        vec4 Q4 = wpos4(su, sd);\n"
                "        if (abs(Q4.w) < 1e-7) continue;\n"
                "        vec3 dq = Q4.xyz / Q4.w - P;\n"
                "        float l = length(dq);\n"
                "        if (l < 1e-3 || l > radius) continue;\n"
                "        float e = dot(dq / l, N);\n"
                "        if (e <= hs) continue;\n"
                "        float band = sqrt(max(1.0 - hs * hs, 0.0)) - sqrt(max(1.0 - e * e, 0.0));\n"
                "        float fall = 1.0 - l / radius;\n"
                "        sum += textureLod(t1, su, 0.0).rgb * band * fall;\n"
                "        hs = e;\n"
                "      }\n"
                "    }\n"
                "  }\n"
                "  o = vec4(sum / float(2 * SLICES), 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "ssgi_blur.frag")) {  // t0 = the raw bounce light, t1 = the scene depth: a 5x5 depth-aware average
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  vec4 c4 = wpos4(uv, depth_at(t1, uv));\n"
                "  if (abs(c4.w) < 1e-7) { o = vec4(0.0); return; }\n"
                "  float cd = length(c4.xyz / c4.w - pc.cam.xyz);\n"
                "  vec3 sum = vec3(0.0);\n"
                "  float wsum = 0.0;\n"
                "  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
                "    vec2 u = uv + vec2(x, y) / pc.sz.xy;\n"
                "    vec4 p4 = wpos4(u, depth_at(t1, u));\n"
                "    float sd = abs(p4.w) < 1e-7 ? 1e9 : length(p4.xyz / p4.w - pc.cam.xyz);\n"
                "    float w = exp(-abs(sd - cd) / (0.03 * cd + 0.01)) * (1.0 / (1.0 + float(x * x + y * y) * 0.25));\n"
                "    sum += texelFetch(t0, clamp(ivec2(gl_FragCoord.xy) + ivec2(x, y), ivec2(0), ivec2(pc.sz.xy) - 1), 0).rgb * w;\n"
                "    wsum += w;\n"
                "  }\n"
                "  o = vec4(sum / max(wsum, 1e-6), 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    // t0 = the filtered bounce light (half size), t1 = the scene depth, t2 = the G-buffer's albedo: drawn with an
    // additive blend onto the lit image (debug: the bounce light alone, replacing it)
    if (!strcmp(name, "ssgi_apply.frag")) {
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.zw;\n"
                "  vec4 c4 = wpos4(uv, depth_at(t1, uv));\n"
                "  float cd = abs(c4.w) < 1e-7 ? 1e9 : length(c4.xyz / c4.w - pc.cam.xyz);\n"
                "  vec2 hp = uv * pc.sz.xy - 0.5, b = floor(hp), fr = hp - b;\n"
                "  vec3 v = vec3(0.0);\n"
                "  float ws = 0.0;\n"
                "  for (int k = 0; k < 4; k++) {\n"
                "    vec2 o2 = vec2(k & 1, k >> 1), tc = b + o2;\n"
                "    vec2 tuv = (tc + 0.5) / pc.sz.xy;\n"
                "    vec4 t4 = wpos4(tuv, depth_at(t1, tuv));\n"
                "    float td = abs(t4.w) < 1e-7 ? 1e9 : length(t4.xyz / t4.w - pc.cam.xyz);\n"
                "    float bw = (o2.x > 0.5 ? fr.x : 1.0 - fr.x) * (o2.y > 0.5 ? fr.y : 1.0 - fr.y) + 1e-3;\n"
                "    float w = bw * exp(-abs(td - cd) / (0.02 * cd + 0.02));\n"
                "    v += w * texelFetch(t0, clamp(ivec2(tc), ivec2(0), ivec2(pc.sz.xy) - 1), 0).rgb; ws += w;\n"
                "  }\n"
                "  v = ws > 1e-5 ? v / ws : texture(t0, uv).rgb;\n"
                "  if (pc.prj.z != 0.0) { o = vec4(v, 1.0); return; }\n"
                "  o = vec4(texture(t2, uv).rgb * v * pc.prm.y, 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    // ---- water reflections (exec.c water_apply), with GTAO's camera (gtao_common; sz.xy = sz.zw = the image's size):
    // t0 = the depth now (the water's surface in it), t1 = the depth before the water, t2 = the lit image before it,
    // t3 = the lighting's reflection cube (where the ray leaves the picture).
    // Where the water drew (its depth in front of the one before), the mirror ray from its surface is marched in the
    // world and projected into the picture (the camera's view-projection: the inverse of the depth -> world matrix)
    // until it passes behind the scene without the water: that colour is what the water reflects, blended over it with
    // Schlick's Fresnel (a little straight down, most at grazing angles) times prm.x, the strength. Rays that leave the
    // picture fade out at its edges; past the scene, the sky where it is on screen. prj.z 1: the reflection alone.
    // water_ssr2.frag (exec.c blend kind 6, dual-source: colour = o + dst x o1) also does what prm.y's bits ask: 1 the
    // depth (the light through the water fades per channel with the path to the floor - the depth before the water -,
    // red first, times prj.x; the water's own colour, lit by the cube's sky, fills in), 2 caustics on that floor (two
    // warped wave layers, brightest where they cancel, times prj.y and the floor's own light), 4 the vivid look.
    if (!strcmp(name, "water_ssr.frag") || !strcmp(name, "water_ssr2.frag")) {
        *stage = STAGE_FRAGMENT;
        bool dual = name[9] == '2';
        static char *srcs[2];
        char **src = &srcs[dual];
        if (!*src) {
            static const char body[] =
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(set = 0, binding = 3) uniform samplerCube t3;\n"
                "layout(set = 0, binding = 4) uniform sampler2D t4;\n"
                "layout(set = 0, binding = 5) uniform sampler2D t5;\n"
                // (the mirror's depth and SSPR's distance, nearest: filtered, a silhouette mixes a real value with the cleared 0)
                "vec4 fetch_at(sampler2D t, vec2 uv) {\n"
                "  ivec2 s = textureSize(t, 0);\n"
                "  return texelFetch(t, clamp(ivec2(uv * vec2(s)), ivec2(0), s - 1), 0);\n"
                "}\n"
                "vec2 to_uv(mat4 vp, vec3 w, out float dz) {\n"
                "  vec4 c = vp * vec4(w, 1.0);\n"
                "  dz = c.z / c.w;\n"
                "  return vec2(c.x / c.w * 0.5 + 0.5, c.y / c.w * pc.prm.z * 0.5 + 0.5);\n"
                "}\n"
                // the ripples' slope (d height / dx, dz): two layers of three waves each, drifting in opposite
                // directions at different speeds and sizes, so the pattern doesn't visibly repeat
                "vec2 ripple_slope(vec2 p, float t) {\n"
                "  vec2 s = vec2(0.0);\n"
                "  const vec2 d[6] = vec2[](vec2(0.96, 0.28), vec2(0.73, 0.68), vec2(0.99, -0.13),\n"
                "                           vec2(-0.35, -0.94), vec2(-0.80, -0.60), vec2(0.10, -0.99));\n"
                "  const float f[6] = float[](5.3, 7.9, 11.4, 6.7, 9.6, 13.8), v[6] = float[](2.1, 2.6, 3.3, -1.7, -2.2, -2.9);\n"
                "  for (int i = 0; i < 6; i++) s += d[i] * cos(dot(p, d[i]) * f[i] + t * v[i]) * pow(f[i], -0.2);\n"
                "  return s;\n"
                "}\n"
                // caustics: cells around drifting points (warped), bright along their borders - where the distances to
                // the nearest two points meet - like the web of light under real ripples; two sizes
                "float caustic_layer(vec2 p, float t) {\n"
                "  p += 0.25 * vec2(sin(p.y * 1.7 + t * 0.6), sin(p.x * 1.5 - t * 0.5));\n"
                "  vec2 i = floor(p), f = fract(p);\n"
                "  float d1 = 8.0, d2 = 8.0;\n"
                "  for (int y = -1; y <= 1; y++)\n"
                "    for (int x = -1; x <= 1; x++) {\n"
                "      vec2 g = vec2(x, y), c = i + g;\n"
                "      vec2 h = fract(sin(vec2(dot(c, vec2(127.1, 311.7)), dot(c, vec2(269.5, 183.3)))) * 43758.5453);\n"
                "      float d = length(g + 0.5 + 0.4 * sin(t * 0.8 + 6.2831 * h) - f);\n"
                "      if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;\n"
                "    }\n"
                "  return pow(clamp(1.0 - (d2 - d1) * 2.5, 0.0, 1.0), 4.0);\n"
                "}\n"
                "float caustic(vec2 p, float t) { return 0.65 * caustic_layer(p * 1.8, t) + 0.35 * caustic_layer(p * 3.1 + 5.0, t * 1.2); }\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  float dn = depth_at(t0, uv), db = depth_at(t1, uv);\n"
                "  if (!(dn > db + 1e-7)) discard;\n"  // (no water here: reversed depth, the nearer the larger)
                "  mat4 vp = inverse(mat4(pc.m0, pc.m1, pc.m2, pc.m3));\n"
                "  vec3 P = wpos(uv, dn);\n"
                "  vec3 V = normalize(pc.cam.xyz - P);\n"
                // (Z-A's water is level: straight up - the depth's own slope left thin lines at its triangles' seams -
                // tilted by gentle moving ripples, prm.w the time)
                "  float tm = pc.prm.w;\n"
                "  vec2 rs = ripple_slope(P.xz, tm);\n"
                "  vec3 N = normalize(vec3(-rs.x * 0.006, 1.0, -rs.y * 0.006));\n"
                "  vec3 R = reflect(-V, N);\n"
                "  float F = 0.05 + 0.95 * pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 5.0);\n"
                "  float t = 0.1, found = 0.0, dz;\n"
                "  vec2 hit = uv, occ_q = uv;\n"
                "  float n_after = 0.0, n_hidden = 0.0, run = 0.0;\n"  // (steps since it first went behind something; of them, hidden)
                // the mirror image (prm.y bit 8; prj.w its plane - water at another level goes on below): a point of the
                // plane shows at its own uv there. The ripples: the ray's point at the distance the mirror shows, mirrored
                // and projected with the normal camera. Nothing drawn there: the screen's search, as without the mirror
                "  bool mir = false;\n"
                "  vec3 mcol = vec3(0.0);\n"
                "  int fl0 = int(pc.prm.y + 0.5);\n"
                "  if ((fl0 & 24) != 0 && abs(P.y - pc.prj.w) < 0.15) {\n"
                "    float dist = -1.0;\n"  // (8: t5 the mirror's depth; 16, SSPR: t4's alpha the distance from the camera)
                "    if ((fl0 & 8) != 0) { float dm = fetch_at(t5, uv).r; if (dm > 0.0) dist = length(wpos(uv, dm) - P); }\n"
                "    else { float L = fetch_at(t4, uv).a; if (L > 0.0) dist = max(L - length(P - pc.cam.xyz), 0.0); }\n"
                "    if (dist >= 0.0) {\n"
                "      vec3 T = P + R * min(dist, 200.0);\n"
                "      T.y = 2.0 * pc.prj.w - T.y;\n"
                "      vec2 q = clamp(to_uv(vp, T, dz), vec2(0.0), vec2(1.0));\n"
                // (SSPR's holes: all four texels filled, or the search - a filtered read would darken towards them)
                "      if ((fl0 & 8) != 0 ? fetch_at(t5, q).r > 0.0 : all(greaterThan(textureGather(t4, q, 3), vec4(0.0)))) {\n"
                "        mir = true;\n"
                "        mcol = textureLod(t4, q, 0.0).rgb;\n"
                "      }\n"
                "    }\n"
                "  }\n"
                "  for (int i = 0; i < 48 && !mir; i++) {\n"
                "    vec3 Q = P + R * t;\n"
                "    vec2 q = to_uv(vp, Q, dz);\n"
                "    if (any(lessThan(q, vec2(0.0))) || any(greaterThan(q, vec2(1.0))) || dz <= 0.0) break;\n"
                "    float ds = depth_at(t1, q);\n"
                "    if (n_after > 0.0 || ds > dz) n_after += 1.0;\n"
                "    if (ds > dz) {\n"  // (the scene is in front of the ray here: it passed behind something)
                "      vec3 S = wpos(q, ds);\n"
                "      if (length(S - Q) < t * 0.2 + 0.25) { found = 1.0; hit = q; break; }\n"
                "      float ls = length(S - pc.cam.xyz), lq = length(Q - pc.cam.xyz);\n"
                // (close behind it, the ray went into a wall: that ends it)
                "      if (ls > 0.8 * lq) { if (n_hidden == 0.0) occ_q = q; n_hidden += 1.0; break; }\n"
                // (something thin - the player, a lamp post - only hides the ray on screen: a metre to either side the
                // ray isn't behind anything, so on past it, and it doesn't count as hidden. Otherwise a broad surface; only
                // a little in front of the ray, step after step, it is a thick one - the face of a bridge over the ray
                // inside its arch - and blocks it: coming out above it, the ray would pick up the lit buildings behind
                // the bridge on screen)
                "      vec3 side = normalize(cross(S - pc.cam.xyz, vec3(0.0, 1.0, 0.0)));\n"
                "      float dzs;\n"
                "      vec2 qa = to_uv(vp, S + side, dzs), qb = to_uv(vp, S - side, dzs);\n"
                "      if (depth_at(t1, qa) <= dz || depth_at(t1, qb) <= dz) run = 0.0;\n"
                "      else {\n"
                "        if (n_hidden == 0.0) occ_q = q;\n"
                "        n_hidden += 1.0;\n"
                "        run = ls > 0.5 * lq ? run + 1.0 : 0.0;\n"
                "        if (run >= 2.0) { n_hidden = n_after; break; }\n"
                "      }\n"
                "    } else run = 0.0;\n"
                "    t *= 1.13;\n"
                "  }\n"
                // (a ray that stayed hidden to its end went under something the camera can't see - a bridge's arch,
                // an awning: what it mirrors is that, in shade, not the cube, which was captured out in the open and
                // at night shows the city's lights there. Mostly hidden: the occluder's colour, darkened)
                "  float hidden = found == 0.0 && n_after > 0.0 ? smoothstep(0.5, 0.9, n_hidden / n_after) : 0.0;\n"
                "  if (found == 0.0 && hidden < 0.5 && !mir) {\n"  // (nothing hit: the sky, where the far end of the ray is on screen and is sky)
                "    vec2 q = to_uv(vp, P + R * 5000.0, dz);\n"
                "    if (all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, vec2(1.0))) && depth_at(t1, q) <= 0.0) { found = 1.0; hit = q; }\n"
                "  }\n"
                "  vec2 e = min(hit, 1.0 - hit);\n"
                "  float edge = found * clamp(min(e.x, e.y) * 12.0, 0.0, 1.0);\n"
                // (off the picture: the game's reflection cube. Its probe sits elsewhere, and at night it holds the lamps
                // around it as bright spots that would show up where there is no lamp: capped at 3x its face's mean)
                "  vec3 env = textureLod(t3, R, 0.0).rgb, avg = textureLod(t3, R, 16.0).rgb;\n"
                "  env *= min(1.0, 3.0 * max(avg.r, max(avg.g, avg.b)) / max(max(env.r, max(env.g, env.b)), 1e-4));\n"
                "  env = mix(env, textureLod(t2, occ_q, 0.0).rgb * 0.35, hidden);\n"
                "  vec3 col = mir ? mcol : mix(env, textureLod(t2, hit, 0.0).rgb, edge);\n"
                "  if (pc.prj.z != 0.0) { o = vec4(pc.prj.z > 1.5 ? env : col, 1.0); return; }\n"  // (2: the cube alone)
                "  float a = clamp(F * pc.prm.x, 0.0, 1.0);\n"
                "#ifdef WATER_DUAL\n"
                "  int fl = int(pc.prm.y + 0.5);\n"
                "  vec3 B = wpos(uv, db);\n"
                "  float path = db > 0.0 ? length(B - P) : 30.0;\n"  // (through the water to its floor, metres)
                "  vec3 Tr = vec3(1.0), add = vec3(0.0);\n"
                "  vec3 sky = textureLod(t3, vec3(0.0, 1.0, 0.0), 16.0).rgb;\n"  // (the light from above: the cube's mean)
                "  if ((fl & 1) != 0) {\n"  // (deep water keeps part of the game's own colour: the fade's floor)
                "    Tr = mix(vec3(0.40, 0.50, 0.60), vec3(1.0), exp(-vec3(0.45, 0.12, 0.07) * pc.prj.x * path));\n"
                "    add += (1.0 - Tr) * sky * vec3(0.02, 0.06, 0.10);\n"
                "  }\n"
                "  if ((fl & 2) != 0 && db > 0.0) {\n"  // (fading with depth, and none right at the shore)
                "    float c = caustic(B.xz, tm) * exp(-0.6 * path) * smoothstep(0.03, 0.25, path);\n"
                "    add += textureLod(t2, uv, 0.0).rgb * c * 0.8 * pc.prj.y * Tr;\n"
                "  }\n"
                "  if ((fl & 4) != 0) {\n"  // (vivid: less red through it - turquoise - and a faint glow of its own)
                "    Tr *= vec3(0.88, 0.98, 1.0);\n"
                "    add += sky * vec3(0.0, 0.035, 0.045) + vec3(0.0, 0.007, 0.009);\n"
                "  }\n"
                "  o = vec4(col * a + (1.0 - a) * add, 1.0);\n"
                "  o1 = vec4((1.0 - a) * Tr, 1.0);\n"
                "#else\n"
                "  o = vec4(col, a);\n"
                "#endif\n"
                "}\n";
            const char *dual_decl = dual ? "layout(location = 0, index = 1) out vec4 o1;\n#define WATER_DUAL 1\n" : "";
            size_t n = strlen(gtao_common) + strlen(dual_decl) + strlen(body) + 1;
            *src = malloc(n);
            snprintf(*src, n, "%s%s%s", gtao_common, dual_decl, body);
        }
        return *src;
    }
    // ---- the planar reflection's depth for its lighting (exec.c planar_light): t0 the depth drawn with the clipping
    // mirrored camera; each pixel back to the world (inv: that camera's inverse; ndc y up) and into the unclipped one
    // (vpm) - the depth the game's own projection decodes. Nothing drawn (0) stays 0.
    if (!strcmp(name, "planar_depth.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(push_constant) uniform P { mat4 inv; mat4 vpm; } pc;\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
               "  vec2 sz = vec2(textureSize(t0, 0));\n"
               "  float d = texelFetch(t0, p, 0).r;\n"
               "  if (d <= 0.0) { o = vec4(0.0); return; }\n"
               "  vec2 uv = (vec2(p) + 0.5) / sz;\n"
               "  vec4 w = pc.inv * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
               "  vec4 c = pc.vpm * vec4(w.xyz / w.w, 1.0);\n"
               "  o = vec4(c.z / c.w, 0.0, 0.0, 1.0);\n"
               "}\n";
    }
    // ---- SSPR (exec.c sspr_run): every pixel of the scene over the water's plane (t, the depth before the water), mirrored
    // (M: its ndc and depth -> the mirror's clip space), writes where it came from at the half-size pixel it lands on. Of
    // those that land together the largest value wins: row << 16 | column, the row counted from the picture's top, so
    // the one lowest on the screen (o.z < 0: the image's rows as they are; else the row is o.z - row, it is upside down)
    if (!strcmp(name, "sspr_project.comp")) {
        *stage = STAGE_COMPUTE;
        return "#version 450\n"
               "layout(local_size_x = 8, local_size_y = 8) in;\n"
               "layout(push_constant) uniform P { mat4 M; vec4 ry, rw, o; } pc;\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(set = 0, binding = 1, r32ui) uniform coherent uimage2D h;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_GlobalInvocationID.xy), s = textureSize(t, 0), hs = imageSize(h);\n"
               "  if (p.x >= s.x || p.y >= s.y) return;\n"
               "  float d = texelFetch(t, p, 0).r;\n"
               "  if (d <= 0.0) return;\n"  // (the sky: reversed depth)
               "  vec2 uv = (vec2(p) + 0.5) / vec2(s);\n"
               "  vec4 n = vec4(uv.x * 2.0 - 1.0, (uv.y * 2.0 - 1.0) * pc.o.x, d, 1.0);\n"
               "  if (dot(pc.ry, n) / dot(pc.rw, n) <= pc.o.y + 0.02) return;\n"  // (under the plane: nothing it mirrors)
               "  vec4 c = pc.M * n;\n"
               "  if (c.w <= 0.0) return;\n"
               "  vec2 q = vec2(c.x / c.w * 0.5 + 0.5, c.y / c.w * pc.o.x * 0.5 + 0.5);\n"
               "  if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, vec2(1.0)))) return;\n"
               "  uint row = pc.o.z < 0.0 ? uint(p.y) : uint(pc.o.z) - uint(p.y);\n"
               "  imageAtomicMax(h, ivec2(q * vec2(hs)), row << 16 | uint(p.x));\n"
               "}\n";
    }
    // t0 the hash, t1 the lit image, t2 the depth before the water; alpha: how far the mirrored point is from the camera,
    // 0 for none. prj.x: as o.z above, prj.w the plane
    if (!strcmp(name, "sspr_resolve.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 m0, m1, m2, m3, cam, prm, sz, prj; } pc;\n"
               "layout(set = 0, binding = 0) uniform usampler2D t0;\n"
               "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
               "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  ivec2 p = ivec2(gl_FragCoord.xy), s = textureSize(t0, 0);\n"
               "  uint v = texelFetch(t0, p, 0).r;\n"
               "  if (v == 0u) {\n"  // (a crack: from the neighbours)
               "    const ivec2 a[4] = ivec2[](ivec2(0, 1), ivec2(0, -1), ivec2(1, 0), ivec2(-1, 0));\n"
               "    for (int i = 0; i < 4; i++) v = max(v, texelFetch(t0, clamp(p + a[i], ivec2(0), s - 1), 0).r);\n"
               "  }\n"
               "  if (v == 0u) { o = vec4(0.0); return; }\n"
               "  ivec2 q = ivec2(int(v & 0xFFFFu), pc.prj.x < 0.0 ? int(v >> 16) : int(pc.prj.x) - int(v >> 16));\n"
               "  vec2 uv = (vec2(q) + 0.5) / pc.sz.zw;\n"
               "  vec4 w = (uv.x * 2.0 - 1.0) * pc.m0 + (uv.y * 2.0 - 1.0) * pc.prm.z * pc.m1 + texelFetch(t2, q, 0).r * pc.m2 + pc.m3;\n"
               "  vec3 W = w.xyz / w.w;\n"
               "  W.y = 2.0 * pc.prj.w - W.y;\n"
               "  o = vec4(texelFetch(t1, q, 0).rgb, length(W - pc.cam.xyz));\n"
               "}\n";
    }
    // ---- the water's height for the planar reflections (exec.c planar_level_probe), into a 1 x 1 image: t0 the depth
    // now, t1 the depth before the water; over a 48 x 27 grid of the picture, where the water drew, the mean world
    // height of the commonest level (a fountain and a river below it are two: their mean would be neither), by metre
    // (.x), and how many (.y)
    if (!strcmp(name, "water_level.frag")) {
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "void main() {\n"
                "  float sum[32], cnt[32];\n"
                "  for (int k = 0; k < 32; k++) sum[k] = cnt[k] = 0.0;\n"
                "  for (int j = 0; j < 27; j++)\n"
                "    for (int i = 0; i < 48; i++) {\n"
                "      vec2 uv = (vec2(i, j) + 0.5) / vec2(48.0, 27.0);\n"
                "      float dn = depth_at(t0, uv), db = depth_at(t1, uv);\n"
                "      if (dn > db + 1e-7) { float y = wpos(uv, dn).y; int k = int(mod(floor(y), 32.0)); sum[k] += y; cnt[k] += 1.0; }\n"
                "    }\n"
                "  int b = 0;\n"
                "  for (int k = 1; k < 32; k++) if (cnt[k] > cnt[b]) b = k;\n"
                "  o = vec4(cnt[b] > 0.0 ? sum[b] / cnt[b] : 0.0, cnt[b], 0.0, 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    // (exec.c gfast_finish) a storing dispatch's device-local copy back to the guest heap: only the words the dispatch
    // changed (copy f vs its untouched twin o), so the CPU's writes to the rest of the buffer meanwhile survive - and
    // not a word the CPU rewrote meanwhile either (h no longer the twin's): the CPU's write is the later one (#70 #81
    // #82 #86: a counter the dispatch counts up and the CPU resets for the next frame got the old count back, and the
    // list it indexes ran on into the indirect draws' parameters). pc.cpu_wins 0: the dispatch's word regardless.
    if (!strcmp(name, "gfast_merge.comp")) {
        *stage = STAGE_COMPUTE;
        return "#version 450\n"
               "layout(local_size_x = 256) in;\n"
               "layout(std430, set = 0, binding = 0) buffer H { uint h[]; };\n"
               "layout(std430, set = 0, binding = 1) readonly buffer F { uint f[]; };\n"
               "layout(std430, set = 0, binding = 2) readonly buffer O { uint o[]; };\n"
               "layout(push_constant) uniform P { uint src; uint dst; uint n; uint cpu_wins; } pc;\n"
               "void main() {\n"
               "  uint i = gl_GlobalInvocationID.x;\n"
               "  if (i >= pc.n) return;\n"
               "  uint v = f[pc.src + i], w = o[pc.src + i];\n"
               "  if (v != w && (pc.cpu_wins == 0u || h[pc.dst + i] == w)) h[pc.dst + i] = v;\n"
               "}\n";
    }
    // (the same, the heap by its device address: vk.h gmem_bda - the window is past maxStorageBufferRange)
    if (!strcmp(name, "gfast_merge_bda.comp")) {
        *stage = STAGE_COMPUTE;
        return "#version 450\n"
               "#extension GL_EXT_buffer_reference : require\n"
               "#extension GL_EXT_buffer_reference_uvec2 : require\n"
               "layout(local_size_x = 256) in;\n"
               "layout(buffer_reference, std430, buffer_reference_align = 4) buffer H { uint h[]; };\n"
               "layout(std430, set = 0, binding = 1) readonly buffer F { uint f[]; };\n"
               "layout(std430, set = 0, binding = 2) readonly buffer O { uint o[]; };\n"
               "layout(push_constant) uniform P { uint src; uint dst; uint n; uint cpu_wins; uvec2 heap; } pc;\n"
               "void main() {\n"
               "  uint i = gl_GlobalInvocationID.x;\n"
               "  if (i >= pc.n) return;\n"
               "  H hp = H(pc.heap);\n"
               "  uint v = f[pc.src + i], w = o[pc.src + i];\n"
               "  if (v != w && (pc.cpu_wins == 0u || hp.h[pc.dst + i] == w)) hp.h[pc.dst + i] = v;\n"
               "}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
