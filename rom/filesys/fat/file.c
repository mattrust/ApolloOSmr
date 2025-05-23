/*
 * fat-handler - FAT12/16/32 filesystem handler
 *
 * Copyright � 2006 Marek Szyprowski
 * Copyright � 2007-2015 The AROS Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the same terms as AROS itself.
 *
 * $Id$
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "fat_fs.h"
#include "fat_protos.h"

#define DEBUG DEBUG_FILE
#include "debug.h"

#if defined(DEBUG_DUMP) && DEBUG_DUMP != 0
#include <ctype.h>

#define CHUNK 16

static void HexDump(unsigned char *buf, int bufsz, struct Globals *glob)
{
    int i, j;
    int count;

    /* Do this in chunks of CHUNK bytes */
    for (i = 0; i < bufsz; i += CHUNK)
    {
        /* Show the offset */
        D(bug("0x%06x  ", i));

        /* Max of CHUNK or remaining bytes */
        count = ((bufsz - i) > CHUNK ? CHUNK : bufsz - i);

        /* Show the bytes */
        for (j = 0; j < count; j++)
        {
            if (j == CHUNK / 2)
                bug(" ");
            D(bug("%02x ", buf[i + j]));
        }

        /* Pad with spaces if less than CHUNK */
        for (j = count; j < CHUNK; j++)
        {
            if (j == CHUNK / 2)
                bug(" ");
            bug("   ");
        }

        /* Divider between hex and ASCII */
        bug(" ");

        for (j = 0; j < count; j++)
            D(bug("%c", (isprint(buf[i + j]) ? buf[i + j] : '.')));

        D(bug("\n"));
    }
}
#else
#define HexDump(b, c, g)
#endif

LONG ReadFileChunk(struct IOHandle *ioh, ULONG file_pos, ULONG nwant,
    UBYTE *data, ULONG *nread)
{
    struct Globals *glob = ioh->sb->glob;
    ULONG sector_offset, byte_offset, cluster_offset, old_sector;
    APTR b;
    ULONG pos, ncopy;
    UBYTE *p;

    /* Files with no data can't be read from */
    if (ioh->first_cluster == 0xffffffff && nwant > 0)
    {
        D(bug("[FAT] file has no first cluster, so nothing to read!\n"));
        return ERROR_OBJECT_NOT_FOUND;
    }

    /* Figure out how far into the file to look for the requested data */
    sector_offset = file_pos >> ioh->sb->sectorsize_bits;
    byte_offset = file_pos & (ioh->sb->sectorsize - 1);

    /* Loop until we get all we want */
    pos = 0;
    while (nwant > 0)
    {

        D(bug("[FAT] trying to read %ld bytes"
            " (%ld sectors + %ld bytes into the file)\n",
            nwant, sector_offset, byte_offset));

        /* Move clusters if necessary */
        cluster_offset = sector_offset >> ioh->sb->cluster_sectors_bits;
        if (ioh->cluster_offset != cluster_offset
            && ioh->first_cluster != 0)
        {
            ULONG i;

            /* If we're already ahead of the wanted cluster, then we need to
             * go back to the start of the cluster list */
            if (ioh->cluster_offset > cluster_offset)
            {
                ioh->cur_cluster = ioh->first_cluster;
                ioh->cluster_offset = 0;
            }

            D(bug("[FAT] moving forward %ld clusters from cluster %ld\n",
                cluster_offset - ioh->cluster_offset, ioh->cur_cluster));

            /* Find it */
            for (i = 0; i < cluster_offset - ioh->cluster_offset; i++)
            {
                /* Get the next one */
                ioh->cur_cluster =
                    GET_NEXT_CLUSTER(ioh->sb, ioh->cur_cluster);

                /* If it was free (shouldn't happen) or we hit the end of the
                 * chain, the requested data isn't here */
                if (ioh->cur_cluster == 0
                    || ioh->cur_cluster >= ioh->sb->eoc_mark - 7)
                {
                    D(bug("[FAT] hit empty or eoc cluster,"
                        " no more file left\n"));

                    RESET_HANDLE(ioh);

                    return ERROR_OBJECT_NOT_FOUND;
                }
            }

            /* Remember how far in we are now */
            ioh->cluster_offset = cluster_offset;

            D(bug("[FAT] moved to cluster %ld\n", ioh->cur_cluster));

            /* Reset the sector offset so the sector recalc gets triggered */
            ioh->sector_offset = 0xffffffff;
        }

        /* Recalculate the sector location if we moved */
        old_sector = ioh->cur_sector;
        if (ioh->sector_offset !=
            (sector_offset & (ioh->sb->cluster_sectors - 1))
            || ioh->first_cluster == 0)
        {

            /* Work out how many sectors in we should be looking */
            ioh->sector_offset =
                sector_offset & (ioh->sb->cluster_sectors - 1);

            /* Simple math to find the absolute sector number */
            ioh->cur_sector = SECTOR_FROM_CLUSTER(ioh->sb, ioh->cur_cluster)
                + ioh->sector_offset;

            /* If the first cluster is zero, we use sector addressing instead
             * of clusters. this is a hack to support FAT12/16 root dirs,
             * which live before the data region */
            if (ioh->first_cluster == 0)
            {
                ioh->sector_offset = sector_offset - ioh->first_sector;
                ioh->cur_sector = ioh->first_sector + sector_offset;

                /* Stop if we've reached the end of the root dir */
                if (ioh->cur_sector >= ioh->sb->first_rootdir_sector
                    + ioh->sb->rootdir_sectors)
                {
                    RESET_HANDLE(ioh);
                    return ERROR_OBJECT_NOT_FOUND;
                }

                D(bug("[FAT] adjusted for cluster 0,"
                    " chunk starts in sector %ld\n",
                    ioh->cur_sector));
            }
            else
                D(bug("[FAT] chunk starts %ld sectors into the cluster,"
                    " which is sector %ld\n",
                    ioh->sector_offset, ioh->cur_sector));
        }

        /* If we don't have the wanted block kicking around, we need to bring
         * it in from the cache */
        if (ioh->block == NULL || ioh->cur_sector != old_sector)
        {
            if (ioh->block != NULL)
            {
                Cache_FreeBlock(ioh->sb->cache, ioh->block);
                ioh->block = NULL;
            }

            D(bug("[FAT] requesting sector %ld from cache\n",
                ioh->cur_sector));

            b = Cache_GetBlock(ioh->sb->cache,
                ioh->sb->first_device_sector + ioh->cur_sector, &p);
            if (b == NULL)
            {
                RESET_HANDLE(ioh);

                D(bug("[FAT] couldn't load sector, returning error %ld\n",
                    IoErr()));

                return IoErr();
            }

            ioh->block = b;
            ioh->data = p;
        }

        else
            D(bug("[FAT] using cached sector %ld\n", ioh->cur_sector));

        /* Now copy in the data */
        ncopy = ioh->sb->sectorsize - byte_offset;
        if (ncopy > nwant)
            ncopy = nwant;
        CopyMem(ioh->data + byte_offset, data + pos, ncopy);

#if defined(DEBUG_DUMP) && DEBUG_DUMP != 0
        D(bug("[FAT] dump of last read, %ld bytes:\n", ncopy));
        HexDump(&(data[pos]), ncopy, glob);
#endif

        pos += ncopy;
        nwant -= ncopy;

        D(bug("[FAT] copied %ld bytes, want %ld more\n", ncopy, nwant));

        if (nwant > 0)
        {
            sector_offset++;
            byte_offset = 0;
        }
    }

    *nread = pos;

    return 0;
}

