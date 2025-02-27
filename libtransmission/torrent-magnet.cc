/*
 * This file Copyright (C) 2012-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <limits.h> /* INT_MAX */
#include <string.h> /* memcpy(), memset(), memcmp() */

#include <event2/buffer.h>

#include "transmission.h"
#include "crypto-utils.h" /* tr_sha1() */
#include "file.h"
#include "log.h"
#include "magnet.h"
#include "metainfo.h"
#include "resume.h"
#include "torrent.h"
#include "torrent-magnet.h"
#include "tr-assert.h"
#include "utils.h"
#include "variant.h"
#include "web.h"

#define dbgmsg(tor, ...) tr_logAddDeepNamed(tr_torrentName(tor), __VA_ARGS__)

/***
****
***/

enum
{
    /* don't ask for the same metadata piece more than this often */
    MIN_REPEAT_INTERVAL_SECS = 3
};

struct metadata_node
{
    time_t requestedAt;
    int piece;
};

struct tr_incomplete_metadata
{
    uint8_t* metadata;
    int metadata_size;
    int pieceCount;

    /** sorted from least to most recently requested */
    struct metadata_node* piecesNeeded;
    int piecesNeededCount;
};

static void incompleteMetadataFree(struct tr_incomplete_metadata* m)
{
    tr_free(m->metadata);
    tr_free(m->piecesNeeded);
    tr_free(m);
}

bool tr_torrentSetMetadataSizeHint(tr_torrent* tor, int64_t size)
{
    if (tr_torrentHasMetadata(tor))
    {
        return false;
    }

    if (tor->incompleteMetadata != nullptr)
    {
        return false;
    }

    int const n = (size <= 0 || size > INT_MAX) ? -1 : size / METADATA_PIECE_SIZE + (size % METADATA_PIECE_SIZE != 0 ? 1 : 0);

    dbgmsg(tor, "metadata is %" PRId64 " bytes in %d pieces", size, n);

    if (n <= 0)
    {
        return false;
    }

    struct tr_incomplete_metadata* m = tr_new(struct tr_incomplete_metadata, 1);

    if (m == nullptr)
    {
        return false;
    }

    m->pieceCount = n;
    m->metadata = tr_new(uint8_t, size);
    m->metadata_size = size;
    m->piecesNeededCount = n;
    m->piecesNeeded = tr_new(struct metadata_node, n);

    if (m->metadata == nullptr || m->piecesNeeded == nullptr)
    {
        incompleteMetadataFree(m);
        return false;
    }

    for (int i = 0; i < n; ++i)
    {
        m->piecesNeeded[i].piece = i;
        m->piecesNeeded[i].requestedAt = 0;
    }

    tor->incompleteMetadata = m;
    return true;
}

static size_t findInfoDictOffset(tr_torrent const* tor)
{
    size_t fileLen;
    uint8_t* fileContents;
    size_t offset = 0;

    /* load the file, and find the info dict's offset inside the file */
    if ((fileContents = tr_loadFile(tor->info.torrent, &fileLen, nullptr)) != nullptr)
    {
        tr_variant top;

        if (tr_variantFromBenc(&top, fileContents, fileLen) == 0)
        {
            tr_variant* infoDict;

            if (tr_variantDictFindDict(&top, TR_KEY_info, &infoDict))
            {
                size_t infoLen;
                char* infoContents = tr_variantToStr(infoDict, TR_VARIANT_FMT_BENC, &infoLen);
                uint8_t const* i = (uint8_t const*)tr_memmem((char*)fileContents, fileLen, infoContents, infoLen);
                offset = i != nullptr ? i - fileContents : 0;
                tr_free(infoContents);
            }

            tr_variantFree(&top);
        }

        tr_free(fileContents);
    }

    return offset;
}

static void ensureInfoDictOffsetIsCached(tr_torrent* tor)
{
    TR_ASSERT(tr_torrentHasMetadata(tor));

    if (!tor->infoDictOffsetIsCached)
    {
        tor->infoDictOffset = findInfoDictOffset(tor);
        tor->infoDictOffsetIsCached = true;
    }
}

