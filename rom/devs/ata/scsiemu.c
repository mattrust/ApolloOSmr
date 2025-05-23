/*
    Copyright � 2011-2020, The AROS Development Team. All rights reserved
    $Id$

    Desc: Simple HD_SCSICMD emulator.
    Lang: English
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/utility.h>

#include <exec/types.h>
#include <exec/exec.h>
#include <devices/scsidisk.h>

#include "ata.h"

#undef UtilityBase


static void wl(UBYTE *p, ULONG v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void ww(UBYTE *p, UWORD v)
{
    p[0] = v >> 8;
    p[1] = v;
}

static ULONG rl(UBYTE *p)
{
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3]);
}
static ULONG rw(UBYTE *p)
{
    return (p[0] << 8) | (p[1]);
}

static UBYTE scsi_read32(struct ata_Unit *unit, APTR data, ULONG offset, ULONG len, ULONG *outlen)
{
    bug("[SCSI] scsi_read32\n");
    
     UBYTE io_Error = 0;
     if ((unit->au_SectorShift == 9) && ((unit->au_XferModes & AF_XFER_PACKET)==0))  /* use cache with 512 Byte sectors only */
     {
         struct ata_Bus *bus = unit->au_Bus;
         struct ataBase *base = bus->ab_Base;
         ULONG unitNum = unit->au_UnitNum;

         ULONG start;
         ULONG i;
         for (start = 0, i = 0; i < len; i++)
         {
             ULONG blockAdr = (offset + i) & CACHE_MASK;
             UQUAD blockTag = (offset + i) & ~CACHE_MASK;

             if ((unitNum < (1<<9)) &&
                 (base->ata_CacheTags[blockAdr] == (blockTag | unitNum))) /* cache hit */
             {
                 CopyMem(base->ata_CacheData + blockAdr*512, data + i*512, 512);
                 start = i + 1;
             }
             else
             {
                 i = len;
                 break;
             }    
         }
         if (start != i)
         {
             /* Call the Unit's access funtion */
             io_Error = unit->au_Read32(unit, offset + start, i - start, data + start*512, outlen);
             if (io_Error) {
                 io_Error = unit->au_Read32(unit, offset + start, i - start, data + start*512, outlen);
                 if (io_Error) {
                   return io_Error;
                 }
             }

             for (ULONG j = start; j < i; j++)
             {
                 ULONG blockAdr = (offset + j) & CACHE_MASK;
                 CopyMem(data + j*512, base->ata_CacheData + blockAdr*512, 512);
                 blockAdr = (offset + j) & CACHE_MASK;
                 ULONG blockTag = (offset + j) & ~CACHE_MASK;
                 base->ata_CacheTags[blockAdr] = blockTag | unitNum;
             }
         }
         *outlen = len << unit->au_SectorShift;
         return io_Error;
     }
     else
     {
         /* Call the Unit's access funtion */
        // return unit->au_Read32(unit, offset, len, data, outlen);
         io_Error = unit->au_Read32(unit, offset, len, data, outlen);
         if (io_Error) /* on error try again */
           return unit->au_Read32(unit, offset, len, data, outlen);
         return io_Error;

     }
}

static UBYTE scsi_write32(struct ata_Unit *unit, APTR data, ULONG offset, ULONG len, ULONG *outlen)
{
    bug("[SCSI] scsi_write32\n");

    UBYTE err;

    if ((unit->au_SectorShift == 9) && ((unit->au_XferModes & AF_XFER_PACKET)==0)) 
    {
        struct ata_Bus *bus = unit->au_Bus;
        struct ataBase *base = bus->ab_Base;
        
        for (int i = 0; i < len; i++)
        {
            ULONG blockAdr = (offset + i) & CACHE_MASK;
            base->ata_CacheTags[blockAdr] = 0xfffffffffffffffful;
        }
    }
    
     err = unit->au_Write32(unit, offset, len, data, outlen);
     if (err) /* on error try again */
          return unit->au_Write32(unit, offset, len, data, outlen);
     return err;
}

