/* Copyright (C) 2009 - 2017 National Aeronautics and Space Administration. All Foreign Rights are Reserved to the U.S. Government.

This software is provided "as is" without any warranty of any, kind either express, implied, or statutory, including, but not
limited to, any warranty that the software will conform to, specifications any implied warranties of merchantability, fitness
for a particular purpose, and freedom from infringement, and any warranty that the documentation will conform to the program, or
any warranty that the software will be error free.

In no event shall NASA be liable for any damages, including, but not limited to direct, indirect, special or consequential damages,
arising out of, resulting from, or in any0 way connected with the software or its documentation.  Whether or not based upon warranty,
contract, tort or otherwise, and whether or not loss was sustained from, or arose out of the results of, or use of, the software,
documentation or services provided hereunder

ITC Team
NASA IV&V
ivv-itc@lists.nasa.gov
*/
#ifndef _crypto_c_
#define _crypto_c_

/*
** Includes
*/
#include "crypto.h"
#include "sadb_routine.h"

#include "itc_aes128.h"
#include "itc_gcm128.h"

#include "crypto_structs.h"
#include "crypto_print.h"
#include "crypto_config.h"
#include "crypto_events.h"



#include <gcrypt.h>


/*
** Static Library Declaration
*/
#ifdef BUILD_STATIC
    CFS_MODULE_DECLARE_LIB(crypto);
#endif

static SadbRoutine sadb_routine = NULL;

/*
** Static Prototypes
*/
// Assisting Functions
static int32  Crypto_Get_tcPayloadLength(void);
static int32  Crypto_Get_tmLength(int len);
static void   Crypto_TM_updatePDU(char* ingest, int len_ingest);
static void   Crypto_TM_updateOCF(void);
static void   Crypto_Local_Config(void);
static void   Crypto_Local_Init(void);
//static int32  Crypto_gcm_err(int gcm_err);
static int32 Crypto_window(uint8 *actual, uint8 *expected, int length, int window);
static int32 Crypto_compare_less_equal(uint8 *actual, uint8 *expected, int length);
static int32  Crypto_FECF(int fecf, char* ingest, int len_ingest);
static uint16 Crypto_Calc_FECF(char* ingest, int len_ingest);
static void   Crypto_Calc_CRC_Init_Table(void);
static uint16 Crypto_Calc_CRC16(char* data, int size);
// Key Management Functions
static int32 Crypto_Key_OTAR(void);
static int32 Crypto_Key_update(uint8 state);
static int32 Crypto_Key_inventory(char*);
static int32 Crypto_Key_verify(char*);
// Security Monitoring & Control Procedure
static int32 Crypto_MC_ping(char* ingest);
static int32 Crypto_MC_status(char* ingest);
static int32 Crypto_MC_dump(char* ingest);
static int32 Crypto_MC_erase(char* ingest);
static int32 Crypto_MC_selftest(char* ingest);
static int32 Crypto_SA_readARSN(char* ingest);
static int32 Crypto_MC_resetalarm(void);
// User Functions
static int32 Crypto_User_IdleTrigger(char* ingest);
static int32 Crypto_User_BadSPI(void);
static int32 Crypto_User_BadIV(void);
static int32 Crypto_User_BadMAC(void);
static int32 Crypto_User_BadFECF(void);
static int32 Crypto_User_ModifyKey(void);
static int32 Crypto_User_ModifyActiveTM(void);
static int32 Crypto_User_ModifyVCID(void);
// Determine Payload Data Unit
static int32 Crypto_PDU(char* ingest);

/*
** Global Variables
*/
// Security
crypto_key_t ek_ring[NUM_KEYS];
//static crypto_key_t ak_ring[NUM_KEYS];
// Local Frames
TC_t tc_frame;
CCSDS_t sdls_frame;
TM_t tm_frame;
// OCF
static uint8 ocf = 0;
static SDLS_FSR_t report;
static TM_FrameCLCW_t clcw;
// Flags
static SDLS_MC_LOG_RPLY_t log_summary;
static SDLS_MC_DUMP_BLK_RPLY_t log;
static uint8 log_count = 0;
static uint16 tm_offset = 0;
// ESA Testing - 0 = disabled, 1 = enabled
static uint8 badSPI = 0;
static uint8 badIV = 0;
static uint8 badMAC = 0;
static uint8 badFECF = 0;
//  CRC
static uint32 crc32Table[256];
static uint16 crc16Table[256];

/*
** Initialization Functions
*/
int32 Crypto_Init(void)
{   
    int32 status = OS_SUCCESS;

    sadb_routine = get_sadb_routine_inmemory();

    // Initialize libgcrypt
    if (!gcry_check_version(GCRYPT_VERSION))
    {
        fprintf(stderr, "Gcrypt Version: %s",GCRYPT_VERSION);
        OS_printf(KRED "ERROR: gcrypt version mismatch! \n" RESET);
    }
    if (gcry_control(GCRYCTL_SELFTEST) != GPG_ERR_NO_ERROR)
    {
        OS_printf(KRED "ERROR: gcrypt self test failed\n" RESET);
    }
    gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

    // Init Security Associations
    status = sadb_routine->sadb_init();
    status = sadb_routine->sadb_config();

    Crypto_Local_Init();
    Crypto_Local_Config();

    // TODO - Add error checking

    // Init table for CRC calculations
    Crypto_Calc_CRC_Init_Table();

    // cFS Standard Initialized Message
    OS_printf (KBLU "Crypto Lib Intialized.  Version %d.%d.%d.%d\n" RESET,
                CRYPTO_LIB_MAJOR_VERSION,
                CRYPTO_LIB_MINOR_VERSION, 
                CRYPTO_LIB_REVISION, 
                CRYPTO_LIB_MISSION_REV);
                                
    return status; 
}

static void Crypto_Local_Config(void)
{
    // Initial TM configuration
    tm_frame.tm_sec_header.spi = 1;

    // Initialize Log
    log_summary.num_se = 2;
    log_summary.rs = LOG_SIZE;
    // Add a two messages to the log
    log_summary.rs--;
    log.blk[log_count].emt = STARTUP;
    log.blk[log_count].emv[0] = 0x4E;
    log.blk[log_count].emv[1] = 0x41;
    log.blk[log_count].emv[2] = 0x53;
    log.blk[log_count].emv[3] = 0x41;
    log.blk[log_count++].em_len = 4;
    log_summary.rs--;
    log.blk[log_count].emt = STARTUP;
    log.blk[log_count].emv[0] = 0x4E;
    log.blk[log_count].emv[1] = 0x41;
    log.blk[log_count].emv[2] = 0x53;
    log.blk[log_count].emv[3] = 0x41;
    log.blk[log_count++].em_len = 4;
}

static void Crypto_Local_Init(void)
{

    // Initialize TM Frame
    // TM Header
    tm_frame.tm_header.tfvn    = 0;	    // Shall be 00 for TM-/TC-SDLP
    tm_frame.tm_header.scid    = SCID & 0x3FF;
    tm_frame.tm_header.vcid    = 0;
    tm_frame.tm_header.ocff    = 1;
    tm_frame.tm_header.mcfc    = 1;
    tm_frame.tm_header.vcfc    = 1;
    tm_frame.tm_header.tfsh    = 0;
    tm_frame.tm_header.sf      = 0;
    tm_frame.tm_header.pof     = 0;	    // Shall be set to 0
    tm_frame.tm_header.slid    = 3;	    // Shall be set to 11
    tm_frame.tm_header.fhp     = 0;
    // TM Security Header
    tm_frame.tm_sec_header.spi = 0x0000;
    for ( int x = 0; x < IV_SIZE; x++)
    { 	// Initialization Vector
        tm_frame.tm_sec_header.iv[x] = 0x00;
    }
    // TM Payload Data Unit
    for ( int x = 0; x < TM_FRAME_DATA_SIZE; x++)
    {	// Zero TM PDU
        tm_frame.tm_pdu[x] = 0x00;
    }
    // TM Security Trailer
    for ( int x = 0; x < MAC_SIZE; x++)
    { 	// Zero TM Message Authentication Code
        tm_frame.tm_sec_trailer.mac[x] = 0x00;
    }
    for ( int x = 0; x < OCF_SIZE; x++)
    { 	// Zero TM Operational Control Field
        tm_frame.tm_sec_trailer.ocf[x] = 0x00;
    }
    tm_frame.tm_sec_trailer.fecf = 0xFECF;

    // Initialize CLCW
    clcw.cwt 	= 0;			// Control Word Type "0"
    clcw.cvn	= 0;			// CLCW Version Number "00"
    clcw.sf  	= 0;    		// Status Field
    clcw.cie 	= 1;			// COP In Effect
    clcw.vci 	= 0;    		// Virtual Channel Identification
    clcw.spare0 = 0;			// Reserved Spare
    clcw.nrfa	= 0;			// No RF Avaliable Flag
    clcw.nbl	= 0;			// No Bit Lock Flag
    clcw.lo		= 0;			// Lock-Out Flag
    clcw.wait	= 0;			// Wait Flag
    clcw.rt		= 0;			// Retransmit Flag
    clcw.fbc	= 0;			// FARM-B Counter
    clcw.spare1 = 0;			// Reserved Spare
    clcw.rv		= 0;        	// Report Value

    // Initialize Frame Security Report
    report.cwt   = 1;			// Control Word Type "0b1""
    report.vnum  = 4;   		// FSR Version "0b100""
    report.af    = 0;			// Alarm Field
    report.bsnf  = 0;			// Bad SN Flag
    report.bmacf = 0;			// Bad MAC Flag
    report.ispif = 0;			// Invalid SPI Flag
    report.lspiu = 0;	    	// Last SPI Used
    report.snval = 0;			// SN Value (LSB)

}

static void Crypto_Calc_CRC_Init_Table(void)
{   
    uint16 val;
    uint32 poly = 0xEDB88320;
    uint32 crc;

    // http://create.stephan-brumme.com/crc32/
    for (unsigned int i = 0; i <= 0xFF; i++)
    {
        crc = i;
        for (unsigned int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (-(int)(crc & 1) & poly);
        }
        crc32Table[i] = crc;
        //OS_printf("crc32Table[%d] = 0x%08x \n", i, crc32Table[i]);
    }
    
    // Code provided by ESA
    for (int i = 0; i < 256; i++)
    {
        val = 0;
        if ( (i &   1) != 0 )  val ^= 0x1021;
        if ( (i &   2) != 0 )  val ^= 0x2042;
        if ( (i &   4) != 0 )  val ^= 0x4084;
        if ( (i &   8) != 0 )  val ^= 0x8108;
        if ( (i &  16) != 0 )  val ^= 0x1231;
        if ( (i &  32) != 0 )  val ^= 0x2462;
        if ( (i &  64) != 0 )  val ^= 0x48C4;
        if ( (i & 128) != 0 )  val ^= 0x9188;
        crc16Table[i] = val;
        //OS_printf("crc16Table[%d] = 0x%04x \n", i, crc16Table[i]);
    }
}

