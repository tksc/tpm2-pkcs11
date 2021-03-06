/* SPDX-License-Identifier: BSD-2 */
/*
 * Copyright (c) 2018, Intel Corporation
 * All rights reserved.
 */
#include <assert.h>
#include <string.h>

#include "pkcs11.h"

#include "digest.h"
#include "encrypt.h"
#include "key.h"
#include "log.h"
#include "general.h"
#include "object.h"
#include "random.h"
#include "session.h"
#include "sign.h"
#include "slot.h"
#include "token.h"

// TODO REMOVE ME
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#define TRACE_CALL LOGV("enter \"%s\"", __func__)
#define TRACE_RET(rv)\
    do { \
        CK_RV _rc = (rv); \
        LOGV("return \"%s\" value: %lu", __func__, _rc);\
        if (_rc == CKR_FUNCTION_NOT_SUPPORTED) {\
            assert(0);\
        }\
        return _rc;\
    } while (0)

/**
 * Interface to calling into the internal interface from a cryptoki
 * routine that takes slot id. Performs all locking on token.
 *
 * @note
 *  - Requires a CK_RV rv to be declared.
 *  - Performs **NO** auth checking. The slot routines implemented
 *    do not need a particular state AFAIK.
 *  - manages token locking
 *
 * @param userfun
 *  The userfunction to call, ie the internal API.
 * @param ...
 *  The arguments to the internal API call from cryptoki.
 * @return
 *  The internal API's result as rv.
 */