static UBYTE scsi_inquiry(struct ata_Unit *unit, struct SCSICmd *cmd, ULONG *outlen)
{
    bug("[SCSI] scsi_inquiry\n");
    
    struct Library *UtilityBase = unit->au_Bus->ab_Base->ata_UtilityBase;
    UBYTE *cmdbuf = cmd->scsi_Command;
    UBYTE *out = (UBYTE*)cmd->scsi_Data;
    UBYTE len;
    
    if ((cmdbuf[1] & 1) || cmdbuf[2] != 0) return 0xff;
    len = cmdbuf[4];
    if (cmdbuf[1] >> 5) return 0xff; /* no lun supported */

    out[0] = 0;
    out[1] = 0;
    out[2] = 2; /* supports SCSI-2 */
    out[3] = 2; /* response data format */
    out[4] = 32; /* additional length */
    out[7] = 0x20; /* 16 bit bus */
    *outlen = len < 36 ? len : 36;
    SetMem(out + 8, ' ', 8 + 16 + 4);
    
    CopyMem(unit->au_Model, out + 8, strlen(unit->au_Model) > 24 ? 24 : strlen(unit->au_Model));

    CopyMem(unit->au_FirmwareRev, out + 32, strlen(unit->au_FirmwareRev) > 4 ? 4 : strlen(unit->au_FirmwareRev));
    return 0;
}

static UBYTE scsi_modesense(struct ata_Unit *unit, struct SCSICmd *cmd, ULONG *outlen)
{
    bug("[SCSI] scsi_modesense\n");
    
    UBYTE *cmdbuf = cmd->scsi_Command;
    UBYTE *out = (UBYTE*)cmd->scsi_Data;
    UBYTE pcode = cmdbuf[2] & 0x3f;
    UBYTE dbd = cmdbuf[1] & 8;
    UWORD blocksize = 1 << unit->au_SectorShift;
    UBYTE *p;
    
    p = out;
    p[0] = 4 - 1;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    p += 4;
    if (!dbd) {
    	p[-1] = 8;
    	wl(out + 0, unit->au_Capacity);
    	wl(out + 4, blocksize);
    	p += 8;
    }
    if (pcode == 0) {
	p[0] = 0;
	p[1] = 0;
	p[2] = 0x20;
	p[3] = 0;
	out[0] += 4;
    } else if (pcode == 3) {
	p[0] = 3;
	p[1] = 24;
	p[3] = 1;
	p[10] = unit->au_Sectors >> 8;
	p[11] = unit->au_Sectors;
	p[12] = blocksize >> 8;
	p[13] = blocksize;
	p[15] = 1; // interleave
	p[20] = 0x80;
	out[0] += p[1];
    } else if (pcode == 4) {
	p[0] = 4;
	wl(p + 1, unit->au_Cylinders);
	p[1] = 24;
	p[5] = unit->au_Heads;
	wl(p + 13, unit->au_Cylinders);
	ww(p + 20, 5400);
	out[0] += p[1];
    } else {
    	return 0xff;
    }
    out[0] += out[3];
    *outlen = out[0] + 1;
    return 0;
}

static UBYTE scsi_readcapacity(struct ata_Unit *unit, struct SCSICmd *cmd, ULONG *outlen)
{
    bug("[SCSI] scsi_readcapacity\n");
    
    UBYTE *cmdbuf = cmd->scsi_Command;
    UBYTE *out = (UBYTE*)cmd->scsi_Data;
    BOOL pmi;
    ULONG lba, blocks;

    blocks = unit->au_Capacity;
    pmi = cmdbuf[8] & 1;
    lba = (cmdbuf[2] << 24) | (cmdbuf[3] << 16) | (cmdbuf[4] << 8) | cmdbuf[5];
    if (pmi == 0 && lba != 0)
    	return 0xff;
    if (pmi) {
    	UWORD cylsize = unit->au_Heads * unit->au_Sectors;
    	lba += cylsize;
    	lba /= cylsize;
    	lba *= cylsize;
    	if (lba > blocks)
    	    lba = blocks;
    	blocks = lba;
    }
    wl (out + 0, blocks);
    wl (out + 4, 1 << unit->au_SectorShift);
    *outlen = 8;
    return 0;
}

