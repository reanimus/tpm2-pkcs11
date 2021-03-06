/* SPDX-License-Identifier: BSD-2 */
/*
 * Copyright (c) 2018, Intel Corporation
 * All rights reserved.
 */
#include "checks.h"
#include "log.h"
#include "object.h"
#include "pkcs11.h"
#include "session_ctx.h"
#include "token.h"
#include "utils.h"

typedef struct tobject_match_list tobject_match_list;
struct tobject_match_list {
    tobject *obj;
    tobject_match_list *next;
};

typedef struct object_find_data object_find_data;
struct object_find_data {
    tobject_match_list *head;
    tobject_match_list *cur;
};

void tobject_free(tobject *tobj) {

    twist_free(tobj->priv);
    twist_free(tobj->pub);
    twist_free(tobj->objauth);
    twist_free(tobj->unsealed_auth);

    CK_ULONG i = 0;
    CK_RV rv = utils_attr_free(tobj->atributes.attrs, tobj->atributes.count);
    assert(rv == CKR_OK);
    free(tobj->atributes.attrs);

    for (i=0; i < tobj->mechanisms.count; i++) {
        CK_MECHANISM_PTR m = &tobj->mechanisms.mech[i];
        if (m->pParameter) {
            free(m->pParameter);
        }
    }

    free(tobj->mechanisms.mech);

    free(tobj);
}

void sobject_free(sobject *sobj) {
    twist_free(sobj->priv);
    twist_free(sobj->pub);
    twist_free(sobj->objauth);
    twist_free(sobj->authraw);
}

void wrappingobject_free(wrappingobject *wobj) {
    twist_free(wobj->priv);
    twist_free(wobj->pub);
    twist_free(wobj->objauth);
}

void sealobject_free(sealobject *sealobj) {
    twist_free(sealobj->soauthsalt);
    twist_free(sealobj->sopriv);
    twist_free(sealobj->sopub);
    twist_free(sealobj->userauthsalt);
    twist_free(sealobj->userpub);
    twist_free(sealobj->userpriv);
}

static bool object_CKM_RSA_PKCS_OAEP_params_supported(
        CK_RSA_PKCS_OAEP_PARAMS_PTR requested,
        CK_RSA_PKCS_OAEP_PARAMS_PTR got) {

    return requested->hashAlg == got->hashAlg &&
            requested->mgf == got->mgf;
}

static bool object_CKM_AES_CBC_params_supported(
        CK_MECHANISM_PTR requested
        ) {

    // IV is blocksize for AES
    return requested->ulParameterLen == 16;
}

CK_RV object_mech_is_supported(tobject *tobj, CK_MECHANISM_PTR mech) {

    bool is_equal;
    CK_ULONG i;
    bool got_to_params = false;
    for (i=0; i < tobj->mechanisms.count; i++) {
        CK_MECHANISM_PTR m = &tobj->mechanisms.mech[i];

        if (mech->mechanism != m->mechanism) {
            continue;
        }

        got_to_params = true;

        /*
         * Ensure the parameters are supported, this would need to be done for each mechanism
         * as things like label, etc are flexible. However, keep a default handler of strict
         * memcmp for things that are empty or can be fully specified in the DB.
         */
        switch (mech->mechanism) {
        case CKM_RSA_X_509:
            /* no params */
            is_equal = true;
            break;
        case CKM_RSA_PKCS_OAEP:
            is_equal = object_CKM_RSA_PKCS_OAEP_params_supported(
                    mech->pParameter,
                    m->pParameter
                    );
            break;
        case CKM_AES_CBC:
            is_equal = object_CKM_AES_CBC_params_supported(
                    mech);
            break;
        default:
            is_equal =
                mech->ulParameterLen == m->ulParameterLen
            && !memcmp(mech->pParameter, m->pParameter, m->ulParameterLen);
        }

        if(!is_equal) {
            continue;
        }

        /* match */
        return CKR_OK;
    }

    return got_to_params ? CKR_MECHANISM_PARAM_INVALID : CKR_MECHANISM_INVALID;
}

tobject *object_attr_filter(tobject *tobj, CK_ATTRIBUTE_PTR templ, CK_ULONG count) {

    CK_ULONG i;
    // If ulCount is set to 0 all items match automatically.
    if (count == 0) {
	    return tobj;
    }

    for (i=0; i < count; i++) {
        CK_ATTRIBUTE_PTR search = &templ[i];

        CK_ATTRIBUTE_PTR compare = NULL;
        bool is_attr_match = false;
        CK_ULONG j;
        for(j=0; j < tobj->atributes.count; j++) {
            compare = &tobj->atributes.attrs[j];

            if (search->type != compare->type) {
                continue;
            }

            if (search->ulValueLen != compare->ulValueLen) {
                continue;
            }

            bool match = !memcmp(compare->pValue, search->pValue, search->ulValueLen);
            if (match) {
                is_attr_match = true;
                break;
            }
        }

        /*
         * If we didn't get an attribute match, then the searched for attribute wasn't
         * found and it's not a match. Ie search attribute set must be subset of compare
         * attribute set
         */

        if (!is_attr_match) {
            return NULL;
        }

    }

    /*
     * Done with matching loops, we always found a match, thus we have a match
     * assign it.
     */

    /* all the specified template attributes matched */
    return tobj;
}