void* tr_torrentGetMetadataPiece(tr_torrent* tor, int piece, size_t* len)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(piece >= 0);
    TR_ASSERT(len != nullptr);

    char* ret = nullptr;

    if (tr_torrentHasMetadata(tor))
    {
        tr_sys_file_t fd;

        ensureInfoDictOffsetIsCached(tor);

        TR_ASSERT(tor->infoDictLength > 0);

        fd = tr_sys_file_open(tor->info.torrent, TR_SYS_FILE_READ, 0, nullptr);

        if (fd != TR_BAD_SYS_FILE)
        {
            size_t const o = piece * METADATA_PIECE_SIZE;

            if (tr_sys_file_seek(fd, tor->infoDictOffset + o, TR_SEEK_SET, nullptr, nullptr))
            {
                size_t const l = o + METADATA_PIECE_SIZE <= tor->infoDictLength ? METADATA_PIECE_SIZE : tor->infoDictLength - o;

                if (0 < l && l <= METADATA_PIECE_SIZE)
                {
                    char* buf = tr_new(char, l);
                    uint64_t n;

                    if (tr_sys_file_read(fd, buf, l, &n, nullptr) && n == l)
                    {
                        *len = l;
                        ret = buf;
                        buf = nullptr;
                    }

                    tr_free(buf);
                }
            }

            tr_sys_file_close(fd, nullptr);
        }
    }

    TR_ASSERT(ret == nullptr || *len > 0);

    return ret;
}

static int getPieceNeededIndex(struct tr_incomplete_metadata const* m, int piece)
{
    for (int i = 0; i < m->piecesNeededCount; ++i)
    {
        if (m->piecesNeeded[i].piece == piece)
        {
            return i;
        }
    }

    return -1;
}

static int getPieceLength(struct tr_incomplete_metadata const* m, int piece)
{
    return piece + 1 == m->pieceCount ? // last piece
        m->metadata_size - (piece * METADATA_PIECE_SIZE) :
        METADATA_PIECE_SIZE;
}

void tr_torrentSetMetadataPiece(tr_torrent* tor, int piece, void const* data, int len)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(data != nullptr);
    TR_ASSERT(len >= 0);

    dbgmsg(tor, "got metadata piece %d of %d bytes", piece, len);

    // are we set up to download metadata?
    struct tr_incomplete_metadata* const m = tor->incompleteMetadata;
    if (m == nullptr)
    {
        return;
    }

    // sanity test: is `piece` in range?
    if ((piece < 0) || (piece >= m->pieceCount))
    {
        return;
    }

    // sanity test: is `len` the right size?
    if (getPieceLength(m, piece) != len)
    {
        return;
    }

    // do we need this piece?
    int const idx = getPieceNeededIndex(m, piece);
    if (idx == -1)
    {
        return;
    }

    size_t const offset = piece * METADATA_PIECE_SIZE;
    memcpy(m->metadata + offset, data, len);

    tr_removeElementFromArray(m->piecesNeeded, idx, sizeof(struct metadata_node), m->piecesNeededCount);
    --m->piecesNeededCount;

    dbgmsg(tor, "saving metainfo piece %d... %d remain", piece, m->piecesNeededCount);

    /* are we done? */
    if (m->piecesNeededCount == 0)
    {
        bool success = false;
        bool metainfoParsed = false;
        uint8_t sha1[SHA_DIGEST_LENGTH];

        /* we've got a complete set of metainfo... see if it passes the checksum test */
        dbgmsg(tor, "metainfo piece %d was the last one", piece);
        tr_sha1(sha1, m->metadata, m->metadata_size, nullptr);

        bool const checksumPassed = memcmp(sha1, tor->info.hash, SHA_DIGEST_LENGTH) == 0;
        if (checksumPassed)
        {
            /* checksum passed; now try to parse it as benc */
            tr_variant infoDict;
            int const err = tr_variantFromBenc(&infoDict, m->metadata, m->metadata_size);
            dbgmsg(tor, "err is %d", err);

            metainfoParsed = err == 0;
            if (metainfoParsed)
            {
                /* yay we have bencoded metainfo... merge it into our .torrent file */
                tr_variant newMetainfo;
                char* path = tr_strdup(tor->info.torrent);

                if (tr_variantFromFile(&newMetainfo, TR_VARIANT_FMT_BENC, path, nullptr))
                {
                    bool hasInfo;
                    tr_info info;
                    size_t infoDictLength;

                    /* remove any old .torrent and .resume files */
                    tr_sys_path_remove(path, nullptr);
                    tr_torrentRemoveResume(tor);

                    dbgmsg(tor, "Saving completed metadata to \"%s\"", path);
                    tr_variantMergeDicts(tr_variantDictAddDict(&newMetainfo, TR_KEY_info, 0), &infoDict);

                    memset(&info, 0, sizeof(tr_info));
                    success = tr_metainfoParse(tor->session, &newMetainfo, &info, &hasInfo, &infoDictLength);

                    if (success && tr_getBlockSize(info.pieceSize) == 0)
                    {
                        tr_torrentSetLocalError(tor, "%s", _("Magnet torrent's metadata is not usable"));
                        tr_metainfoFree(&info);
                        success = false;
                    }

                    if (success)
                    {
                        /* keep the new info */
                        tor->info = info;
                        tor->infoDictLength = infoDictLength;

                        /* save the new .torrent file */
                        tr_variantToFile(&newMetainfo, TR_VARIANT_FMT_BENC, tor->info.torrent);
                        tr_sessionSetTorrentFile(tor->session, tor->info.hashString, tor->info.torrent);
                        tr_torrentGotNewInfoDict(tor);
                        tr_torrentSetDirty(tor);
                    }

                    tr_variantFree(&newMetainfo);
                }

                tr_variantFree(&infoDict);
                tr_free(path);
            }
        }

        if (success)
        {
            incompleteMetadataFree(tor->incompleteMetadata);
            tor->incompleteMetadata = nullptr;
            tor->isStopping = true;
            tor->magnetVerify = true;
            tor->startAfterVerify = !tor->prefetchMagnetMetadata;
            tr_torrentMarkEdited(tor);
        }
        else /* drat. */
        {
            int const n = m->pieceCount;

            for (int i = 0; i < n; ++i)
            {
                m->piecesNeeded[i].piece = i;
                m->piecesNeeded[i].requestedAt = 0;
            }

            m->piecesNeededCount = n;
            dbgmsg(tor, "metadata error; trying again. %d pieces left", n);

            tr_logAddError("magnet status: checksum passed %d, metainfo parsed %d", (int)checksumPassed, (int)metainfoParsed);
        }
    }
}