#define TOKEN_WITH_LOCK_BY_SLOT(userfunc, slot, ...) \
do { \
    \
    check_is_init(); \
    \
    token *t = slot_get_token(slot); \
    if (!t) { \
        return CKR_SLOT_ID_INVALID; \
    } \
    \
    token_lock(t); \
    rv = userfunc(t, ##__VA_ARGS__); \
    token_unlock(t); \
} while (0)

/**
 * Raw interface (DO NOT USE DIRECTLY) for calling into the internal interface from a cryptoki
 * routine that takes a session handle. Performs all locking on token.
 *
 * @note
 *  - Requires a CK_RV rv to be declared.
 *  - do not use directly, use the ones with auth model.
 *  - manages token locking
 *
 * @param userfun
 *  The userfunction to call, ie the internal API.
 * @param ...
 *  The arguments to the internal API call from cryptoki.
 * @return
 *  The internal API's result as rv.
 */
#define __TOKEN_WITH_LOCK_BY_SESSION(authfn, userfunc, session, ...) \
do { \
    \
    check_is_init(); \
    \
    token *t = NULL; \
    session_ctx *ctx = NULL; \
    rv = session_lookup(session, &t, &ctx); \
    if (rv != CKR_OK) { \
        return rv; \
    } \
    \
    rv = authfn(ctx); \
    if (rv != CKR_OK) { \
        token_unlock(t); \
        return rv; \
    } \
    rv = userfunc(t, ##__VA_ARGS__); \
    token_unlock(t); \
} while (0)

/*
 * Same as: __TOKEN_WITH_LOCK_BY_SESSION but hands the session_ctx to the internal routine.
 */
#define __TOKEN_WITH_LOCK_BY_SESSION_KEEP_CTX(authfn, userfunc, session, ...) \
do { \
    \
    check_is_init(); \
    \
    token *t = NULL; \
    session_ctx *ctx = NULL; \
    rv = session_lookup(session, &t, &ctx); \
    if (rv != CKR_OK) { \
        return rv; \
    } \
    \
    rv = authfn(ctx); \
    if (rv != CKR_OK) { \
        token_unlock(t); \
        return rv; \
    } \
    rv = userfunc(t, ctx, ##__VA_ARGS__); \
    token_unlock(t); \
} while (0)

/*
 * Below you'll find the auth routines that validate session
 * context. Becuase session context is required to be in a
 * certain state for things, these auth plugins check a vary
 * specific condition. Add more if you need different checks.
 */
static inline CK_RV auth_min_ro_pub(session_ctx *ctx) {

    UNUSED(ctx);
    return CKR_OK;
}

static inline CK_RV auth_min_ro_user(session_ctx *ctx) {

    CK_STATE state = session_ctx_state_get(ctx);
    switch(state) {
    case CKS_RO_USER_FUNCTIONS:
        /* falls-thru */
    case CKS_RW_USER_FUNCTIONS:
        return CKR_OK;
        /* no default */
    }

    return CKR_USER_NOT_LOGGED_IN;
}

static inline CK_RV auth_any_logged_in(session_ctx *ctx) {
    CK_STATE state = session_ctx_state_get(ctx);
    switch(state) {
    case CKS_RO_USER_FUNCTIONS:
    case CKS_RW_USER_FUNCTIONS:
    case CKS_RW_SO_FUNCTIONS:
        return CKR_OK;
        /* no default */
    }

    return CKR_USER_NOT_LOGGED_IN;
}

/*
 * The macros below are used to call into the cryptoki API and perform a myriad of checking using certain
 * auth models. Not using these is dangerous.
 */

/*
 * Does what __TOKEN_WITH_LOCK_BY_SESSION does, and checks that the session is at least RO Public Ie any session would work.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_PUB_RO(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION(auth_min_ro_pub, userfunc, session, ##__VA_ARGS__)

/*
 * Does what TOKEN_WITH_LOCK_BY_SESSION_PUB_RO does, but passes the session_ctx to the internal api.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_PUB_RO_KEEP_CTX(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION_KEEP_CTX(auth_min_ro_pub, userfunc, session, ##__VA_ARGS__)

/*
 * Does what __TOKEN_WITH_LOCK_BY_SESSION does, and checks that the session is at least RW Public. Ie no one logged in and R/W session.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_PUB_RW(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION(auth_min_rw_pub, userfunc, session, ##__VA_ARGS__)

/*
 * Does what __TOKEN_WITH_LOCK_BY_SESSION does, and checks that the session is at least RO User. Ie user logged in and R/O or R/W session.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_USER_RO(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION(auth_min_ro_user, userfunc, session, ##__VA_ARGS__)

/*
 * Does what __TOKEN_WITH_LOCK_BY_SESSION does, and checks that the session is at least RO User. Ie user logged in and R/W session.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_USER_RW(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION(auth_min_rw_user, userfunc, session, ##__VA_ARGS__)

/*
 * Does what __TOKEN_WITH_LOCK_BY_SESSION does, and checks that the session is at least RO User. Ie so logged in and R/W session.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_SO_RW(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION(auth_min_rw_so, userfunc, session, ##__VA_ARGS__)

/*
 * Does what __TOKEN_WITH_LOCK_BY_SESSION does, and checks that the session is at least RO User. Ie user or so logged in and R/O or R/W session.
 */
#define TOKEN_WITH_LOCK_BY_SESSION_LOGGED_IN(userfunc, session, ...) __TOKEN_WITH_LOCK_BY_SESSION(auth_any_logged_in, userfunc, session, ##__VA_ARGS__)

CK_RV C_Initialize (void *init_args) {
    TRACE_CALL;
    TRACE_RET(general_init(init_args));
}

CK_RV C_Finalize (void *pReserved) {
    TRACE_CALL;
    TRACE_RET(general_finalize(pReserved));
}

CK_RV C_GetInfo (CK_INFO *info) {
    TRACE_CALL;
    TRACE_RET(general_get_info(info));
}

CK_RV C_GetFunctionList (CK_FUNCTION_LIST **function_list) {
    TRACE_CALL;
    TRACE_RET(general_get_func_list(function_list));
}

CK_RV C_GetSlotList (unsigned char token_present, CK_SLOT_ID *slot_list, unsigned long *count) {
    TRACE_CALL;
    TRACE_RET(slot_get_list(token_present, slot_list, count));
}

CK_RV C_GetSlotInfo (CK_SLOT_ID slotID, CK_SLOT_INFO *info) {
    TRACE_CALL;
    TRACE_RET(slot_get_info(slotID, info));
}

CK_RV C_GetTokenInfo (CK_SLOT_ID slotID, CK_TOKEN_INFO *info) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SLOT(token_get_info, slotID, info);
    TRACE_RET(rv);
}

CK_RV C_WaitForSlotEvent (CK_FLAGS flags, CK_SLOT_ID *slot, void *pReserved) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GetMechanismList (CK_SLOT_ID slotID, CK_MECHANISM_TYPE *mechanism_list, unsigned long *count) {
    TRACE_CALL;
    TRACE_RET(slot_mechanism_list_get(slotID, mechanism_list, count));
}

CK_RV C_GetMechanismInfo (CK_SLOT_ID slotID, CK_MECHANISM_TYPE type, CK_MECHANISM_INFO *info) {
    TRACE_CALL;
    TRACE_RET(slot_mechanism_info_get(slotID, type, info));
}

CK_RV C_InitToken (CK_SLOT_ID slotID, unsigned char *pin, unsigned long pin_len, unsigned char *label) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_InitPIN (CK_SESSION_HANDLE session, unsigned char *pin, unsigned long pin_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SetPIN (CK_SESSION_HANDLE session, unsigned char *old_pin, unsigned long old_len, unsigned char *new_pin, unsigned long new_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_OpenSession (CK_SLOT_ID slotID, CK_FLAGS flags, void *application, CK_NOTIFY notify, CK_SESSION_HANDLE *session) {
    TRACE_CALL;
    TRACE_RET(session_open(slotID, flags, application, notify, session));
}

CK_RV C_CloseSession (CK_SESSION_HANDLE session) {
    TRACE_CALL;
    TRACE_RET(session_close(session));
}

CK_RV C_CloseAllSessions (CK_SLOT_ID slotID) {
    TRACE_CALL;
    TRACE_RET(session_closeall(slotID));
}

CK_RV C_GetSessionInfo (CK_SESSION_HANDLE session, CK_SESSION_INFO *info) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_PUB_RO_KEEP_CTX(session_get_info, session, info);
    TRACE_RET(rv);
}

CK_RV C_GetOperationState (CK_SESSION_HANDLE session, unsigned char *operation_state, unsigned long *operation_state_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SetOperationState (CK_SESSION_HANDLE session, unsigned char *operation_state, unsigned long operation_state_len, CK_OBJECT_HANDLE encryption_key, CK_OBJECT_HANDLE authentiation_key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_Login (CK_SESSION_HANDLE session, CK_USER_TYPE user_type, unsigned char *pin, unsigned long pin_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_PUB_RO(session_login, session, user_type, pin, pin_len);
    TRACE_RET(rv);
}

CK_RV C_Logout (CK_SESSION_HANDLE session) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_LOGGED_IN(session_logout, session);
    TRACE_RET(rv);
}

CK_RV C_CreateObject (CK_SESSION_HANDLE session, CK_ATTRIBUTE *templ, unsigned long count, CK_OBJECT_HANDLE *object) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_CopyObject (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, CK_ATTRIBUTE *templ, unsigned long count, CK_OBJECT_HANDLE *new_object) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DestroyObject (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GetObjectSize (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, unsigned long *size) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GetAttributeValue (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, CK_ATTRIBUTE *templ, unsigned long count) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_PUB_RO(object_get_attributes, session, object, templ, count);
    TRACE_RET(rv);
}

CK_RV C_SetAttributeValue (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE object, CK_ATTRIBUTE *templ, unsigned long count) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_FindObjectsInit (CK_SESSION_HANDLE session, CK_ATTRIBUTE *templ, unsigned long count) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_PUB_RO(object_find_init, session, templ, count);
    TRACE_RET(rv);
}

CK_RV C_FindObjects (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE *object, unsigned long max_object_count, unsigned long *object_count) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_PUB_RO(object_find, session, object, max_object_count, object_count);
    TRACE_RET(rv);
}

CK_RV C_FindObjectsFinal (CK_SESSION_HANDLE session) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_PUB_RO(object_find_final, session);
    TRACE_RET(rv);
}

CK_RV C_EncryptInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(encrypt_init, session, mechanism, key);
    TRACE_RET(rv);
}