/*
** Assisting Functions
*/
static int32 Crypto_Get_tcPayloadLength(void)
// Returns the payload length of current tc_frame in BYTES!
{
    return (tc_frame.tc_header.fl - (5 + 2 + IV_SIZE ) - (MAC_SIZE + FECF_SIZE) );
}

static int32 Crypto_Get_tmLength(int len)
// Returns the total length of the current tm_frame in BYTES!
{
    #ifdef FILL
        len = TM_FILL_SIZE;
    #else
        len = TM_FRAME_PRIMARYHEADER_SIZE + TM_FRAME_SECHEADER_SIZE + len + TM_FRAME_SECTRAILER_SIZE + TM_FRAME_CLCW_SIZE;
    #endif

    return len;
}

static void Crypto_TM_updatePDU(char* ingest, int len_ingest)
// Update the Telemetry Payload Data Unit
{	// Copy ingest to PDU
    int x = 0;
    int fill_size = 0;
    SecurityAssociation_t* sa_ptr;

    if(sadb_routine->sadb_get_sa_from_spi(tm_frame.tm_sec_header.spi,&sa_ptr) != OS_SUCCESS){
        //TODO - Error handling
        return; //Error -- unable to get SA from SPI.
    }

    if ((sa_ptr->est == 1) && (sa_ptr->ast == 1))
    {
        fill_size = 1129 - MAC_SIZE - IV_SIZE + 2; // +2 for padding bytes
    }
    else
    {
        fill_size = 1129;
    }

    #ifdef TM_ZERO_FILL
        for (int x = 0; x < TM_FILL_SIZE; x++)
        {
            if (x < len_ingest)
            {	// Fill
                tm_frame.tm_pdu[x] = (uint8)ingest[x];
            }
            else
            {	// Zero
                tm_frame.tm_pdu[x] = 0x00;
            }
        }
    #else
        // Pre-append remaining packet if exist
        if (tm_offset == 63)
        {
            tm_frame.tm_pdu[x++] = 0xff;
            tm_offset--;
        }
        if (tm_offset == 62)
        {
            tm_frame.tm_pdu[x++] = 0x00;
            tm_offset--;
        }
        if (tm_offset == 61)
        {
            tm_frame.tm_pdu[x++] = 0x00;
            tm_offset--;
        }
        if (tm_offset == 60)
        {
            tm_frame.tm_pdu[x++] = 0x00;
            tm_offset--;
        }
        if (tm_offset == 59)
        {
            tm_frame.tm_pdu[x++] = 0x39;
            tm_offset--;
        }
        while (x < tm_offset)
        {
            tm_frame.tm_pdu[x] = 0x00;
            x++;
        }
        // Copy actual packet
        while (x < len_ingest + tm_offset)
        {
            //OS_printf("ingest[x - tm_offset] = 0x%02x \n", (uint8)ingest[x - tm_offset]);
            tm_frame.tm_pdu[x] = (uint8)ingest[x - tm_offset];
            x++;
        }
        #ifdef TM_IDLE_FILL
            // Check for idle frame trigger
            if (((uint8)ingest[0] == 0x08) && ((uint8)ingest[1] == 0x90))
            { 
                // Don't fill idle frames   
            }
            else
            {
                while (x < (fill_size - 64) )
                {
                    tm_frame.tm_pdu[x++] = 0x07;
                    tm_frame.tm_pdu[x++] = 0xff;
                    tm_frame.tm_pdu[x++] = 0x00;
                    tm_frame.tm_pdu[x++] = 0x00;
                    tm_frame.tm_pdu[x++] = 0x00;
                    tm_frame.tm_pdu[x++] = 0x39;
                    for (int y = 0; y < 58; y++)
                    {
                        tm_frame.tm_pdu[x++] = 0x00;
                    }
                }
                // Add partial packet, if possible, and set offset
                if (x < fill_size)
                {
                    tm_frame.tm_pdu[x++] = 0x07;
                    tm_offset = 63;
                }
                if (x < fill_size)
                {
                    tm_frame.tm_pdu[x++] = 0xff;
                    tm_offset--;
                }
                if (x < fill_size)
                {
                    tm_frame.tm_pdu[x++] = 0x00;
                    tm_offset--;
                }
                if (x < fill_size)
                {
                    tm_frame.tm_pdu[x++] = 0x00;
                    tm_offset--;
                }
                if (x < fill_size)
                {
                    tm_frame.tm_pdu[x++] = 0x00;
                    tm_offset--;
                }
                if (x < fill_size)
                {
                    tm_frame.tm_pdu[x++] = 0x39;
                    tm_offset--;
                }
                for (int y = 0; x < fill_size; y++)
                {
                    tm_frame.tm_pdu[x++] = 00;
                    tm_offset--;
                }
            }
            while (x < TM_FILL_SIZE)
            {
                tm_frame.tm_pdu[x++] = 0x00;
            }
        #endif 
    #endif

    return;
}

static void Crypto_TM_updateOCF(void)
{
    if (ocf == 0)
    {	// CLCW
        clcw.vci = tm_frame.tm_header.vcid;

        tm_frame.tm_sec_trailer.ocf[0] = (clcw.cwt << 7) | (clcw.cvn << 5) | (clcw.sf << 2) | (clcw.cie);
        tm_frame.tm_sec_trailer.ocf[1] = (clcw.vci << 2) | (clcw.spare0);
        tm_frame.tm_sec_trailer.ocf[2] = (clcw.nrfa << 7) | (clcw.nbl << 6) | (clcw.lo << 5) | (clcw.wait << 4) | (clcw.rt << 3) | (clcw.fbc << 1) | (clcw.spare1);
        tm_frame.tm_sec_trailer.ocf[3] = (clcw.rv);
        // Alternate OCF
        ocf = 1;
        #ifdef OCF_DEBUG
            Crypto_clcwPrint(&clcw);
        #endif
    } 
    else
    {	// FSR
        tm_frame.tm_sec_trailer.ocf[0] = (report.cwt << 7) | (report.vnum << 4) | (report.af << 3) | (report.bsnf << 2) | (report.bmacf << 1) | (report.ispif);
        tm_frame.tm_sec_trailer.ocf[1] = (report.lspiu & 0xFF00) >> 8;
        tm_frame.tm_sec_trailer.ocf[2] = (report.lspiu & 0x00FF);
        tm_frame.tm_sec_trailer.ocf[3] = (report.snval);  
        // Alternate OCF
        ocf = 0;
        #ifdef OCF_DEBUG
            Crypto_fsrPrint(&report);
        #endif
    }
}

int32 Crypto_increment(uint8 *num, int length)
{
    int i;
    /* go from right (least significant) to left (most signifcant) */
    for(i = length - 1; i >= 0; --i)
    {
        ++(num[i]); /* increment current byte */

        if(num[i] != 0) /* if byte did not overflow, we're done! */
           break;
    }

    if(i < 0) /* this means num[0] was incremented and overflowed */
        return OS_ERROR;
    else
        return OS_SUCCESS;
}

static int32 Crypto_window(uint8 *actual, uint8 *expected, int length, int window)
{
    int status = OS_ERROR;
    int result = 0;
    uint8 temp[length];

    CFE_PSP_MemCpy(temp, expected, length);

    for (int i = 0; i < window; i++)
    {   
        result = 0;
        /* go from right (least significant) to left (most signifcant) */
        for (int j = length - 1; j >= 0; --j)
        {
            if (actual[j] == temp[j])
            {
                result++;
            }
        }        
        if (result == length)
        {
            status = OS_SUCCESS;
            break;
        }
        Crypto_increment(&temp[0], length);
    }
    return status;
}

static int32 Crypto_compare_less_equal(uint8 *actual, uint8 *expected, int length)
{
    int status = OS_ERROR;

    for(int i = 0; i < length - 1; i++)
    {
        if (actual[i] > expected[i])
        {
            status = OS_SUCCESS;
            break;
        }
        else if (actual[i] < expected[i])
        {
            status = OS_ERROR;
            break;
        }
    }
    return status;
}

uint8 Crypto_Prep_Reply(char* ingest, uint8 appID)
// Assumes that both the pkt_length and pdu_len are set properly
{
    uint8 count = 0;
    
    // Prepare CCSDS for reply
    sdls_frame.hdr.pvn   = 0;
    sdls_frame.hdr.type  = 0;
    sdls_frame.hdr.shdr  = 1;
    sdls_frame.hdr.appID = appID;

    sdls_frame.pdu.type	 = 1;
    
    // Fill ingest with reply header
    ingest[count++] = (sdls_frame.hdr.pvn << 5) | (sdls_frame.hdr.type << 4) | (sdls_frame.hdr.shdr << 3) | ((sdls_frame.hdr.appID & 0x700 >> 8));	
    ingest[count++] = (sdls_frame.hdr.appID & 0x00FF);
    ingest[count++] = (sdls_frame.hdr.seq << 6) | ((sdls_frame.hdr.pktid & 0x3F00) >> 8);
    ingest[count++] = (sdls_frame.hdr.pktid & 0x00FF);
    ingest[count++] = (sdls_frame.hdr.pkt_length & 0xFF00) >> 8;
    ingest[count++] = (sdls_frame.hdr.pkt_length & 0x00FF);

    // Fill ingest with PUS
    //ingest[count++] = (sdls_frame.pus.shf << 7) | (sdls_frame.pus.pusv << 4) | (sdls_frame.pus.ack);
    //ingest[count++] = (sdls_frame.pus.st);
    //ingest[count++] = (sdls_frame.pus.sst);
    //ingest[count++] = (sdls_frame.pus.sid << 4) | (sdls_frame.pus.spare);
    
    // Fill ingest with Tag and Length
    ingest[count++] = (sdls_frame.pdu.type << 7) | (sdls_frame.pdu.uf << 6) | (sdls_frame.pdu.sg << 4) | (sdls_frame.pdu.pid);
    ingest[count++] = (sdls_frame.pdu.pdu_len & 0xFF00) >> 8;
    ingest[count++] = (sdls_frame.pdu.pdu_len & 0x00FF);

    return count;
}

static int32 Crypto_FECF(int fecf, char* ingest, int len_ingest)
// Calculate the Frame Error Control Field (FECF), also known as a cyclic redundancy check (CRC)
{
    int32 result = OS_SUCCESS;
    uint16 calc_fecf = Crypto_Calc_FECF(ingest, len_ingest);

    if ( (fecf & 0xFFFF) != calc_fecf )
        {
            if (((uint8)ingest[18] == 0x0B) && ((uint8)ingest[19] == 0x00) && (((uint8)ingest[20] & 0xF0) == 0x40))
            {   
                // User packet check only used for ESA Testing!
            }
            else
            {   // TODO: Error Correction
                OS_printf(KRED "Error: FECF incorrect!\n" RESET);
                if (log_summary.rs > 0)
                {
                    Crypto_increment((uint8*)&log_summary.num_se, 4);
                    log_summary.rs--;
                    log.blk[log_count].emt = FECF_ERR_EID;
                    log.blk[log_count].emv[0] = 0x4E;
                    log.blk[log_count].emv[1] = 0x41;
                    log.blk[log_count].emv[2] = 0x53;
                    log.blk[log_count].emv[3] = 0x41;
                    log.blk[log_count++].em_len = 4;
                }
                #ifdef FECF_DEBUG
                    OS_printf("\t Calculated = 0x%04x \n\t Received   = 0x%04x \n", calc_fecf, tc_frame.tc_sec_trailer.fecf);
                #endif
                result = OS_ERROR;
            }
        }

    return result;
}

