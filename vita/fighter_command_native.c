#include "fighter_command_native.h"

#include <dolphin/os.h>
#include <sysdolphin/baselib/debug.h>

#include <stdlib.h>
#include <string.h>

#define FIGHTER_CMD_SEEN_CAP 32768u

typedef struct FighterCommandContext {
    MvDat* dat;
    uint32_t* seen;
    MvFighterCommandRawResult* out;
    const char* label;
} FighterCommandContext;

static void fighter_cmd_fail(const char* label, const char* detail, uint32_t off)
{
    OSReport("VITA_FIGHTER_CMD_RAW_INVALID file=%s detail=%s off=%08x\n",
             label != NULL ? label : "?",
             detail != NULL ? detail : "?", off);
    HSD_Panic(__FILE__, __LINE__, "fighter command raw schema invalid");
}

static uint8_t* fighter_cmd_span(MvDat* dat, uint32_t off, size_t size,
                                 const char* label, const char* detail)
{
    const uint8_t* p = mv_dat_span(dat, off, size);
    if (p == NULL) {
        fighter_cmd_fail(label, detail, off);
    }
    return (uint8_t*) (uintptr_t) p;
}

static int fighter_cmd_is_pointer(const MvDat* dat, uint32_t off)
{
    return dat->pointer_bits != NULL &&
           (dat->pointer_bits[off / 8] & (1u << (off % 8))) != 0;
}

static int fighter_cmd_local_pointer(MvDat* dat, uint32_t field,
                                     uint32_t* target, const char* label,
                                     const char* detail)
{
    if (mv_dat_external(dat, field)) {
        fighter_cmd_fail(label, "external command target", field);
    }
    int r = mv_dat_pointer(dat, field, target);
    if (r < 0) {
        fighter_cmd_fail(label, detail, field);
    }
    return r;
}

static uint32_t fighter_cmd_pack_fields(uint32_t raw, const uint8_t* widths,
                                        size_t count, const char* label,
                                        uint32_t off)
{
    uint32_t native = 0;
    uint32_t ppc_shift = 32;
    uint32_t arm_shift = 0;

    for (size_t i = 0; i < count; ++i) {
        uint32_t width = widths[i];
        if (width == 0 || width > 32 || width > ppc_shift ||
            arm_shift + width > 32)
        {
            fighter_cmd_fail(label, "bitfield layout", off);
        }

        ppc_shift -= width;
        uint32_t mask =
            width == 32 ? UINT32_MAX : ((1u << width) - 1u);
        uint32_t value =
            width == 32 ? raw : ((raw >> ppc_shift) & mask);
        if (width == 32) {
            native = value;
        } else {
            native |= value << arm_shift;
        }
        arm_shift += width;
    }
    return native;
}

static void fighter_cmd_store_native32(MvDat* dat, uint32_t off,
                                        uint32_t value, const char* label,
                                        const char* detail)
{
    if (fighter_cmd_is_pointer(dat, off) || mv_dat_external(dat, off)) {
        fighter_cmd_fail(label, "scalar store on command pointer", off);
    }
    uint8_t* p = fighter_cmd_span(dat, off, 4, label, detail);
    memcpy(p, &value, sizeof(value));
}

static void fighter_cmd_repack(FighterCommandContext* ctx, uint32_t off,
                               const uint8_t* widths, size_t count,
                               const char* detail)
{
    uint8_t* p = fighter_cmd_span(ctx->dat, off, 4, ctx->label, detail);
    uint32_t raw = mv_be32(p);
    fighter_cmd_store_native32(
        ctx->dat, off,
        fighter_cmd_pack_fields(raw, widths, count, ctx->label, off),
        ctx->label, detail);
}

static void fighter_cmd_scalar32(FighterCommandContext* ctx, uint32_t off,
                                 const char* detail)
{
    uint8_t* p = fighter_cmd_span(ctx->dat, off, 4, ctx->label, detail);
    fighter_cmd_store_native32(ctx->dat, off, mv_be32(p), ctx->label, detail);
}

static int fighter_cmd_seen(FighterCommandContext* ctx, uint32_t off)
{
    uint32_t key = off + 1u;
    size_t slot =
        ((off >> 2) * 2654435761u) & (FIGHTER_CMD_SEEN_CAP - 1u);

    for (size_t probe = 0; probe < FIGHTER_CMD_SEEN_CAP; ++probe) {
        uint32_t* entry = &ctx->seen[slot];
        if (*entry == 0) {
            *entry = key;
            return 0;
        }
        if (*entry == key) {
            return 1;
        }
        slot = (slot + 1u) & (FIGHTER_CMD_SEEN_CAP - 1u);
    }
    fighter_cmd_fail(ctx->label, "command visited overflow", off);
    return 1;
}