void free_object_find_data(object_find_data *fd) {

    if (!fd) {
        return;
    }

    tobject_match_list *cur = fd->head;
    while (cur) {
        tobject_match_list *tmp = cur;
        cur = cur->next;
        free(tmp);
    }

    free(fd);
}

CK_RV object_find_init(token *tok, CK_ATTRIBUTE_PTR templ, CK_ULONG count) {

    // if count is 0 template is not used and all objects are requested so templ can be NULL.
    if (count > 0) {
        check_pointer(templ);
    }

    CK_RV rv = CKR_GENERAL_ERROR;

    object_find_data *fd = NULL;

    bool is_active = token_opdata_is_active(tok);
    if (is_active) {
        rv = CKR_OPERATION_ACTIVE;
        goto out;
    }

    fd = calloc(1, sizeof(*fd));
    if (!fd) {
        rv = CKR_HOST_MEMORY;
        goto out;
    }

    if (!tok->tobjects) {
        goto empty;
    }

    tobject_match_list *match_cur = NULL;
    list *cur = &tok->tobjects->l;
    while(cur) {

        // Get the current object, and grab it's id for the object handle
        tobject *tobj = list_entry(cur, tobject, l);
        cur = cur->next;

        tobject *match = object_attr_filter(tobj, templ, count);
        if (!match) {
            continue;
        }

        /* we have a match, build the list */
        if (!fd->head) {
            /* set the head to point into the list */
            fd->head = calloc(1, sizeof(*match_cur));
            if (!fd->head) {
                rv = CKR_HOST_MEMORY;
                goto out;
            }

            match_cur = fd->head;
            match_cur->obj = tobj;

        } else {
            assert(match_cur);
            match_cur->next = calloc(1, sizeof(*match_cur));
            if (!match_cur->next) {
                rv = CKR_HOST_MEMORY;
                goto out;
            }

            match_cur->next->obj = tobj;
            match_cur = match_cur->next;
        }
    }

    fd->cur = fd->head;

empty:

    token_opdata_set(tok, operation_find, fd);

    rv = CKR_OK;

out:
    if (rv != CKR_OK) {
        free_object_find_data(fd);
    }

    return rv;
}

CK_RV object_find(token *tok, CK_OBJECT_HANDLE *object, CK_ULONG max_object_count, CK_ULONG_PTR object_count) {

    check_pointer(object);
    check_pointer(object_count);

    UNUSED(max_object_count);

    CK_RV rv = CKR_OK;

    object_find_data *opdata = NULL;
    rv = token_opdata_get(tok, operation_find, &opdata);
    if (rv != CKR_OK) {
        return rv;
    }

    CK_ULONG count = 0;
    while(opdata->cur && count < max_object_count) {

        // Get the current object, and grab it's id for the object handle
        tobject *tobj = opdata->cur->obj;
        object[count] = tobj->id;

        // Update our iterator
        opdata->cur = opdata->cur->next;

        count++;
    }

    *object_count = count;

    return CKR_OK;
}

CK_RV object_find_final(token *tok) {


    CK_RV rv = CKR_GENERAL_ERROR;

    object_find_data *opdata = NULL;
    rv = token_opdata_get(tok, operation_find, &opdata);
    if (rv != CKR_OK) {
        return rv;
    }

    free_object_find_data(opdata);

    token_opdata_clear(tok);

    return CKR_OK;
}

static tobject *find_object_by_id(CK_OBJECT_HANDLE handle, token *tok) {

    list *cur = &tok->tobjects->l;
    while(cur) {

        tobject *cur_tobj = list_entry(cur, tobject, l);

        if (handle == cur_tobj->id) {
            return cur_tobj;
        }
        cur = cur->next;
    }

    return NULL;
}

CK_ATTRIBUTE_PTR object_get_attribute_by_type(tobject *tobj, CK_ATTRIBUTE_TYPE atype) {

    CK_ULONG i;
    for (i=0; i < tobj->atributes.count; i++) {

        CK_ATTRIBUTE_PTR a = &tobj->atributes.attrs[i];

        if (a->type == atype) {
            return a;
        }
    }

    return NULL;
}