static uint16 Crypto_Calc_FECF(char* ingest, int len_ingest)
// Calculate the Frame Error Control Field (FECF), also known as a cyclic redundancy check (CRC)
{
    uint16 fecf = 0xFFFF;
    uint16 poly = 0x1021;	// TODO: This polynomial is (CRC-CCITT) for ESA testing, may not match standard protocol
    uint8 bit;
    uint8 c15;

    for (int i = 0; i <= len_ingest; i++)
    {	// Byte Logic
        for (int j = 0; j < 8; j++)
        {	// Bit Logic
            bit = ((ingest[i] >> (7 - j) & 1) == 1); 
            c15 = ((fecf >> 15 & 1) == 1); 
            fecf <<= 1;
            if (c15 ^ bit)
            {
                fecf ^= poly;
            }
        }
    }

    // Check if Testing
    if (badFECF == 1)
    {
        fecf++;
    }

    #ifdef FECF_DEBUG
        OS_printf(KCYN "Crypto_Calc_FECF: 0x%02x%02x%02x%02x%02x, len_ingest = %d\n" RESET, ingest[0], ingest[1], ingest[2], ingest[3], ingest[4], len_ingest);
        OS_printf(KCYN "0x" RESET);
        for (int x = 0; x < len_ingest; x++)
        {
            OS_printf(KCYN "%02x" RESET, ingest[x]);
        }
        OS_printf(KCYN "\n" RESET);
        OS_printf(KCYN "In Crypto_Calc_FECF! fecf = 0x%04x\n" RESET, fecf);
    #endif
    
    return fecf;
}

static uint16 Crypto_Calc_CRC16(char* data, int size)
{   // Code provided by ESA
    uint16 crc = 0xFFFF;

    for ( ; size > 0; size--)
    {  
        //OS_printf("*data = 0x%02x \n", (uint8) *data);
        crc = ((crc << 8) & 0xFF00) ^ crc16Table[(crc >> 8) ^ *data++];
    }
       
   return crc;
}

/*
** Key Management Services
*/
static int32 Crypto_Key_OTAR(void)
// The OTAR Rekeying procedure shall have the following Service Parameters:
//  a- Key ID of the Master Key (Integer, unmanaged)
//  b- Size of set of Upload Keys (Integer, managed)
//  c- Set of Upload Keys (Integer[Session Key]; managed)
// NOTE- The size of the session keys is mission specific.
//  a- Set of Key IDs of Upload Keys (Integer[Key IDs]; managed)
//  b- Set of Encrypted Upload Keys (Integer[Size of set of Key ID]; unmanaged)
//  c- Agreed Cryptographic Algorithm (managed)
{
    // Local variables
    SDLS_OTAR_t packet;
    int count = 0;
    int x = 0;
    int32 status = OS_SUCCESS;
    int pdu_keys = (sdls_frame.pdu.pdu_len - 30) / (2 + KEY_SIZE);

    gcry_cipher_hd_t tmp_hd;
    gcry_error_t gcry_error = GPG_ERR_NO_ERROR;

    // Master Key ID
    packet.mkid = (sdls_frame.pdu.data[0] << 8) | (sdls_frame.pdu.data[1]);

    if (packet.mkid >= 128)
    {
        report.af = 1;
        if (log_summary.rs > 0)
        {
            Crypto_increment((uint8*)&log_summary.num_se, 4);
            log_summary.rs--;
            log.blk[log_count].emt = MKID_INVALID_EID;
            log.blk[log_count].emv[0] = 0x4E;
            log.blk[log_count].emv[1] = 0x41;
            log.blk[log_count].emv[2] = 0x53;
            log.blk[log_count].emv[3] = 0x41;
            log.blk[log_count++].em_len = 4;
        }
        OS_printf(KRED "Error: MKID is not valid! \n" RESET);
        status = OS_ERROR;
        return status;
    }

    for (int count = 2; count < (2 + IV_SIZE); count++)
    {	// Initialization Vector
        packet.iv[count-2] = sdls_frame.pdu.data[count];
        //OS_printf("packet.iv[%d] = 0x%02x\n", count-2, packet.iv[count-2]);
    }
    
    count = sdls_frame.pdu.pdu_len - MAC_SIZE; 
    for (int w = 0; w < 16; w++)
    {	// MAC
        packet.mac[w] = sdls_frame.pdu.data[count + w];
        //OS_printf("packet.mac[%d] = 0x%02x\n", w, packet.mac[w]);
    }

    gcry_error = gcry_cipher_open(
        &(tmp_hd), 
        GCRY_CIPHER_AES256, 
        GCRY_CIPHER_MODE_GCM, 
        GCRY_CIPHER_CBC_MAC
    );
    if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
    {
        OS_printf(KRED "ERROR: gcry_cipher_open error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        status = OS_ERROR;
        return status;
    }
    gcry_error = gcry_cipher_setkey(
        tmp_hd, 
        &(ek_ring[packet.mkid].value[0]), 
        KEY_SIZE
    );
    if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
    {
        OS_printf(KRED "ERROR: gcry_cipher_setkey error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        status = OS_ERROR;
        return status;
    }
    gcry_error = gcry_cipher_setiv(
        tmp_hd, 
        &(packet.iv[0]), 
        IV_SIZE
    );
    if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
    {
        OS_printf(KRED "ERROR: gcry_cipher_setiv error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        status = OS_ERROR;
        return status;
    }
    gcry_error = gcry_cipher_decrypt(
        tmp_hd,
        &(sdls_frame.pdu.data[14]),                     // plaintext output
        pdu_keys * (2 + KEY_SIZE),   			 		// length of data
        NULL,                                           // in place decryption
        0                                               // in data length
    );
    if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
    {
        OS_printf(KRED "ERROR: gcry_cipher_decrypt error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        status = OS_ERROR;
        return status;
    }
    gcry_error = gcry_cipher_checktag(
        tmp_hd,
        &(packet.mac[0]),                               // tag input
        MAC_SIZE                                        // tag size
    );
    if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
    {
        OS_printf(KRED "ERROR: gcry_cipher_checktag error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        status = OS_ERROR;
        return status;
    }
    gcry_cipher_close(tmp_hd);
    
    // Read in Decrypted Data
    for (int count = 14; x < pdu_keys; x++)
    {	// Encrypted Key Blocks
        packet.EKB[x].ekid = (sdls_frame.pdu.data[count] << 8) | (sdls_frame.pdu.data[count+1]);
        if (packet.EKB[x].ekid < 128)
        {
            report.af = 1;
            if (log_summary.rs > 0)
            {
                Crypto_increment((uint8*)&log_summary.num_se, 4);
                log_summary.rs--;
                log.blk[log_count].emt = OTAR_MK_ERR_EID;
                log.blk[log_count].emv[0] = 0x4E; // N
                log.blk[log_count].emv[1] = 0x41; // A
                log.blk[log_count].emv[2] = 0x53; // S
                log.blk[log_count].emv[3] = 0x41; // A
                log.blk[log_count++].em_len = 4;
            }
            OS_printf(KRED "Error: Cannot OTAR master key! \n" RESET);
            status = OS_ERROR;
        return status;
        }
        else
        {   
            count = count + 2;
            for (int y = count; y < (KEY_SIZE + count); y++)
            {	// Encrypted Key
                packet.EKB[x].ek[y-count] = sdls_frame.pdu.data[y];
                #ifdef SA_DEBUG
                    OS_printf("\t packet.EKB[%d].ek[%d] = 0x%02x\n", x, y-count, packet.EKB[x].ek[y-count]);
                #endif

                // Setup Key Ring
                ek_ring[packet.EKB[x].ekid].value[y - count] = sdls_frame.pdu.data[y];
            }
            count = count + KEY_SIZE;

            // Set state to PREACTIVE
            ek_ring[packet.EKB[x].ekid].key_state = KEY_PREACTIVE;
        }
    }

    #ifdef PDU_DEBUG
        OS_printf("Received %d keys via master key %d: \n", pdu_keys, packet.mkid);
        for (int x = 0; x < pdu_keys; x++)
        {
            OS_printf("%d) Key ID = %d, 0x", x+1, packet.EKB[x].ekid);
            for(int y = 0; y < KEY_SIZE; y++)
            {
                OS_printf("%02x", packet.EKB[x].ek[y]);
            }
            OS_printf("\n");
        } 
    #endif
    
    return OS_SUCCESS; 
}

static int32 Crypto_Key_update(uint8 state)
// Updates the state of the all keys in the received SDLS EP PDU
{	// Local variables
    SDLS_KEY_BLK_t packet;
    int count = 0;
    int pdu_keys = sdls_frame.pdu.pdu_len / 2;
    #ifdef PDU_DEBUG
        OS_printf("Keys ");
    #endif
    // Read in PDU
    for (int x = 0; x < pdu_keys; x++)
    {
        packet.kblk[x].kid = (sdls_frame.pdu.data[count] << 8) | (sdls_frame.pdu.data[count+1]);
        count = count + 2;
        #ifdef PDU_DEBUG
            if (x != (pdu_keys - 1))
            {
                OS_printf("%d, ", packet.kblk[x].kid);
            }
            else
            {
                OS_printf("and %d ", packet.kblk[x].kid);
            }
        #endif
    }
    #ifdef PDU_DEBUG
        OS_printf("changed to state ");
        switch (state)
        {
            case KEY_PREACTIVE:
                OS_printf("PREACTIVE. \n");
                break;
            case KEY_ACTIVE:
                OS_printf("ACTIVE. \n");
                break;
            case KEY_DEACTIVATED:
                OS_printf("DEACTIVATED. \n");
                break;
            case KEY_DESTROYED:
                OS_printf("DESTROYED. \n");
                break;
            case KEY_CORRUPTED:
                OS_printf("CORRUPTED. \n");
                break;
            default:
                OS_printf("ERROR. \n");
                break;
        }
    #endif
    // Update Key State
    for (int x = 0; x < pdu_keys; x++)
    {
        if (packet.kblk[x].kid < 128)
        {
            report.af = 1;
            if (log_summary.rs > 0)
            {
                Crypto_increment((uint8*)&log_summary.num_se, 4);
                log_summary.rs--;
                log.blk[log_count].emt = MKID_STATE_ERR_EID;
                log.blk[log_count].emv[0] = 0x4E;
                log.blk[log_count].emv[1] = 0x41;
                log.blk[log_count].emv[2] = 0x53;
                log.blk[log_count].emv[3] = 0x41;
                log.blk[log_count++].em_len = 4;
            }
            OS_printf(KRED "Error: MKID state cannot be changed! \n" RESET);
            // TODO: Exit
        }

        if (ek_ring[packet.kblk[x].kid].key_state == (state - 1))
        {
            ek_ring[packet.kblk[x].kid].key_state = state;
            #ifdef PDU_DEBUG
                //OS_printf("Key ID %d state changed to ", packet.kblk[x].kid);
            #endif
        }
        else 
        {
            if (log_summary.rs > 0)
            {
                Crypto_increment((uint8*)&log_summary.num_se, 4);
                log_summary.rs--;
                log.blk[log_count].emt = KEY_TRANSITION_ERR_EID;
                log.blk[log_count].emv[0] = 0x4E;
                log.blk[log_count].emv[1] = 0x41;
                log.blk[log_count].emv[2] = 0x53;
                log.blk[log_count].emv[3] = 0x41;
                log.blk[log_count++].em_len = 4;
            }
            OS_printf(KRED "Error: Key %d cannot transition to desired state! \n" RESET, packet.kblk[x].kid);
        }
    }
    return OS_SUCCESS; 
}

static int32 Crypto_Key_inventory(char* ingest)
{
    // Local variables
    SDLS_KEY_INVENTORY_t packet;
    int count = 0;
    uint16_t range = 0;

    // Read in PDU
    packet.kid_first = ((uint8)sdls_frame.pdu.data[count] << 8) | ((uint8)sdls_frame.pdu.data[count+1]);
    count = count + 2;
    packet.kid_last = ((uint8)sdls_frame.pdu.data[count] << 8) | ((uint8)sdls_frame.pdu.data[count+1]);
    count = count + 2;

    // Prepare for Reply
    range = packet.kid_last - packet.kid_first;
    sdls_frame.pdu.pdu_len = 2 + (range * (2 + 1));
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);
    ingest[count++] = (range & 0xFF00) >> 8;
    ingest[count++] = (range & 0x00FF);
    for (uint16_t x = packet.kid_first; x < packet.kid_last; x++)
    {   // Key ID
        ingest[count++] = (x & 0xFF00) >> 8;
        ingest[count++] = (x & 0x00FF);
        // Key State
        ingest[count++] = ek_ring[x].key_state;
    }
    return count;
}

static int32 Crypto_Key_verify(char* ingest)
{
    // Local variables
    SDLS_KEYV_CMD_t packet;
    int count = 0;
    int pdu_keys = sdls_frame.pdu.pdu_len / SDLS_KEYV_CMD_BLK_SIZE;

    gcry_error_t gcry_error = GPG_ERR_NO_ERROR;
    gcry_cipher_hd_t tmp_hd;
    uint8 iv_loc;

    //uint8 tmp_mac[MAC_SIZE];

    #ifdef PDU_DEBUG
        OS_printf("Crypto_Key_verify: Requested %d key(s) to verify \n", pdu_keys);
    #endif
    
    // Read in PDU
    for (int x = 0; x < pdu_keys; x++)
    {	
        // Key ID
        packet.blk[x].kid = ((uint8)sdls_frame.pdu.data[count] << 8) | ((uint8)sdls_frame.pdu.data[count+1]);
        count = count + 2;
        #ifdef PDU_DEBUG
            OS_printf("Crypto_Key_verify: Block %d Key ID is %d \n", x, packet.blk[x].kid);
        #endif
        // Key Challenge
        for (int y = 0; y < CHALLENGE_SIZE; y++)
        {
            packet.blk[x].challenge[y] = sdls_frame.pdu.data[count++];
        }
        #ifdef PDU_DEBUG
            OS_printf("\n");
        #endif
    }
    
    // Prepare for Reply
    sdls_frame.pdu.pdu_len = pdu_keys * (2 + IV_SIZE + CHALLENGE_SIZE + CHALLENGE_MAC_SIZE);
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);

    for (int x = 0; x < pdu_keys; x++)
    {   // Key ID
        ingest[count++] = (packet.blk[x].kid & 0xFF00) >> 8;
        ingest[count++] = (packet.blk[x].kid & 0x00FF);

        // Initialization Vector
        iv_loc = count;
        for (int y = 0; y < IV_SIZE; y++)
        {   
            ingest[count++] = tc_frame.tc_sec_header.iv[y];
        }
        ingest[count-1] = ingest[count-1] + x + 1;

        // Encrypt challenge 
        gcry_error = gcry_cipher_open(
            &(tmp_hd), 
            GCRY_CIPHER_AES256, 
            GCRY_CIPHER_MODE_GCM, 
            GCRY_CIPHER_CBC_MAC
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_open error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        }
        gcry_error = gcry_cipher_setkey(
            tmp_hd, 
            &(ek_ring[packet.blk[x].kid].value[0]),
            KEY_SIZE
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_setkey error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        }
        gcry_error = gcry_cipher_setiv(
            tmp_hd, 
            &(ingest[iv_loc]), 
            IV_SIZE
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_setiv error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        }
        gcry_error = gcry_cipher_encrypt(
            tmp_hd,
            &(ingest[count]),                               // ciphertext output
            CHALLENGE_SIZE,			 		                // length of data
            &(packet.blk[x].challenge[0]),                  // plaintext input
            CHALLENGE_SIZE                                  // in data length
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_encrypt error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        }
        count = count + CHALLENGE_SIZE; // Don't forget to increment count!
        
        gcry_error = gcry_cipher_gettag(
            tmp_hd,
            &(ingest[count]),                               // tag output
            CHALLENGE_MAC_SIZE                              // tag size
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_gettag error code %d \n" RESET,gcry_error & GPG_ERR_CODE_MASK);
        }
        count = count + CHALLENGE_MAC_SIZE; // Don't forget to increment count!

        // Copy from tmp_mac into ingest
        //for( int y = 0; y < CHALLENGE_MAC_SIZE; y++)
        //{
        //    ingest[count++] = tmp_mac[y];
        //}
        gcry_cipher_close(tmp_hd);
    }

    #ifdef PDU_DEBUG
        OS_printf("Crypto_Key_verify: Response is %d bytes \n", count);
    #endif

    return count;
}

/*
** Security Association Monitoring and Control
*/
static int32 Crypto_MC_ping(char* ingest)
{
    int count = 0;

    // Prepare for Reply
    sdls_frame.pdu.pdu_len = 0;
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);

    return count;
}

static int32 Crypto_MC_status(char* ingest)
{
    int count = 0;

    // TODO: Update log_summary.rs;

    // Prepare for Reply
    sdls_frame.pdu.pdu_len = 2; // 4
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);
    
    // PDU
    //ingest[count++] = (log_summary.num_se & 0xFF00) >> 8;
    ingest[count++] = (log_summary.num_se & 0x00FF);
    //ingest[count++] = (log_summary.rs & 0xFF00) >> 8;
    ingest[count++] = (log_summary.rs & 0x00FF);
    
    #ifdef PDU_DEBUG
        OS_printf("log_summary.num_se = 0x%02x \n",log_summary.num_se);
        OS_printf("log_summary.rs = 0x%02x \n",log_summary.rs);
    #endif

    return count;
}

static int32 Crypto_MC_dump(char* ingest)
{
    int count = 0;
    
    // Prepare for Reply
    sdls_frame.pdu.pdu_len = (log_count * 6);  // SDLS_MC_DUMP_RPLY_SIZE
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);

    // PDU
    for (int x = 0; x < log_count; x++)
    {
        ingest[count++] = log.blk[x].emt;
        //ingest[count++] = (log.blk[x].em_len & 0xFF00) >> 8;
        ingest[count++] = (log.blk[x].em_len & 0x00FF);
        for (int y = 0; y < EMV_SIZE; y++)
        {
            ingest[count++] = log.blk[x].emv[y];
        }
    }

    #ifdef PDU_DEBUG
        OS_printf("log_count = %d \n", log_count);
        OS_printf("log_summary.num_se = 0x%02x \n",log_summary.num_se);
        OS_printf("log_summary.rs = 0x%02x \n",log_summary.rs);
    #endif

    return count; 
}

