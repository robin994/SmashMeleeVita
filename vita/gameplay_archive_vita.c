#include <dolphin/os.h>
#include <sysdolphin/baselib/debug.h>

#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hsd_data.h"

extern void mv_hsd_joint_graph_prepare_raw(void* bytes, size_t size,
                                           uint32_t root_offset,
                                           const char* filename,
                                           const char* label);
extern void mv_hsd_anim_graph_prepare_raw(void* bytes, size_t size,
                                          uint32_t root_offset,
                                          const char* filename,
                                          const char* label);
extern void mv_hsd_graph_set_prepare_raw(void* bytes, size_t size,
                                         const uint32_t* joint_roots,
                                         size_t joint_count,
                                         const uint32_t* anim_roots,
                                         size_t anim_count,
                                         const uint32_t* matanim_roots,
                                         size_t matanim_count,
                                         const uint32_t* shape_roots,
                                         size_t shape_count,
                                         const char* filename,
                                         const char* label);

static void gp_fail(const char* kind, const char* detail, uint32_t off)
{
    OSReport("VITA_GAMEPLAY_DAT_INVALID kind=%s detail=%s off=%08x\n",
             kind != NULL ? kind : "?", detail != NULL ? detail : "?", off);
    HSD_Panic(__FILE__, __LINE__, "gameplay DAT schema invalid");
}

static uint8_t* gp_span(MvDat* dat, uint32_t off, size_t size,
                        const char* kind, const char* detail)
{
    const uint8_t* p = mv_dat_span(dat, off, size);
    if (p == NULL) {
        gp_fail(kind, detail, off);
    }
    return (uint8_t*) p;
}

static int gp_is_pointer(const MvDat* dat, uint32_t off)
{
    return dat->pointer_bits != NULL &&
           (dat->pointer_bits[off / 8] & (1u << (off % 8))) != 0;
}

static int gp_pointer(MvDat* dat, uint32_t field, uint32_t* target,
                      const char* kind, const char* detail)
{
    int r = mv_dat_pointer(dat, field, target);
    if (r < 0) {
        gp_fail(kind, detail, field);
    }
    return r;
}

static void gp_swap16(MvDat* dat, uint32_t off, const char* kind,
                      const char* detail)
{
    uint8_t* p = gp_span(dat, off, 2, kind, detail);
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    v = __builtin_bswap16(v);
    memcpy(p, &v, sizeof(v));
}

static void gp_swap32(MvDat* dat, uint32_t off, const char* kind,
                      const char* detail)
{
    if (gp_is_pointer(dat, off)) {
        gp_fail(kind, "attempted scalar swap on relocation", off);
    }
    uint8_t* p = gp_span(dat, off, 4, kind, detail);
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    v = __builtin_bswap32(v);
    memcpy(p, &v, sizeof(v));
}

static void gp_store32_native(MvDat* dat, uint32_t off, uint32_t value,
                              const char* kind, const char* detail)
{
    if (gp_is_pointer(dat, off)) {
        gp_fail(kind, "attempted native scalar store on relocation", off);
    }
    uint8_t* p = gp_span(dat, off, 4, kind, detail);
    memcpy(p, &value, sizeof(value));
}

static size_t gp_target_span(const MvDat* dat, uint32_t target)
{
    uint32_t next = dat->data_size;
    for (uint32_t i = 0; i < dat->relocation_count; ++i) {
        uint32_t field = mv_be32(dat->relocations + i * 4);
        const uint8_t* p = mv_dat_span(dat, field, 4);
        if (p != NULL) {
            uint32_t value = mv_be32(p);
            if (value > target && value < next) {
                next = value;
            }
        }
    }
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char* ignored;
        uint32_t value;
        if (mv_dat_public(dat, i, &ignored, &value) == 0 && value > target &&
            value < next)
        {
            next = value;
        }
    }
    return next >= target ? (size_t) (next - target) : 0;
}

static int gp_find_public(const MvDat* dat, const char* exact,
                          const char* prefix, const char** name,
                          uint32_t* target)
{
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char* n;
        uint32_t t;
        if (mv_dat_public(dat, i, &n, &t) != 0) {
            return -1;
        }
        if ((exact != NULL && strcmp(n, exact) == 0) ||
            (prefix != NULL && strncmp(n, prefix, strlen(prefix)) == 0))
        {
            if (name != NULL) {
                *name = n;
            }
            *target = t;
            return 1;
        }
    }
    return 0;
}

static void gp_swap_words(MvDat* dat, uint32_t off, size_t bytes,
                          const char* kind, const char* detail)
{
    if ((bytes & 3) != 0) {
        gp_fail(kind, detail, off);
    }
    (void) gp_span(dat, off, bytes, kind, detail);
    for (size_t i = 0; i < bytes; i += 4) {
        gp_swap32(dat, off + (uint32_t) i, kind, detail);
    }
}

typedef struct GpColorCommandContext {
    MvDat* dat;
    uint32_t* seen;
    size_t seen_count;
    size_t seen_cap;
    unsigned histogram[24];
} GpColorCommandContext;

static int gp_color_seen(GpColorCommandContext* ctx, uint32_t off)
{
    for (size_t i = 0; i < ctx->seen_count; ++i) {
        if (ctx->seen[i] == off) return 1;
    }
    if (ctx->seen_count >= ctx->seen_cap) {
        gp_fail("PlCo", "ColorOverlay command graph too large", off);
    }
    ctx->seen[ctx->seen_count++] = off;
    return 0;
}

static uint32_t gp_color_word(MvDat* dat, uint32_t off, const char* detail)
{
    return mv_be32(gp_span(dat, off, 4, "PlCo", detail));
}

static void gp_color_store(GpColorCommandContext* ctx, uint32_t off,
                           uint32_t value, const char* detail)
{
    gp_store32_native(ctx->dat, off, value, "PlCo", detail);
}

static uint32_t gp_pack_op_value26(uint32_t raw)
{
    return (raw >> 26) | ((raw & 0x03ffffffu) << 6);
}

static void gp_color_convert_script(GpColorCommandContext* ctx, uint32_t off,
                                    unsigned depth)
{
    if (depth > 64) gp_fail("PlCo", "ColorOverlay command recursion", off);
    for (;;) {
        if (gp_color_seen(ctx, off)) return;
        uint32_t raw = gp_color_word(ctx->dat, off, "ColorOverlay command");
        uint32_t opcode = raw >> 26;
        if (opcode >= 24) gp_fail("PlCo", "ColorOverlay opcode", off);
        ++ctx->histogram[opcode];

        switch (opcode) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 6:
        case 8:
            gp_color_store(ctx, off, gp_pack_op_value26(raw),
                           "ColorOverlay generic command");
            if (opcode == 0 || opcode == 6) return;
            off += 4;
            break;
        case 5:
        case 7: {
            gp_color_store(ctx, off, gp_pack_op_value26(raw),
                           "ColorOverlay pointer command");
            uint32_t target = 0;
            if (gp_pointer(ctx->dat, off + 4, &target, "PlCo",
                           "ColorOverlay command pointer") != 1)
                gp_fail("PlCo", "ColorOverlay command pointer missing", off + 4);
            gp_color_convert_script(ctx, target, depth + 1);
            if (opcode == 7) return;
            off += 8;
            break;
        }
        case 9: {
            uint32_t id = raw >> 26;
            uint32_t param1 = (raw >> 18) & 0xffu;
            uint32_t param2 = raw & 0x3ffffu;
            gp_color_store(ctx, off, id | (param1 << 6) | (param2 << 14),
                           "ColorOverlay bg-flash command");
            off += 4;
            break;
        }
        case 10:
            gp_color_store(ctx, off, opcode, "ColorOverlay end command");
            return;
        case 11:
        case 15:
        case 19: {
            uint32_t timer = raw & 0x03ffffffu;
            gp_color_store(ctx, off, opcode | (timer << 6),
                           "ColorOverlay timer command");
            off += (opcode == 11 ? 4 : 8);
            break;
        }
        case 12:
        case 14:
        case 17:
        case 18:
        case 20:
            gp_color_store(ctx, off, opcode, "ColorOverlay simple command");
            off += (opcode == 14 || opcode == 18 ? 8 : 4);
            break;
        case 13: {
            uint32_t light_enable = (raw >> 25) & 1u;
            uint32_t spare = (raw >> 24) & 1u;
            uint32_t x = (raw >> 12) & 0xfffu;
            uint32_t yz = raw & 0xfffu;
            gp_color_store(ctx, off,
                           opcode | (light_enable << 6) | (spare << 7) |
                               (x << 8) | (yz << 20),
                           "ColorOverlay light-rotation command");
            off += 8;
            break;
        }
        case 16: {
            uint32_t x = (raw >> 13) & 0x1fffu;
            uint32_t yz = raw & 0x1fffu;
            gp_color_store(ctx, off, opcode | (x << 6) | (yz << 19),
                           "ColorOverlay light-direction command");
            off += 4;
            break;
        }
        case 21: {
            (void) gp_span(ctx->dat, off, 20, "PlCo", "ColorOverlay GFX command");
            uint32_t bone = (raw >> 18) & 0xffu;
            uint32_t common = (raw >> 17) & 1u;
            uint32_t destroy = (raw >> 16) & 1u;
            uint32_t use_unk = (raw >> 15) & 1u;
            uint32_t tail = raw & 0x7fffu;
            gp_color_store(ctx, off,
                           opcode | (bone << 6) | (common << 14) |
                               (destroy << 15) | (use_unk << 16) | (tail << 17),
                           "ColorOverlay GFX command word0");
            for (uint32_t i = 1; i < 5; ++i) {
                uint32_t w = gp_color_word(ctx->dat, off + i * 4,
                                           "ColorOverlay GFX payload");
                uint32_t native = (w >> 16) | ((w & 0xffffu) << 16);
                gp_color_store(ctx, off + i * 4, native,
                               "ColorOverlay GFX payload");
            }
            off += 20;
            break;
        }
        case 22: {
            (void) gp_span(ctx->dat, off, 12, "PlCo", "ColorOverlay SFX command");
            uint32_t behavior = (raw >> 18) & 0xffu;
            uint32_t unknown = raw & 0x3ffffu;
            gp_color_store(ctx, off,
                           opcode | (behavior << 6) | (unknown << 14),
                           "ColorOverlay SFX command word0");
            uint32_t sfx = gp_color_word(ctx->dat, off + 4,
                                         "ColorOverlay SFX id");
            gp_color_store(ctx, off + 4, sfx, "ColorOverlay SFX id");
            uint32_t params = gp_color_word(ctx->dat, off + 8,
                                            "ColorOverlay SFX params");
            uint32_t padding = params >> 16;
            uint32_t volume = (params >> 8) & 0xffu;
            uint32_t panning = params & 0xffu;
            gp_color_store(ctx, off + 8,
                           padding | (volume << 16) | (panning << 24),
                           "ColorOverlay SFX params");
            off += 12;
            break;
        }
        case 23: {
            if ((raw & 0x1ffffu) != 0)
                gp_fail("PlCo", "ColorOverlay opcode23 padding", off);
            uint32_t arg1 = (raw >> 25) & 1u;
            uint32_t arg2 = (raw >> 17) & 0xffu;
            gp_color_store(ctx, off, opcode | (arg1 << 6) | (arg2 << 7),
                           "ColorOverlay fighter command");
            off += 4;
            break;
        }
        default:
            gp_fail("PlCo", "ColorOverlay unsupported opcode", off);
        }
    }
}