BYTE SCSIEmu(struct ata_Unit *unit, struct SCSICmd *cmd)
{
    bug("[SCSI] SCSIEmu\n");
    
    struct Library *UtilityBase = unit->au_Bus->ab_Base->ata_UtilityBase;
    ULONG len, offset;
    ULONG scsi_len;
    UWORD scsi_sense_len = (cmd->scsi_Flags & (1 << SCSIB_OLDAUTOSENSE)) ? 4 :
    	(cmd->scsi_Flags & (1 << SCSIB_AUTOSENSE)) ? cmd->scsi_SenseLength : 0;
    UBYTE *cmdbuf = cmd->scsi_Command;
    UBYTE sense[32];
    UWORD senselen;
    UBYTE err, status;
 
    bug("SCSIEMU CMD=%02x\n", cmdbuf[0]); 
    err = 0;
    status = 0;
    scsi_len = 0;
    senselen = 0;
    if (scsi_sense_len > sizeof sense)
        scsi_sense_len = sizeof sense;
    switch(cmdbuf[0])
    {
    	case 0x00: /* TEST UNIT READY */
	    break;

        case 0x08: /* READ (6) */
        offset = ((cmdbuf[1] & 31) << 16) | (cmdbuf[2] << 8) | cmdbuf[3];
        len = cmdbuf[4];
        if (!len)
            len = 256;
        err = scsi_read32(unit, cmd->scsi_Data, offset, len, &scsi_len);
        break;

        case 0x28: /* READ (10) */
        offset = rl(cmdbuf + 2);
        len = rw(cmdbuf + 7);
        err = scsi_read32(unit, cmd->scsi_Data, offset, len, &scsi_len);
        break;	

        case 0xa8: /* READ (12) */
        offset = rl(cmdbuf + 2);
        len = rl(cmdbuf + 6);
        err = scsi_read32(unit, cmd->scsi_Data, offset, len, &scsi_len);
        break;

        case 0x0a: /* WRITE (6) */
        offset = ((cmdbuf[1] & 31) << 16) | (cmdbuf[2] << 8) | cmdbuf[3];
        len = cmdbuf[4];
        if (!len)
            len = 256;
        err = scsi_write32(unit, cmd->scsi_Data, offset, len, &scsi_len);
        break;

        case 0x2a: /* WRITE (10) */
        offset = rl(cmdbuf + 2);
        len = rw(cmdbuf + 7);
        err = scsi_write32(unit, cmd->scsi_Data, offset, len, &scsi_len);
        break;

        case 0xaa: /* WRITE (12) */
        offset = rl(cmdbuf + 2);
        len = rl(cmdbuf + 6);
        err = scsi_write32(unit, cmd->scsi_Data, offset, len, &scsi_len);
        break;

        case 0x37: /* READ DEFECT DATA */
        status = 2;
        senselen = 32;
        SetMem(sense, 0, senselen);
        sense[0] = 0x70;
        sense[2] = 0x00;
        sense[12] = 0x1c;
        break;

        case 0x12: /* INQUIRY */
        err = scsi_inquiry(unit, cmd, &scsi_len);
        break;

        case 0x1a: /* MODE SENSE(6) */
        err = scsi_modesense(unit, cmd, &scsi_len);
        break;

        case 0x25: /* READ CAPACITY */
        err = scsi_readcapacity(unit, cmd, &scsi_len);
        break;

        case 0x1d: /* SEND DIAGNOSTICS */
        case 0x35: /* SYNCHRONIZE CACHE */
        break;
        
        default:
        err = 0xff;
        break;
        }
        
        if (err == 0xff) {
        status = 2; /* CHECK CONDITION */
        senselen = 32;
        SetMem(sense, 0, senselen);
        sense[0] = 0x70;
        sense[2] = 5; /* ILLEGAL REQUEST */
        sense[12] = 0x24; /* ILLEGAL FIELD IN CDB */
        err = TDERR_NotSpecified;
    }
    
    if (senselen && scsi_sense_len) {
    	if (senselen > scsi_sense_len)
    	    senselen = scsi_sense_len;
    	CopyMem(sense, cmd->scsi_SenseData, senselen);
    	cmd->scsi_SenseActual = senselen;
    }
    cmd->scsi_Status = status;
    cmd->scsi_CmdActual = status != 0 ? 0 : cmd->scsi_CmdLength;
    cmd->scsi_Actual = scsi_len;
    
    return err;
}
