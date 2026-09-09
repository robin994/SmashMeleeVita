#include "hsd_data.h"
#include "hsd_scene.h"
#include "gx_texture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *b, uint32_t n)
{ b[0] = n >> 24; b[1] = n >> 16; b[2] = n >> 8; b[3] = n; }
static void pixel(const uint8_t *p, unsigned r, unsigned g, unsigned b, unsigned a)
{ assert(p[0] == r && p[1] == g && p[2] == b && p[3] == a); }
static void texture_tests(void)
{
    uint8_t s[128] = {0}, p[64] = {0}, out[512];
#define DECODE(fmt, pf) assert(mv_gx_decode(out, sizeof(out), 32, s, sizeof(s), 8, 8, fmt, p, sizeof(p), pf) == 0)
    s[0] = 0xa5; DECODE(0, 0); pixel(out, 170, 170, 170, 170); pixel(out+4, 85, 85, 85, 85);
    s[0] = 0x7e; DECODE(1, 0); pixel(out, 126, 126, 126, 126);
    s[0] = 0xa5; DECODE(2, 0); pixel(out, 85, 85, 85, 170);
    s[0] = 0x81; s[1] = 0x42; DECODE(3, 0); pixel(out, 66, 66, 66, 129);
    s[0] = 0xf8; s[1] = 0; DECODE(4, 0); pixel(out, 255, 0, 0, 255);
    s[0] = 0xfc; DECODE(5, 0); pixel(out, 255, 0, 0, 255);
    s[0] = 0x31; s[1] = 0x23; DECODE(5, 0); pixel(out, 17, 34, 51, 109);
    /* RGBA8 needs 4 * 64 bytes for an 8x8 image; use one tile here. */
    s[0] = 10; s[1] = 20; s[32] = 30; s[33] = 40;
    assert(!mv_gx_decode(out, sizeof(out), 16, s, 64, 4, 4, 6, NULL, 0, 0));
    pixel(out, 20, 30, 40, 10);
    memset(s, 0, sizeof(s)); s[0] = 0x30; p[6] = 0x81; p[7] = 0x42;
    DECODE(8, 0); pixel(out, 66, 66, 66, 129);
    s[0] = 17; p[34] = 0; p[35] = 0x1f; DECODE(9, 1); pixel(out, 0, 0, 255, 255);
    s[0] = 0xc0; s[1] = 3; p[6] = 0x31; p[7] = 0x23;
    DECODE(10, 2); pixel(out, 17, 34, 51, 109);
    memset(s, 0, sizeof(s)); s[0] = 0xf8; s[3] = 0x1f; s[4] = 0x1b;
    DECODE(14, 0); pixel(out, 255, 0, 0, 255); pixel(out+4, 0, 0, 255, 255);
    pixel(out+8, 159, 0, 95, 255); pixel(out+12, 95, 0, 159, 255);
    s[0] = 0; s[2] = 255; s[3] = 255; s[4] = 0xff;
    DECODE(14, 0); pixel(out, 127, 127, 127, 0);
    for (unsigned i = 0; i < 4; ++i) memset(s+i*32, (int)((i+1)*16), 32);
    memset(out, 0xee, sizeof(out));
    assert(mv_gx_texture_size(9, 5, 1) == 128);
    assert(!mv_gx_decode(out, sizeof(out), 48, s, 128, 9, 5, 1, NULL, 0, 0));
    pixel(out, 16, 16, 16, 16); pixel(out+32, 32, 32, 32, 32);
    pixel(out+4*48, 48, 48, 48, 48); pixel(out+4*48+32, 64, 64, 64, 64);
    assert(out[36] == 0xee && out[47] == 0xee && out[228] == 0xee);
    assert(mv_gx_decode(out, 227, 48, s, 128, 9, 5, 1, NULL, 0, 0) < 0);
    assert(mv_gx_decode(out, sizeof(out), 48, s, 127, 9, 5, 1, NULL, 0, 0) < 0);
    assert(mv_gx_decode(out, sizeof(out), 32, s, 128, 8, 8, 9, p, 2, 1) < 0);
    assert(!mv_gx_texture_size(0, 8, 1) && !mv_gx_texture_size(1025, 8, 1));
    assert(!mv_gx_texture_size(8, 8, 7));
#undef DECODE
}
static void archive_tests(void)
{
    uint8_t b[119] = {0}; /* 32 header + 64 data + 4 reloc + 8 public + 11 string. */
    put32(b, sizeof(b)); put32(b+4, 64); put32(b+8, 1); put32(b+12, 1);
    put32(b+96, 8); memcpy(b+108, "Test_joint", 11);
    MvDat v;
    assert(!mv_dat_open(&v, b, sizeof(b)));
    uint32_t target = 123;
    assert(mv_dat_pointer(&v, 8, &target) == 1 && target == 0);
    assert(mv_dat_pointer(&v, 0, &target) == 0);
    /* Self-referencing joint with a registered zero offset must terminate. */
    MvImage images[2]; size_t count = 9;
    assert(!mv_menu_textures(&v, images, 2, &count) && count == 0);
    const char *name;
    assert(!mv_dat_public(&v, 0, &name, &target) && target == 0 && !strcmp(name, "Test_joint"));
    mv_dat_close(&v);
    put32(b+96, 1); /* Unaligned fields are legal in original archives. */
    assert(!mv_dat_open(&v, b, sizeof(b)));
    assert(mv_dat_pointer(&v, 1, &target) == 1 && target == 0);
    mv_dat_close(&v);
    put32(b+33, 64); assert(!mv_dat_open(&v, b, sizeof(b)));
    assert(mv_dat_pointer(&v, 1, &target) == 1 && target == 64);
    assert(!mv_dat_span(&v, target, 1)); mv_dat_close(&v);
    put32(b+33, 65); assert(mv_dat_open(&v, b, sizeof(b)) < 0);
    put32(b+33, 0); b[118] = 'x'; assert(mv_dat_open(&v, b, sizeof(b)) < 0);
    b[118] = 0; put32(b+8, 0xffffffff); assert(mv_dat_open(&v, b, sizeof(b)) < 0);
    assert(mv_dat_open(&v, b, 31) < 0);
}
int main(void)
{
    texture_tests(); archive_tests();
    puts("PASS: 11 GX formats, CMPR transparency, tile edges/stride/bounds, HSD references/cycles/malformed data");
    return 0;
}