static void plco_convert_color_commands(MvDat* dat, uint32_t p6, uint32_t p7)
{
    if (gp_target_span(dat, p6) != 0x3d8 || gp_target_span(dat, p7) != 0x30)
        gp_fail("PlCo", "ColorOverlay table span", p6);
    GpColorCommandContext ctx = { 0 };
    ctx.dat = dat;
    ctx.seen_cap = 4096;
    ctx.seen = calloc(ctx.seen_cap, sizeof(*ctx.seen));
    if (ctx.seen == NULL) gp_fail("PlCo", "ColorOverlay seen allocation", p6);

    const uint32_t tables[2] = { p6, p7 };
    const size_t spans[2] = { 0x3d8, 0x30 };
    for (int t = 0; t < 2; ++t) {
        for (size_t off = 0; off < spans[t]; off += 8) {
            uint32_t script = 0;
            int r = gp_pointer(dat, tables[t] + (uint32_t) off, &script,
                               "PlCo", "ColorOverlay script");
            if (r == 1) gp_color_convert_script(&ctx, script, 0);
        }
    }
    OSReport("VITA_PLCO_COLOR_COMMAND_NATIVE_PASS commands=%u op3=%u op4=%u op5=%u op7=%u op10=%u op11=%u op13=%u op18=%u op19=%u op21=%u op22=%u op23=%u\n",
             (unsigned) ctx.seen_count, ctx.histogram[3], ctx.histogram[4],
             ctx.histogram[5], ctx.histogram[7], ctx.histogram[10],
             ctx.histogram[11], ctx.histogram[13], ctx.histogram[18],
             ctx.histogram[19], ctx.histogram[21], ctx.histogram[22],
             ctx.histogram[23]);
    free(ctx.seen);
}

static void plco_convert_cpu_tables(MvDat* dat, uint32_t root)
{
    if (gp_target_span(dat, root) != 0x30)
        gp_fail("PlCo", "CPU root span", root);
    uint32_t table[10];
    for (uint32_t i = 0; i < 10; ++i) {
        if (gp_pointer(dat, root + i * 4, &table[i], "PlCo", "CPU root pointer") != 1)
            gp_fail("PlCo", "CPU root pointer missing", root + i * 4);
    }
    if (gp_target_span(dat, table[0]) != 0xf8)
        gp_fail("PlCo", "CPU cmdscript pointer table span", table[0]);
    for (uint32_t off = 0; off < 0xf8; off += 4) {
        uint32_t ignored = 0;
        (void) gp_pointer(dat, table[0] + off, &ignored, "PlCo", "CPU cmdscript pointer");
    }

    uint32_t seen[256];
    size_t seen_count = 0;
    unsigned lists = 0;
    unsigned entries = 0;
    for (uint32_t field = 1; field <= 7; ++field) {
        if (gp_target_span(dat, table[field]) != 0x80)
            gp_fail("PlCo", "CPU attack pointer table span", table[field]);
        for (uint32_t i = 0; i < 32; ++i) {
            uint32_t list = 0;
            int r = gp_pointer(dat, table[field] + i * 4, &list, "PlCo",
                               "CPU attack list");
            if (r != 1) continue;
            int duplicate = 0;
            for (size_t s = 0; s < seen_count; ++s) {
                if (seen[s] == list) { duplicate = 1; break; }
            }
            if (duplicate) continue;
            if (seen_count >= sizeof(seen) / sizeof(seen[0]))
                gp_fail("PlCo", "CPU attack list count", list);
            seen[seen_count++] = list;
            ++lists;

            uint32_t count = 0;
            for (; count < 64; ++count) {
                uint32_t rec = list + count * 0x24;
                uint8_t* raw = gp_span(dat, rec, 0x24, "PlCo", "CPU attack record");
                uint32_t cmd = mv_be32(raw);
                if (cmd == 0) break;
                gp_swap_words(dat, rec, 0x24, "PlCo", "CPU attack record");
            }
            if (count == 64) gp_fail("PlCo", "CPU attack list unterminated", list);
            entries += count;
        }
    }
    if (gp_target_span(dat, table[8]) != 0x80 ||
        gp_target_span(dat, table[9]) != 0x18)
        gp_fail("PlCo", "CPU float-table spans", table[8]);
    gp_swap_words(dat, table[8], 0x80, "PlCo", "CPU fighter reach");
    gp_swap_words(dat, table[9], 0x18, "PlCo", "CPU weapon reach");
    OSReport("VITA_PLCO_CPU_NATIVE_PASS lists=%u entries=%u fighter_reach=32 weapon_reach=6\n",
             lists, entries);
}

static int gp_mark_unique(uint32_t* values, size_t* count, size_t capacity,
                          uint32_t value, const char* kind,
                          const char* detail)
{
    for (size_t i = 0; i < *count; ++i) {
        if (values[i] == value) {
            return 0;
        }
    }
    if (*count >= capacity) {
        gp_fail(kind, detail, value);
    }
    values[(*count)++] = value;
    return 1;
}

static size_t fighter_ext_size(const char* root)
{
    static const struct {
        const char* root;
        uint16_t size;
    } schema[] = {
        { "ftDataBoy", 0x004 },       { "ftDataCaptain", 0x08C },
        { "ftDataCrazyhand", 0x144 }, { "ftDataClink", 0x0DC },
        { "ftDataDonkey", 0x074 },    { "ftDataDrmario", 0x084 },
        { "ftDataFalco", 0x0D4 },     { "ftDataEmblem", 0x098 },
        { "ftDataFox", 0x0D4 },       { "ftDataGkoopa", 0x0A0 },
        { "ftDataGirl", 0x004 },      { "ftDataGanon", 0x08C },
        { "ftDataGamewatch", 0x094 }, { "ftDataKirby", 0x424 },
        { "ftDataKoopa", 0x0A0 },     { "ftDataLuigi", 0x098 },
        { "ftDataLink", 0x0DC },      { "ftDataMasterhand", 0x17C },
        { "ftDataMario", 0x084 },     { "ftDataMars", 0x098 },
        { "ftDataMewtwo", 0x088 },    { "ftDataNana", 0x15C },
        { "ftDataNess", 0x0DC },      { "ftDataPichu", 0x0F8 },
        { "ftDataPeach", 0x0C0 },     { "ftDataPikachu", 0x0F8 },
        { "ftDataPopo", 0x15C },      { "ftDataPurin", 0x100 },
        { "ftDataSandbag", 0x008 },   { "ftDataSeak", 0x074 },
        { "ftDataSamus", 0x0D4 },     { "ftDataYoshi", 0x138 },
        { "ftDataZelda", 0x0A8 },
    };
    for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); ++i) {
        if (strcmp(root, schema[i].root) == 0) {
            return schema[i].size;
        }
    }
    return 0;
}

