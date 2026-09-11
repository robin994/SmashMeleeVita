#include <dolphin/os.h>
#include <sysdolphin/baselib/debug.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hsd_data.h"

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
    uint32_t arrays[2] = { 0, 0 };
    int ar0 = gp_pointer(dat, off + 0x00, &arrays[0], root, "FtSFX.smash");
    int ar1 = gp_pointer(dat, off + 0x20, &arrays[1], root, "FtSFX.x20");
    for (uint32_t p = 0x04; p < 0x20; p += 4)
        gp_swap32(dat, off + p, root, "FtSFX scalar");
    for (uint32_t p = 0x24; p < 0x38; p += 4)
        gp_swap32(dat, off + p, root, "FtSFX scalar");
    int present[2] = { ar0, ar1 };
    for (int i = 0; i < 2; ++i) {
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
        uint32_t ignored;
        (void) gp_span(dat, t[2], 0x15, root_name, "ftData_x8");
        (void) gp_pointer(dat, t[2] + 0x04, &ignored, root_name, "vis_table");
        (void) gp_pointer(dat, t[2] + 0x0C, &ignored, root_name, "tobj table");
        gp_swap32(dat, t[2] + 0x00, root_name, "model_num");
        gp_swap32(dat, t[2] + 0x08, root_name, "costume tobj count");
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
    if (present[21] == 1) gp_swap_words(dat, p[21], 0x44, "PlCo", "CrowdConfig");
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

static void itco_convert_model(MvDat* dat, uint32_t off)
{
    uint8_t* m = gp_span(dat, off, 0x10, "ItCo", "ItemModelDesc");
    uint32_t ignored = 0;
    (void) gp_pointer(dat, off, &ignored, "ItCo", "ItemModelDesc.joint");
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
                                 uint32_t* seen_sources, size_t* source_count)
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
        itco_convert_model(dat, field[4]);
    }
    if (present[5] == 1 &&
        gp_mark_unique(seen_dynamics, dynamics_count, 16, field[5], "ItCo",
                       "dynamics set overflow"))
    {
        itco_convert_dynamics(dat, field[5], seen_sources, source_count, 32);
    }
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
    size_t article_count = 0, attr_count = 0, hurt_count = 0;
    size_t model_count = 0, dynamics_count = 0, source_count = 0;
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
                                     &source_count);
            }
        }
    }
    if (present[4] == 1)
        gp_swap_words(dat, p[4], 0x1C, "ItCo", "item global scalar table");
    OSReport("VITA_ITCO_NATIVE_PASS file=%s root=%08x articles=%u attrs=%u hurts=%u models=%u dynamics=%u sources=%u\n",
             filename != NULL ? filename : "?", root,
             (unsigned) article_count, (unsigned) attr_count,
             (unsigned) hurt_count, (unsigned) model_count,
             (unsigned) dynamics_count, (unsigned) source_count);
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
