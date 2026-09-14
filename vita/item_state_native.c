#include "item_state_native.h"

#include <dolphin/os.h>
#include <sysdolphin/baselib/debug.h>

#include <stdlib.h>
#include <string.h>

#define ITEM_SCRIPT_SEEN_CAP 8192u

typedef struct ItemScriptContext {
    MvDat* dat;
    uint32_t* seen;
    size_t seen_count;
    MvItemStateRawResult* out;
    const char* label;
} ItemScriptContext;

static void item_fail(const char* label, const char* detail, uint32_t off)
{
    OSReport("VITA_ITEM_STATE_RAW_INVALID file=%s detail=%s off=%08x\n",
             label != NULL ? label : "?",
             detail != NULL ? detail : "?", off);
    HSD_Panic(__FILE__, __LINE__, "item state raw schema invalid");
}

static uint8_t* item_span(MvDat* dat, uint32_t off, size_t size,
                          const char* label, const char* detail)
{
    const uint8_t* p = mv_dat_span(dat, off, size);
    if (p == NULL) {
        item_fail(label, detail, off);
    }
    return (uint8_t*) (uintptr_t) p;
}

static int item_is_pointer(const MvDat* dat, uint32_t off)
{
    return dat->pointer_bits != NULL &&
           (dat->pointer_bits[off / 8] & (1u << (off % 8))) != 0;
}

static int item_local_pointer(MvDat* dat, uint32_t field, uint32_t* target,
                              const char* label, const char* detail)
{
    if (mv_dat_external(dat, field)) {
        return 2;
    }
    int r = mv_dat_pointer(dat, field, target);
    if (r < 0) {
        item_fail(label, detail, field);
    }
    return r;
}

static size_t item_target_span(const MvDat* dat, uint32_t target)
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
        if (mv_dat_public(dat, i, &ignored, &value) == 0 &&
            value > target && value < next)
        {
            next = value;
        }
    }
    return next >= target ? (size_t) (next - target) : 0;
}

/* The next relocation target is normally the exact end of an ItemStateDesc
 * array. Kirby cutter-beam is the one PAL fighter exception: its next target
 * lands eight bytes after the single descriptor. Accept such a partial tail
 * only when the available words prove that another descriptor cannot start
 * there (a nonzero scalar in a pointer/null slot). Ambiguous tails remain a
 * hard failure instead of being rounded down silently. */
static size_t item_state_desc_count(MvDat* dat, uint32_t table, size_t span,
                                    const char* label)
{
    if (span < 0x10 || span > 0x1000) {
        item_fail(label, "state table span", table);
    }
    size_t count = span / 0x10;
    size_t tail = span % 0x10;
    if (tail == 0) {
        return count;
    }
    if ((tail & 3) != 0 || count == 0) {
        item_fail(label, "state table partial tail", table);
    }

    uint32_t partial = table + (uint32_t) count * 0x10;
    int proves_boundary = 0;
    for (size_t off = 0; off < tail; off += 4) {
        uint32_t field = partial + (uint32_t) off;
        const uint8_t* p = mv_dat_span(dat, field, 4);
        if (p == NULL) {
            item_fail(label, "state table partial tail range", field);
        }
        if (!item_is_pointer(dat, field) && !mv_dat_external(dat, field) &&
            mv_be32(p) != 0)
        {
            proves_boundary = 1;
            break;
        }
    }
    if (!proves_boundary) {
        item_fail(label, "ambiguous state table partial tail", partial);
    }
    OSReport("VITA_ITEM_STATE_PARTIAL_TAIL_BOUNDARY file=%s table=%08x span=%u descs=%u tail=%u\n",
             label != NULL ? label : "?", table, (unsigned) span,
             (unsigned) count, (unsigned) tail);
    return count;
}

static int item_add_unique(uint32_t* values, size_t* count, size_t capacity,
                           uint32_t value, const char* label,
                           const char* detail)
{
    for (size_t i = 0; i < *count; ++i) {
        if (values[i] == value) {
            return 0;
        }
    }
    if (*count >= capacity) {
        item_fail(label, detail, value);
    }
    values[(*count)++] = value;
    return 1;
}