CK_RV C_Encrypt (CK_SESSION_HANDLE session, unsigned char *data, unsigned long data_len, unsigned char *encrypted_data, unsigned long *encrypted_data_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(encrypt_oneshot, session, data, data_len, encrypted_data, encrypted_data_len);
    TRACE_RET(rv);
}

CK_RV C_EncryptUpdate (CK_SESSION_HANDLE session, unsigned char *part, unsigned long part_len, unsigned char *encrypted_part, unsigned long *encrypted_part_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(encrypt_update, session, part, part_len, encrypted_part, encrypted_part_len);
    TRACE_RET(rv);
}

CK_RV C_EncryptFinal (CK_SESSION_HANDLE session, unsigned char *last_encrypted_part, unsigned long *last_encrypted_part_len) {

    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(encrypt_final, session, last_encrypted_part, last_encrypted_part_len);
    TRACE_RET(rv);
}

CK_RV C_DecryptInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(decrypt_init, session, mechanism, key);
    TRACE_RET(rv);
}

CK_RV C_Decrypt (CK_SESSION_HANDLE session, unsigned char *encrypted_data, unsigned long encrypted_data_len, unsigned char *data, unsigned long *data_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(decrypt_oneshot, session, encrypted_data, encrypted_data_len, data, data_len);
    TRACE_RET(rv);
}

