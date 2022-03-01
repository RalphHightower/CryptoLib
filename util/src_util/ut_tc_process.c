/* Copyright (C) 2009 - 2022 National Aeronautics and Space Administration.
   All Foreign Rights are Reserved to the U.S. Government.

   This software is provided "as is" without any warranty of any kind, either expressed, implied, or statutory,
   including, but not limited to, any warranty that the software will conform to specifications, any implied warranties
   of merchantability, fitness for a particular purpose, and freedom from infringement, and any warranty that the
   documentation will conform to the program, or any warranty that the software will be error free.

   In no event shall NASA be liable for any damages, including, but not limited to direct, indirect, special or
   consequential damages, arising out of, resulting from, or in any way connected with the software or its
   documentation, whether or not based upon warranty, contract, tort or otherwise, and whether or not loss was sustained
   from, or arose out of the results of, or use of, the software, documentation or services provided hereunder.

   ITC Team
   NASA IV&V
   jstar-development-team@mail.nasa.gov
*/

/**
 *  Unit Tests that macke use of TC_ProcessSecurity function on the data.
 **/
#include "ut_tc_process.h"
#include "crypto.h"
#include "crypto_error.h"
#include "sadb_routine.h"
#include "utest.h"

/**
 * @brief Exercise the IV window checking logic
 * Test Cases: Replay, outside of window
 **/