bool tr_torrentGetNextMetadataRequest(tr_torrent* tor, time_t now, int* setme_piece)
{
    TR_ASSERT(tr_isTorrent(tor));

    bool have_request = false;
    struct tr_incomplete_metadata* m = tor->incompleteMetadata;

    if (m != nullptr && m->piecesNeededCount > 0 && m->piecesNeeded[0].requestedAt + MIN_REPEAT_INTERVAL_SECS < now)
    {
        int const piece = m->piecesNeeded[0].piece;
        tr_removeElementFromArray(m->piecesNeeded, 0, sizeof(struct metadata_node), m->piecesNeededCount);

        int i = m->piecesNeededCount - 1;
        m->piecesNeeded[i].piece = piece;
        m->piecesNeeded[i].requestedAt = now;

        dbgmsg(tor, "next piece to request: %d", piece);
        *setme_piece = piece;
        have_request = true;
    }

    return have_request;
}

double tr_torrentGetMetadataPercent(tr_torrent const* tor)
{
    double ret;

    if (tr_torrentHasMetadata(tor))
    {
        ret = 1.0;
    }
    else
    {
        struct tr_incomplete_metadata const* m = tor->incompleteMetadata;

        if (m == nullptr || m->pieceCount == 0)
        {
            ret = 0.0;
        }
        else
        {
            ret = (m->pieceCount - m->piecesNeededCount) / (double)m->pieceCount;
        }
    }

    return ret;
}

/* TODO: this should be renamed tr_metainfoGetMagnetLink() and moved to metainfo.c for consistency */
char* tr_torrentInfoGetMagnetLink(tr_info const* inf)
{
    char const* name;
    struct evbuffer* s = evbuffer_new();

    evbuffer_add_printf(s, "magnet:?xt=urn:btih:%s", inf->hashString);

    name = inf->name;

    if (!tr_str_is_empty(name))
    {
        evbuffer_add_printf(s, "%s", "&dn=");
        tr_http_escape(s, name, TR_BAD_SIZE, true);
    }

    for (unsigned int i = 0; i < inf->trackerCount; ++i)
    {
        evbuffer_add_printf(s, "%s", "&tr=");
        tr_http_escape(s, inf->trackers[i].announce, TR_BAD_SIZE, true);
    }

    for (unsigned int i = 0; i < inf->webseedCount; i++)
    {
        evbuffer_add_printf(s, "%s", "&ws=");
        tr_http_escape(s, inf->webseeds[i], TR_BAD_SIZE, true);
    }

    return evbuffer_free_to_str(s, nullptr);
}