static int32 Crypto_MC_erase(char* ingest)
{
    int count = 0;

    // Zero Logs
    for (int x = 0; x < LOG_SIZE; x++)
    {
        log.blk[x].emt = 0;
        log.blk[x].em_len = 0;
        for (int y = 0; y < EMV_SIZE; y++)
        {
            log.blk[x].emv[y] = 0;
        }
    }

    // Compute Summary
    log_count = 0;
    log_summary.num_se = 0;
    log_summary.rs = LOG_SIZE;

    // Prepare for Reply
    sdls_frame.pdu.pdu_len = 2; // 4
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);

    // PDU
    //ingest[count++] = (log_summary.num_se & 0xFF00) >> 8;
    ingest[count++] = (log_summary.num_se & 0x00FF);
    //ingest[count++] = (log_summary.rs & 0xFF00) >> 8;
    ingest[count++] = (log_summary.rs & 0x00FF);

    return count; 
}

static int32 Crypto_MC_selftest(char* ingest)
{
    uint8 count = 0;
    uint8 result = ST_OK;

    // TODO: Perform test

    // Prepare for Reply
    sdls_frame.pdu.pdu_len = 1;
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);

    ingest[count++] = result;
    
    return count; 
}

static int32 Crypto_SA_readARSN(char* ingest)
{
    uint8 count = 0;
    uint16 spi = 0x0000;
    SecurityAssociation_t* sa_ptr;

    // Read ingest
    spi = ((uint8)sdls_frame.pdu.data[0] << 8) | (uint8)sdls_frame.pdu.data[1];

    // Prepare for Reply
    sdls_frame.pdu.pdu_len = 2 + IV_SIZE;
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 128);

    // Write SPI to reply
    ingest[count++] = (spi & 0xFF00) >> 8;
    ingest[count++] = (spi & 0x00FF);


    if(sadb_routine->sadb_get_sa_from_spi(spi,&sa_ptr) != OS_SUCCESS){
        //TODO - Error handling
        return OS_ERROR; //Error -- unable to get SA from SPI.
    }


    if (sa_ptr->iv_len > 0)
    {   // Set IV - authenticated encryption
        for (int x = 0; x < sa_ptr->iv_len - 1; x++)
        {
            ingest[count++] = sa_ptr->iv[x];
        }
        
        // TODO: Do we need this?
        if (sa_ptr->iv[IV_SIZE - 1] > 0)
        {   // Adjust to report last received, not expected
            ingest[count++] = sa_ptr->iv[IV_SIZE - 1] - 1;
        }
        else
        {   
            ingest[count++] = sa_ptr->iv[IV_SIZE - 1];
        }
    }
    else
    {   
        // TODO
    }

    #ifdef PDU_DEBUG
        OS_printf("spi = %d \n", spi);
        if (sa_ptr->iv_len > 0)
        {
            OS_printf("ARSN = 0x");
            for (int x = 0; x < sa_ptr->iv_len; x++)
            {
                OS_printf("%02x", sa_ptr->iv[x]);
            }
            OS_printf("\n");
        }
    #endif
    
    return count; 
}