static int fighter_ext_skip_word(const char* root, uint32_t off)
{
    if ((!strcmp(root, "ftDataMario") || !strcmp(root, "ftDataDrmario")) &&
        off == 0x80)
        return 1;
    if ((!strcmp(root, "ftDataFox") || !strcmp(root, "ftDataFalco")) &&
        off == 0xD0)
        return 1;
    if (!strcmp(root, "ftDataKirby") && (off == 0x34 || off == 0x420))
        return 1;
    if ((!strcmp(root, "ftDataLink") || !strcmp(root, "ftDataClink")) &&
        off == 0xC0)
        return 1;
    if ((!strcmp(root, "ftDataMars") || !strcmp(root, "ftDataEmblem")) &&
        off >= 0x80 && off <= 0x88)
        return 1;
    if (!strcmp(root, "ftDataMewtwo") && off == 0x3C)
        return 1;
    if (!strcmp(root, "ftDataNess") && off == 0xD8)
        return 1;
    if (!strcmp(root, "ftDataZelda") && off == 0xA4)
        return 1;
    if ((!strcmp(root, "ftDataPopo") || !strcmp(root, "ftDataNana")) &&
        (off == 0xCC || (off >= 0xD4 && off < 0x12C) || off >= 0x150))
        return 1;
    if (!strcmp(root, "ftDataPurin") &&
        (off == 0x48 || off == 0x60 || off == 0x64 || off == 0xB0 ||
         off == 0xF8 || off == 0xFC))
        return 1;
    if (!strcmp(root, "ftDataYoshi") &&
        ((off >= 0xEC && off < 0x114) || (off >= 0x12C && off < 0x138)))
        return 1;
    return 0;
}

static void fighter_convert_motion(MvDat* dat, uint32_t off, size_t count,
                                   uint32_t max_size, const char* root,
                                   const char* detail)
{
    if (count > 2048 || mv_dat_span(dat, off, count * 0x18) == NULL) {
        gp_fail(root, detail, off);
    }
    for (size_t i = 0; i < count; ++i) {
        uint32_t base = off + (uint32_t) i * 0x18;
        uint32_t ignored;
        (void) gp_pointer(dat, base + 0x00, &ignored, root, detail);
        (void) gp_pointer(dat, base + 0x0C, &ignored, root, detail);
        uint32_t payload_size = mv_be32(gp_span(dat, base + 0x08, 4, root,
                                                detail));
        if (payload_size > max_size) {
            gp_fail(root, "motion payload exceeds retail bound", base + 8);
        }
        gp_swap32(dat, base + 0x04, root, detail);
        gp_swap32(dat, base + 0x08, root, detail);
        gp_swap32(dat, base + 0x10, root, detail);
        gp_swap32(dat, base + 0x14, root, detail);
    }
}

static void fighter_convert_wait(MvDat* dat, uint32_t off, const char* root,
                                 const char* detail)
{
    size_t span = gp_target_span(dat, off);
    if (span == 0 || span > 0x400 || (span & 7) != 0) {
        gp_fail(root, detail, off);
    }
    gp_swap_words(dat, off, span, root, detail);
}

static void fighter_convert_dynamics(MvDat* dat, uint32_t off,
                                     const char* root)
{
    uint8_t* p = gp_span(dat, off, 0x14, root, "ftDynamics");
    uint32_t n = mv_be32(p + 0x00);
    uint32_t bones = 0, hit_count = mv_be32(p + 0x08), hits = 0, figa = 0;
    if (n >= 10 || hit_count > 11) {
        gp_fail(root, "ftDynamics count", off);
    }
    int bones_r = gp_pointer(dat, off + 0x04, &bones, root, "ftDynamics.bones");
    int hits_r = gp_pointer(dat, off + 0x0C, &hits, root, "ftDynamics.hits");
    (void) gp_pointer(dat, off + 0x10, &figa, root, "ftDynamics.figa");
    if ((n != 0 && bones_r != 1) || (hit_count != 0 && hits_r != 1)) {
        gp_fail(root, "ftDynamics missing table", off);
    }
    gp_swap32(dat, off + 0x00, root, "ftDynamics.count");
    gp_swap32(dat, off + 0x08, root, "ftDynamics.hit_count");

    uint32_t seen_data[10];
    size_t seen_count = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t base = bones + i * 0x18;
        uint8_t* b = gp_span(dat, base, 0x18, root, "BoneDynamicsDesc");
        uint32_t source = 0;
        int source_r = gp_pointer(dat, base + 0x04, &source, root,
                                  "BoneDynamicsDesc.data");
        uint32_t count = mv_be32(b + 0x08);
        if (count > 32 || (count != 0 && source_r != 1)) {
            gp_fail(root, "BoneDynamicsDesc count/source", base);
        }
        gp_swap32(dat, base + 0x00, root, "BoneDynamicsDesc.bone");
        gp_swap32(dat, base + 0x08, root, "BoneDynamicsDesc.count");
        gp_swap32(dat, base + 0x0C, root, "BoneDynamicsDesc.pos");
        gp_swap32(dat, base + 0x10, root, "BoneDynamicsDesc.pos");
        gp_swap32(dat, base + 0x14, root, "BoneDynamicsDesc.pos");
        int duplicate = 0;
        for (size_t j = 0; j < seen_count; ++j) {
            if (seen_data[j] == source) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate && source_r == 1) {
            gp_swap_words(dat, source, count * 0x3C, root,
                          "fighter dynamics source");
            seen_data[seen_count++] = source;
        }
    }
    if (hit_count != 0) {
        gp_swap_words(dat, hits, hit_count * 0x14, root,
                      "fighter dynamics collision table");
    }
}

static void fighter_convert_sfx(MvDat* dat, uint32_t off, const char* root)
{
    (void) gp_span(dat, off, 0x38, root, "FtSFX");
    uint32_t arrays[3] = { 0, 0, 0 };
    int present[3] = {
        gp_pointer(dat, off + 0x00, &arrays[0], root, "FtSFX.smash"),
        gp_pointer(dat, off + 0x1C, &arrays[1], root, "FtSFX.x1C"),
        gp_pointer(dat, off + 0x20, &arrays[2], root, "FtSFX.x20"),
    };
    for (uint32_t p = 0x04; p < 0x1C; p += 4)
        gp_swap32(dat, off + p, root, "FtSFX scalar");
    for (uint32_t p = 0x24; p < 0x38; p += 4)
        gp_swap32(dat, off + p, root, "FtSFX scalar");
    for (int i = 0; i < 3; ++i) {
        if (present[i] != 1) continue;
        uint8_t* a = gp_span(dat, arrays[i], 8, root, "FtSFXArr");
        uint32_t n = mv_be32(a);
        uint32_t ids = 0;
        int ids_r = gp_pointer(dat, arrays[i] + 4, &ids, root, "FtSFXArr.ids");
        if (n > 2048 || (n != 0 && ids_r != 1)) {
            gp_fail(root, "FtSFXArr count", arrays[i]);
        }
        gp_swap32(dat, arrays[i], root, "FtSFXArr.count");
        if (n != 0) gp_swap_words(dat, ids, n * 4, root, "FtSFX ids");
    }
}

static void fighter_convert_visibility(MvDat* dat, uint32_t model_num,
                                       uint32_t vis_table, const char* root,
                                       const char* filename)
{
    size_t vis_span = gp_target_span(dat, vis_table);
    if (model_num > 11 || vis_span == 0 || vis_span > 0x80 ||
        (vis_span % 0x10) != 0)
    {
        gp_fail(root, "fighter visibility table", vis_table);
    }

    uint32_t seen_lookups[128] = { 0 };
    uint32_t seen_temps[256] = { 0 };
    size_t lookup_count = 0;
    size_t temp_count = 0;
    uint32_t temp_entries = 0;
    uint32_t byte_indices = 0;

    for (size_t cell = 0; cell < vis_span / 4; ++cell) {
        uint32_t lookup = 0;
        int lookup_r = gp_pointer(dat, vis_table + (uint32_t) cell * 4,
                                  &lookup, root, "visibility lookup");
        if (lookup_r != 1 ||
            !gp_mark_unique(seen_lookups, &lookup_count,
                            sizeof(seen_lookups) / sizeof(seen_lookups[0]),
                            lookup, root, "visibility lookup overflow"))
        {
            continue;
        }

        (void) gp_span(dat, lookup, (size_t) model_num * 8, root,
                       "FtPartsVisLookup[]");
        for (uint32_t model = 0; model < model_num; ++model) {
            uint32_t rec = lookup + model * 8;
            uint8_t* raw = gp_span(dat, rec, 8, root, "FtPartsVisLookup");
            uint32_t count = mv_be32(raw);
            uint32_t temps = 0;
            int temps_r = gp_pointer(dat, rec + 4, &temps, root,
                                     "FtPartsVisLookup.temp");
            if (count > 32 || (count != 0 && temps_r != 1)) {
                gp_fail(root, "FtPartsVisLookup count", rec);
            }
            gp_swap32(dat, rec, root, "FtPartsVisLookup count");

            if (temps_r != 1 ||
                !gp_mark_unique(seen_temps, &temp_count,
                                sizeof(seen_temps) / sizeof(seen_temps[0]),
                                temps, root, "TempS array overflow"))
            {
                continue;
            }

            (void) gp_span(dat, temps, (size_t) count * 8, root, "TempS[]");
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t entry = temps + i * 8;
                uint8_t* temp_raw = gp_span(dat, entry, 8, root, "TempS");
                uint32_t index_count = mv_be32(temp_raw);
                uint32_t indices = 0;
                int indices_r = gp_pointer(dat, entry + 4, &indices, root,
                                           "TempS.indices");
                if (index_count > 256 ||
                    (index_count != 0 && indices_r != 1))
                {
                    gp_fail(root, "TempS count", entry);
                }
                if (index_count != 0) {
                    (void) gp_span(dat, indices, index_count, root,
                                   "TempS byte indices");
                }
                gp_swap32(dat, entry, root, "TempS count");
                ++temp_entries;
                byte_indices += index_count;
            }
        }
    }

    OSReport("VITA_FIGHTER_VIS_NATIVE_PASS file=%s root=%s lookups=%u temp_arrays=%u temp_entries=%u byte_indices=%u\n",
             filename != NULL ? filename : "?", root,
             (unsigned) lookup_count, (unsigned) temp_count,
             (unsigned) temp_entries, (unsigned) byte_indices);
}