static uint32_t item_pack_ppc_fields(uint32_t raw, const uint8_t* widths,
                                     size_t count, const char* label,
                                     uint32_t off)
{
    uint32_t native = 0;
    uint32_t ppc_shift = 32;
    uint32_t arm_shift = 0;

    for (size_t i = 0; i < count; ++i) {
        uint32_t width = widths[i];
        if (width == 0 || width > 31 || width > ppc_shift ||
            arm_shift + width > 32)
        {
            item_fail(label, "bitfield layout", off);
        }
        ppc_shift -= width;
        uint32_t mask = (1u << width) - 1u;
        native |= ((raw >> ppc_shift) & mask) << arm_shift;
        arm_shift += width;
    }
    if (ppc_shift != 0 || arm_shift != 32) {
        item_fail(label, "bitfield width", off);
    }
    return native;
}

static void item_store_native32(MvDat* dat, uint32_t off, uint32_t value,
                                const char* label, const char* detail)
{
    if (item_is_pointer(dat, off) || mv_dat_external(dat, off)) {
        item_fail(label, "attempted scalar store on pointer", off);
    }
    uint8_t* p = item_span(dat, off, 4, label, detail);
    memcpy(p, &value, sizeof(value));
}

static void item_repack(ItemScriptContext* ctx, uint32_t off,
                        const uint8_t* widths, size_t count,
                        const char* detail)
{
    uint8_t* p = item_span(ctx->dat, off, 4, ctx->label, detail);
    uint32_t raw = mv_be32(p);
    item_store_native32(ctx->dat, off,
                        item_pack_ppc_fields(raw, widths, count,
                                             ctx->label, off),
                        ctx->label, detail);
}

static void item_swap_scalar32(ItemScriptContext* ctx, uint32_t off,
                               const char* detail)
{
    uint8_t* p = item_span(ctx->dat, off, 4, ctx->label, detail);
    uint32_t value = __builtin_bswap32(*(uint32_t*) (void*) p);
    item_store_native32(ctx->dat, off, value, ctx->label, detail);
}

static int item_script_seen(ItemScriptContext* ctx, uint32_t off)
{
    uint32_t key = off + 1u;
    size_t slot = ((off >> 2) * 2654435761u) & (ITEM_SCRIPT_SEEN_CAP - 1u);
    for (size_t probe = 0; probe < ITEM_SCRIPT_SEEN_CAP; ++probe) {
        uint32_t* entry = &ctx->seen[slot];
        if (*entry == 0) {
            *entry = key;
            ++ctx->seen_count;
            return 0;
        }
        if (*entry == key) {
            return 1;
        }
        slot = (slot + 1u) & (ITEM_SCRIPT_SEEN_CAP - 1u);
    }
    item_fail(ctx->label, "script visited overflow", off);
    return 1;
}