static int32 Crypto_MC_resetalarm(void)
{   // Reset all alarm flags
    report.af = 0;
    report.bsnf = 0;
    report.bmacf = 0;
    report.ispif = 0;    
    return OS_SUCCESS; 
}

static int32 Crypto_User_IdleTrigger(char* ingest)
{
    uint8 count = 0;

    // Prepare for Reply
    sdls_frame.pdu.pdu_len = 0;
    sdls_frame.hdr.pkt_length = sdls_frame.pdu.pdu_len + 9;
    count = Crypto_Prep_Reply(ingest, 144);
    
    return count; 
}
                         
static int32 Crypto_User_BadSPI(void)
{
    // Toggle Bad Sequence Number
    if (badSPI == 0)
    {
        badSPI = 1;
    }   
    else
    {
        badSPI = 0;
    }
    
    return OS_SUCCESS; 
}

static int32 Crypto_User_BadMAC(void)
{
    // Toggle Bad MAC
    if (badMAC == 0)
    {
        badMAC = 1;
    }   
    else
    {
        badMAC = 0;
    }
    
    return OS_SUCCESS; 
}

static int32 Crypto_User_BadIV(void)
{
    // Toggle Bad MAC
    if (badIV == 0)
    {
        badIV = 1;
    }   
    else
    {
        badIV = 0;
    }
    
    return OS_SUCCESS; 
}

static int32 Crypto_User_BadFECF(void)
{
    // Toggle Bad FECF
    if (badFECF == 0)
    {
        badFECF = 1;
    }   
    else
    {
        badFECF = 0;
    }
    
    return OS_SUCCESS; 
}

static int32 Crypto_User_ModifyKey(void)
{
    // Local variables
    uint16 kid = ((uint8)sdls_frame.pdu.data[0] << 8) | ((uint8)sdls_frame.pdu.data[1]);
    uint8 mod = (uint8)sdls_frame.pdu.data[2];

    switch (mod)
    {
        case 1: // Invalidate Key
            ek_ring[kid].value[KEY_SIZE-1]++;
            OS_printf("Key %d value invalidated! \n", kid);
            break;
        case 2: // Modify key state
            ek_ring[kid].key_state = (uint8)sdls_frame.pdu.data[3] & 0x0F;
            OS_printf("Key %d state changed to %d! \n", kid, mod);
            break;
        default:
            // Error
            break;
    }
    
    return OS_SUCCESS; 
}

static int32 Crypto_User_ModifyActiveTM(void)
{
    tm_frame.tm_sec_header.spi = (uint8)sdls_frame.pdu.data[0];   
    return OS_SUCCESS; 
}

static int32 Crypto_User_ModifyVCID(void)
{
    tm_frame.tm_header.vcid = (uint8)sdls_frame.pdu.data[0];
    SecurityAssociation_t* sa_ptr;

    for (int i = 0; i < NUM_GVCID; i++)
    {
        if(sadb_routine->sadb_get_sa_from_spi(i,&sa_ptr) != OS_SUCCESS){
            //TODO - Error handling
            return OS_ERROR; //Error -- unable to get SA from SPI.
        }
        for (int j = 0; j < NUM_SA; j++)
        {

            if (sa_ptr->gvcid_tm_blk[j].mapid == TYPE_TM)
            {
                if (sa_ptr->gvcid_tm_blk[j].vcid == tm_frame.tm_header.vcid)
                {
                    tm_frame.tm_sec_header.spi = i;
                    OS_printf("TM Frame SPI changed to %d \n",i);
                    break;
                }
            }
        }
    }

    return OS_SUCCESS;
}

/*
** Procedures Specifications
*/
static int32 Crypto_PDU(char* ingest)
{
    int32 status = OS_SUCCESS;
    
    switch (sdls_frame.pdu.type)
    {
        case 0:	// Command
            switch (sdls_frame.pdu.uf)
            {
                case 0:	// CCSDS Defined Command
                    switch (sdls_frame.pdu.sg)
                    {
                        case SG_KEY_MGMT:  // Key Management Procedure
                            switch (sdls_frame.pdu.pid)
                            {
                                case PID_OTAR:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "Key OTAR\n" RESET);
                                    #endif
                                    status = Crypto_Key_OTAR();
                                    break;
                                case PID_KEY_ACTIVATION:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "Key Activate\n" RESET);
                                    #endif
                                    status = Crypto_Key_update(KEY_ACTIVE);
                                    break;
                                case PID_KEY_DEACTIVATION:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "Key Deactivate\n" RESET);
                                    #endif
                                    status = Crypto_Key_update(KEY_DEACTIVATED);
                                    break;
                                case PID_KEY_VERIFICATION:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "Key Verify\n" RESET);
                                    #endif
                                    status = Crypto_Key_verify(ingest);
                                    break;
                                case PID_KEY_DESTRUCTION:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "Key Destroy\n" RESET);
                                    #endif
                                    status = Crypto_Key_update(KEY_DESTROYED);
                                    break;
                                case PID_KEY_INVENTORY:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "Key Inventory\n" RESET);
                                    #endif
                                    status = Crypto_Key_inventory(ingest);
                                    break;
                                default:
                                    OS_printf(KRED "Error: Crypto_PDU failed interpreting Key Management Procedure Identification Field! \n" RESET);
                                    break;
                            }
                            break;
                        case SG_SA_MGMT:  // Security Association Management Procedure
                            switch (sdls_frame.pdu.pid)
                            {
                                case PID_CREATE_SA:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Create\n" RESET); 
                                    #endif
                                    status = sadb_routine->sadb_sa_create();
                                    break;
                                case PID_DELETE_SA:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Delete\n" RESET);
                                    #endif
                                    status = sadb_routine->sadb_sa_delete();
                                    break;
                                case PID_SET_ARSNW:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA setARSNW\n" RESET);
                                    #endif
                                    status = sadb_routine->sadb_sa_setARSNW();
                                    break;
                                case PID_REKEY_SA:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Rekey\n" RESET); 
                                    #endif
                                    status = sadb_routine->sadb_sa_rekey();
                                    break;
                                case PID_EXPIRE_SA:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Expire\n" RESET);
                                    #endif
                                    status = sadb_routine->sadb_sa_expire();
                                    break;
                                case PID_SET_ARSN:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA SetARSN\n" RESET);
                                    #endif
                                    status = sadb_routine->sadb_sa_setARSN();
                                    break;
                                case PID_START_SA:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Start\n" RESET); 
                                    #endif
                                    status = sadb_routine->sadb_sa_start();
                                    break;
                                case PID_STOP_SA:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Stop\n" RESET);
                                    #endif
                                    status = sadb_routine->sadb_sa_stop();
                                    break;
                                case PID_READ_ARSN:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA readARSN\n" RESET);
                                    #endif
                                    status = Crypto_SA_readARSN(ingest);
                                    break;
                                case PID_SA_STATUS:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "SA Status\n" RESET);
                                    #endif
                                    status = sadb_routine->sadb_sa_status(ingest);
                                    break;
                                default:
                                    OS_printf(KRED "Error: Crypto_PDU failed interpreting SA Procedure Identification Field! \n" RESET);
                                    break;
                            }
                            break;
                        case SG_SEC_MON_CTRL:  // Security Monitoring & Control Procedure
                            switch (sdls_frame.pdu.pid)
                            {
                                case PID_PING:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "MC Ping\n" RESET);
                                    #endif
                                    status = Crypto_MC_ping(ingest);
                                    break;
                                case PID_LOG_STATUS:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "MC Status\n" RESET);
                                    #endif
                                    status = Crypto_MC_status(ingest);
                                    break;
                                case PID_DUMP_LOG:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "MC Dump\n" RESET);
                                    #endif
                                    status = Crypto_MC_dump(ingest);
                                    break;
                                case PID_ERASE_LOG:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "MC Erase\n" RESET);
                                    #endif
                                    status = Crypto_MC_erase(ingest);
                                    break;
                                case PID_SELF_TEST:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "MC Selftest\n" RESET);
                                    #endif
                                    status = Crypto_MC_selftest(ingest);
                                    break;
                                case PID_ALARM_FLAG:
                                    #ifdef PDU_DEBUG
                                        OS_printf(KGRN "MC Reset Alarm\n" RESET);
                                    #endif
                                    status = Crypto_MC_resetalarm();
                                    break;
                                default:
                                    OS_printf(KRED "Error: Crypto_PDU failed interpreting MC Procedure Identification Field! \n" RESET);
                                    break;
                            }
                            break;
                        default: // ERROR
                            OS_printf(KRED "Error: Crypto_PDU failed interpreting Service Group! \n" RESET);
                            break;
                    }
                    break;
                    
                case 1: 	// User Defined Command
                    switch (sdls_frame.pdu.sg)
                    {
                        default:
                            switch (sdls_frame.pdu.pid)
                            {
                                case 0: // Idle Frame Trigger
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Idle Trigger\n" RESET);
                                    #endif
                                    status = Crypto_User_IdleTrigger(ingest);
                                    break;
                                case 1: // Toggle Bad SPI
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Toggle Bad SPI\n" RESET);
                                    #endif
                                    status = Crypto_User_BadSPI();
                                    break;
                                case 2: // Toggle Bad IV
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Toggle Bad IV\n" RESET);
                                    #endif
                                    status = Crypto_User_BadIV();\
                                    break;
                                case 3: // Toggle Bad MAC
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Toggle Bad MAC\n" RESET);
                                    #endif
                                    status = Crypto_User_BadMAC();
                                    break; 
                                case 4: // Toggle Bad FECF
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Toggle Bad FECF\n" RESET);
                                    #endif
                                    status = Crypto_User_BadFECF();
                                    break;
                                case 5: // Modify Key
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Modify Key\n" RESET);
                                    #endif
                                    status = Crypto_User_ModifyKey();
                                    break;
                                case 6: // Modify ActiveTM
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Modify Active TM\n" RESET);
                                    #endif
                                    status = Crypto_User_ModifyActiveTM();
                                    break;
                                case 7: // Modify TM VCID
                                    #ifdef PDU_DEBUG
                                        OS_printf(KMAG "User Modify VCID\n" RESET);
                                    #endif
                                    status = Crypto_User_ModifyVCID();
                                    break;
                                default:
                                    OS_printf(KRED "Error: Crypto_PDU received user defined command! \n" RESET);
                                    break;
                            }
                    }
                    break;
            }
            break;
            
        case 1:	// Reply
            OS_printf(KRED "Error: Crypto_PDU failed interpreting PDU Type!  Received a Reply!?! \n" RESET);
            break;
    }

    #ifdef CCSDS_DEBUG
        if (status > 0)
        {
            OS_printf(KMAG "CCSDS message put on software bus: 0x" RESET);
            for (int x = 0; x < status; x++)
            {
                OS_printf(KMAG "%02x" RESET, x, (uint8) ingest[x]);
            }
            OS_printf("\n");
        }
    #endif

    return status;
}

