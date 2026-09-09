#include "archive.h"

#include <string.h>

#include <dolphin/os.h>

#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
static u32 archive_be32(const void* ptr)
{
    const u8* p = ptr;
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}
#endif

static inline void Locate(HSD_Archive* archive)
{
    u32 i;
    u32* ptr;

    for (i = 0; i < archive->header.nb_reloc; i++) {
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
        u32 field = archive_be32(&archive->reloc_info[i].offset);
        ptr = (u32*) (archive->data + field);
        *ptr = (u32) (uintptr_t) (archive->data + archive_be32(ptr));
#else
        ptr = (u32*) (archive->data + archive->reloc_info[i].offset);
        *ptr += (u32) archive->data;
#endif
    }
}

s32 HSD_ArchiveParse(HSD_Archive* archive, u8* src, size_t file_size)
{
    u32 offset;

    if (archive == NULL) {
        return -1;
    }

    memset(archive, 0, sizeof(HSD_Archive));
    archive->flags |= 1;
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
    if (src == NULL || file_size < sizeof(HSD_ArchiveHeader)) {
        return -1;
    }
    archive->header.file_size = archive_be32(src + 0x00);
    archive->header.data_size = archive_be32(src + 0x04);
    archive->header.nb_reloc = archive_be32(src + 0x08);
    archive->header.nb_public = archive_be32(src + 0x0C);
    archive->header.nb_extern = archive_be32(src + 0x10);
    memcpy(archive->header.version, src + 0x14, sizeof(archive->header.version));
    archive->header.pad[0] = archive_be32(src + 0x18);
    archive->header.pad[1] = archive_be32(src + 0x1C);
#else
    memcpy(archive, src, sizeof(HSD_ArchiveHeader));
#endif

    if (archive->header.file_size != file_size) {
        OSReport("HSD_ArchiveParse: byte-order mismatch! Please check data "
                 "format %x %x\n",
                 archive->header.file_size, file_size);
        return -1;
    }

#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
    {
        uint64_t metadata_end = sizeof(HSD_ArchiveHeader);
        metadata_end += archive->header.data_size;
        metadata_end += (uint64_t) archive->header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
        metadata_end += (uint64_t) archive->header.nb_public * sizeof(HSD_ArchivePublicInfo);
        metadata_end += (uint64_t) archive->header.nb_extern * sizeof(HSD_ArchiveExternInfo);
        if (metadata_end > file_size) {
            return -1;
        }
    }
#endif

    offset = sizeof(HSD_ArchiveHeader);
    if (archive->header.data_size != 0) { // Body Size
        archive->data = src + sizeof(HSD_ArchiveHeader);
        offset = archive->header.data_size + sizeof(HSD_ArchiveHeader);
    }
    if (archive->header.nb_reloc != 0) { // Relocation Size
        archive->reloc_info =
            (HSD_ArchiveRelocationInfo*) ((uintptr_t) src + offset);
        offset = offset +
                 archive->header.nb_reloc * sizeof(HSD_ArchiveRelocationInfo);
    }
    if (archive->header.nb_public != 0) { // Root Size
        archive->public_info =
            (HSD_ArchivePublicInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_public * sizeof(HSD_ArchivePublicInfo);
    }
    if (archive->header.nb_extern != 0) { // XRef Size
        archive->extern_info =
            (HSD_ArchiveExternInfo*) ((uintptr_t) src + offset);
        offset =
            offset + archive->header.nb_extern * sizeof(HSD_ArchiveExternInfo);
    }
    if (offset < archive->header.file_size) { // File Size
        archive->symbols = (char*) ((uintptr_t) src + offset);
    }

    archive->top_ptr = (void*) src;
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
    for (u32 i = 0; i < archive->header.nb_reloc; ++i) {
        u32 field = archive_be32(&archive->reloc_info[i].offset);
        if (field > archive->header.data_size ||
            archive->header.data_size - field < sizeof(u32)) {
            return -1;
        }
        u32 target = archive_be32(archive->data + field);
        if (target > archive->header.data_size) {
            return -1;
        }
    }
#endif
    Locate(archive);

    return 0;
}

void* HSD_ArchiveGetPublicAddress(HSD_Archive* archive, const char* symbols)
{
    u32 i;

    for (i = 0; i < archive->header.nb_public; i++) {
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
        u32 symbol_offset = archive_be32(&archive->public_info[i].symbol);
        u32 data_offset = archive_be32(&archive->public_info[i].offset);
        int comparison = strcmp(archive->symbols + symbol_offset, symbols);
#else
        int comparison =
            strcmp(archive->symbols + archive->public_info[i].symbol, symbols);
#endif

        if (comparison == 0) {
            // If both strings are equal, we've found the node
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
            return archive->data + data_offset;
#else
            return archive->data + archive->public_info[i].offset;
#endif
        }
    }

    return NULL;
}

char* HSD_ArchiveGetExtern(HSD_Archive* archive, int offset)
{
    if (offset < 0 || archive->header.nb_extern <= (unsigned) offset) {
        return NULL;
    }

#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
    return archive->symbols + archive_be32(&archive->extern_info[offset].symbol);
#else
    return archive->symbols + archive->extern_info[offset].symbol;
#endif
}

void HSD_ArchiveLocateExtern(HSD_Archive* archive, const char* symbols,
                             void* addr)
{
    uintptr_t next;
    uintptr_t offset = -1;
    u32 i;

    for (i = 0; i < archive->header.nb_extern; i++) {
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
        u32 symbol_offset = archive_be32(&archive->extern_info[i].symbol);
        int comparison = strcmp(symbols, archive->symbols + symbol_offset);
#else
        int comparison =
            strcmp(symbols, archive->symbols + archive->extern_info[i].symbol);
#endif

        if (comparison == 0) {
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
            offset = archive_be32(&archive->extern_info[i].offset);
#else
            offset = archive->extern_info[i].offset;
#endif
            break;
        }
    }

    if (offset == -1U) {
        return;
    }

    while (offset != -1U && offset < archive->header.data_size) {
#ifdef MELEE_VITA_BIG_ENDIAN_ARCHIVE
        next = archive_be32((u8*) archive->data + offset);
#else
        next = *(uintptr_t*) ((uintptr_t) archive->data + offset);
#endif
        *(u32*) ((uintptr_t) archive->data + offset) = (uintptr_t) addr;
        offset = next;
    }
}