static void fighter_cmd_convert(FighterCommandContext* ctx, uint32_t off,
                                unsigned depth)
{
    static const uint8_t generic[] = { 6, 26 };
    static const uint8_t w_6_8_18[] = { 6, 8, 18 };
    static const uint8_t w_16_16[] = { 16, 16 };
    static const uint8_t w_6_3_23[] = { 6, 3, 23 };
    static const uint8_t w_6_24_1_1[] = { 6, 24, 1, 1 };
    static const uint8_t w_6_2_24[] = { 6, 2, 24 };
    static const uint8_t w_6_7_19[] = { 6, 7, 19 };
    static const uint8_t w_9_9_9[] = { 9, 9, 9 };
    static const uint8_t w_9_4_3_4[] = { 9, 4, 3, 4 };
    static const uint8_t w_6_1_7_7_11[] = { 6, 1, 7, 7, 11 };
    static const uint8_t w_6_7_7_12[] = { 6, 7, 7, 12 };
    static const uint8_t w_6_13_13[] = { 6, 13, 13 };
    static const uint8_t w_6_1_12_13[] = { 6, 1, 12, 13 };
    static const uint8_t w_6_2_10_14[] = { 6, 2, 10, 14 };
    static const uint8_t w_6_8[] = { 6, 8 };
    static const uint8_t w_6_1_25[] = { 6, 1, 25 };
    static const uint8_t w_6_1_8[] = { 6, 1, 8 };
    static const uint8_t spawn_gfx0[] = { 6, 8, 1, 1, 1, 15 };
    static const uint8_t spawn_hitbox0[] = { 6, 3, 3, 1, 8, 1, 10 };
    static const uint8_t spawn_hitbox3[] = { 9, 9, 9, 1, 1, 1, 1, 1 };
    static const uint8_t spawn_hitbox4[] = { 9, 5, 8, 3, 5, 1, 1 };
    static const uint8_t random_sfx0[] = { 6, 8, 8, 4, 6 };
    static const uint8_t stage_sfx0[] = { 6, 10, 8, 8 };
    static const uint8_t w_16_8_8[] = { 16, 8, 8 };
    static const uint8_t footstep0[] = { 6, 8, 1, 1, 8, 8 };
    static const uint8_t unk_fx0[] = { 6, 2, 8, 8, 8 };
    static const uint8_t smash_charge0[] = { 6, 10, 16 };
    static const uint8_t smash_charge1[] = { 8, 24 };
    static const uint8_t wind_fx0[] = { 6, 18, 8 };

    static const uint8_t command_words[59] = {
        1, 1, 1, 1, 1, 2, 1, 2, 1, 1,
        5, 5, 1, 1, 1, 1, 1, 3, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 3, 1, 1, 1, 7, 4,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 3, 3, 2, 1, 4
    };

    if (depth > 64) {
        fighter_cmd_fail(ctx->label, "command recursion", off);
    }

    for (;;) {
        if (fighter_cmd_seen(ctx, off)) {
            return;
        }

        uint8_t* p =
            fighter_cmd_span(ctx->dat, off, 4, ctx->label, "command word");
        if (fighter_cmd_is_pointer(ctx->dat, off) ||
            mv_dat_external(ctx->dat, off))
        {
            fighter_cmd_fail(ctx->label, "command starts on pointer", off);
        }

        uint32_t raw = mv_be32(p);
        uint32_t opcode = raw >> 26;
        if (opcode >= 59) {
            fighter_cmd_fail(ctx->label, "opcode outside fighter dispatch",
                             off);
        }
        ++ctx->out->command_count;
        ++ctx->out->histogram[opcode];

        if (opcode == 5 || opcode == 7) {
            uint32_t target = 0;
            fighter_cmd_repack(ctx, off, generic, sizeof(generic),
                               opcode == 5 ? "subroutine" : "goto");
            int r = fighter_cmd_local_pointer(
                ctx->dat, off + 4, &target, ctx->label,
                opcode == 5 ? "subroutine target" : "goto target");
            if (r == 1) {
                fighter_cmd_convert(ctx, target, depth + 1);
            } else if (opcode == 5) {
                fighter_cmd_fail(ctx->label, "subroutine target missing",
                                 off + 4);
            }
            if (opcode == 7) {
                return;
            }
            off += 8;
            continue;
        }

        switch (opcode) {
        case 9:
            fighter_cmd_repack(ctx, off, w_6_8_18, sizeof(w_6_8_18),
                               "background flash");
            break;
        case 10:
            fighter_cmd_repack(ctx, off, spawn_gfx0, sizeof(spawn_gfx0),
                               "gfx word0");
            for (uint32_t i = 1; i < 5; ++i) {
                fighter_cmd_repack(ctx, off + i * 4, w_16_16,
                                   sizeof(w_16_16), "gfx payload");
            }
            break;
        case 11:
            fighter_cmd_repack(ctx, off, spawn_hitbox0,
                               sizeof(spawn_hitbox0), "hitbox word0");
            fighter_cmd_repack(ctx, off + 4, w_16_16, sizeof(w_16_16),
                               "hitbox word1");
            fighter_cmd_repack(ctx, off + 8, w_16_16, sizeof(w_16_16),
                               "hitbox word2");
            fighter_cmd_repack(ctx, off + 12, spawn_hitbox3,
                               sizeof(spawn_hitbox3), "hitbox word3");
            fighter_cmd_repack(ctx, off + 16, spawn_hitbox4,
                               sizeof(spawn_hitbox4), "hitbox word4");
            break;
        case 12:
        case 13:
        case 34:
            fighter_cmd_repack(ctx, off, w_6_3_23, sizeof(w_6_3_23),
                               "hitbox scalar");
            if (opcode == 34) {
                fighter_cmd_repack(ctx, off + 4, w_9_9_9,
                                   sizeof(w_9_9_9), "throw hitbox word1");
                fighter_cmd_repack(ctx, off + 8, w_9_4_3_4,
                                   sizeof(w_9_4_3_4), "throw hitbox word2");
            }
            break;
        case 14:
            fighter_cmd_repack(ctx, off, w_6_24_1_1,
                               sizeof(w_6_24_1_1), "hitbox flags");
            break;
        case 17:
            fighter_cmd_repack(ctx, off, w_6_8_18, sizeof(w_6_8_18),
                               "sound word0");
            fighter_cmd_scalar32(ctx, off + 4, "sound id");
            fighter_cmd_repack(ctx, off + 8, w_16_8_8,
                               sizeof(w_16_8_8), "sound params");
            break;
        case 19:
            fighter_cmd_repack(ctx, off, w_6_2_24, sizeof(w_6_2_24),
                               "command variable");
            break;
        case 28:
        case 46:
            fighter_cmd_repack(ctx, off, w_6_8_18, sizeof(w_6_8_18),
                               opcode == 28 ? "hurt state" : "command pair");
            break;
        case 31:
            fighter_cmd_repack(ctx, off, w_6_7_19, sizeof(w_6_7_19),
                               "dobj flags");
            break;
        case 38:
            fighter_cmd_repack(ctx, off, random_sfx0, sizeof(random_sfx0),
                               "random sound word0");
            for (uint32_t i = 1; i < 7; ++i) {
                fighter_cmd_scalar32(ctx, off + i * 4,
                                     "random sound id");
            }
            break;
        case 39:
            fighter_cmd_repack(ctx, off, stage_sfx0, sizeof(stage_sfx0),
                               "stage sound word0");
            fighter_cmd_scalar32(ctx, off + 4, "stage sound id");
            fighter_cmd_repack(ctx, off + 8, w_16_16, sizeof(w_16_16),
                               "stage sound word2");
            fighter_cmd_repack(ctx, off + 12, w_16_8_8,
                               sizeof(w_16_8_8), "stage sound word3");
            break;
        case 40:
            fighter_cmd_repack(ctx, off, w_6_1_7_7_11,
                               sizeof(w_6_1_7_7_11), "texture animation");
            break;
        case 41:
            fighter_cmd_repack(ctx, off, w_6_7_7_12,
                               sizeof(w_6_7_7_12), "part animation");
            break;
        case 42:
            fighter_cmd_repack(ctx, off, w_6_13_13,
                               sizeof(w_6_13_13), "fighter pair");
            break;
        case 43:
            fighter_cmd_repack(ctx, off, w_6_1_12_13,
                               sizeof(w_6_1_12_13), "fighter triple");
            break;
        case 45:
            fighter_cmd_repack(ctx, off, w_6_2_10_14,
                               sizeof(w_6_2_10_14), "fighter command");
            break;
        case 47:
            fighter_cmd_repack(ctx, off, w_6_8, sizeof(w_6_8),
                               "fighter byte command");
            break;
        case 49:
            fighter_cmd_repack(ctx, off, w_6_1_25,
                               sizeof(w_6_1_25), "fighter signed command");
            break;
        case 54:
            fighter_cmd_repack(ctx, off, footstep0, sizeof(footstep0),
                               "footstep word0");
            fighter_cmd_scalar32(ctx, off + 4, "footstep sound id");
            fighter_cmd_repack(ctx, off + 8, w_16_8_8,
                               sizeof(w_16_8_8), "footstep sound params");
            break;
        case 55:
            fighter_cmd_repack(ctx, off, unk_fx0, sizeof(unk_fx0),
                               "fighter effect word0");
            fighter_cmd_scalar32(ctx, off + 4, "fighter effect sound id");
            fighter_cmd_repack(ctx, off + 8, w_16_8_8,
                               sizeof(w_16_8_8), "fighter effect sound params");
            break;
        case 56:
            fighter_cmd_repack(ctx, off, smash_charge0,
                               sizeof(smash_charge0), "smash charge word0");
            fighter_cmd_repack(ctx, off + 4, smash_charge1,
                               sizeof(smash_charge1), "smash charge word1");
            break;
        case 57:
            fighter_cmd_repack(ctx, off, w_6_1_8, sizeof(w_6_1_8),
                               "fighter short command");
            break;
        case 58:
            fighter_cmd_repack(ctx, off, wind_fx0, sizeof(wind_fx0),
                               "wind word0");
            for (uint32_t i = 1; i < 4; ++i) {
                fighter_cmd_repack(ctx, off + i * 4, w_16_16,
                                   sizeof(w_16_16), "wind payload");
            }
            break;
        default:
            fighter_cmd_repack(ctx, off, generic, sizeof(generic),
                               "fighter command");
            break;
        }

        if (opcode == 0 || opcode == 6) {
            return;
        }
        off += (uint32_t) command_words[opcode] * 4u;
    }
}