static void item_script_convert(ItemScriptContext* ctx, uint32_t off,
                                unsigned depth)
{
    static const uint8_t op_value26[] = { 6, 26 };
    static const uint8_t op_8_18[] = { 6, 8, 18 };
    static const uint8_t op_3_23[] = { 6, 3, 23 };
    static const uint8_t op_13_13[] = { 6, 13, 13 };
    static const uint8_t op10_word0[] = { 6, 10, 16 };
    static const uint8_t op11_word0[] = { 6, 3, 3, 7, 13 };
    static const uint8_t pair16[] = { 16, 16 };
    static const uint8_t op11_word3[] = { 9, 9, 9, 1, 1, 1, 1, 1 };
    static const uint8_t op11_word4[] = { 9, 5, 1, 8, 3, 4, 1, 1 };
    static const uint8_t op16_word0[] = { 6, 8, 2, 16 };

    if (depth > 64) {
        item_fail(ctx->label, "script recursion", off);
    }
    ++ctx->out->script_root_count;

    for (;;) {
        if (item_script_seen(ctx, off)) {
            return;
        }
        uint8_t* p = item_span(ctx->dat, off, 4, ctx->label, "script command");
        uint32_t raw = mv_be32(p);
        uint32_t opcode = raw >> 26;
        if (opcode >= 26) {
            item_fail(ctx->label, "opcode outside item dispatch", off);
        }
        ++ctx->out->command_count;
        ++ctx->out->histogram[opcode];

        switch (opcode) {
        case 0:
            item_repack(ctx, off, op_value26, sizeof(op_value26), "reset");
            return;
        case 1:
        case 2:
        case 3:
        case 4:
        case 8:
            item_repack(ctx, off, op_value26, sizeof(op_value26),
                        "common command");
            off += 4;
            break;
        case 5: {
            uint32_t target = 0;
            item_repack(ctx, off, op_value26, sizeof(op_value26),
                        "subroutine command");
            if (item_local_pointer(ctx->dat, off + 4, &target, ctx->label,
                                   "subroutine target") != 1)
            {
                item_fail(ctx->label, "subroutine target missing", off + 4);
            }
            item_script_convert(ctx, target, depth + 1);
            off += 8;
            break;
        }
        case 6:
            item_repack(ctx, off, op_value26, sizeof(op_value26), "return");
            return;
        case 7: {
            uint32_t target = 0;
            item_repack(ctx, off, op_value26, sizeof(op_value26), "goto");
            if (item_local_pointer(ctx->dat, off + 4, &target, ctx->label,
                                   "goto target") != 1)
            {
                item_fail(ctx->label, "goto target missing", off + 4);
            }
            item_script_convert(ctx, target, depth + 1);
            return;
        }
        case 9:
            item_repack(ctx, off, op_8_18, sizeof(op_8_18),
                        "background flash");
            off += 4;
            break;
        case 10:
            (void) item_span(ctx->dat, off, 20, ctx->label, "effect command");
            item_repack(ctx, off, op10_word0, sizeof(op10_word0),
                        "effect word0");
            for (uint32_t i = 1; i < 5; ++i) {
                item_repack(ctx, off + i * 4, pair16, sizeof(pair16),
                            "effect payload");
            }
            off += 20;
            break;
        case 11:
            (void) item_span(ctx->dat, off, 24, ctx->label, "hitbox command");
            item_repack(ctx, off + 0x00, op11_word0, sizeof(op11_word0),
                        "hitbox word0");
            item_repack(ctx, off + 0x04, pair16, sizeof(pair16),
                        "hitbox word1");
            item_repack(ctx, off + 0x08, pair16, sizeof(pair16),
                        "hitbox word2");
            item_repack(ctx, off + 0x0C, op11_word3, sizeof(op11_word3),
                        "hitbox word3");
            item_repack(ctx, off + 0x10, op11_word4, sizeof(op11_word4),
                        "hitbox word4");
            /* word5 is intentionally consumed byte-by-byte by it_802790C0 */
            off += 24;
            break;
        case 12:
        case 13:
            item_repack(ctx, off, op_3_23, sizeof(op_3_23),
                        "hitbox scalar");
            off += 4;
            break;
        case 14:
        case 15:
        case 17:
        case 18:
        case 19:
        case 20:
        case 22:
        case 25:
            item_repack(ctx, off, op_value26, sizeof(op_value26),
                        "item command");
            off += 4;
            break;
        case 16: {
            uint32_t subopcode = (raw >> 18) & 0xffu;
            ++ctx->out->special_histogram[subopcode];
            item_repack(ctx, off, op16_word0, sizeof(op16_word0),
                        "special command");
            if (subopcode <= 2) {
                (void) item_span(ctx->dat, off, 12, ctx->label,
                                 "special command payload");
                item_swap_scalar32(ctx, off + 4, "special arg1");
                /* off+8 is byte-addressed for arg2/arg3 and remains raw. */
                off += 12;
            } else if (subopcode == 10 || subopcode == 11) {
                (void) item_span(ctx->dat, off, 12, ctx->label,
                                 "special skipped payload");
                off += 12;
            } else {
                (void) item_span(ctx->dat, off, 8, ctx->label,
                                 "special skipped word");
                off += 8;
            }
            break;
        }
        case 21:
        case 23:
            item_repack(ctx, off, op_13_13, sizeof(op_13_13),
                        "item pair command");
            off += 4;
            break;
        case 24:
            item_repack(ctx, off, op_8_18, sizeof(op_8_18),
                        "color overlay command");
            off += 4;
            break;
        default:
            item_fail(ctx->label, "unhandled item opcode", off);
        }
    }
}