int32 Crypto_TC_ApplySecurity(char** ingest, int* len_ingest)
{
    // Local Variables
    int32 status = OS_SUCCESS;
    unsigned char* tc_ingest = *ingest;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_TC_ApplySecurity START -----\n" RESET);
    #endif

    // TODO: This whole function!
    //len_ingest = len_ingest;
    //ingest[0] = ingest[0];

    int security_header_bytes = 18;
    int security_trailer_bytes = 16;
    int tc_size = *len_ingest + security_header_bytes + security_trailer_bytes;

    unsigned char * tempTC=NULL;
    tempTC = (unsigned char *)malloc(tc_size * sizeof (unsigned char));
    CFE_PSP_MemSet(tempTC, 0, tc_size);

    int count = 0;
    //Create Security Header
    for (;count < security_header_bytes;count++){
        tempTC[count]= 0x55; //put dummy filler bits in security header for now.
    }

    //Create Frame Body
    CFE_PSP_MemCpy(&tempTC[security_header_bytes],&tc_ingest[0],*len_ingest);
    count+=*len_ingest;

    //Create Security Trailer
    for(;count < tc_size;count++){
        tempTC[count]=0x55; //put dummy filler bits in security trailer for now.
    }

    *ingest = tempTC;
    *len_ingest = tc_size;

    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_TC_ApplySecurity END -----\n" RESET);
    #endif

    return status;
}