static void fighter_convert_x48(MvDat* dat, const char* root_name,
                                int table_present, uint32_t table,
                                const char* filename);

static void fighter_convert(MvDat* dat, const char* root_name,
                            uint32_t root, const char* filename)
{
    uint32_t t[24] = { 0 };
    int r[24] = { 0 };
    (void) gp_span(dat, root, 0x60, root_name, "ftData root");
    for (uint32_t i = 0; i < 24; ++i) {
        r[i] = gp_pointer(dat, root + i * 4, &t[i], root_name,
                          "ftData root pointer");
    }
    if (r[0] != 1 || r[1] != 1 || r[2] != 1 || r[3] != 1 || r[5] != 1) {
        gp_fail(root_name, "required ftData pointers", root);
    }

    /* x5C is a complete serialized HSD_Joint graph used by ft_800C85B8() to
     * append fighter model parts. HSD relocation fixes its pointers but not
     * its PPC-endian descriptor scalars, so nativeize the whole graph while
     * the relocation metadata is still available. */
    if (r[23] == 1) {
        mv_hsd_joint_graph_prepare_raw((void*) (uintptr_t) dat->file,
                                       dat->file_size, t[23], filename,
                                       "ftData.x5C");
    }

    (void) gp_span(dat, t[0], 0x184, root_name, "ftCo_DatAttrs");
    gp_swap_words(dat, t[0], 0x180, root_name, "ftCo_DatAttrs");

    size_t ext_size = fighter_ext_size(root_name);
    if (ext_size == 0 || gp_target_span(dat, t[1]) < ext_size) {
        gp_fail(root_name, "unknown/short ext_attr schema", t[1]);
    }
    for (uint32_t off = 0; off < ext_size; off += 4) {
        if (fighter_ext_skip_word(root_name, off)) continue;
        gp_swap32(dat, t[1] + off, root_name, "fighter ext_attr");
    }
    if (!strcmp(root_name, "ftDataKirby")) {
        gp_swap16(dat, t[1] + 0x34, root_name, "Kirby ext_attr s16");
    }

    if (r[2] == 1) {
        uint32_t vis_table = 0;
        uint32_t tobj_table = 0;
        uint32_t seen_tobj_indices[6];
        size_t seen_tobj_index_count = 0;
        uint32_t converted_tobj_arrays = 0;
        uint32_t converted_tobj_indices = 0;
        (void) gp_span(dat, t[2], 0x15, root_name, "ftData_x8");
        uint32_t model_num = mv_be32(gp_span(dat, t[2], 4, root_name,
                                             "model_num"));
        int vis_table_r = gp_pointer(dat, t[2] + 0x04, &vis_table, root_name,
                                     "vis_table");
        uint32_t tobj_count =
            mv_be32(gp_span(dat, t[2] + 0x08, 4, root_name,
                            "costume tobj count"));
        int tobj_table_r = gp_pointer(dat, t[2] + 0x0C, &tobj_table,
                                      root_name, "tobj table");
        if (model_num > 11 || vis_table_r != 1 || tobj_count > 5 ||
            tobj_table_r != 1)
        {
            gp_fail(root_name, "costume tobj count/table", t[2] + 0x08);
        }

        fighter_convert_visibility(dat, model_num, vis_table, root_name,
                                   filename);

        size_t tobj_table_span = gp_target_span(dat, tobj_table);
        if (tobj_table_span == 0 || tobj_table_span > 6 * sizeof(uint32_t) ||
            (tobj_table_span & 3) != 0)
        {
            gp_fail(root_name, "costume tobj pointer table", tobj_table);
        }
        for (size_t i = 0; i < tobj_table_span / 4; ++i) {
            uint32_t indices = 0;
            int indices_r = gp_pointer(dat, tobj_table + (uint32_t) i * 4,
                                       &indices, root_name,
                                       "costume tobj index array");
            if (indices_r != 1 ||
                !gp_mark_unique(seen_tobj_indices, &seen_tobj_index_count,
                                sizeof(seen_tobj_indices) /
                                    sizeof(seen_tobj_indices[0]),
                                indices, root_name,
                                "too many costume tobj index arrays"))
            {
                continue;
            }
            (void) gp_span(dat, indices, (size_t) tobj_count * 2, root_name,
                           "costume tobj indices");
            for (uint32_t j = 0; j < tobj_count; ++j) {
                uint8_t* p = gp_span(dat, indices + j * 2, 2, root_name,
                                     "costume tobj index");
                uint16_t raw;
                memcpy(&raw, p, sizeof(raw));
                uint16_t native = __builtin_bswap16(raw);
                if (native > 0xFF) {
                    gp_fail(root_name, "costume tobj index bound",
                            indices + j * 2);
                }
                gp_swap16(dat, indices + j * 2, root_name,
                          "costume tobj index");
                converted_tobj_indices++;
            }
            converted_tobj_arrays++;
        }
        gp_swap32(dat, t[2] + 0x00, root_name, "model_num");
        gp_swap32(dat, t[2] + 0x08, root_name, "costume tobj count");
        OSReport("VITA_FIGHTER_TOBJ_INDEX_NATIVE_PASS file=%s root=%s count=%u arrays=%u indices=%u\n",
                 filename != NULL ? filename : "?", root_name,
                 (unsigned) tobj_count, (unsigned) converted_tobj_arrays,
                 (unsigned) converted_tobj_indices);
    }

    if (t[5] <= t[3] || ((t[5] - t[3]) % 0x18) != 0) {
        gp_fail(root_name, "main motion table extent", t[3]);
    }
    size_t main_motion_count = (t[5] - t[3]) / 0x18;
    fighter_convert_motion(dat, t[3], main_motion_count, 0x8000, root_name,
                           "main motion table");

    size_t demo_span = gp_target_span(dat, t[5]);
    if (demo_span == 0 || (demo_span % 0x18) != 0) {
        gp_fail(root_name, "demo motion table extent", t[5]);
    }
    fighter_convert_motion(dat, t[5], demo_span / 0x18, 0xB000, root_name,
                           "demo motion table");

    if (r[7] == 1) {
        size_t span = gp_target_span(dat, t[7]);
        if (span == 0 || span > 0x100 || (span & 3) != 0) {
            gp_fail(root_name, "x1C pointer table", t[7]);
        }
        for (size_t i = 0; i < span / 4; ++i) {
            uint32_t rec = 0;
            int pr = gp_pointer(dat, t[7] + (uint32_t) i * 4, &rec, root_name,
                                "x1C record pointer");
            if (pr == 1) {
                uint32_t ignored;
                (void) gp_span(dat, rec, 0x0C, root_name, "x1C record");
                gp_swap16(dat, rec + 0, root_name, "x1C part");
                gp_swap16(dat, rec + 2, root_name, "x1C count");
                (void) gp_pointer(dat, rec + 4, &ignored, root_name, "x1C bytes");
                (void) gp_pointer(dat, rec + 8, &ignored, root_name, "x1C anims");
            }
        }
    }
    if (r[9] == 1) fighter_convert_wait(dat, t[9], root_name, "wait table x24");
    if (r[10] == 1) fighter_convert_wait(dat, t[10], root_name, "wait table x28");
    if (r[11] == 1) fighter_convert_dynamics(dat, t[11], root_name);

    if (r[12] == 1) {
        uint8_t* h = gp_span(dat, t[12], 8, root_name, "hurtbox root");
        uint32_t n = mv_be32(h), list = 0;
        int lr = gp_pointer(dat, t[12] + 4, &list, root_name, "hurtbox list");
        if (n > 15 || (n != 0 && lr != 1)) gp_fail(root_name, "hurtbox count", t[12]);
        gp_swap32(dat, t[12], root_name, "hurtbox count");
        if (n != 0) gp_swap_words(dat, list, n * 0x28, root_name, "hurtbox records");
    }
    if (r[13] == 1) {
        size_t span = gp_target_span(dat, t[13]);
        if (span == 0 || span > 0x200 || (span % 8) != 0)
            gp_fail(root_name, "x34 table", t[13]);
        gp_swap_words(dat, t[13], span, root_name, "x34 table");
    }
    if (r[14] == 1) {
        size_t span = gp_target_span(dat, t[14]);
        if (span == 0 || span > 0x400 || (span % 0x14) != 0)
            gp_fail(root_name, "x38 table", t[14]);
        gp_swap_words(dat, t[14], span, root_name, "x38 table");
    }
    if (r[15] == 1) gp_swap_words(dat, t[15], 0x18, root_name, "camera vectors");
    if (r[16] == 1) gp_swap_words(dat, t[16], 0x30, root_name, "item pickup vectors");
    if (r[17] == 1) {
        (void) gp_span(dat, t[17], 0x1C, root_name, "ledge data");
        for (uint32_t o = 0; o < 0x0C; o += 2)
            gp_swap16(dat, t[17] + o, root_name, "ledge s16");
        for (uint32_t o = 0x0C; o < 0x1C; o += 4)
            gp_swap32(dat, t[17] + o, root_name, "ledge float");
    }
    if (r[19] == 1) fighter_convert_sfx(dat, t[19], root_name);
    if (r[20] == 1) gp_swap_words(dat, t[20], 8, root_name, "x50 Vec2");
    if (r[21] == 1) {
        size_t span = gp_target_span(dat, t[21]);
        if (span == 0 || span > 0x100 || (span & 3) != 0)
            gp_fail(root_name, "x54 int table", t[21]);
        gp_swap_words(dat, t[21], span, root_name, "x54 int table");
    }
    if (r[22] == 1) {
        (void) gp_span(dat, t[22], 0x1C, root_name, "x58 mixed data");
        gp_swap32(dat, t[22] + 0x04, root_name, "x58 float");
        gp_swap32(dat, t[22] + 0x0C, root_name, "x58 float");
        gp_swap32(dat, t[22] + 0x18, root_name, "x58 float");
    }

    /* x48 is not one homogeneous pointer table.  The fighter OnLoad
     * callbacks identify which entries are Article records and a small set of
     * additional entries are direct HSD roots/accessory bundles.  Nativeize
     * that typed schema here rather than treating the whole table as Article[]
     * (which would corrupt Link/Yoshi/Samus/etc. special entries). */
    fighter_convert_x48(dat, root_name, r[18], t[18], filename);

    OSReport("VITA_FIGHTER_DATA_NATIVE_PASS file=%s root=%s motions=%u demos=%u ext=%u\n",
             filename != NULL ? filename : "?", root_name,
             (unsigned) main_motion_count, (unsigned) (demo_span / 0x18),
             (unsigned) ext_size);
}