CK_RV C_DecryptUpdate (CK_SESSION_HANDLE session, unsigned char *encrypted_part, unsigned long encrypted_part_len, unsigned char *part, unsigned long *part_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(decrypt_update, session, encrypted_part, encrypted_part_len, part, part_len);
    TRACE_RET(rv);
}

CK_RV C_DecryptFinal (CK_SESSION_HANDLE session, unsigned char *last_part, unsigned long *last_part_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(decrypt_final, session, last_part, last_part_len);
    TRACE_RET(rv);
}

CK_RV C_DigestInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(digest_init, session, mechanism);
    TRACE_RET(rv);
}

CK_RV C_Digest (CK_SESSION_HANDLE session, unsigned char *data, unsigned long data_len, unsigned char *digest, unsigned long *digest_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(digest_oneshot, session, data, data_len, digest, digest_len);
    TRACE_RET(rv);
}

CK_RV C_DigestUpdate (CK_SESSION_HANDLE session, unsigned char *part, unsigned long part_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(digest_update, session, part, part_len);
    TRACE_RET(rv);
}

CK_RV C_DigestKey (CK_SESSION_HANDLE session, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DigestFinal (CK_SESSION_HANDLE session, unsigned char *digest, unsigned long *digest_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(digest_final, session, digest, digest_len);
    TRACE_RET(rv);
}

CK_RV C_SignInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(sign_init, session, mechanism, key);
    TRACE_RET(rv);
}

CK_RV C_Sign (CK_SESSION_HANDLE session, unsigned char *data, unsigned long data_len, unsigned char *signature, unsigned long *signature_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(sign, session, data, data_len, signature, signature_len);
    TRACE_RET(rv);
}

CK_RV C_SignUpdate (CK_SESSION_HANDLE session, unsigned char *part, unsigned long part_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(sign_update, session, part, part_len);
    TRACE_RET(rv);
}

CK_RV C_SignFinal (CK_SESSION_HANDLE session, unsigned char *signature, unsigned long *signature_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(sign_final, session, signature, signature_len);
    TRACE_RET(rv);
}