int32 Crypto_TC_ProcessSecurity( char* ingest, int* len_ingest)
// Loads the ingest frame into the global tc_frame while performing decrpytion
{
    // Local Variables
    int32 status = OS_SUCCESS;
    int x = 0;
    int y = 0;
    gcry_cipher_hd_t tmp_hd;
    gcry_error_t gcry_error = GPG_ERR_NO_ERROR;
    SecurityAssociation_t* sa_ptr = NULL;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_TC_ProcessSecurity START -----\n" RESET);
    #endif

    // Primary Header
    tc_frame.tc_header.tfvn   = ((uint8)ingest[0] & 0xC0) >> 6;
    tc_frame.tc_header.bypass = ((uint8)ingest[0] & 0x20) >> 5;
    tc_frame.tc_header.cc     = ((uint8)ingest[0] & 0x10) >> 4;
    tc_frame.tc_header.spare  = ((uint8)ingest[0] & 0x0C) >> 2;
    tc_frame.tc_header.scid   = ((uint8)ingest[0] & 0x03) << 8;
    tc_frame.tc_header.scid   = tc_frame.tc_header.scid | (uint8)ingest[1];
    tc_frame.tc_header.vcid   = ((uint8)ingest[2] & 0xFC) >> 2;
    tc_frame.tc_header.fl     = ((uint8)ingest[2] & 0x03) << 8;
    tc_frame.tc_header.fl     = tc_frame.tc_header.fl | (uint8)ingest[3];
    tc_frame.tc_header.fsn	  = (uint8)ingest[4];

    // Security Header
    tc_frame.tc_sec_header.sh  = (uint8)ingest[5]; 
    tc_frame.tc_sec_header.spi = ((uint8)ingest[6] << 8) | (uint8)ingest[7];
    #ifdef TC_DEBUG
        OS_printf("vcid = %d \n", tc_frame.tc_header.vcid );
        OS_printf("spi  = %d \n", tc_frame.tc_sec_header.spi);
    #endif

    // Checks
    if (((uint8)ingest[18] == 0x0B) && ((uint8)ingest[19] == 0x00) && (((uint8)ingest[20] & 0xF0) == 0x40))
    {   
        // User packet check only used for ESA Testing!
    }
    else
    {   // Update last spi used
        report.lspiu = tc_frame.tc_sec_header.spi;

        // Verify 
        if (tc_frame.tc_header.scid != (SCID & 0x3FF))
        {
            OS_printf(KRED "Error: SCID incorrect! \n" RESET);
            status = OS_ERROR;
        }
        else
        {   
            switch (report.lspiu)
            {	// Invalid SPIs fall through to trigger flag in FSR
                case 0x0000:
                case 0xFFFF:
                    status = OS_ERROR;
                    report.ispif = 1;
                    OS_printf(KRED "Error: SPI invalid! \n" RESET);
                    break;
                default:
                    break;
            }
        }
        if ((report.lspiu > NUM_SA) && (status == OS_SUCCESS))
        {
            report.ispif = 1;
            OS_printf(KRED "Error: SPI value greater than NUM_SA! \n" RESET);
            status = OS_ERROR;
        }
        if (status == OS_SUCCESS)
        {
            if(sadb_routine->sadb_get_sa_from_spi(report.lspiu,&sa_ptr) != OS_SUCCESS){
                //TODO - Error handling
                status = OS_ERROR; //Error -- unable to get SA from SPI.
            }
        }
        if (status == OS_SUCCESS)
        {
            if (sa_ptr->gvcid_tc_blk[tc_frame.tc_header.vcid].mapid != TYPE_TC)
            {	
                OS_printf(KRED "Error: SA invalid type! \n" RESET);
                status = OS_ERROR;
            }
        }
        // TODO: I don't think this is needed.
        //if (status == OS_SUCCESS)
        //{
        //    if (sa[report.lspiu].gvcid_tc_blk[tc_frame.tc_header.vcid].vcid != tc_frame.tc_header.vcid)
        //    {	
        //        OS_printf(KRED "Error: VCID not mapped to provided SPI! \n" RESET);
        //        status = OS_ERROR;
        //    }
        //}
        if (status == OS_SUCCESS)
        {
            if (sa_ptr->sa_state != SA_OPERATIONAL)
            {
                OS_printf(KRED "Error: SA state not operational! \n" RESET);
                status = OS_ERROR;
            }
        }
        if (status != OS_SUCCESS)
        {
            report.af = 1;
            if (log_summary.rs > 0)
            {
                Crypto_increment((uint8*)&log_summary.num_se, 4);
                log_summary.rs--;
                log.blk[log_count].emt = SPI_INVALID_EID;
                log.blk[log_count].emv[0] = 0x4E;
                log.blk[log_count].emv[1] = 0x41;
                log.blk[log_count].emv[2] = 0x53;
                log.blk[log_count].emv[3] = 0x41;
                log.blk[log_count++].em_len = 4;
            }
            *len_ingest = 0;
            return status;
        }
    }
    if(sadb_routine->sadb_get_sa_from_spi(tc_frame.tc_sec_header.spi,&sa_ptr) != OS_SUCCESS){
        //TODO - Error handling
        status = OS_ERROR; //Error -- unable to get SA from SPI.
        return status;
    }
    // Determine mode via SPI
    if ((sa_ptr->est == 1) &&
        (sa_ptr->ast == 1))
    {	// Authenticated Encryption
        #ifdef DEBUG
            OS_printf(KBLU "ENCRYPTED TC Received!\n" RESET);
        #endif
        #ifdef TC_DEBUG
            OS_printf("IV: \n");
        #endif
        for (x = 8; x < (8 + IV_SIZE); x++)
        {
            tc_frame.tc_sec_header.iv[x-8] = (uint8)ingest[x];
            #ifdef TC_DEBUG
                OS_printf("\t iv[%d] = 0x%02x\n", x-8, tc_frame.tc_sec_header.iv[x-8]);
            #endif
        }
        report.snval = tc_frame.tc_sec_header.iv[IV_SIZE-1];

        #ifdef DEBUG
            OS_printf("\t tc_sec_header.iv[%d] = 0x%02x \n", IV_SIZE-1, tc_frame.tc_sec_header.iv[IV_SIZE-1]);
            OS_printf("\t sa[%d].iv[%d] = 0x%02x \n", tc_frame.tc_sec_header.spi, IV_SIZE-1, sa_ptr->iv[IV_SIZE-1]);
        #endif

        // Check IV is in ARCW
        if ( Crypto_window(tc_frame.tc_sec_header.iv, sa_ptr->iv, IV_SIZE,
            sa_ptr->arcw[sa_ptr->arcw_len-1]) != OS_SUCCESS )
        {
            report.af = 1;
            report.bsnf = 1;
            if (log_summary.rs > 0)
            {
                Crypto_increment((uint8*)&log_summary.num_se, 4);
                log_summary.rs--;
                log.blk[log_count].emt = IV_WINDOW_ERR_EID;
                log.blk[log_count].emv[0] = 0x4E;
                log.blk[log_count].emv[1] = 0x41;
                log.blk[log_count].emv[2] = 0x53;
                log.blk[log_count].emv[3] = 0x41;
                log.blk[log_count++].em_len = 4;
            }
            OS_printf(KRED "Error: IV not in window! \n" RESET);
            #ifdef OCF_DEBUG
                Crypto_fsrPrint(&report);
            #endif
            status = OS_ERROR;
        }
        else 
        {
            if ( Crypto_compare_less_equal(tc_frame.tc_sec_header.iv, sa_ptr->iv, IV_SIZE) == OS_SUCCESS )
            {   // Replay - IV value lower than expected
                report.af = 1;
                report.bsnf = 1;
                if (log_summary.rs > 0)
                {
                    Crypto_increment((uint8*)&log_summary.num_se, 4);
                    log_summary.rs--;
                    log.blk[log_count].emt = IV_REPLAY_ERR_EID;
                    log.blk[log_count].emv[0] = 0x4E;
                    log.blk[log_count].emv[1] = 0x41;
                    log.blk[log_count].emv[2] = 0x53;
                    log.blk[log_count].emv[3] = 0x41;
                    log.blk[log_count++].em_len = 4;
                }
                OS_printf(KRED "Error: IV replay! Value lower than expected! \n" RESET);
                #ifdef OCF_DEBUG
                    Crypto_fsrPrint(&report);
                #endif
                status = OS_ERROR;
            } 
            else
            {   // Adjust expected IV to acceptable received value
                for (int i = 0; i < (IV_SIZE); i++)
                {
                    sa_ptr->iv[i] = tc_frame.tc_sec_header.iv[i];
                }
            }
        }
        
        if ( status == OS_ERROR )
        {   // Exit
            *len_ingest = 0;
            return status;
        }

        x = x + Crypto_Get_tcPayloadLength();

        #ifdef TC_DEBUG
            OS_printf("TC: \n"); 
            for (int temp = 0; temp < Crypto_Get_tcPayloadLength(); temp++)
            {	
                OS_printf("\t ingest[%d] = 0x%02x \n", temp, (uint8)ingest[temp+20]);
            }
        #endif

        // Security Trailer
        #ifdef TC_DEBUG
            OS_printf("MAC: \n");
        #endif
        for (y = x; y < (x + MAC_SIZE); y++)
        {
            tc_frame.tc_sec_trailer.mac[y-x]  = (uint8)ingest[y];
            #ifdef TC_DEBUG
                OS_printf("\t mac[%d] = 0x%02x\n", y-x, tc_frame.tc_sec_trailer.mac[y-x]);
            #endif
        }
        x = x + MAC_SIZE;

        // FECF
        tc_frame.tc_sec_trailer.fecf = ((uint8)ingest[x] << 8) | ((uint8)ingest[x+1]);
        Crypto_FECF(tc_frame.tc_sec_trailer.fecf, ingest, (tc_frame.tc_header.fl - 2));

        // Initialize the key
        //itc_gcm128_init(&sa[tc_frame.tc_sec_header.spi].gcm_ctx, (const unsigned char*) &ek_ring[sa[tc_frame.tc_sec_header.spi].ekid]);

        gcry_error = gcry_cipher_open(
            &(tmp_hd),
            GCRY_CIPHER_AES256, 
            GCRY_CIPHER_MODE_GCM, 
            GCRY_CIPHER_CBC_MAC
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_open error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
            status = OS_ERROR;
            return status;
        }
        #ifdef DEBUG
            OS_printf("Key ID = %d, 0x", sa_ptr->ekid);
            for(int y = 0; y < KEY_SIZE; y++)
            {
                OS_printf("%02x", ek_ring[sa_ptr->ekid].value[y]);
            }
            OS_printf("\n");
        #endif
        gcry_error = gcry_cipher_setkey(
            tmp_hd,
            &(ek_ring[sa_ptr->ekid].value[0]),
            KEY_SIZE
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_setkey error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
            status = OS_ERROR;
            return status;
        }
        gcry_error = gcry_cipher_setiv(
            tmp_hd,
            &(sa_ptr->iv[0]),
            sa_ptr->iv_len
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_setiv error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
            status = OS_ERROR;
            return status;
        }
        #ifdef MAC_DEBUG
            OS_printf("AAD = 0x");
        #endif
        // Prepare additional authenticated data (AAD)
        for (y = 0; y < sa_ptr->abm_len; y++)
        {
            ingest[y] = (uint8) ((uint8)ingest[y] & (uint8)sa_ptr->abm[y]);
            #ifdef MAC_DEBUG
                OS_printf("%02x", (uint8) ingest[y]);
            #endif
        }
        #ifdef MAC_DEBUG
            OS_printf("\n");
        #endif

        gcry_error = gcry_cipher_decrypt(
            tmp_hd, 
            &(tc_frame.tc_pdu[0]),                          // plaintext output
            Crypto_Get_tcPayloadLength(),			 		// length of data
            &(ingest[20]),                                  // ciphertext input
            Crypto_Get_tcPayloadLength()                    // in data length
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_decrypt error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
            status = OS_ERROR;
            return status;
        }
        gcry_error = gcry_cipher_checktag(
            tmp_hd, 
            &(tc_frame.tc_sec_trailer.mac[0]),              // tag input
            MAC_SIZE                                        // tag size
        );
        if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
        {
            OS_printf(KRED "ERROR: gcry_cipher_checktag error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
            
            OS_printf("Actual MAC   = 0x");
            for (int z = 0; z < MAC_SIZE; z++)
            {
                OS_printf("%02x",tc_frame.tc_sec_trailer.mac[z]);
            }
            OS_printf("\n");
            
            gcry_error = gcry_cipher_gettag(
                tmp_hd,
                &(tc_frame.tc_sec_trailer.mac[0]),          // tag output
                MAC_SIZE                                    // tag size
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_checktag error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
            }

            OS_printf("Expected MAC = 0x");
            for (int z = 0; z < MAC_SIZE; z++)
            {
                OS_printf("%02x",tc_frame.tc_sec_trailer.mac[z]);
            }
            OS_printf("\n");
            status = OS_ERROR;
            report.bmacf = 1;
            #ifdef OCF_DEBUG
                Crypto_fsrPrint(&report);
            #endif
            return status;
        }
        gcry_cipher_close(tmp_hd);
        
        // Increment the IV for next time
        #ifdef INCREMENT
            Crypto_increment(sa_ptr->iv, IV_SIZE);
        #endif
    }
    else
    {	// Clear
        #ifdef DEBUG
            OS_printf(KBLU "CLEAR TC Received!\n" RESET);
        #endif

        for (y = 10; y <= (tc_frame.tc_header.fl - 2); y++)
        {	
            tc_frame.tc_pdu[y - 10] = (uint8)ingest[y]; 
        }
        // FECF
        tc_frame.tc_sec_trailer.fecf = ((uint8)ingest[y] << 8) | ((uint8)ingest[y+1]);
        Crypto_FECF((int) tc_frame.tc_sec_trailer.fecf, ingest, (tc_frame.tc_header.fl - 2));
    }
    
    #ifdef TC_DEBUG
        Crypto_tcPrint(&tc_frame);
    #endif

    // Zero ingest
    for (x = 0; x < *len_ingest; x++)
    {
        ingest[x] = 0;
    }
    
    if ((tc_frame.tc_pdu[0] == 0x18) && (tc_frame.tc_pdu[1] == 0x80))	
    // Crypto Lib Application ID
    {
        #ifdef DEBUG
            OS_printf(KGRN "Received SDLS command: " RESET);
        #endif
        // CCSDS Header
        sdls_frame.hdr.pvn  	  = (tc_frame.tc_pdu[0] & 0xE0) >> 5;
        sdls_frame.hdr.type 	  = (tc_frame.tc_pdu[0] & 0x10) >> 4;
        sdls_frame.hdr.shdr 	  = (tc_frame.tc_pdu[0] & 0x08) >> 3;
        sdls_frame.hdr.appID      = ((tc_frame.tc_pdu[0] & 0x07) << 8) | tc_frame.tc_pdu[1];
        sdls_frame.hdr.seq  	  = (tc_frame.tc_pdu[2] & 0xC0) >> 6;
        sdls_frame.hdr.pktid      = ((tc_frame.tc_pdu[2] & 0x3F) << 8) | tc_frame.tc_pdu[3];
        sdls_frame.hdr.pkt_length = (tc_frame.tc_pdu[4] << 8) | tc_frame.tc_pdu[5];
        
        // CCSDS PUS
        sdls_frame.pus.shf		  = (tc_frame.tc_pdu[6] & 0x80) >> 7;
        sdls_frame.pus.pusv		  = (tc_frame.tc_pdu[6] & 0x70) >> 4;
        sdls_frame.pus.ack		  = (tc_frame.tc_pdu[6] & 0x0F);
        sdls_frame.pus.st		  = tc_frame.tc_pdu[7];
        sdls_frame.pus.sst		  = tc_frame.tc_pdu[8];
        sdls_frame.pus.sid		  = (tc_frame.tc_pdu[9] & 0xF0) >> 4;
        sdls_frame.pus.spare	  = (tc_frame.tc_pdu[9] & 0x0F);
        
        // SDLS TLV PDU
        sdls_frame.pdu.type 	  = (tc_frame.tc_pdu[10] & 0x80) >> 7;
        sdls_frame.pdu.uf   	  = (tc_frame.tc_pdu[10] & 0x40) >> 6;
        sdls_frame.pdu.sg   	  = (tc_frame.tc_pdu[10] & 0x30) >> 4;
        sdls_frame.pdu.pid  	  = (tc_frame.tc_pdu[10] & 0x0F);
        sdls_frame.pdu.pdu_len 	  = (tc_frame.tc_pdu[11] << 8) | tc_frame.tc_pdu[12];
        for (x = 13; x < (13 + sdls_frame.hdr.pkt_length); x++)
        {
            sdls_frame.pdu.data[x-13] = tc_frame.tc_pdu[x]; 
        }
        
        #ifdef CCSDS_DEBUG
            Crypto_ccsdsPrint(&sdls_frame); 
        #endif

        // Determine type of PDU
        *len_ingest = Crypto_PDU(ingest);
    }
    else
    {	// CCSDS Pass-through
        #ifdef DEBUG
            OS_printf(KGRN "CCSDS Pass-through \n" RESET);
        #endif
        // TODO: Remove PUS Header
        for (x = 0; x < (tc_frame.tc_header.fl - 11); x++)
        {
            ingest[x] = tc_frame.tc_pdu[x];
            #ifdef CCSDS_DEBUG
                OS_printf("tc_frame.tc_pdu[%d] = 0x%02x\n", x, tc_frame.tc_pdu[x]);
            #endif
        }
        *len_ingest = x;
    }

    #ifdef OCF_DEBUG
        Crypto_fsrPrint(&report);
    #endif
    
    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_TC_ProcessSecurity END -----\n" RESET);
    #endif

    return status;
}


int32 Crypto_TM_ApplySecurity( char* ingest, int* len_ingest)
// Accepts CCSDS message in ingest, and packs into TM before encryption
{
    int32 status = ITC_GCM128_SUCCESS;
    int count = 0;
    int pdu_loc = 0;
    int pdu_len = *len_ingest - TM_MIN_SIZE;
    int pad_len = 0;
    int mac_loc = 0;
    int fecf_loc = 0;
    uint8 tempTM[TM_SIZE];
    int x = 0;
    int y = 0;
    uint8 aad[20];
    uint16 spi = tm_frame.tm_sec_header.spi;
    uint16 spp_crc = 0x0000;
    SecurityAssociation_t* sa_ptr;
    SecurityAssociation_t sa;

    gcry_cipher_hd_t tmp_hd;
    gcry_error_t gcry_error = GPG_ERR_NO_ERROR;
    CFE_PSP_MemSet(&tempTM, 0, TM_SIZE);
    
    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_TM_ApplySecurity START -----\n" RESET);
    #endif

    // Check for idle frame trigger
    if (((uint8)ingest[0] == 0x08) && ((uint8)ingest[1] == 0x90))
    {   // Zero ingest
        for (x = 0; x < *len_ingest; x++)
        {
            ingest[x] = 0;
        }
        // Update TM First Header Pointer
        tm_frame.tm_header.fhp = 0xFE;
    }   
    else
    {   // Update the length of the ingest from the CCSDS header
        *len_ingest = (ingest[4] << 8) | ingest[5];
        ingest[5] = ingest[5] - 5;
        // Remove outgoing secondary space packet header flag
        ingest[0] = 0x00;
        // Change sequence flags to 0xFFFF
        ingest[2] = 0xFF;
        ingest[3] = 0xFF;
        // Add 2 bytes of CRC to space packet
        spp_crc = Crypto_Calc_CRC16((char*) ingest, *len_ingest);
        ingest[*len_ingest] = (spp_crc & 0xFF00) >> 8;
        ingest[*len_ingest+1] = (spp_crc & 0x00FF);
        *len_ingest = *len_ingest + 2;
        // Update TM First Header Pointer
        tm_frame.tm_header.fhp = tm_offset;
        #ifdef TM_DEBUG
            OS_printf("tm_offset = %d \n", tm_offset);
        #endif
    }             

    // Update Current Telemetry Frame in Memory
        // Counters
        tm_frame.tm_header.mcfc++;
        tm_frame.tm_header.vcfc++;
        // Operational Control Field 
        Crypto_TM_updateOCF();
        // Payload Data Unit
        Crypto_TM_updatePDU(ingest, *len_ingest);

        if(sadb_routine->sadb_get_sa_from_spi(spi,&sa_ptr) != OS_SUCCESS){
            //TODO - Error handling
            return OS_ERROR; //Error -- unable to get SA from SPI.
        }


    // Check test flags
        if (badSPI == 1)
        {
            tm_frame.tm_sec_header.spi++; 
        }
        if (badIV == 1)
        {
            sa_ptr->iv[IV_SIZE-1]++;
        }
        if (badMAC == 1)
        {
            tm_frame.tm_sec_trailer.mac[MAC_SIZE-1]++;
        }

    // Initialize the temporary TM frame
        // Header
        tempTM[count++] = (uint8) ((tm_frame.tm_header.tfvn << 6) | ((tm_frame.tm_header.scid & 0x3F0) >> 4));
        tempTM[count++] = (uint8) (((tm_frame.tm_header.scid & 0x00F) << 4) | (tm_frame.tm_header.vcid << 1) | (tm_frame.tm_header.ocff));
        tempTM[count++] = (uint8) (tm_frame.tm_header.mcfc);
        tempTM[count++] = (uint8) (tm_frame.tm_header.vcfc);
        tempTM[count++] = (uint8) ((tm_frame.tm_header.tfsh << 7) | (tm_frame.tm_header.sf << 6) | (tm_frame.tm_header.pof << 5) | (tm_frame.tm_header.slid << 3) | ((tm_frame.tm_header.fhp & 0x700) >> 8));
        tempTM[count++] = (uint8) (tm_frame.tm_header.fhp & 0x0FF);
        //	tempTM[count++] = (uint8) ((tm_frame.tm_header.tfshvn << 6) | tm_frame.tm_header.tfshlen);
        // Security Header
        tempTM[count++] = (uint8) ((spi & 0xFF00) >> 8);
        tempTM[count++] = (uint8) ((spi & 0x00FF));
        CFE_PSP_MemCpy(tm_frame.tm_sec_header.iv, sa_ptr->iv, IV_SIZE);

        // Padding Length
            pad_len = Crypto_Get_tmLength(*len_ingest) - TM_MIN_SIZE + IV_SIZE + TM_PAD_SIZE - *len_ingest;
        
        // Only add IV for authenticated encryption 
        if ((sa_ptr->est == 1) &&
            (sa_ptr->ast == 1))
        {	// Initialization Vector
            #ifdef INCREMENT
                Crypto_increment(sa_ptr->iv, IV_SIZE);
            #endif
            if ((sa_ptr->est == 1) || (sa_ptr->ast == 1))
            {	for (x = 0; x < IV_SIZE; x++)
                {
                    tempTM[count++] = sa_ptr->iv[x];
                }
            }
            pdu_loc = count;
            pad_len = pad_len - IV_SIZE - TM_PAD_SIZE + OCF_SIZE;
            pdu_len = *len_ingest + pad_len;
        }
        else	
        {	// Include padding length bytes - hard coded per ESA testing
            tempTM[count++] = 0x00;  // pad_len >> 8; 
            tempTM[count++] = 0x1A;  // pad_len
            pdu_loc = count;
            pdu_len = *len_ingest + pad_len;
        }
        
        // Payload Data Unit
        for (x = 0; x < (pdu_len); x++)	
        {
            tempTM[count++] = (uint8) tm_frame.tm_pdu[x];
        }
        // Message Authentication Code
        mac_loc = count;
        for (x = 0; x < MAC_SIZE; x++)
        {
            tempTM[count++] = 0x00;
        }
        // Operational Control Field
        for (x = 0; x < OCF_SIZE; x++)
        {
            tempTM[count++] = (uint8) tm_frame.tm_sec_trailer.ocf[x];
        }
        // Frame Error Control Field
        fecf_loc = count;
        tm_frame.tm_sec_trailer.fecf = Crypto_Calc_FECF((char*) tempTM, count);	
        tempTM[count++] = (uint8) ((tm_frame.tm_sec_trailer.fecf & 0xFF00) >> 8);
        tempTM[count++] = (uint8) (tm_frame.tm_sec_trailer.fecf & 0x00FF);

    // Determine Mode
        // Clear
        if ((sa_ptr->est == 0) &&
            (sa_ptr->ast == 0))
        {
            #ifdef DEBUG
                OS_printf(KBLU "Creating a TM - CLEAR! \n" RESET);
            #endif
            // Copy temporary frame to ingest
            CFE_PSP_MemCpy(ingest, tempTM, count);
        }
        // Authenticated Encryption
        else if ((sa_ptr->est == 1) &&
                 (sa_ptr->ast == 1))
        {
            #ifdef DEBUG
                OS_printf(KBLU "Creating a TM - AUTHENTICATED ENCRYPTION! \n" RESET);
            #endif

            // Copy TM to ingest
            CFE_PSP_MemCpy(ingest, tempTM, pdu_loc);
            
            #ifdef MAC_DEBUG
                OS_printf("AAD = 0x");
            #endif
            // Prepare additional authenticated data
            for (y = 0; y < sa_ptr->abm_len; y++)
            {
                aad[y] = ingest[y] & sa_ptr->abm[y];
                #ifdef MAC_DEBUG
                    OS_printf("%02x", aad[y]);
                #endif
            }
            #ifdef MAC_DEBUG
                OS_printf("\n");
            #endif

            gcry_error = gcry_cipher_open(
                &(tmp_hd), 
                GCRY_CIPHER_AES256, 
                GCRY_CIPHER_MODE_GCM, 
                GCRY_CIPHER_CBC_MAC
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_open error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
                status = OS_ERROR;
                return status;
            }
            gcry_error = gcry_cipher_setkey(
                tmp_hd, 
                &(ek_ring[sa_ptr->ekid].value[0]),
                KEY_SIZE
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_setkey error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
                status = OS_ERROR;
                return status;
            }
            gcry_error = gcry_cipher_setiv(
                tmp_hd, 
                &(sa_ptr->iv[0]),
                sa_ptr->iv_len
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_setiv error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
                status = OS_ERROR;
                return status;
            }
            gcry_error = gcry_cipher_encrypt(
                tmp_hd,
                &(ingest[pdu_loc]),                             // ciphertext output
                pdu_len,			 		                    // length of data
                &(tempTM[pdu_loc]),                             // plaintext input
                pdu_len                                         // in data length
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_decrypt error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
                status = OS_ERROR;
                return status;
            }
            gcry_error = gcry_cipher_authenticate(
                tmp_hd,
                &(aad[0]),                                      // additional authenticated data
                sa_ptr->abm_len       		                        // length of AAD
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_authenticate error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
                status = OS_ERROR;
                return status;
            }
            gcry_error = gcry_cipher_gettag(
                tmp_hd,
                &(ingest[mac_loc]),                             // tag output
                MAC_SIZE                                        // tag size
            );
            if((gcry_error & GPG_ERR_CODE_MASK) != GPG_ERR_NO_ERROR)
            {
                OS_printf(KRED "ERROR: gcry_cipher_checktag error code %d\n" RESET,gcry_error & GPG_ERR_CODE_MASK);
                status = OS_ERROR;
                return status;
            }

            #ifdef MAC_DEBUG
                OS_printf("MAC = 0x");
                for(x = 0; x < MAC_SIZE; x++)
                {
                    OS_printf("%02x", (uint8) ingest[x + mac_loc]);
                }
                OS_printf("\n");
            #endif

            // Update OCF
            y = 0;
            for (x = OCF_SIZE; x > 0; x--)
            {
                ingest[fecf_loc - x] = tm_frame.tm_sec_trailer.ocf[y++];
            }

            // Update FECF
            tm_frame.tm_sec_trailer.fecf = Crypto_Calc_FECF((char*) ingest, fecf_loc - 1);
            ingest[fecf_loc] = (uint8) ((tm_frame.tm_sec_trailer.fecf & 0xFF00) >> 8);
            ingest[fecf_loc + 1] = (uint8) (tm_frame.tm_sec_trailer.fecf & 0x00FF); 
        }
        // Authentication
        else if ((sa_ptr->est == 0) &&
                 (sa_ptr->ast == 1))
        {
            #ifdef DEBUG
                OS_printf(KBLU "Creating a TM - AUTHENTICATED! \n" RESET);
            #endif
            // TODO: Future work. Operationally same as clear.
            CFE_PSP_MemCpy(ingest, tempTM, count);
        }
        // Encryption
        else if ((sa_ptr->est == 1) &&
                 (sa_ptr->ast == 0))
        {
            #ifdef DEBUG
                OS_printf(KBLU "Creating a TM - ENCRYPTED! \n" RESET);
            #endif
            // TODO: Future work. Operationally same as clear.
            CFE_PSP_MemCpy(ingest, tempTM, count);
        }

    #ifdef TM_DEBUG
        Crypto_tmPrint(&tm_frame);		
    #endif	
    
    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_TM_ApplySecurity END -----\n" RESET);
    #endif

    *len_ingest = count;
    return status;    
}

int32 Crypto_TM_ProcessSecurity(char* ingest, int* len_ingest)
{
    // Local Variables
    int32 status = OS_SUCCESS;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_TM_ProcessSecurity START -----\n" RESET);
    #endif

    // TODO: This whole function!
    len_ingest = len_ingest;
    ingest[0] = ingest[0];

    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_TM_ProcessSecurity END -----\n" RESET);
    #endif

    return status;
}

int32 Crypto_AOS_ApplySecurity(char* ingest, int* len_ingest)
{
    // Local Variables
    int32 status = OS_SUCCESS;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_AOS_ApplySecurity START -----\n" RESET);
    #endif

    // TODO: This whole function!
    len_ingest = len_ingest;
    ingest[0] = ingest[0];

    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_AOS_ApplySecurity END -----\n" RESET);
    #endif

    return status;
}

int32 Crypto_AOS_ProcessSecurity(char* ingest, int* len_ingest)
{
    // Local Variables
    int32 status = OS_SUCCESS;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_AOS_ProcessSecurity START -----\n" RESET);
    #endif

    // TODO: This whole function!
    len_ingest = len_ingest;
    ingest[0] = ingest[0];

    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_AOS_ProcessSecurity END -----\n" RESET);
    #endif

    return status;
}

int32 Crypto_ApplySecurity(char* ingest, int* len_ingest)
{
    // Local Variables
    int32 status = OS_SUCCESS;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_ApplySecurity START -----\n" RESET);
    #endif

    // TODO: This whole function!
    len_ingest = len_ingest;
    ingest[0] = ingest[0];

    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_ApplySecurity END -----\n" RESET);
    #endif

    return status;
}

int32 Crypto_ProcessSecurity(char* ingest, int* len_ingest)
{
    // Local Variables
    int32 status = OS_SUCCESS;

    #ifdef DEBUG
        OS_printf(KYEL "\n----- Crypto_ProcessSecurity START -----\n" RESET);
    #endif

    // TODO: This whole function!
    len_ingest = len_ingest;
    ingest[0] = ingest[0];

    #ifdef DEBUG
        OS_printf(KYEL "----- Crypto_ProcessSecurity END -----\n" RESET);
    #endif

    return status;
}

#endif