static void plco_convert(MvDat* dat, uint32_t root, const char* filename)
{
    uint32_t p[23] = { 0 };
    int present[23] = { 0 };
    (void) gp_span(dat, root, 23 * 4, "PlCo", "ftLoadCommonData root");
    for (uint32_t i = 0; i < 23; ++i)
        present[i] = gp_pointer(dat, root + i * 4, &p[i], "PlCo", "root pointer");
    if (present[0] != 1) gp_fail("PlCo", "ftCommonData missing", root);

    (void) gp_span(dat, p[0], 0x818, "PlCo", "ftCommonData");
    for (uint32_t o = 0; o < 0x818; o += 4) {
        if ((o >= 0x6DC && o < 0x6F0) || o == 0x7D8) continue;
        gp_swap32(dat, p[0] + o, "PlCo", "ftCommonData");
    }
    if (present[1] == 1) {
        if (gp_target_span(dat, p[1]) != 0x138)
            gp_fail("PlCo", "item-throw attrs span", p[1]);
        gp_swap_words(dat, p[1], 0x138, "PlCo", "item-throw attrs");
    }
    if (present[2] == 1) gp_swap_words(dat, p[2], 0x78, "PlCo", "swing table");
    if (present[3] == 1) gp_swap_words(dat, p[3], 0x24, "PlCo", "stale table");
    if (present[4] == 1) {
        size_t span = gp_target_span(dat, p[4]);
        if (span == 0 || span > 0x200 || (span & 3) != 0)
            gp_fail("PlCo", "parts pointer table", p[4]);
        for (size_t i = 0; i < span / 4; ++i) {
            uint32_t rec = 0;
            if (gp_pointer(dat, p[4] + (uint32_t) i * 4, &rec, "PlCo",
                           "parts record") == 1)
            {
                uint32_t ignored;
                (void) gp_span(dat, rec, 12, "PlCo", "FighterPartsTable");
                (void) gp_pointer(dat, rec + 0, &ignored, "PlCo", "joint_to_part");
                (void) gp_pointer(dat, rec + 4, &ignored, "PlCo", "part_to_joint");
                if (mv_be32(gp_span(dat, rec + 8, 4, "PlCo", "parts_num")) > 255)
                    gp_fail("PlCo", "parts_num", rec + 8);
                gp_swap32(dat, rec + 8, "PlCo", "parts_num");
            }
        }
    }
    if (present[5] == 1) {
        size_t span = gp_target_span(dat, p[5]);
        if (span != 34 * 4)
            gp_fail("PlCo", "special-parts pointer table", p[5]);
        unsigned records = 0;
        unsigned entries_total = 0;
        for (size_t i = 0; i < 34; ++i) {
            uint32_t rec = 0;
            if (gp_pointer(dat, p[5] + (uint32_t) i * 4, &rec, "PlCo",
                           "special-parts record") != 1)
                continue;
            uint8_t* r = gp_span(dat, rec, 8, "PlCo", "special-parts record");
            uint32_t entries = 0;
            int er = gp_pointer(dat, rec, &entries, "PlCo", "special-parts entries");
            uint32_t count = mv_be32(r + 4);
            if (count > 32 || (count != 0 && er != 1))
                gp_fail("PlCo", "special-parts count/entries", rec);
            if (count != 0)
                (void) gp_span(dat, entries, (size_t) count * 4, "PlCo",
                               "special-parts byte records");
            gp_swap32(dat, rec + 4, "PlCo", "special-parts count");
            ++records;
            entries_total += count;
        }
        OSReport("VITA_PLCO_SPECIAL_PARTS_NATIVE_PASS records=%u entries=%u\n",
                 records, entries_total);
    }
    if (present[6] == 1 && present[7] == 1) {
        plco_convert_color_commands(dat, p[6], p[7]);
    } else if (present[6] == 1 || present[7] == 1) {
        gp_fail("PlCo", "partial ColorOverlay tables", root);
    }
    if (present[9] == 1) {
        if (gp_target_span(dat, p[9]) != 0x18)
            gp_fail("PlCo", "model-shift table span", p[9]);
        unsigned vectors_total = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t vectors = 0;
            uint32_t field = p[9] + i * 8;
            int vr = gp_pointer(dat, field, &vectors, "PlCo", "model-shift vectors");
            uint32_t count = mv_be32(gp_span(dat, field + 4, 4, "PlCo",
                                             "model-shift count"));
            if (count > 64 || (count != 0 && vr != 1))
                gp_fail("PlCo", "model-shift count/vectors", field);
            gp_swap32(dat, field + 4, "PlCo", "model-shift count");
            if (count != 0)
                gp_swap_words(dat, vectors, (size_t) count * 8, "PlCo",
                              "model-shift Vec2");
            vectors_total += count;
        }
        OSReport("VITA_PLCO_MODEL_SHIFT_NATIVE_PASS vectors=%u\n", vectors_total);
    }
    for (int idx = 10; idx <= 11; ++idx) {
        if (present[idx] != 1) continue;
        uint8_t* s = gp_span(dat, p[idx], 8, "PlCo", "shake table");
        uint32_t vecs = 0, n = mv_be32(s + 4);
        int vr = gp_pointer(dat, p[idx], &vecs, "PlCo", "shake vectors");
        if (n > 256 || (n != 0 && vr != 1)) gp_fail("PlCo", "shake count", p[idx]);
        gp_swap32(dat, p[idx] + 4, "PlCo", "shake count");
        if (n != 0) gp_swap_words(dat, vecs, n * 8, "PlCo", "shake vectors");
    }
    if (present[12] == 1) gp_swap_words(dat, p[12], 0x9C, "PlCo", "scale modifiers");
    if (present[13] == 1) gp_swap_words(dat, p[13], 0x3C, "PlCo", "bunnyhood modifiers");
    if (present[14] == 1) gp_swap_words(dat, p[14], 0x24, "PlCo", "metal modifiers");
    if (present[15] == 1) gp_swap_words(dat, p[15], 0x08, "PlCo", "gravity/weight modifiers");
    if (present[8] == 1) {
        if (gp_target_span(dat, p[8]) != 8)
            gp_fail("PlCo", "common accessory roots span", p[8]);
        uint32_t joint = 0, anim = 0;
        if (gp_pointer(dat, p[8], &joint, "PlCo", "common accessory joint") != 1 ||
            gp_pointer(dat, p[8] + 4, &anim, "PlCo", "common accessory anim") != 1)
            gp_fail("PlCo", "common accessory roots", p[8]);
        mv_hsd_joint_graph_prepare_raw((void*) (uintptr_t) dat->file,
                                       dat->file_size, joint, filename,
                                       "PlCo.accessory_joint");
        mv_hsd_anim_graph_prepare_raw((void*) (uintptr_t) dat->file,
                                      dat->file_size, anim, filename,
                                      "PlCo.accessory_anim");
    }
    if (present[16] == 1) {
        mv_hsd_joint_graph_prepare_raw((void*) (uintptr_t) dat->file,
                                       dat->file_size, p[16], filename,
                                       "PlCo.trophy_platform_joint");
    }
    if (present[20] == 1) {
        mv_hsd_joint_graph_prepare_raw((void*) (uintptr_t) dat->file,
                                       dat->file_size, p[20], filename,
                                       "PlCo.common_joint_20");
    }
    if (present[21] == 1) gp_swap_words(dat, p[21], 0x44, "PlCo", "CrowdConfig");
    if (present[22] == 1) plco_convert_cpu_tables(dat, p[22]);
    OSReport("VITA_PLCO_NATIVE_PASS file=%s root=%08x\n",
             filename != NULL ? filename : "?", root);
}