CK_RV C_SignRecoverInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SignRecover (CK_SESSION_HANDLE session, unsigned char *data, unsigned long data_len, unsigned char *signature, unsigned long *signature_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_VerifyInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(verify_init, session, mechanism, key);
    TRACE_RET(rv);
}

CK_RV C_Verify (CK_SESSION_HANDLE session, unsigned char *data, unsigned long data_len, unsigned char *signature, unsigned long signature_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(verify, session, data, data_len, signature, signature_len);
    TRACE_RET(rv);
}

CK_RV C_VerifyUpdate (CK_SESSION_HANDLE session, unsigned char *part, unsigned long part_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(verify_update, session, part, part_len);
    TRACE_RET(rv);
}

CK_RV C_VerifyFinal (CK_SESSION_HANDLE session, unsigned char *signature, unsigned long signature_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(verify_final, session, signature, signature_len);
    TRACE_RET(rv);
}

CK_RV C_VerifyRecoverInit (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_VerifyRecover (CK_SESSION_HANDLE session, unsigned char *signature, unsigned long signature_len, unsigned char *data, unsigned long *data_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DigestEncryptUpdate (CK_SESSION_HANDLE session, unsigned char *part, unsigned long part_len, unsigned char *encrypted_part, unsigned long *encrypted_part_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DecryptDigestUpdate (CK_SESSION_HANDLE session, unsigned char *encrypted_part, unsigned long encrypted_part_len, unsigned char *part, unsigned long *part_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SignEncryptUpdate (CK_SESSION_HANDLE session, unsigned char *part, unsigned long part_len, unsigned char *encrypted_part, unsigned long *encrypted_part_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DecryptVerifyUpdate (CK_SESSION_HANDLE session, unsigned char *encrypted_part, unsigned long encrypted_part_len, unsigned char *part, unsigned long *part_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GenerateKey (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_ATTRIBUTE *templ, unsigned long count, CK_OBJECT_HANDLE *key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_GenerateKeyPair (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_ATTRIBUTE *public_key_template, unsigned long public_key_attribute_count, CK_ATTRIBUTE *private_key_template, unsigned long private_key_attribute_count, CK_OBJECT_HANDLE *public_key, CK_OBJECT_HANDLE *private_key) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(key_gen, session, mechanism, public_key_template, public_key_attribute_count, private_key_template, private_key_attribute_count, public_key, private_key);
    TRACE_RET(rv);
}

CK_RV C_WrapKey (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE wrapping_key, CK_OBJECT_HANDLE key, unsigned char *wrapped_key, unsigned long *wrapped_key_len) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_UnwrapKey (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE unwrapping_key, unsigned char *wrapped_key, unsigned long wrapped_key_len, CK_ATTRIBUTE *templ, unsigned long attribute_count, CK_OBJECT_HANDLE *key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_DeriveKey (CK_SESSION_HANDLE session, CK_MECHANISM *mechanism, CK_OBJECT_HANDLE base_key, CK_ATTRIBUTE *templ, unsigned long attribute_count, CK_OBJECT_HANDLE *key) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_SeedRandom (CK_SESSION_HANDLE session, unsigned char *seed, unsigned long seed_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(seed_random, session, seed, seed_len);
    TRACE_RET(rv);
}

CK_RV C_GenerateRandom (CK_SESSION_HANDLE session, unsigned char *random_data, unsigned long random_len) {
    TRACE_CALL;
    CK_RV rv = CKR_GENERAL_ERROR;
    TOKEN_WITH_LOCK_BY_SESSION_USER_RO(random_get, session, random_data, random_len);
    TRACE_RET(rv);
}

CK_RV C_GetFunctionStatus (CK_SESSION_HANDLE session) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

CK_RV C_CancelFunction (CK_SESSION_HANDLE session) {
    TRACE_CALL;
    TRACE_RET(CKR_FUNCTION_NOT_SUPPORTED);
}

// TODO REMOVE ME
#pragma GCC diagnostic pop