CK_ATTRIBUTE_PTR object_get_attribute_full(tobject *tobj, CK_ATTRIBUTE_PTR attr) {

    CK_ULONG i;
    for (i=0; i < tobj->atributes.count; i++) {

        CK_ATTRIBUTE_PTR a = &tobj->atributes.attrs[i];

        if (a->type == attr->type
         && a->ulValueLen == attr->ulValueLen) {
            if (a->ulValueLen > 0
             && memcmp(a->pValue, attr->pValue, attr->ulValueLen)) {
                /* length is greater then 0 and don't match, keep looking */
                continue;
            }
            /* length is both 0 OR length > 0 and matched on memcmp */
            return a;
        }
    }

    return NULL;
}

CK_RV object_get_attributes(token *tok, CK_OBJECT_HANDLE object, CK_ATTRIBUTE *templ, CK_ULONG count) {

    tobject *tobj = find_object_by_id(object, tok);
    /* no match */
    if (!tobj) {
        return CKR_OBJECT_HANDLE_INVALID;
    }

    /*
     * For each item requested in the template, find if the request has a match
     * and copy the size and possibly data (if allocated).
     */

    CK_ULONG i;
    for (i=0; i < count; i++) {

        CK_ATTRIBUTE_PTR t = &templ[i];

        CK_ATTRIBUTE_PTR found = object_get_attribute_by_type(tobj, t->type);
        if (found) {
            if (!t->pValue) {
                /* only populate size if the buffer is null */
                t->ulValueLen = found->ulValueLen;
                continue;
            }

            /* The found attribute should fit inside the one to copy to */
            if (found->ulValueLen > t->ulValueLen) {
                return CKR_BUFFER_TOO_SMALL;
            }

            t->ulValueLen = found->ulValueLen;
            memcpy(t->pValue, found->pValue, found->ulValueLen);
       } else {
           /* If it's not found it defaults to empty. */
           t->pValue = NULL;
           t->ulValueLen = 0;
       }
    }

    return CKR_OK;
}

tobject *tobject_new(void) {

    tobject *tobj = calloc(1, sizeof(tobject));
    if (!tobj) {
        LOGE("oom");
        return NULL;
    }

    return tobj;
}

void tobject_set_blob_data(tobject *tobj, twist pub, twist priv) {
    assert(priv);
    assert(pub);

    tobj->priv = priv;
    tobj->pub = pub;
}

void tobject_set_auth(tobject *tobj, twist authbin, twist wrappedauthhex) {
    assert(tobj);
    assert(authbin);
    assert(wrappedauthhex);

    tobj->unsealed_auth = authbin;
    tobj->objauth = wrappedauthhex;
}

void tobject_set_handle(tobject *tobj, uint32_t handle) {
    assert(tobj);

    tobj->handle = handle;
}

CK_RV tobject_append_attrs(tobject *tobj, CK_ATTRIBUTE_PTR attrs, CK_ULONG count) {
    assert(tobj);
    assert(attrs);

    if (!attrs->ulValueLen) {
        return CKR_OK;
    }

    size_t offset = tobj->atributes.count;
    size_t newlen = (tobj->atributes.count + count);
    size_t newbytes = sizeof(*tobj->atributes.attrs) * newlen;
    void *newattrs = realloc(tobj->atributes.attrs, newbytes);
    if (!newattrs) {
        return CKR_HOST_MEMORY;
    }

    tobj->atributes.count = newlen;
    tobj->atributes.attrs = newattrs;

    /* clear out the newly allocated memory */
    memset(&tobj->atributes.attrs[offset], 0, count * sizeof(*tobj->atributes.attrs));

    return utils_attr_deep_copy(attrs, count, &tobj->atributes.attrs[offset]);
}

void tobject_set_id(tobject *tobj, unsigned id) {
    assert(tobj);
    tobj->id = id;
}

CK_RV tobject_append_mechs(tobject *tobj, CK_MECHANISM_PTR mech, CK_ULONG count) {
    assert(tobj);

    size_t offset = tobj->mechanisms.count;
    size_t newcnt = tobj->mechanisms.count + count;
    size_t newbytes = sizeof(*tobj->mechanisms.mech) * newcnt;

    void *newmechs = realloc(tobj->mechanisms.mech, newbytes);
    if (!newmechs) {
        LOGE("oom");
        return CKR_HOST_MEMORY;
    }

    tobj->mechanisms.count = newcnt;
    tobj->mechanisms.mech = newmechs;

    /* clear out the newly allocated memory */
    memset(&tobj->mechanisms.mech[offset], 0, count * sizeof(*tobj->mechanisms.mech));

    return utils_mech_deep_copy(mech, count, &tobj->mechanisms.mech[offset]);
}