static uint8_t itco_itemattr_byte0(uint8_t raw)
{
    /* PPC allocates these u8 bitfields from the MSB; ARM allocates them from
     * the LSB. Preserve each field's numeric value rather than bit-reversing
     * the byte wholesale. */
    uint8_t heavy = (raw >> 7) & 1u;
    uint8_t action = (raw >> 3) & 0x0Fu;
    uint8_t hold = raw & 0x07u;
    return heavy | (uint8_t) (action << 1) | (uint8_t) (hold << 5);
}

static uint8_t itco_itemattr_byte1(uint8_t raw)
{
    uint8_t x1_1 = (raw >> 6) & 0x03u;
    uint8_t x1_3 = (raw >> 5) & 1u;
    uint8_t x1_4 = (raw >> 4) & 1u;
    uint8_t x1_5 = (raw >> 3) & 1u;
    uint8_t cam = (raw >> 1) & 0x03u;
    uint8_t x1_8 = raw & 1u;
    return x1_1 | (uint8_t) (x1_3 << 2) | (uint8_t) (x1_4 << 3) |
           (uint8_t) (x1_5 << 4) | (uint8_t) (cam << 5) |
           (uint8_t) (x1_8 << 7);
}

static void itco_convert_itemattr(MvDat* dat, uint32_t off)
{
    uint8_t* p = gp_span(dat, off, 0x84, "ItCo", "ItemAttr");
    if (gp_is_pointer(dat, off) || gp_is_pointer(dat, off + 1) ||
        gp_is_pointer(dat, off + 2) || gp_is_pointer(dat, off + 3))
    {
        gp_fail("ItCo", "ItemAttr header relocation", off);
    }
    p[0] = itco_itemattr_byte0(p[0]);
    p[1] = itco_itemattr_byte1(p[1]);
    for (uint32_t o = 4; o < 0x84; o += 4) {
        gp_swap32(dat, off + o, "ItCo", "ItemAttr scalar");
    }
}

static void itco_convert_hurtboxes(MvDat* dat, uint32_t off)
{
    uint8_t* h = gp_span(dat, off, 8, "ItCo", "ItHurtBoneList");
    uint32_t count = mv_be32(h);
    uint32_t descs = 0;
    int dr = gp_pointer(dat, off + 4, &descs, "ItCo", "ItHurtBoneList.descs");
    if (count > 2 || (count != 0 && dr != 1)) {
        OSReport("VITA_ITCO_HURTBOX_INVALID off=%08x count=%u desc=%08x\n",
                 off, count, descs);
        gp_fail("ItCo", "ItHurtBoneList count/descs", off);
    }
    gp_swap32(dat, off, "ItCo", "ItHurtBoneList.count");
    if (count != 0) {
        gp_swap_words(dat, descs, (size_t) count * 0x20, "ItCo",
                      "ItHurtBoneDesc");
    }
}

static void itco_convert_model(MvDat* dat, uint32_t off,
                               uint32_t* joint_roots,
                               size_t* joint_root_count)
{
    uint8_t* m = gp_span(dat, off, 0x10, "ItCo", "ItemModelDesc");
    uint32_t joint = 0;
    uint32_t joint_word = mv_be32(m);
    int joint_result;
    /* Some fighter-owned Articles (for example Ice Climbers' GumStrings)
     * use -1 as the serialized "no model joint" sentinel.  It is endian
     * invariant, so preserve it and do not mistake it for a malformed
     * relocation. */
    if (!gp_is_pointer(dat, off) && joint_word == UINT32_MAX) {
        joint_result = 0;
    } else {
        joint_result =
            gp_pointer(dat, off, &joint, "ItCo", "ItemModelDesc.joint");
    }
    if (joint_result == 1) {
        (void) gp_mark_unique(joint_roots, joint_root_count, 128, joint,
                              "ItCo", "item model HSD root set overflow");
    }
    uint32_t bone_count = mv_be32(m + 4);
    int32_t attach_id = (int32_t) mv_be32(m + 8);
    if (bone_count > 100 || attach_id < -1 || attach_id > 100) {
        gp_fail("ItCo", "ItemModelDesc scalar", off);
    }
    gp_swap32(dat, off + 4, "ItCo", "ItemModelDesc.bone_count");
    gp_swap32(dat, off + 8, "ItCo", "ItemModelDesc.attach_id");
    /* xC is an explicit byte bit-mask, not a C bitfield. */
}

static void itco_convert_dynamics(MvDat* dat, uint32_t off,
                                  uint32_t* seen_sources,
                                  size_t* seen_source_count,
                                  size_t seen_source_capacity)
{
    uint8_t* d = gp_span(dat, off, 0x10, "ItCo", "ItemDynamics");
    uint32_t bone_count = mv_be32(d + 0x00);
    uint32_t bones = 0;
    uint32_t coll_count = mv_be32(d + 0x08);
    uint32_t coll = 0;
    int br = gp_pointer(dat, off + 0x04, &bones, "ItCo", "ItemDynamics.descs");
    int cr = gp_pointer(dat, off + 0x0C, &coll, "ItCo", "ItCollDynamics.descs");
    if (bone_count > 24 || coll_count > 2 ||
        (bone_count != 0 && br != 1) || (coll_count != 0 && cr != 1))
    {
        OSReport("VITA_ITCO_DYNAMICS_INVALID off=%08x bones=%u coll=%u bone_desc=%08x coll_desc=%08x\n",
                 off, bone_count, coll_count, bones, coll);
        gp_fail("ItCo", "ItemDynamics count/descs", off);
    }
    gp_swap32(dat, off + 0x00, "ItCo", "ItemDynamics.count");
    gp_swap32(dat, off + 0x08, "ItCo", "ItCollDynamics.count");

    for (uint32_t i = 0; i < bone_count; ++i) {
        uint32_t base = bones + i * 0x18;
        uint8_t* desc = gp_span(dat, base, 0x18, "ItCo", "BoneDynamicsDesc");
        int32_t bone_id = (int32_t) mv_be32(desc + 0x00);
        uint32_t source = 0;
        int sr = gp_pointer(dat, base + 0x04, &source, "ItCo",
                            "BoneDynamicsDesc.data");
        uint32_t source_count = mv_be32(desc + 0x08);
        if (bone_id < 0 || bone_id >= 100 || source_count > 32 ||
            (source_count != 0 && sr != 1))
        {
            gp_fail("ItCo", "BoneDynamicsDesc scalar/source", base);
        }
        gp_swap32(dat, base + 0x00, "ItCo", "BoneDynamicsDesc.bone");
        gp_swap32(dat, base + 0x08, "ItCo", "BoneDynamicsDesc.count");
        gp_swap32(dat, base + 0x0C, "ItCo", "BoneDynamicsDesc.pos");
        gp_swap32(dat, base + 0x10, "ItCo", "BoneDynamicsDesc.pos");
        gp_swap32(dat, base + 0x14, "ItCo", "BoneDynamicsDesc.pos");
        if (source_count != 0 &&
            gp_mark_unique(seen_sources, seen_source_count,
                           seen_source_capacity, source, "ItCo",
                           "dynamics source set overflow"))
        {
            gp_swap_words(dat, source, (size_t) source_count * 0x3C, "ItCo",
                          "item dynamics source");
        }
    }

    if (coll_count != 0) {
        gp_swap_words(dat, coll, (size_t) coll_count * 0x14, "ItCo",
                      "ItCollDynamicsDesc");
    }
}