UTEST(TC_PROCESS, EXERCISE_IV)
{
    uint8_t* ptr_enc_frame = NULL;
    // Setup & Initialize CryptoLib
    Crypto_Config_CryptoLib(SADB_TYPE_INMEMORY, CRYPTOGRAPHY_TYPE_LIBGCRYPT, CRYPTO_TC_CREATE_FECF_TRUE, TC_PROCESS_SDLS_PDUS_TRUE, TC_HAS_PUS_HDR,
                            TC_IGNORE_SA_STATE_FALSE, TC_IGNORE_ANTI_REPLAY_FALSE, TC_UNIQUE_SA_PER_MAP_ID_FALSE,
                            TC_CHECK_FECF_TRUE, 0x3F);
    Crypto_Config_Add_Gvcid_Managed_Parameter(0, 0x0003, 0, TC_HAS_FECF, TC_HAS_SEGMENT_HDRS);
    Crypto_Config_Add_Gvcid_Managed_Parameter(0, 0x0003, 1, TC_HAS_FECF, TC_HAS_SEGMENT_HDRS);
    Crypto_Init();
    SadbRoutine sadb_routine = get_sadb_routine_inmemory();
    crypto_key_t* ek_ring = cryptography_if->get_ek_ring();
    int status = 0;

    // NIST supplied vectors
    // NOTE: Added Transfer Frame header to the plaintext
    char* buffer_nist_key_h = "ef9f9284cf599eac3b119905a7d18851e7e374cf63aea04358586b0f757670f8";
    char* buffer_nist_iv_h = "b6ac8e4963f49207ffd6374b"; // The last valid IV that was seen by the SA
    char* buffer_replay_h = "2003002500FF0009B6AC8E4963F49207FFD6374B1224DFEFB72A20D49E09256908874979DFC1"; // IV is one less than library expects
    char* buffer_outside_window_h = "2003002500FF0009B6AC8E4963F49207FFD6375C1224DFEFB72A20D49E09256908874979B36E"; // IV is outside the positive window
    char* buffer_good_iv_h = "2003002500FF0009B6AC8E4963F49207FFD6374C1224DFEFB72A20D49E09256908874979AD6F"; // IV is the next one expected
    char* buffer_good_iv_with_gap_h = "2003002500FF0009B6AC8E4963F49207FFD6374F1224DFEFB72A20D49E092569088749799C49"; // IV is valid, but not next one expected
    uint8_t *buffer_replay_b, *buffer_outside_window_b, *buffer_good_iv_b, *buffer_good_iv_with_gap_b, *buffer_nist_iv_b, *buffer_nist_key_b = NULL;
    int buffer_replay_len, buffer_outside_window_len, buffer_good_iv_len, buffer_good_iv_with_gap_len, buffer_nist_iv_len, buffer_nist_key_len = 0;

    // Setup Processed Frame For Decryption
    TC_t* tc_nist_processed_frame;
    tc_nist_processed_frame = malloc(sizeof(uint8_t) * TC_SIZE);

    // Expose/setup SAs for testing
    SecurityAssociation_t* test_association = NULL;
    test_association = malloc(sizeof(SecurityAssociation_t) * sizeof(uint8_t));
    // Deactivate SA 1
    sadb_routine->sadb_get_sa_from_spi(1, &test_association);
    test_association->sa_state = SA_NONE;
    // Activate SA 9
    sadb_routine->sadb_get_sa_from_spi(9, &test_association);
    test_association->sa_state = SA_OPERATIONAL;
    sadb_routine->sadb_get_sa_from_spi(9, &test_association);
    test_association->ecs = calloc(1, test_association->ecs_len * sizeof(uint8_t));
    *test_association->ecs = CRYPTO_AES256_GCM;
    test_association->arsn_len = 1;
    test_association->arsnw = 5;
    // Insert key into keyring of SA 9
    hex_conversion(buffer_nist_key_h, (char**) &buffer_nist_key_b, &buffer_nist_key_len);
    memcpy(ek_ring[test_association->ekid].value, buffer_nist_key_b, buffer_nist_key_len);

    // Convert frames that will be processed
    hex_conversion(buffer_replay_h, (char**) &buffer_replay_b, &buffer_replay_len);
    hex_conversion(buffer_outside_window_h, (char**) &buffer_outside_window_b, &buffer_outside_window_len);
    hex_conversion(buffer_good_iv_h, (char**) &buffer_good_iv_b, &buffer_good_iv_len);
    hex_conversion(buffer_good_iv_with_gap_h, (char**) &buffer_good_iv_with_gap_b, &buffer_good_iv_with_gap_len);
    // Convert/Set input IV
    hex_conversion(buffer_nist_iv_h, (char**) &buffer_nist_iv_b, &buffer_nist_iv_len);
    memcpy(test_association->iv, buffer_nist_iv_b, buffer_nist_iv_len);

    // Expect to fail on replay
    printf(KGRN "Checking replay - using previous received IV...\n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_replay_b, &buffer_replay_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_ERR_IV_OUTSIDE_WINDOW, status);

    // Expect to fail on counter being too high
    printf(KGRN "Checking replay - using IV outside the window...\n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_outside_window_b, &buffer_outside_window_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_ERR_IV_OUTSIDE_WINDOW, status);

    // Expect success on valid IV
    printf(KGRN "Checking valid IV... should be able to receive it... \n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_good_iv_b, &buffer_good_iv_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_SUCCESS, status);

    // Expect success on valid IV within window, but has a gap
    printf(KGRN "Checking valid IV within window... should be able to receive it... \n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_good_iv_with_gap_b, &buffer_good_iv_with_gap_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_SUCCESS, status);

    // Validate that the SA IV is updated to the most recently received IV
    // IV length in this testing is 12 bytes
    printf(KGRN "Verifying IV updated correctly...\n" RESET);
    printf("SA IV is now:\t");
    for (int i = 0; i < test_association->shivf_len; i++)
        {
            ASSERT_EQ(*(test_association->iv + i), *(buffer_good_iv_with_gap_b +8 + i)); // 8 is IV offset into packet
            printf("%02X", *(test_association->iv + i));
        }
    printf("\n");
    Crypto_Shutdown();
    free(ptr_enc_frame);
    free(buffer_nist_iv_b);
    free(buffer_nist_key_b);
}

/**
 * @brief Exercise the ARSN window checking logic using AES CMAC
 * Test Cases: Replay, outside of window
 **/
UTEST(TC_PROCESS, EXERCISE_ARSN)
{
    uint8_t* ptr_enc_frame = NULL;
    // Setup & Initialize CryptoLib
    Crypto_Config_CryptoLib(SADB_TYPE_INMEMORY, CRYPTOGRAPHY_TYPE_LIBGCRYPT, CRYPTO_TC_CREATE_FECF_TRUE, TC_PROCESS_SDLS_PDUS_TRUE, TC_HAS_PUS_HDR,
                            TC_IGNORE_SA_STATE_FALSE, TC_IGNORE_ANTI_REPLAY_FALSE, TC_UNIQUE_SA_PER_MAP_ID_FALSE,
                            TC_CHECK_FECF_TRUE, 0x3F);
    Crypto_Config_Add_Gvcid_Managed_Parameter(0, 0x0003, 0, TC_HAS_FECF, TC_HAS_SEGMENT_HDRS);
    Crypto_Config_Add_Gvcid_Managed_Parameter(0, 0x0003, 1, TC_HAS_FECF, TC_HAS_SEGMENT_HDRS);
    Crypto_Init();
    SadbRoutine sadb_routine = get_sadb_routine_inmemory();
    crypto_key_t* ek_ring = cryptography_if->get_ek_ring();
    int status = 0;

    // NIST supplied vectors
    // NOTE: Added Transfer Frame header to the plaintext
    char* buffer_nist_key_h = "ef9f9284cf599eac3b119905a7d18851e7e374cf63aea04358586b0f757670f8";
    char* buffer_arsn_h = "0123"; // The last valid ARSN that was seen by the SA
    // For reference:        | Header  |SH SPI SN| Payload                       | MAC                           |FECF                
    char* buffer_replay_h = "2003002B00FF000901231224DFEFB72A20D49E09256908874979fd56ca1ffc2697a700dbe6292c10e9ef1B49"; // ARSN is one less than library expects
    char* buffer_outside_window_h = "2003002B00FF000904441224DFEFB72A20D49E09256908874979fd56ca1ffc2697a700dbe6292c10e9ef9C5C"; // ARSN is outside the positive window
    char* buffer_good_arsn_h = "2003002B00FF000901241224DFEFB72A20D49E09256908874979fd56ca1ffc2697a700dbe6292c10e9ef8A3E"; // ARSN is the next one expected
    char* buffer_good_arsn_with_gap_h = "2003002B00FF000901291224DFEFB72A20D49E09256908874979fd56ca1ffc2697a700dbe6292c10e9ef3EB4"; // ARSN is valid, but not next one expected
    uint8_t *buffer_replay_b, *buffer_outside_window_b, *buffer_good_arsn_b, *buffer_good_arsn_with_gap_b, *buffer_arsn_b, *buffer_nist_key_b = NULL;
    int buffer_replay_len, buffer_outside_window_len, buffer_good_arsn_len, buffer_good_arsn_with_gap_len, buffer_arsn_len, buffer_nist_key_len = 0;

    // Setup Processed Frame For Decryption
    TC_t* tc_nist_processed_frame;
    tc_nist_processed_frame = malloc(sizeof(uint8_t) * TC_SIZE);

    // Expose/setup SAs for testing
    SecurityAssociation_t* test_association = NULL;
    test_association = malloc(sizeof(SecurityAssociation_t) * sizeof(uint8_t));
    // Deactivate SA 1
    sadb_routine->sadb_get_sa_from_spi(1, &test_association);
    test_association->sa_state = SA_NONE;
    // Activate SA 9
    sadb_routine->sadb_get_sa_from_spi(9, &test_association);
    test_association->sa_state = SA_OPERATIONAL;
    sadb_routine->sadb_get_sa_from_spi(9, &test_association);
    test_association->ecs = calloc(1, test_association->ecs_len * sizeof(uint8_t));
    *test_association->ecs = CRYPTO_ECS_NONE;
    test_association->acs = calloc(1, test_association->acs_len * sizeof(uint8_t));
    *test_association->acs = CRYPTO_AES256_CMAC;
    test_association->est = 0;
    test_association->ast = 1;
    test_association->shivf_len = 0;
    test_association->shsnf_len = 2;
    test_association->arsn_len = 2;
    test_association->arsnw = 5;
    test_association->abm_len = 1024;
    // memset(test_association->abm, 0x00, (test_association->abm_len * sizeof(uint8_t)));
    test_association->abm = calloc(1, test_association->abm_len * sizeof(uint8_t));
    test_association->stmacf_len = 16;
    // Insert key into keyring of SA 9
    hex_conversion(buffer_nist_key_h, (char**) &buffer_nist_key_b, &buffer_nist_key_len);
    memcpy(ek_ring[test_association->akid].value, buffer_nist_key_b, buffer_nist_key_len);
    // Convert frames that will be processed
    hex_conversion(buffer_replay_h, (char**) &buffer_replay_b, &buffer_replay_len);
    hex_conversion(buffer_outside_window_h, (char**) &buffer_outside_window_b, &buffer_outside_window_len);
    hex_conversion(buffer_good_arsn_h, (char**) &buffer_good_arsn_b, &buffer_good_arsn_len);
    hex_conversion(buffer_good_arsn_with_gap_h, (char**) &buffer_good_arsn_with_gap_b, &buffer_good_arsn_with_gap_len);
    // Convert/Set input IV
    hex_conversion(buffer_arsn_h, (char**) &buffer_arsn_b, &buffer_arsn_len);
    test_association->arsn = calloc(1, test_association->arsn_len);
    memcpy(test_association->arsn, buffer_arsn_b, buffer_arsn_len);
    // Expect to fail on replay
    printf(KGRN "Checking replay - using previous received IV...\n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_replay_b, &buffer_replay_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_ERR_ARSN_OUTSIDE_WINDOW, status);

    // Expect to fail on counter being too high
    printf(KGRN "Checking replay - using IV outside the window...\n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_outside_window_b, &buffer_outside_window_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_ERR_ARSN_OUTSIDE_WINDOW, status);

    // Expect success on valid IV
    printf(KGRN "Checking next valid IV... should be able to receive it... \n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_good_arsn_b, &buffer_good_arsn_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_SUCCESS, status);

    // // Expect success on valid IV within window, but has a gap
    printf(KGRN "Checking valid IV within window... should be able to receive it... \n" RESET);
    status = Crypto_TC_ProcessSecurity(buffer_good_arsn_with_gap_b, &buffer_good_arsn_with_gap_len, tc_nist_processed_frame);
    ASSERT_EQ(CRYPTO_LIB_SUCCESS, status);

    // Validate that the SA ARSN is updated to the most recently received ARSN
    // ARSN length in this testing is 2 bytes
    printf(KGRN "Verifying ARSN updated correctly...\n" RESET);
    printf("SA ARSN is now:\t");
    for (int i = 0; i < test_association->shsnf_len; i++)
        {
            printf("%02X", *(test_association->arsn + i));
            ASSERT_EQ(*(test_association->arsn + i), *(buffer_good_arsn_with_gap_b + 8 + i)); // 8 is ARSN offset into packet
        }
    printf("\n");
    Crypto_Shutdown();
    free(ptr_enc_frame);
    free(buffer_nist_key_b);
}

UTEST_MAIN();