int mv_item_state_tables_prepare_raw(void* bytes, size_t size,
                                     const uint32_t* state_tables,
                                     size_t state_table_count,
                                     MvItemStateRawResult* out,
                                     const char* label)
{
    MvDat dat;
    uint32_t seen_tables[256] = { 0 };

    if (out == NULL || state_tables == NULL || state_table_count == 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (mv_dat_open(&dat, bytes, size) != 0) {
        item_fail(label, "DAT parse", 0);
    }

    ItemScriptContext scripts = { 0 };
    scripts.dat = &dat;
    scripts.out = out;
    scripts.label = label;
    scripts.seen = calloc(ITEM_SCRIPT_SEEN_CAP, sizeof(*scripts.seen));
    if (scripts.seen == NULL) {
        mv_dat_close(&dat);
        item_fail(label, "script visited allocation", 0);
    }

    size_t unique_table_count = 0;
    for (size_t t = 0; t < state_table_count; ++t) {
        uint32_t table = state_tables[t];
        if (!item_add_unique(seen_tables, &unique_table_count,
                             sizeof(seen_tables) / sizeof(seen_tables[0]),
                             table, label, "state table set overflow"))
        {
            continue;
        }

        size_t span = item_target_span(&dat, table);
        size_t desc_count = item_state_desc_count(&dat, table, span, label);
        ++out->table_count;
        out->desc_count += (uint32_t) desc_count;

        for (size_t i = 0; i < desc_count; ++i) {
            uint32_t base = table + (uint32_t) i * 0x10;
            uint32_t target = 0;
            int r = item_local_pointer(&dat, base + 0x00, &target, label,
                                       "state anim");
            if (r == 1) {
                (void) item_add_unique(out->anim_roots, &out->anim_count,
                                       MV_ITEM_STATE_ROOT_CAP, target, label,
                                       "anim root overflow");
            }
            r = item_local_pointer(&dat, base + 0x04, &target, label,
                                   "state matanim");
            if (r == 1) {
                (void) item_add_unique(out->matanim_roots,
                                       &out->matanim_count,
                                       MV_ITEM_STATE_ROOT_CAP, target, label,
                                       "matanim root overflow");
            }
            r = item_local_pointer(&dat, base + 0x08, &target, label,
                                   "state shapeanim");
            if (r == 1) {
                (void) item_add_unique(out->shape_roots, &out->shape_count,
                                       MV_ITEM_STATE_ROOT_CAP, target, label,
                                       "shape root overflow");
            }
            r = item_local_pointer(&dat, base + 0x0C, &target, label,
                                   "state script");
            if (r == 1) {
                item_script_convert(&scripts, target, 0);
            }
        }
    }

    OSReport("VITA_ITEM_STATE_RAW_NATIVE_PASS file=%s tables=%u descs=%u scripts=%u commands=%u anim=%u matanim=%u shape=%u op0=%u op1=%u op2=%u op3=%u op4=%u op5=%u op6=%u op7=%u op9=%u op10=%u op11=%u op16=%u op21=%u op23=%u op24=%u\n",
             label != NULL ? label : "?",
             out->table_count, out->desc_count, out->script_root_count,
             out->command_count, (unsigned) out->anim_count,
             (unsigned) out->matanim_count, (unsigned) out->shape_count,
             out->histogram[0], out->histogram[1], out->histogram[2],
             out->histogram[3], out->histogram[4], out->histogram[5],
             out->histogram[6], out->histogram[7], out->histogram[9],
             out->histogram[10], out->histogram[11], out->histogram[16],
             out->histogram[21], out->histogram[23], out->histogram[24]);

    free(scripts.seen);
    mv_dat_close(&dat);
    return 0;
}