static void itco_convert_article(MvDat* dat, uint32_t off,
                                 uint32_t* seen_attrs, size_t* attr_count,
                                 uint32_t* seen_hurts, size_t* hurt_count,
                                 uint32_t* seen_models, size_t* model_count,
                                 uint32_t* seen_dynamics, size_t* dynamics_count,
                                 uint32_t* seen_sources, size_t* source_count,
                                 uint32_t* joint_roots,
                                 size_t* joint_root_count)
{
    uint32_t field[6] = { 0 };
    int present[6] = { 0 };
    (void) gp_span(dat, off, 0x18, "ItCo", "Article");
    for (uint32_t i = 0; i < 6; ++i) {
        present[i] = gp_pointer(dat, off + i * 4, &field[i], "ItCo",
                                "Article pointer");
    }

    if (present[0] == 1 &&
        gp_mark_unique(seen_attrs, attr_count, 128, field[0], "ItCo",
                       "ItemAttr set overflow"))
    {
        itco_convert_itemattr(dat, field[0]);
    }
    if (present[2] == 1 &&
        gp_mark_unique(seen_hurts, hurt_count, 64, field[2], "ItCo",
                       "hurtbox set overflow"))
    {
        itco_convert_hurtboxes(dat, field[2]);
    }
    if (present[4] == 1 &&
        gp_mark_unique(seen_models, model_count, 128, field[4], "ItCo",
                       "model set overflow"))
    {
        itco_convert_model(dat, field[4], joint_roots, joint_root_count);
    }
    if (present[5] == 1 &&
        gp_mark_unique(seen_dynamics, dynamics_count, 16, field[5], "ItCo",
                       "dynamics set overflow"))
    {
        itco_convert_dynamics(dat, field[5], seen_sources, source_count, 32);
    }
}

typedef struct FighterItemArticleSchema {
    const char* root;
    uint32_t mask;
} FighterItemArticleSchema;

static uint32_t fighter_item_article_mask(const char* root_name)
{
    /* Bits are derived from the retail OnLoad registrations which call
     * it_8026B3F8(x48_items[index], ...).  Entries outside the mask are
     * deliberately not guessed: several are direct HSD roots or unrelated
     * accessory data. */
    static const FighterItemArticleSchema schema[] = {
        { "ftDataCrazyhand", 0x007u }, { "ftDataClink", 0x03Fu },
        { "ftDataDrmario", 0x00Au },   { "ftDataFalco", 0x00Bu },
        { "ftDataFox", 0x007u },       { "ftDataGamewatch", 0x3FFu },
        { "ftDataGkoopa", 0x001u },    { "ftDataKirby", 0x00Fu },
        { "ftDataKoopa", 0x001u },     { "ftDataLink", 0x01Fu },
        { "ftDataLuigi", 0x001u },     { "ftDataMario", 0x005u },
        { "ftDataMasterhand", 0x003u },{ "ftDataMewtwo", 0x003u },
        { "ftDataNess", 0x7FFu },      { "ftDataPeach", 0x01Fu },
        { "ftDataPichu", 0x007u },     { "ftDataPikachu", 0x007u },
        { "ftDataPopo", 0x007u },      { "ftDataSamus", 0x00Fu },
        { "ftDataSeak", 0x00Fu },      { "ftDataYoshi", 0x007u },
        { "ftDataZelda", 0x003u },
    };
    for (size_t i = 0; i < sizeof(schema) / sizeof(schema[0]); ++i) {
        if (strcmp(root_name, schema[i].root) == 0) return schema[i].mask;
    }
    return 0;
}

static void fighter_x48_add_direct_joint(MvDat* dat, uint32_t table,
                                         uint32_t index, const char* root_name,
                                         uint32_t* roots, size_t* root_count)
{
    uint32_t joint = 0;
    if (gp_pointer(dat, table + index * 4, &joint, root_name,
                   "x48 direct HSD joint") != 1)
    {
        gp_fail(root_name, "x48 direct HSD joint missing", table + index * 4);
    }
    (void) gp_mark_unique(roots, root_count, 128, joint, root_name,
                          "x48 HSD joint set overflow");
}

static void fighter_convert_x48(MvDat* dat, const char* root_name,
                                int table_present, uint32_t table,
                                const char* filename)
{
    uint32_t mask = fighter_item_article_mask(root_name);
    int has_special_hsd =
        strcmp(root_name, "ftDataClink") == 0 ||
        strcmp(root_name, "ftDataLink") == 0 ||
        strcmp(root_name, "ftDataKirby") == 0 ||
        strcmp(root_name, "ftDataSamus") == 0 ||
        strcmp(root_name, "ftDataSeak") == 0 ||
        strcmp(root_name, "ftDataYoshi") == 0;

    if (mask == 0 && !has_special_hsd) return;
    if (table_present != 1) gp_fail(root_name, "required x48 table missing", table);

    uint32_t seen_articles[128] = { 0 };
    uint32_t seen_attrs[128] = { 0 };
    uint32_t seen_hurts[64] = { 0 };
    uint32_t seen_models[128] = { 0 };
    uint32_t seen_dynamics[16] = { 0 };
    uint32_t seen_sources[32] = { 0 };
    uint32_t joint_roots[128] = { 0 };
    uint32_t anim_roots[32] = { 0 };
    uint32_t matanim_roots[16] = { 0 };
    size_t article_count = 0, attr_count = 0, hurt_count = 0;
    size_t model_count = 0, dynamics_count = 0, source_count = 0;
    size_t joint_root_count = 0, anim_root_count = 0, matanim_root_count = 0;

    if (mask != 0) {
        uint32_t highest = 0;
        for (uint32_t i = 0; i < 32; ++i) if (mask & (1u << i)) highest = i;
        (void) gp_span(dat, table, ((size_t) highest + 1) * 4, root_name,
                       "x48 Article table");
        for (uint32_t i = 0; i <= highest; ++i) {
            if ((mask & (1u << i)) == 0) continue;
            uint32_t article = 0;
            if (gp_pointer(dat, table + i * 4, &article, root_name,
                           "x48 Article") != 1)
            {
                gp_fail(root_name, "x48 Article missing", table + i * 4);
            }
            if (gp_mark_unique(seen_articles, &article_count, 128, article,
                               root_name, "x48 Article set overflow"))
            {
                itco_convert_article(dat, article, seen_attrs, &attr_count,
                                     seen_hurts, &hurt_count, seen_models,
                                     &model_count, seen_dynamics,
                                     &dynamics_count, seen_sources,
                                     &source_count, joint_roots,
                                     &joint_root_count);
            }
        }
    }

    /* Direct HSD entries in the same x48 table, identified from their actual
     * consumers rather than by structural guessing. */
    if (strcmp(root_name, "ftDataClink") == 0 ||
        strcmp(root_name, "ftDataLink") == 0)
    {
        fighter_x48_add_direct_joint(dat, table, 6, root_name,
                                     joint_roots, &joint_root_count);
    } else if (strcmp(root_name, "ftDataKirby") == 0) {
        fighter_x48_add_direct_joint(dat, table, 4, root_name,
                                     joint_roots, &joint_root_count);
    } else if (strcmp(root_name, "ftDataSeak") == 0) {
        fighter_x48_add_direct_joint(dat, table, 4, root_name,
                                     joint_roots, &joint_root_count);
        fighter_x48_add_direct_joint(dat, table, 5, root_name,
                                     joint_roots, &joint_root_count);
    } else if (strcmp(root_name, "ftDataYoshi") == 0) {
        fighter_x48_add_direct_joint(dat, table, 3, root_name,
                                     joint_roots, &joint_root_count);
    } else if (strcmp(root_name, "ftDataSamus") == 0) {
        uint32_t bundle = 0;
        if (gp_pointer(dat, table + 4 * 4, &bundle, root_name,
                       "Samus x48 accessory bundle") != 1)
        {
            gp_fail(root_name, "Samus x48 accessory bundle missing", table + 16);
        }
        (void) gp_span(dat, bundle, 0x10, root_name, "Samus accessory bundle");
        uint32_t joint = 0, anim_table = 0, anim = 0, matanim = 0;
        if (gp_pointer(dat, bundle + 0x00, &joint, root_name, "Samus accessory joint") != 1 ||
            gp_pointer(dat, bundle + 0x04, &anim_table, root_name, "Samus accessory anim table") != 1 ||
            gp_pointer(dat, bundle + 0x08, &anim, root_name, "Samus accessory anim") != 1 ||
            gp_pointer(dat, bundle + 0x0C, &matanim, root_name, "Samus accessory matanim") != 1)
        {
            gp_fail(root_name, "Samus accessory HSD root missing", bundle);
        }
        (void) gp_mark_unique(joint_roots, &joint_root_count, 128, joint,
                              root_name, "x48 HSD joint set overflow");
        (void) gp_mark_unique(anim_roots, &anim_root_count, 32, anim,
                              root_name, "x48 anim set overflow");
        (void) gp_mark_unique(matanim_roots, &matanim_root_count, 16, matanim,
                              root_name, "x48 matanim set overflow");
        size_t anim_span = gp_target_span(dat, anim_table);
        if (anim_span == 0 || anim_span > 0x40 || (anim_span & 3) != 0) {
            gp_fail(root_name, "Samus accessory anim table span", anim_table);
        }
        for (size_t i = 0; i < anim_span / 4; ++i) {
            uint32_t anim_root = 0;
            int ar = gp_pointer(dat, anim_table + (uint32_t) i * 4,
                                &anim_root, root_name,
                                "Samus accessory anim table entry");
            if (ar == 1) {
                (void) gp_mark_unique(anim_roots, &anim_root_count, 32,
                                      anim_root, root_name,
                                      "x48 anim set overflow");
            }
        }
    }

    if (joint_root_count != 0 || anim_root_count != 0 || matanim_root_count != 0) {
        mv_hsd_graph_set_prepare_raw(
            (void*) (uintptr_t) dat->file, dat->file_size,
            joint_roots, joint_root_count,
            anim_roots, anim_root_count,
            matanim_roots, matanim_root_count,
            NULL, 0, filename, "ftData.x48_items");
    }

    OSReport("VITA_FIGHTER_X48_NATIVE_PASS file=%s root=%s articles=%u attrs=%u hurts=%u models=%u joint_roots=%u anim_roots=%u matanim_roots=%u dynamics=%u\n",
             filename != NULL ? filename : "?", root_name,
             (unsigned) article_count, (unsigned) attr_count,
             (unsigned) hurt_count, (unsigned) model_count,
             (unsigned) joint_root_count, (unsigned) anim_root_count,
             (unsigned) matanim_root_count, (unsigned) dynamics_count);
}