LONG WriteFileChunk(struct IOHandle *ioh, ULONG file_pos, ULONG nwant,
    UBYTE *data, ULONG *nwritten)
{
    struct Globals *glob = ioh->sb->glob;
    LONG err = 0;
    ULONG sector_offset, byte_offset, cluster_offset, old_sector;
    struct cache_block *b;
    ULONG pos, ncopy;
    UBYTE *p;

    /* Figure out how far into the file to start */
    sector_offset = file_pos >> ioh->sb->sectorsize_bits;
    byte_offset = file_pos & (ioh->sb->sectorsize - 1);

    /* Loop until we've finished writing */
    pos = 0;
    while (nwant > 0)
    {

        D(bug("[FAT] trying to write %ld bytes"
            " (%ld sectors + %ld bytes into the file)\n",
            nwant, sector_offset, byte_offset));

        /* Move clusters if necessary */
        cluster_offset = sector_offset >> ioh->sb->cluster_sectors_bits;
        if (ioh->cluster_offset != cluster_offset
            && ioh->first_cluster != 0)
        {
            ULONG i;

            /* If we have no first cluster, this is a new file. We allocate
             * the first cluster and then update the ioh */
            if (ioh->first_cluster == 0xffffffff)
            {
                ULONG cluster;

                D(bug("[FAT] no first cluster, allocating one\n"));

                /* Allocate a cluster */
                if ((err = FindFreeCluster(ioh->sb, &cluster)) != 0)
                {
                    RESET_HANDLE(ioh);
                    return err;
                }

                /* Mark the cluster used */
                AllocCluster(ioh->sb, cluster);

                /* Now setup the ioh */
                ioh->first_cluster = cluster;
                RESET_HANDLE(ioh);
            }

            /* If we're already ahead of the wanted cluster, then we need to
             * go back to the start of the cluster list */
            if (ioh->cluster_offset > cluster_offset)
            {
                ioh->cur_cluster = ioh->first_cluster;
                ioh->cluster_offset = 0;
            }

            D(bug("[FAT] moving forward %ld clusters from cluster %ld\n",
                cluster_offset - ioh->cluster_offset, ioh->cur_cluster));

            /* Find it */
            for (i = 0; i < cluster_offset - ioh->cluster_offset; i++)
            {
                /* Get the next one */
                ULONG next_cluster =
                    GET_NEXT_CLUSTER(ioh->sb, ioh->cur_cluster);

                /* If it was free (shouldn't happen) or we hit the end of the
                 * chain, there is no next cluster, so we have to allocate a
                 * new one */
                if (next_cluster == 0
                    || next_cluster >= ioh->sb->eoc_mark - 7)
                {
                    D(bug("[FAT] hit empty or eoc cluster,"
                        " allocating another\n"));

                    if ((err = FindFreeCluster(ioh->sb, &next_cluster)) != 0)
                    {
                        RESET_HANDLE(ioh);
                        return err;
                    }

                    /* Link the current cluster to the new one */
                    SET_NEXT_CLUSTER(ioh->sb, ioh->cur_cluster,
                        next_cluster);

                    /* And mark the new one used */
                    AllocCluster(ioh->sb, next_cluster);

                    ioh->cur_cluster = next_cluster;

                    D(bug("[FAT] allocated cluster %d\n", next_cluster));
                }
                else
                    ioh->cur_cluster = next_cluster;
            }

            /* Remember how far in we are now */
            ioh->cluster_offset = cluster_offset;

            D(bug("[FAT] moved to cluster %ld\n", ioh->cur_cluster));

            /* Reset the sector offset so the sector recalc gets triggered */
            ioh->sector_offset = 0xffffffff;
        }

        /* Recalculate the sector location if we moved */
        old_sector = ioh->cur_sector;
        if (ioh->sector_offset !=
            (sector_offset & (ioh->sb->cluster_sectors - 1))
            || ioh->first_cluster == 0)
        {

            /* Work out how many sectors in we should be looking */
            ioh->sector_offset =
                sector_offset & (ioh->sb->cluster_sectors - 1);

            /* Simple math to find the absolute sector number */
            ioh->cur_sector = SECTOR_FROM_CLUSTER(ioh->sb, ioh->cur_cluster)
                + ioh->sector_offset;

            /* If the first cluster is zero, we use sector addressing instead
             * of clusters. this is a hack to support FAT12/16 root dirs,
             * which live before the data region */
            if (ioh->first_cluster == 0)
            {
                ioh->sector_offset = sector_offset - ioh->first_sector;
                ioh->cur_sector = ioh->first_sector + sector_offset;

                D(bug("[FAT] adjusted for cluster 0,"
                    " chunk starts in sector %ld\n", ioh->cur_sector));
            }
            else
                D(bug("[FAT] chunk starts %ld sectors into the cluster,"
                    " which is sector %ld\n",
                    ioh->sector_offset, ioh->cur_sector));
        }

        /* If we don't have the wanted block kicking around, we need to bring
         * it in from the cache */
        if (ioh->block == NULL || ioh->cur_sector != old_sector)
        {
            if (ioh->block != NULL)
            {
                Cache_FreeBlock(ioh->sb->cache, ioh->block);
                ioh->block = NULL;
            }

            D(bug("[FAT] requesting sector %ld from cache\n",
                ioh->cur_sector));

            b = Cache_GetBlock(ioh->sb->cache, ioh->sb->first_device_sector
                + ioh->cur_sector, &p);
            if (b == NULL)
            {
                RESET_HANDLE(ioh);

                D(bug("[FAT] couldn't load sector, returning error %ld\n",
                    IoErr()));

                return IoErr();
            }

            ioh->block = b;
            ioh->data = p;
        }
        else
            D(bug("[FAT] using cached sector %ld\n", ioh->cur_sector));

        /* Copy our data into the block */
        ncopy = ioh->sb->sectorsize - byte_offset;
        if (ncopy > nwant)
            ncopy = nwant;
        CopyMem(data + pos, ioh->data + byte_offset, ncopy);

#if defined(DEBUG_DUMP) && DEBUG_DUMP != 0
        D(bug("[FAT] dump of last write, %ld bytes:\n", ncopy));
        HexDump(&(ioh->data[byte_offset]), ncopy, glob);
#endif

        Cache_MarkBlockDirty(ioh->sb->cache, ioh->block);

        pos += ncopy;
        nwant -= ncopy;

        D(bug("[FAT] wrote %ld bytes, want %ld more\n", ncopy, nwant));

        if (nwant > 0)
        {
            sector_offset++;
            byte_offset = 0;
        }
    }

    *nwritten = pos;

    return 0;
}