int mv_fighter_command_scripts_prepare_raw(
    void* bytes, size_t size, const uint32_t* script_roots,
    size_t script_root_count, MvFighterCommandRawResult* out,
    const char* label)
{
    MvDat dat;
    if (out == NULL || script_roots == NULL || script_root_count == 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    if (mv_dat_open(&dat, bytes, size) != 0) {
        fighter_cmd_fail(label, "DAT parse", 0);
    }

    FighterCommandContext ctx = { 0 };
    ctx.dat = &dat;
    ctx.out = out;
    ctx.label = label;
    ctx.seen = calloc(FIGHTER_CMD_SEEN_CAP, sizeof(*ctx.seen));
    if (ctx.seen == NULL) {
        mv_dat_close(&dat);
        fighter_cmd_fail(label, "visited allocation", 0);
    }

    uint32_t unique_roots[4096] = { 0 };
    size_t unique_root_count = 0;
    for (size_t i = 0; i < script_root_count; ++i) {
        uint32_t root = script_roots[i];
        int duplicate = 0;
        for (size_t j = 0; j < unique_root_count; ++j) {
            if (unique_roots[j] == root) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (unique_root_count >=
            sizeof(unique_roots) / sizeof(unique_roots[0]))
        {
            free(ctx.seen);
            mv_dat_close(&dat);
            fighter_cmd_fail(label, "script root overflow", root);
        }
        unique_roots[unique_root_count++] = root;
        fighter_cmd_convert(&ctx, root, 0);
    }
    out->root_count = (uint32_t) unique_root_count;

    OSReport(
        "VITA_FIGHTER_CMD_RAW_NATIVE_PASS file=%s roots=%u commands=%u "
        "op0=%u op1=%u op2=%u op3=%u op4=%u op5=%u op6=%u op7=%u "
        "op10=%u op11=%u op17=%u op28=%u op38=%u op39=%u op55=%u\n",
        label != NULL ? label : "?", out->root_count, out->command_count,
        out->histogram[0], out->histogram[1], out->histogram[2],
        out->histogram[3], out->histogram[4], out->histogram[5],
        out->histogram[6], out->histogram[7], out->histogram[10],
        out->histogram[11], out->histogram[17], out->histogram[28],
        out->histogram[38], out->histogram[39], out->histogram[55]);

    free(ctx.seen);
    mv_dat_close(&dat);
    return 0;
}