static void itco_convert(MvDat* dat, uint32_t root, const char* filename)
{
    uint32_t p[6] = { 0 };
    int present[6] = { 0 };
    (void) gp_span(dat, root, 0x18, "ItCo", "itPublicData");
    for (uint32_t i = 0; i < 6; ++i)
        present[i] = gp_pointer(dat, root + i * 4, &p[i], "ItCo", "root pointer");
    if (present[0] != 1) gp_fail("ItCo", "ItemCommonData missing", root);

    (void) gp_span(dat, p[0], 0x160, "ItCo", "ItemCommonData");
    for (uint32_t o = 0; o < 0x160; o += 4) {
        if (o == 0x48 || o == 0xE4 || o == 0xEC) continue;
        gp_swap32(dat, p[0] + o, "ItCo", "ItemCommonData");
    }

    static const uint16_t article_counts[3] = { 43, 118, 47 };
    uint32_t seen_articles[128] = { 0 };
    uint32_t seen_attrs[128] = { 0 };
    uint32_t seen_hurts[64] = { 0 };
    uint32_t seen_models[128] = { 0 };
    uint32_t seen_dynamics[16] = { 0 };
    uint32_t seen_sources[32] = { 0 };
    uint32_t joint_roots[128] = { 0 };
    size_t article_count = 0, attr_count = 0, hurt_count = 0;
    size_t model_count = 0, dynamics_count = 0, source_count = 0;
    size_t joint_root_count = 0;
    for (uint32_t table = 0; table < 3; ++table) {
        uint32_t root_index = table + 1;
        if (present[root_index] != 1) {
            gp_fail("ItCo", "required Article table missing", root + root_index * 4);
        }
        (void) gp_span(dat, p[root_index],
                       (size_t) article_counts[table] * 4, "ItCo",
                       "Article pointer table");
        for (uint32_t i = 0; i < article_counts[table]; ++i) {
            uint32_t article = 0;
            int ar = gp_pointer(dat, p[root_index] + i * 4, &article, "ItCo",
                                "Article table entry");
            if (ar == 1 &&
                gp_mark_unique(seen_articles, &article_count, 128, article,
                               "ItCo", "Article set overflow"))
            {
                itco_convert_article(dat, article, seen_attrs, &attr_count,
                                     seen_hurts, &hurt_count, seen_models,
                                     &model_count, seen_dynamics,
                                     &dynamics_count, seen_sources,
                                     &source_count, joint_roots,
                                     &joint_root_count);
            }
        }
    }
    if (joint_root_count != 0) {
        mv_hsd_graph_set_prepare_raw(
            (void*) (uintptr_t) dat->file, dat->file_size,
            joint_roots, joint_root_count, NULL, 0, NULL, 0, NULL, 0,
            filename, "ItCo.ItemModelDesc");
    }
    if (present[4] == 1)
        gp_swap_words(dat, p[4], 0x1C, "ItCo", "item global scalar table");
    OSReport("VITA_ITCO_NATIVE_PASS file=%s root=%08x articles=%u attrs=%u hurts=%u models=%u model_joints=%u dynamics=%u sources=%u\n",
             filename != NULL ? filename : "?", root,
             (unsigned) article_count, (unsigned) attr_count,
             (unsigned) hurt_count, (unsigned) model_count,
             (unsigned) joint_root_count,
             (unsigned) dynamics_count, (unsigned) source_count);
}

void mv_fighter_figatree_prepare_raw(void* bytes, size_t size,
                                     const char* symbol)
{
    MvDat dat;
    const char* public_name = NULL;
    uint32_t root = 0;
    if (bytes == NULL || size == 0 || mv_dat_open(&dat, bytes, size) != 0) {
        HSD_Panic(__FILE__, __LINE__, "fighter FigaTree DAT parse failed");
    }

    int found = symbol != NULL ?
        gp_find_public(&dat, symbol, NULL, &public_name, &root) : 0;
    if (found <= 0 && dat.public_count == 1) {
        if (mv_dat_public(&dat, 0, &public_name, &root) != 0) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "fighter FigaTree public root invalid");
        }
        found = 1;
    }
    if (found <= 0) {
        OSReport("VITA_FIGATREE_INVALID symbol=%s reason=public-root count=%u\n",
                 symbol != NULL ? symbol : "?", dat.public_count);
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "fighter FigaTree public root missing");
    }

    uint8_t* tree = gp_span(&dat, root, 0x14, "FigaTree", "root");
    uint32_t type = mv_be32(tree + 0x00);
    uint32_t flags = mv_be32(tree + 0x04);
    uint32_t frame_bits = mv_be32(tree + 0x08);
    float frames;
    memcpy(&frames, &frame_bits, sizeof(frames));
    if (type > 0xFFu || !isfinite(frames) || frames < 0.0f ||
        frames > 100000.0f)
    {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "fighter FigaTree scalar invalid");
    }

    uint32_t nodes = 0, tracks = 0;
    if (gp_pointer(&dat, root + 0x0C, &nodes, "FigaTree", "nodes") != 1 ||
        gp_pointer(&dat, root + 0x10, &tracks, "FigaTree", "tracks") != 1)
    {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "fighter FigaTree nodes/tracks missing");
    }

    size_t node_count = 0;
    size_t track_count = 0;
    for (; node_count < 512; ++node_count) {
        int8_t value = *(int8_t*) gp_span(&dat, nodes + (uint32_t) node_count,
                                         1, "FigaTree", "nodes");
        if (value == -1) {
            ++node_count;
            break;
        }
        if (value < 0 || track_count + (uint8_t) value > 4096) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "fighter FigaTree node stream invalid");
        }
        track_count += (uint8_t) value;
    }
    if (node_count == 0 || node_count > 512 ||
        *(int8_t*) gp_span(&dat, nodes + (uint32_t) node_count - 1, 1,
                           "FigaTree", "nodes terminator") != -1)
    {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "fighter FigaTree nodes unterminated");
    }

    (void) gp_span(&dat, tracks, track_count * 0x0C,
                   "FigaTree", "track table");
    for (size_t i = 0; i < track_count; ++i) {
        uint32_t off = tracks + (uint32_t) i * 0x0C;
        uint32_t ignored = 0;
        (void) gp_pointer(&dat, off + 0x08, &ignored,
                          "FigaTree", "track data");
        gp_swap16(&dat, off + 0x00, "FigaTree", "track length");
        gp_swap16(&dat, off + 0x02, "FigaTree", "track startframe");
    }
    gp_swap32(&dat, root + 0x00, "FigaTree", "type");
    gp_swap32(&dat, root + 0x04, "FigaTree", "flags");
    gp_swap32(&dat, root + 0x08, "FigaTree", "frames");

    OSReport("VITA_FIGATREE_NATIVE_PASS symbol=%s nodes=%u tracks=%u type=%u flags=%08x frames=%f\n",
             public_name != NULL ? public_name : (symbol != NULL ? symbol : "?"),
             (unsigned) node_count, (unsigned) track_count, type, flags, frames);
    mv_dat_close(&dat);
}

void mv_gameplay_archive_prepare_raw(void* bytes, size_t size,
                                     const char* filename)
{
    MvDat dat;
    const char* name = NULL;
    uint32_t root = 0;
    if (bytes == NULL || size == 0 || mv_dat_open(&dat, bytes, size) != 0)
        return;

    int r = gp_find_public(&dat, NULL, "ftData", &name, &root);
    if (r < 0) {
        mv_dat_close(&dat);
        return;
    }
    if (r > 0) {
        fighter_convert(&dat, name, root, filename);
        mv_dat_close(&dat);
        return;
    }
    r = gp_find_public(&dat, "ftLoadCommonData", NULL, &name, &root);
    if (r > 0) {
        plco_convert(&dat, root, filename);
        mv_dat_close(&dat);
        return;
    }
    r = gp_find_public(&dat, "itPublicData", NULL, &name, &root);
    if (r > 0) {
        itco_convert(&dat, root, filename);
    }
    mv_dat_close(&dat);
}
